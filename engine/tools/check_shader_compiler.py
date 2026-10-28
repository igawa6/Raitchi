#!/usr/bin/env python3
"""Actual direct/legacy compiler selector, exact fallback, ownership and policy."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'ports/shared-android/runtime/nvn/maxwell'
OUT = ROOT / 'build/shader-compiler'
TEST = ROOT / 'tests/compilerpolicy53_test.c'


def run(args, name):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
    (OUT / name).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(name + ': ' + result.stdout + result.stderr)
    return result.stdout


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    files = [RT / 'shader_compiler.c', RT / 'shader_compiler.h', RT / 'direct_spirv.h',
             RT / 'shader.h', RT / 'translate.c', TEST, Path(__file__).resolve()]
    pins = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    results = {}
    for name, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined',
                         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]:
        objects = []
        for source in [RT / 'shader_compiler.c', RT / 'translate.c', TEST]:
            obj = OUT / (name + '-' + source.stem + '.o')
            extra = ['-Dmalloc=compiler_test_malloc'] if source.name == 'shader_compiler.c' else []
            run([shutil.which('clang'), '-std=gnu11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-function', '-Wno-macro-redefined', '-Wno-unused-parameter',
                 '-Wno-missing-field-initializers', *flags, *extra,
                 '-I', RT, '-I', ROOT / 'native', '-c', source, '-o', obj], name + '-' + source.stem + '-build.log')
            objects.append(obj)
        executable = OUT / name
        run([shutil.which('clang'), *flags, *objects, '-pthread', '-lm', '-o', executable], name + '-link.log')
        results[name] = json.loads(run([executable], name + '-run.log'))
    assert pins == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}, 'sources changed during checks'
    report = {'state': 'passed', 'results': results, 'source_sha256': pins,
              'scope': 'Actual compiler policy/selector and legacy translator; synthetic direct emitter isolates unsupported/invalid/OOM/malformed output, wrapper allocation failure, ownership, shaderc overrides, debug/dialect/bounds guards and concurrent counter behavior. No real direct-emitter coverage, GPU, device or game evidence.'}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
