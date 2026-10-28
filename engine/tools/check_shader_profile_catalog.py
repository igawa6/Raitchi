#!/usr/bin/env python3
"""Synthetic host gate for exact APK metadata catalogs and packaging allowlists."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import package_shared_android as packager

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'ports/shared-android/runtime'
OUT = ROOT / 'build/shader-profile-catalog'


def run(args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
    if p.returncode:
        raise RuntimeError(' '.join(map(str, args)) + '\n' + p.stdout + p.stderr)
    return p.stdout


def packaging_tests(tmp):
    directory = tmp / 'assets'
    directory.mkdir()
    identity = '010203' + '00' * 29
    profile = directory / (identity + '.bin')
    shutil.copyfile(tmp / 'catalog.bin', profile)
    entry = {'game_version_identity': identity, 'file': profile.name,
             'sha256': hashlib.sha256(profile.read_bytes()).hexdigest(), 'records': 150}
    manifest = {'schema': 1, 'metadata_only': True, 'profiles': [entry]}

    def save():
        (directory / 'manifest.json').write_text(json.dumps(manifest))

    def rejected():
        try:
            packager.shader_profile_catalog_assets(directory)
        except (RuntimeError, subprocess.CalledProcessError):
            return
        raise AssertionError('unsafe catalog accepted')

    save()
    resources = packager.shader_profile_catalog_assets(directory)
    assert set(resources) == {'shader-profiles/manifest.json', 'shader-profiles/' + profile.name}
    entry['file'] = '../' + profile.name
    save(); rejected(); entry['file'] = profile.name
    entry['records'] = 1
    save(); rejected(); entry['records'] = 150
    entry['sha256'] = '00' * 32
    save(); rejected(); entry['sha256'] = hashlib.sha256(profile.read_bytes()).hexdigest()
    save()
    (directory / 'unexpected.bin').write_bytes(b'not allowed')
    rejected(); (directory / 'unexpected.bin').unlink()
    profile.rename(directory / 'original')
    profile.symlink_to(directory / 'original')
    rejected(); profile.unlink(); (directory / 'original').rename(profile)
    # Matching byte digest cannot disguise a wrong-version profile.
    wrong = 'ff' * 32
    profile.rename(directory / (wrong + '.bin'))
    entry['game_version_identity'] = wrong; entry['file'] = wrong + '.bin'
    save(); rejected()
    (directory / (wrong + '.bin')).rename(profile)
    entry['game_version_identity'] = identity; entry['file'] = profile.name
    good = profile.read_bytes(); profile.write_bytes(good + b'payload')
    entry['sha256'] = hashlib.sha256(profile.read_bytes()).hexdigest()
    save(); rejected(); profile.write_bytes(good)
    manifest['profiles'] = []
    profile.unlink(); save()
    assert set(packager.shader_profile_catalog_assets(directory)) == {'shader-profiles/manifest.json'}
    return {'validated_metadata': True, 'wrong_scope_digest_path_and_count_rejected': True,
            'unlisted_symlink_and_payload_rejected': True, 'empty_catalog_allowed': True}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cc, cxx = shutil.which(os.environ.get('CC', 'clang')), shutil.which(os.environ.get('CXX', 'clang++'))
    include = [RT / 'nvn', ROOT / 'sources/las-android/runtime/nvn', RT]
    paths = [RT / 'nvn/shader_profile.c', RT / 'nvn/shader_profile_catalog.c',
             ROOT / 'tests/shader_profile_catalog_test.c']
    results = {}
    for mode, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined',
                         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]:
        objects = []
        for path in paths:
            obj = OUT / (mode + '-' + path.stem + '.o')
            run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', *flags,
                 *sum((['-I', p] for p in include), []), '-c', path, '-o', obj])
            objects.append(obj)
        hobj = OUT / (mode + '-hash.o')
        run([cxx, '-std=c++17', *flags, '-I', ROOT / 'sources/las-android/runtime', '-I', RT,
             '-I', ROOT / 'native', '-c', RT / 'shared_hash.cpp', '-o', hobj])
        exe = OUT / (mode + '-test')
        run([cxx, *flags, *objects, hobj, '-o', exe])
        with tempfile.TemporaryDirectory(prefix='profile-catalog-') as tmp:
            result = json.loads(run([exe, tmp]))
            results[mode] = {'runtime': result, 'packaging': packaging_tests(Path(tmp))}
    report = {'state': 'passed', 'results': results,
              'scope': 'Synthetic metadata only. Exact asset paths, bounded reads, parser validation, '
                       'wrong identity, malformed/truncated input, atomic unresolved metadata union, '
                       'no-overwrite failures, unchanged no-rewrite, packaging manifest allowlist. '
                       'No device/game/GPU execution.'}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
