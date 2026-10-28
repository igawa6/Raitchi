#!/usr/bin/env python3
"""Asset-free source-verified upstream recipe importer regression."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import import_shader_variants as importer
ROOT = importer.ROOT
OUT = ROOT / 'build/import-shader-variants'


def main():
    results = {}
    for sanitized in (False, True):
        importer.build(sanitized)
        name = 'sanitized' if sanitized else 'optimized'
        flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if sanitized else ['-O2']
        includes = [OUT, importer.RT / 'nvn', ROOT / 'sources/las-android/runtime/nvn', importer.RT]
        obj = OUT / (name + '-test.o')
        importer.run([shutil.which('clang'), '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                      '-Wno-unused-function', '-Wno-macro-redefined', '-Wno-logical-op-parentheses', *flags,
                      *sum((['-I', p] for p in includes), []), '-c', ROOT / 'tests/import_shader_variants_test.c', '-o', obj])
        exe = OUT / (name + '-test')
        objects = [OUT / (name + '-' + stem + '.o') for stem in ('shader_profile', 'shader_source_index', 'nxpkg', 'hash')]
        importer.run([shutil.which('clang++'), *flags, obj, *objects, '-pthread', '-o', exe])
        with tempfile.TemporaryDirectory(prefix='shader-variant-import-') as tmp:
            results[name] = json.loads(importer.run([exe, tmp]))
            tmp = Path(tmp)
            cli = [OUT / name, tmp / 'valid.txt', '11' + '00' * 31, tmp / 'profile.bin', '-', tmp, '-', '-', '-']
            valid = json.loads(importer.run(cli))
            assert valid['input_union_records'] == 2 and valid['union_records'] == 3
            assert valid['cold_verified_recipes'] == 2 and valid['warm_reused'] == 2 and valid['warm_resolver_reads'] == 0
            before = (tmp / 'profile.bin').read_bytes()
            for bad in ('invalid.txt', 'nul.txt', 'oversized-line.txt', 'huge.txt'):
                rejected = list(cli)
                rejected[1] = tmp / bad
                p = subprocess.run(list(map(str, rejected)), capture_output=True, text=True, timeout=120)
                assert p.returncode != 0 and (tmp / 'profile.bin').read_bytes() == before
            wrong = list(cli)
            wrong[2] = '12' + '00' * 31
            wrong[-1] = tmp / 'profile.bin'
            assert subprocess.run(list(map(str, wrong)), capture_output=True, timeout=120).returncode != 0
            assert (tmp / 'profile.bin').read_bytes() == before
            results[name]['cli_existing_union_count'] = 3
            results[name]['cli_invalid_preserves_destination'] = True
            results[name]['cli_cold_reconstruct_warm_no_source_reads'] = True
    paths = [ROOT / 'tools/import_shader_variants.py', ROOT / 'tools/import_shader_variants.c', Path(__file__).resolve(),
             ROOT / 'tests/import_shader_variants_test.c']
    report = {'state': 'passed', 'results': results,
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
              'production_sources': json.loads((OUT / 'optimized-build.json').read_text())['sources'],
              'scope': 'Actual upstream parser/formatter and current renderer key block, original-source stage/hash/partition checks, '
                       'bounded strict canonical parsing, malformed blend-count/length and unknown/overflow fields, duplicate recipe '
                       'dedup, source corruption/missing/wrong-stage negatives, metadata-only exact wire codec. Synthetic only; '
                       'no game/device/GPU test and no complete texture-binding coverage claim.'}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
