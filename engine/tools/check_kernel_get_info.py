#!/usr/bin/env python3
"""Exercise actual GetInfo helpers/ABI with controlled resources; no games/device."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android', action='store_true', help='Also compile canonical kernel using the local Android build database')
    options = parser.parse_args()
    out = ROOT / 'build/kernel-get-info'
    out.mkdir(parents=True, exist_ok=True)
    kernel = ROOT / 'ports/shared-android/runtime/kernel.c'
    source = kernel.read_text()

    def body(signature):
        start = source.index(signature)
        end = source.index('{', start) + 1
        depth = 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]

    branch = body('    case 0x29: {')
    header = '\n'.join(body(n) for n in ('static int physical_memory_info(', 'static uint32_t process_info('))
    header += '\nstatic void dispatch_get_info(uint64_t *x) { switch (0x29) {\n' + branch + '\n} }\n'
    (out / 'kernel_get_info_bodies.h').write_text(header)

    def run(command, log):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=90)
        (out / log).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(log + ': ' + result.stdout + result.stderr)
        return result.stdout.strip()

    results = {}
    for mode, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
        binary = out / mode
        run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', out, *flags,
             ROOT / 'tests/kernel_get_info_test.c', '-o', binary], mode + '-compile.log')
        results[mode] = run([binary], mode + '-run.log')

    if options.android:
        # Compile the entire canonical kernel against the existing Android build flags.
        # Only a private build overlay is written; staging/work files stay untouched.
        overlay = out / 'production/runtime'
        for tree in (ROOT / 'work/shared-android/runtime', ROOT / 'ports/shared-android/runtime'):
            for path in tree.rglob('*'):
                if path.is_file() and path.suffix in ('.h', '.inc'):
                    target = overlay / path.relative_to(tree)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, target)
        target = overlay / 'kernel.c'
        shutil.copyfile(kernel, target)
        rows = json.loads((ROOT / 'build/shared-android/native/compile_commands.json').read_text())
        row = next(r for r in rows if r['file'].endswith('/runtime/kernel.c'))
        args = row.get('arguments') or shlex.split(row['command'])
        command = [args[0], '-I' + str(overlay), '-I' + str(ROOT / 'native')]
        i = 1
        while i < len(args):
            if args[i] in ('-o', '-MF', '-MT', '-MQ'):
                i += 2
                continue
            if args[i] in ('-MD', '-MMD', '-MP'):
                i += 1
                continue
            arg = args[i].replace(str(ROOT / 'work/shared-android/runtime'), str(overlay))
            command.append(str(target) if args[i] == row['file'] else arg)
            i += 1
        run([*command, '-o', out / 'kernel.o'], 'android-kernel-compile.log')
    paths = [kernel, ROOT / 'tests/kernel_get_info_test.c', Path(__file__).resolve()]
    report = {'state': 'passed', 'host': results, 'whole_kernel_android_compile': options.android,
              'scope': 'Synthetic actual-source contract and ABI checks; no guest launch/device verification',
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
