#!/usr/bin/env python3
"""Verify optional SDK relocation logic using only synthetic resource tables."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path,
                        default=Path.home() / 'android-sdk/ndk/27.2.12479018')
    args = parser.parse_args()
    out = ROOT / 'build/native-sdk-relocation'
    out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which('clang')
    if not cc:
        raise RuntimeError('clang is required')
    tools = args.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    android_cc = tools / 'aarch64-linux-android30-clang'
    source = ROOT / 'ports/shared-android/runtime/native_sdk_fastpath.c'
    header = source.with_suffix('.h')
    test = ROOT / 'tests/native_sdk_relocation_test.c'
    session = ROOT / 'native/runtime/session_layout.c'
    paths = [source, header, test, session,
             ROOT / 'native/runtime/session_layout.h',
             ROOT / 'native/runtime/aot_cpu.h', Path(__file__).resolve()]
    pinned = {str(p.relative_to(ROOT)): sha(p) for p in paths}

    def run(command, name):
        result = subprocess.run(list(map(str, command)), text=True,
                                capture_output=True, timeout=60)
        (out / name).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{name}: {result.stdout}{result.stderr}')
        return result.stdout

    common = ['-std=c11', '-Wall', '-Wextra', '-Werror',
              '-I', ROOT / 'native', '-I', source.parent]
    reports = {}
    for mode, mode_flags in [('optimized', ['-O2']),
                             ('sanitized', ['-O1', '-g',
                                            '-fsanitize=address,undefined',
                                            '-fno-omit-frame-pointer'])]:
        for abi in ('standalone', 'production'):
            name = mode + '-' + abi
            flags = ['-DNATIVE_SDK_RELOCATION_STANDALONE'] if abi == 'standalone' else []
            sources = [source, test] + ([] if abi == 'standalone' else [session])
            executable = out / name
            run([cc, *common, *mode_flags, *flags, *sources, '-o', executable],
                name + '-build.log')
            result = run([executable], name + '-run.log')
            match = re.search(r'SDK relocation assertions=(\d+);', result)
            if not match or int(match[1]) < 2500:
                raise RuntimeError('Missing fixture assertion count')
            reports[name] = {'assertions': int(match[1]), 'passed': True}

    # Link the production source, including the exact resolver and layout API.
    # This is an asset-free diagnostic DSO, not an Android runtime replacement.
    helper = out / 'libnative_sdk_relocation_check.so'
    exports = out / 'exports.map'
    symbols_expected = ['native_sdk_fastpath', 'native_sdk_relocate_table',
                        'native_sdk_relocate_table_ex']
    exports.write_text('SDK_RELOCATION_CHECK_1 { global: ' +
                       '; '.join(symbols_expected) + '; local: *; };\n')
    run([android_cc, *common, '-O2', '-g', '-ffixed-x18', '-fPIC', '-shared',
         source, session, '-Wl,--no-undefined',
         '-Wl,--version-script=' + str(exports),
         '-Wl,-z,max-page-size=16384', '-Wl,-z,relro', '-Wl,-z,now',
         '-Wl,-z,noexecstack', '-o', helper], 'android-link.log')
    symbols = run([tools / 'llvm-nm', '--dynamic', '--defined-only', helper],
                  'android-exports.log')
    actual = [line.split()[-1].split('@')[0] for line in symbols.splitlines()]
    if actual != symbols_expected:
        raise RuntimeError('Unexpected Android exports: ' + repr(actual))
    if pinned != {str(p.relative_to(ROOT)): sha(p) for p in paths}:
        raise RuntimeError('Source changed during verification')
    report = {'schema': 1, 'host_tests': reports,
              'android_production_link_passed': True,
              'source_sha256': pinned, 'android_helper_sha256': sha(helper),
              'actual_sdk_differential': 'separate coordinator-owned probe pending',
              'scope': 'Synthetic tables, optional resolver gating and complete Cpu preservation; no game files, no device execution'}
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
