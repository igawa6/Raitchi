#!/usr/bin/env python3
"""Host-only tests of extracted production shader-cache code, with fake shaderc/GPU.

Executes the real headstart identity/open/append and SPIR-V hash/cache/compile
functions, plus the pinned production SHA256 wrapper. No game data or GPU use.
"""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/shader-cache-identity'
EXEC = ROOT / 'ports/shared-android/runtime/nvn/vulkan/exec.c'
SHADERS = ROOT / 'ports/shared-android/runtime/nvn/vulkan/shaders.c'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    at = brace + 1
    while depth:
        if source[at] == '{':
            depth += 1
        elif source[at] == '}':
            depth -= 1
        at += 1
    return source[start:at] + '\n'


def run(command, cwd=OUT):
    result = subprocess.run(list(map(str, command)), cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise SystemExit(f'{command}\n{result.stdout}{result.stderr}')
    return result.stdout.strip()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cc, cxx = shutil.which(os.environ.get('CC', 'clang')), shutil.which('clang++')
    if not cc or not cxx:
        raise SystemExit('Host clang and clang++ required.')
    sources = {p: p.read_text() for p in (EXEC, SHADERS)}
    e, s = sources[EXEC], sources[SHADERS]
    identity = e[e.index('// BEGIN headstart cache identity'):e.index('// END headstart cache identity')]
    key_header = (ROOT / 'ports/shared-android/runtime/nvn/maxwell/shader.h').read_text()
    key_start = key_header.index('typedef struct {\n    uint8_t attr_kind[32];')
    key_end = key_header.index('} ShaderKey;', key_start) + len('} ShaderKey;')
    (OUT / 'shader_key_test_types.inc').write_text(key_header[key_start:key_end] + '\n')
    # Use the actual magic and record tags, not frozen copies in the test.
    defines = e[e.index('#define HS_MAGIC'):e.index('// BEGIN headstart cache identity')]
    prep = e[e.index('static FILE *hs_out;'):e.index('static _Atomic int hs_total')]
    helper = (ROOT / 'ports/shared-android/runtime/shared_gpu_cache.c').read_text()
    kernel = (ROOT / 'ports/shared-android/runtime/kernel.h').read_text()
    limit = re.search(r'(?m)^#define KERNEL_READABLE_COPY_MAX[^\n]+', kernel).group(0)
    shader_prep = (ROOT / 'sources/las-android/runtime/nvn/shader_prep.c').read_text()
    pieces = limit + '\n' + function(shader_prep, 'uint64_t shader_code_hash(') + function(e, 'static uint64_t hash_words(uint64_t h, const void *p, size_t n) {') + function(helper, 'bool shared_gpu_environment(') + defines + identity + 'static pthread_mutex_t hs_lock = PTHREAD_MUTEX_INITIALIZER;\n' + prep
    pieces += function(e, 'static void hs_write(')
    pieces += s[s.index('static int spv_flags('):s.index('// Vertex shaders:')]
    pieces += function(s, 'uint32_t *vk_glsl_to_spirv(')
    (OUT / 'shader_cache_production.inc').write_text(pieces)
    # Compile the actual SHA wrapper with only its declarations supplied separately.
    # CacheSha256 is pinned upstream MIT code; do not replace it with a test hash.
    (OUT / 'common_session.h').write_text('#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n')
    hash_source = ROOT / 'ports/shared-android/runtime/shared_hash.cpp'
    (OUT / 'shared_hash.cpp').write_text(hash_source.read_text())
    results = {}
    for variant, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined'])]:
        obj = OUT / f'test-{variant}.o'
        sha = OUT / f'sha-{variant}.o'
        binary = OUT / f'shader-cache-{variant}'
        run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             *flags, '-I', OUT, '-c', ROOT / 'tests/shader_cache_identity_test.c', '-o', obj])
        run([cxx, '-std=c++17', *flags, '-I', ROOT / 'sources/las-android/runtime',
             '-c', OUT / 'shared_hash.cpp', '-o', sha])
        run([cxx, *flags, obj, sha, '-pthread', '-o', binary])
        directory = OUT / variant
        directory.mkdir(exist_ok=True)
        for path in (directory / 'cache').glob('**/*') if (directory / 'cache').exists() else ():
            if path.is_file():
                path.unlink()
        results[variant] = [run([binary, 'headstart'], directory)]
        for mode in ('default', 'fs', 'nc'):
            results[variant].append(run([binary, mode], directory))
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    if any(p.read_text() != text for p, text in sources.items()):
        raise SystemExit('Production source changed during checks; rerun.')
    report = {'production_sha256': hashes, 'results': results,
              'scope': 'host cache identity/IO; fake GPU properties and shaderc, actual production SHA256'}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
