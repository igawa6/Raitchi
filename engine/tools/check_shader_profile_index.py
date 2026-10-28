#!/usr/bin/env python3
"""Exercise actual startup profile/source integration with synthetic RomFS/BNSH."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'ports/shared-android/runtime'
OUT = ROOT / 'build/shader-profile-index'


def run(args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
    if p.returncode:
        raise RuntimeError(' '.join(map(str, args)) + '\n' + p.stdout + p.stderr)
    return p.stdout.strip()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cc, cxx = shutil.which('clang'), shutil.which('clang++')
    includes = [RT / 'nvn', ROOT / 'work/shared-android/runtime/nvn', RT]
    paths = [RT / 'nvn/shader_profile.c', RT / 'nvn/shader_profile.h', RT / 'nvn/shader_preparation.c',
             RT / 'nvn/shader_preparation.h', RT / 'nvn/shader_source_index.c', RT / 'nvn/shader_source_index.h',
             RT / 'nxpkg.c', RT / 'nxpkg.h', ROOT / 'tests/shader_profile_index_test.c', Path(__file__).resolve()]
    paths.extend([RT / 'nvn/shader_profile_catalog.c', RT / 'nvn/shader_profile_catalog.h'])
    results = {}
    for mode, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined',
                         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]:
        objects = []
        for path in [paths[0], paths[2], paths[8], paths[10]]:
            obj = OUT / (mode + '-' + path.stem + '.o')
            run([cc, '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-macro-redefined',
                 *flags, *sum((['-I', p] for p in includes), []), '-c', path, '-o', obj])
            objects.append(obj)
        hobj = OUT / (mode + '-hash.o')
        run([cxx, '-std=c++17', *flags, '-I', ROOT / 'sources/las-android/runtime', '-I', RT,
             '-I', ROOT / 'native', '-c', RT / 'shared_hash.cpp', '-o', hobj])
        exe = OUT / (mode + '-test')
        run([cxx, *flags, *objects, hobj, '-pthread', '-o', exe])
        with tempfile.TemporaryDirectory(prefix='shader-profile-index-') as tmp:
            results[mode] = json.loads(run([exe, tmp]))
    report = {'state': 'passed', 'results': results,
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
              'scope': 'Actual metadata profile + discovery store + chunked BNSH index + startup source/profile coordinator; '
                       'synthetic extracted folder and synthetic NxRomfs file reader with stub package opener, '
                       'graphics/depth-only/compute exact inputs, cold global profile import and warm no-source/no-package-open, '
                       'NSO-only absolute prepared-root cold/warm flow, wrong-version local profile preserved while current global '
                       'profile imports and receives new learned variants, failed package fallback, wrong game, absent profile '
                       'and cancellation; cold bundled catalog-only preparation, warm no-source catalog reuse, absent/corrupt/'
                       'wrong-version assets, unresolved metadata persistence through learned export and later re-resolution. '
                       'No real NSP selection/decryption/JNI, GPU, game or device.'}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
