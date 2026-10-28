#!/usr/bin/env python3
"""Export/inspect game-version shader profiles without exposing shader payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'ports/shared-android/runtime'
OUT = ROOT / 'build/shader-profile-tool'


def build():
    paths = [Path(__file__).resolve(), ROOT / 'tools/shader_profile_tool.c',
             RT / 'nvn/shader_profile.c', RT / 'nvn/shader_profile.h',
             RT / 'nvn/shader_discovery.h', RT / 'nvn/maxwell/shader.h', RT / 'shared_hash.cpp',
             ROOT / 'sources/las-android/runtime/nvn/shader_prep.h']
    pins = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    receipt, exe = OUT / 'build.json', OUT / 'shader-profile-tool'
    if exe.is_file() and receipt.is_file() and json.loads(receipt.read_text()).get('sources') == pins:
        return exe
    OUT.mkdir(parents=True, exist_ok=True)
    cc, cxx = shutil.which('clang'), shutil.which('clang++')
    if not cc or not cxx:
        raise RuntimeError('clang and clang++ are required for the host profile tool')
    include = [RT / 'nvn', ROOT / 'sources/las-android/runtime/nvn', RT]

    def run(args):
        p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)

    objects = []
    for path in [paths[1], paths[2]]:
        obj = OUT / (path.stem + '.o')
        run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             *sum((['-I', p] for p in include), []), '-c', path, '-o', obj])
        objects.append(obj)
    hobj = OUT / 'hash.o'
    run([cxx, '-std=c++17', '-O2', '-I', ROOT / 'sources/las-android/runtime', '-I', RT,
         '-I', ROOT / 'native', '-c', RT / 'shared_hash.cpp', '-o', hobj])
    run([cxx, '-O2', *objects, hobj, '-o', exe])
    receipt.write_text(json.dumps({'sources': pins}, indent=2) + '\n')
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    export = commands.add_parser('export', help='merge exact learned recipe metadata into a scoped profile')
    export.add_argument('--journal', type=Path, required=True)
    export.add_argument('--output', type=Path, required=True)
    inspect = commands.add_parser('inspect', help='validate and summarize profile metadata')
    inspect.add_argument('--profile', type=Path, required=True)
    args = parser.parse_args()
    try:
        exe = build()
        values = [args.command, args.profile] if args.command == 'inspect' else [args.command, args.journal, args.output]
        result = subprocess.run([str(exe), *map(str, values)], timeout=120)
        return result.returncode
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
