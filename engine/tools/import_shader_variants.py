#!/usr/bin/env python3
"""Import pinned recomp shader-tour metadata using exact original source hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'ports/shared-android/runtime'
OUT = ROOT / 'build/import-shader-variants'


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def production_header():
    upstream = (ROOT / 'sources/las-android/runtime/nvn/shader_prep.c').read_text()
    renderer = (RT / 'nvn/vulkan/exec.c').read_text()
    prep = function(renderer, 'bool renderer_prepare(')
    key_block = prep[prep.index('    ShaderKey vkey ='):prep.index('    uint64_t ck = prog_content_key')]
    keys = ('static void import_keys(const ShaderRec *r, ShaderKey *v, ShaderKey *f) {\n'
            ' memset(v,0,sizeof *v); memset(f,0,sizeof *f); if(r->kind==\'C\'){v->glsl=1;return;}\n'
            + key_block + ' *v=vkey; *f=fkey;\n}\n')
    notice = (ROOT / 'native/runtime/LICENSE-LASRecomp').read_text()
    return ('/* Extracted LAS Recomp parser/formatter and adapted renderer helpers.\n'
            + notice + '\n*/\n#include <inttypes.h>\n') + ''.join(function(upstream, s) for s in (
        'void shader_rec_format(', 'static const char *field(', 'static int hex_list(', 'bool shader_rec_parse(')) + ''.join(
        function(renderer, s) for s in ('static int attr_kind(', 'static int rt_kind(')) + keys


def run(args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=180)
    if p.returncode:
        raise RuntimeError(p.stdout + p.stderr)
    return p.stdout


def build(sanitized=False):
    OUT.mkdir(parents=True, exist_ok=True)
    paths = [Path(__file__).resolve(), ROOT / 'tools/import_shader_variants.c',
             ROOT / 'sources/las-android/runtime/nvn/shader_prep.c', ROOT / 'sources/las-android/runtime/nvn/shader_prep.h',
             RT / 'nvn/vulkan/exec.c', RT / 'nvn/shader_profile.c', RT / 'nvn/shader_profile.h',
             RT / 'nvn/shader_discovery.h', RT / 'nvn/shader_source_index.c', RT / 'nvn/shader_source_index.h',
             RT / 'nxpkg.c', RT / 'nxpkg.h', RT / 'shared_hash.cpp', RT / 'nvn/maxwell/shader.h',
             ROOT / 'native/runtime/LICENSE-LASRecomp']
    pins = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    name = 'sanitized' if sanitized else 'optimized'
    receipt, exe = OUT / (name + '-build.json'), OUT / name
    if receipt.exists() and exe.exists() and json.loads(receipt.read_text())['sources'] == pins:
        return exe
    (OUT / 'variant_import_production.inc').write_text(production_header())
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if sanitized else ['-O2']
    include = [OUT, RT / 'nvn', ROOT / 'sources/las-android/runtime/nvn', RT]
    objects = []
    for path in [paths[1], RT / 'nvn/shader_profile.c', RT / 'nvn/shader_source_index.c', RT / 'nxpkg.c']:
        obj = OUT / (name + '-' + path.stem + '.o')
        run([shutil.which('clang'), '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-macro-redefined', '-Wno-logical-op-parentheses', *flags,
             *sum((['-I', p] for p in include), []), '-c', path, '-o', obj])
        objects.append(obj)
    obj = OUT / (name + '-hash.o')
    run([shutil.which('clang++'), '-std=c++17', *flags, '-I', ROOT / 'sources/las-android/runtime', '-I', RT,
         '-I', ROOT / 'native', '-c', RT / 'shared_hash.cpp', '-o', obj])
    run([shutil.which('clang++'), *flags, *objects, obj, '-pthread', '-o', exe])
    receipt.write_text(json.dumps({'sources': pins}, indent=2) + '\n')
    return exe


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variants', type=Path, required=True)
    p.add_argument('--game-id', required=True, help='Verified exact selected-game/version identity, 64 hex digits')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--main-nso', type=Path)
    p.add_argument('--romfs', type=Path)
    p.add_argument('--package', type=Path)
    p.add_argument('--keys', type=Path)
    p.add_argument('--observed-profile', type=Path)
    p.add_argument('--sanitized', action='store_true')
    a = p.parse_args()
    if bool(a.package) != bool(a.keys):
        p.error('--package and --keys must be supplied together')
    if not a.main_nso and not a.romfs and not a.package:
        p.error('At least one original game source is required')
    exe = build(a.sanitized)
    command = [exe, a.variants, a.game_id, a.output, a.main_nso or '-', a.romfs or '-',
               a.package or '-', a.keys or '-', a.observed_profile or '-']
    result = subprocess.run(list(map(str, command)), timeout=300)
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
