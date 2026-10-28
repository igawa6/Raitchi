#!/usr/bin/env python3
"""Check native startup failure gates and player notices without games/devices."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-session-prepare'
SOURCES = [
    'tests/native_session_prepare_test.c',
    'ports/shared-android/runtime/crash_explain.c',
    'native/runtime/native_prepare.c', 'native/module_abi.c',
    'native/runtime/native_manifest.c', 'native/runtime/nso_image.c',
    'native/runtime/session_layout.c', 'native/adapters/title_adapter.c',
    'native/adapters/las/profile.c', 'native/adapters/letsgo/profile.c',
    'native/adapters/echoes/profile.c', 'native/adapters/wonder/profile.c',
    'native/adapters/generic/generic.c',
    'ports/shared-android/runtime/shared_hash.cpp',
]


def run(command):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout.strip()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('prepare', ROOT / 'tools/prepare_native_game.py')
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    (OUT / 'shared_contract.h').write_text(
        '#define SHARED_NATIVE_CONTRACT_SHA256_HEX "' + prepare.contract_digest().hex() + '"\n'
        '#define SHARED_AOT_CONTRACT_SHA256_HEX "' + '00' * 32 + '"\n')
    results = {}
    for variant, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
        objects = []
        for index, source in enumerate(SOURCES):
            path = ROOT / source
            cpp = path.suffix == '.cpp'
            obj = OUT / (variant + '-' + str(index) + '-' + path.stem + '.o')
            run([shutil.which('clang++' if cpp else 'clang'),
                 '-std=c++17' if cpp else '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                 '-ffunction-sections', '-fdata-sections', *flags,
                 '-I', OUT, '-I', ROOT / 'native',
                 '-I', ROOT / 'ports/shared-android/runtime',
                 '-I', ROOT / 'sources/las-android/runtime', '-c', path, '-o', obj])
            objects.append(obj)
        binary = OUT / variant
        run([shutil.which('clang++'), *flags, *objects, '-Wl,--gc-sections', '-pthread', '-ldl', '-o', binary])
        cases = []
        for title in ('eevee', 'las'):
            for case in ('ok', 'selected', 'active', 'collision', 'memory', 'hint', 'files'):
                with tempfile.TemporaryDirectory(prefix='native-startup-', dir=OUT) as directory:
                    result = run([binary, directory, title, case])
                    print(variant + ': ' + result, flush=True)
                    cases.append(result)
        results[variant] = cases
    inputs = [*SOURCES, 'ports/shared-android/runtime/gamelib.c',
              'tools/check_native_session_prepare.py']
    report = {'scope': 'Production native startup, real preparation/hash/manifest/adapter/layout, '
              'synthetic original NSOs, controlled reservation and registry, actual crash notice. '
              'No guest execution, Android mapping or device compatibility established.',
              'results': results, 'source_sha256': {
                  path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in inputs}}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
