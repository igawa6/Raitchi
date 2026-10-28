#!/usr/bin/env python3
"""Verify production game/version preparation profiles using synthetic shaders."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'ports/shared-android/runtime'
OUT = ROOT / 'build/shader-profile'


def run(args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90)
    if p.returncode:
        raise RuntimeError(' '.join(map(str, args)) + '\n' + p.stdout + p.stderr)
    return p.stdout.strip()


def inspect(path):
    b = path.read_bytes()
    assert b[:8] == b'NVNPROF\0'
    schema, header, count, record = struct.unpack_from('<IIII', b, 8)
    assert (schema, header, count, record) == (1, 120, 3, 513)
    assert len(b) == header + count * record
    assert hashlib.sha256(b[:88]).digest() == b[88:120]
    assert hashlib.sha256(b[120:]).digest() == b[56:88]
    for i in range(count):
        entry = b[header + i * record:header + (i + 1) * record]
        assert hashlib.sha256(entry[:481]).digest() == entry[481:]
        assert len(entry) == 513  # no code/constants bytes after fixed metadata
    first = b[120:120 + 481]
    assert struct.unpack_from('<I', first, 369 + 44)[0] == 0x12345678
    assert struct.unpack_from('<I', first, 369 + 48 + 44)[0] == 0x80000001
    assert struct.unpack_from('<IIII', first, 465) == (128, 16, 128, 8)
    return {'bytes': len(b), 'records': count, 'metadata_only': True,
            'independent_sha256_and_exact_input_fields': True}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cc = shutil.which(os.environ.get('CC', 'clang'))
    cxx = shutil.which(os.environ.get('CXX', 'clang++'))
    if not cc or not cxx:
        raise SystemExit('clang and clang++ required')
    includes = [RT / 'nvn', ROOT / 'work/shared-android/runtime/nvn', RT]
    paths = [RT / 'nvn/shader_profile.c', RT / 'nvn/shader_profile.h',
             RT / 'nvn/shader_discovery.h', ROOT / 'tests/shader_profile_test.c',
             RT / 'shared_hash.cpp', Path(__file__).resolve()]
    results = {}
    for mode, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined',
                         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]:
        objects = []
        for path in [paths[0], paths[3]]:
            obj = OUT / (mode + '-' + path.stem + '.o')
            run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', *flags,
                 *sum((['-I', p] for p in includes), []), '-c', path, '-o', obj])
            objects.append(obj)
        hobj = OUT / (mode + '-hash.o')
        run([cxx, '-std=c++17', *flags, '-I', ROOT / 'sources/las-android/runtime', '-I', RT,
             '-I', ROOT / 'native', '-c', RT / 'shared_hash.cpp', '-o', hobj])
        exe = OUT / (mode + '-test')
        run([cxx, *flags, *objects, hobj, '-o', exe])
        with tempfile.TemporaryDirectory(prefix='shader-profile-') as tmp:
            result = json.loads(run([exe, tmp]))
            wire = inspect(Path(tmp) / 'profile.bin')
            journal, output = Path(tmp) / 'learned.bin', Path(tmp) / 'cli-profile.bin'
            before = journal.read_bytes()
            exported = json.loads(run(['python3', ROOT / 'tools/shader_profile_tool.py', 'export',
                                      '--journal', journal, '--output', output]))
            inspected = json.loads(run(['python3', ROOT / 'tools/shader_profile_tool.py', 'inspect', '--profile', output]))
            assert exported['records'] == inspected['records'] == 3
            assert exported['graphics'] == 2 and exported['compute'] == 1 and exported['depth_only'] == 1
            assert not exported['shader_payload_included'] and not inspected['shader_payload_included']
            assert journal.read_bytes() == before
            inspect(output)
            torn = Path(tmp) / 'cli-torn.bin'
            torn.write_bytes(before[:-5])
            recovered = json.loads(run(['python3', ROOT / 'tools/shader_profile_tool.py', 'export',
                                       '--journal', torn, '--output', Path(tmp) / 'cli-recovered.bin']))
            assert recovered['journal_recovered_prefix'] and recovered['records'] == 2
            assert torn.read_bytes() == before[:-5]
            corrupt = Path(tmp) / 'cli-corrupt.bin'
            bad = bytearray(before); bad[100] ^= 1; corrupt.write_bytes(bad)
            refused = Path(tmp) / 'cli-refused.bin'
            failure = subprocess.run(['python3', str(ROOT / 'tools/shader_profile_tool.py'), 'export',
                                      '--journal', str(corrupt), '--output', str(refused)], capture_output=True, text=True)
            assert failure.returncode != 0 and not refused.exists() and not failure.stdout
            cli = {'export_inspect_roundtrip': True, 'input_preserved': True,
                   'recovered_prefix_reported': True, 'corrupt_journal_refused': True}
        results[mode] = {'host': result, 'independent_wire': wire, 'metadata_cli': cli}
    report = {'state': 'passed', 'results': results,
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in [*paths, ROOT / 'tools/shader_profile_tool.c', ROOT / 'tools/shader_profile_tool.py']},
              'scope': 'Production scoped metadata codec + merge/export + actual discovery store and SHA256; synthetic code only. '
                       'Typed-texture keys, graphics/depth-only/compute partition, warm no-source resolution, dedup, cold import/next-open, '
                       'wrong identity, malformed metadata with recomputed digests, corruption, truncation/trailing/limits, cancel, '
                       'source hash failure, atomic-preservation and clean no-rewrite. No game/device/GPU execution.'}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
