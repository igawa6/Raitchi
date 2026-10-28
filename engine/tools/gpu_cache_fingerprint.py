#!/usr/bin/env python3
"""Build GPU cache identities from real renderer preprocessing dependencies.

No target code runs. The generated identity header is the only excluded input;
its generator/schema/source are included, avoiding a self-referential digest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import re
import subprocess

SCHEMA = 1
COMPILER_SOURCES = {'shaders.c', 'translate.c', 'direct_spirv.c', 'shader_compiler.c'}

def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def bootstrap(path):
    path = Path(path)
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#pragma once\n#define GPU_CACHE_BUILD_READY 0\n#define GPU_RENDERER_ID_HEX ""\n#define GPU_COMPILER_ID_HEX ""\n')

def build(commands, source_root, repository_root, generated_root, shaderc, toolchain_inputs=()):
    source_root, repository_root, generated_root = map(lambda p: Path(p).resolve(),
        (source_root, repository_root, generated_root))
    commands = Path(commands).resolve()
    rows = json.loads(commands.read_text())
    def directory(row):
        p = Path(row['directory'])
        return (p if p.is_absolute() else commands.parent / p).resolve()
    def source_file(row):
        p = Path(row['file'])
        return (p if p.is_absolute() else directory(row) / p).resolve()
    def arguments(row):
        args = list(row['arguments']) if row.get('arguments') else shlex.split(row['command'])
        p = Path(args[0])
        if not p.is_absolute():
            resolved = directory(row) / p if '/' in args[0] else shutil.which(args[0])
            if not resolved: raise RuntimeError('Compiler not found: ' + args[0])
            p = Path(resolved)
        args[0] = str(p.resolve())
        return args
    header = generated_root / 'gpu_cache_build.h'
    if not header.is_file():
        raise RuntimeError('GPU cache identity header must be bootstrapped before dependency scanning')
    roots = [('GENERATED', generated_root), ('SOURCE', source_root), ('ROOT', repository_root)]
    # NDK location is not an identity: its actual compiler, headers and archives are.
    for row in rows:
        args = arguments(row)
        for value in args:
            if value.startswith('--sysroot='):
                sysroot = Path(value.split('=', 1)[1]).resolve()
                roots.insert(0, ('NDK_SYSROOT', sysroot))
                compiler_root = Path(args[0]).resolve().parent.parent
                roots.insert(0, ('NDK_TOOLCHAIN', compiler_root))
                break
        if any(label == 'NDK_SYSROOT' for label, _ in roots):
            break
    roots.sort(key=lambda item: len(str(item[1])), reverse=True)
    def name(path):
        path = Path(path).resolve()
        for label, root in roots:
            if path.is_relative_to(root):
                return label + '/' + path.relative_to(root).as_posix()
        return 'EXTERNAL/' + path.as_posix().lstrip('/')
    def normalized_arg(arg):
        for label, root in roots:
            arg = re.sub(re.escape(str(root)) + r'(?=/|$|=)', lambda _: '${' + label + '}', arg)
        return arg
    def selected(row):
        p = source_file(row)
        return (p.is_relative_to(source_root / 'runtime/nvn') or
            p.name in ('imgui_impl_vulkan.cpp', 'volk.c', 'shared_gpu_cache.c', 'shared_hash.cpp'))
    seeds = sorted((row for row in rows if selected(row)), key=lambda row: name(source_file(row)))
    if not seeds or not any(Path(r['file']).name == 'shaders.c' for r in seeds):
        raise RuntimeError('Compiled renderer/shader sources absent from actual compile commands')
    all_inputs, compiler_inputs, semantic = {}, {}, []
    digests, versions = {}, {}
    def file_sha(path):
        path = str(Path(path).resolve())
        if path not in digests: digests[path] = sha(path)
        return digests[path]
    def compiler_version(path):
        path = str(path)
        if path not in versions:
            versions[path] = subprocess.run([path, '--version'], capture_output=True,
                text=True, check=True, timeout=30).stdout.strip()
        return versions[path]
    for row in seeds:
        args = arguments(row)
        if any(a.startswith('-flto') for a in args):
            raise RuntimeError('Whole-program LTO needs a whole-program dependency closure; GPU-only identity refused')
        compiler = Path(args[0]).resolve()
        # Strip only output/dependency bookkeeping and the old, redundant cache-tag
        # macro. Never remove renderer feature defines or optimization/ABI flags.
        filtered = [args[0]]
        i = 1
        while i < len(args):
            arg = args[i]
            if arg in ('-o', '-MF', '-MT', '-MQ'):
                i += 2
                continue
            if arg in ('-c', '-MD', '-MMD', '-MP') or arg.startswith('-DSHADER_TRANSLATOR_ID='):
                i += 1
                continue
            filtered.append(arg)
            i += 1
        result = subprocess.run([*filtered, '-M', '-MT', 'gpu_cache_inputs'], cwd=directory(row),
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError('Renderer dependency scan failed for ' + name(source_file(row)) + ': ' + result.stderr)
        tokens = shlex.split(result.stdout.replace('\\\n', ' '))
        if not tokens or tokens[0] != 'gpu_cache_inputs:':
            raise RuntimeError('Unexpected compiler dependency output for ' + name(source_file(row)))
        dependencies = {Path(p if Path(p).is_absolute() else directory(row) / p).resolve()
                        for p in tokens[1:]}
        dependencies.add(source_file(row))
        dependencies.discard(header)
        record = {'source': name(source_file(row)), 'directory': name(directory(row)), 'compiler': name(compiler),
                  'arguments': [normalized_arg(a) for a in filtered[1:]],
                  'compiler_sha256': file_sha(compiler),
                  'compiler_version': compiler_version(compiler)}
        semantic.append(record)
        for dependency in sorted(dependencies):
            if not dependency.is_file():
                raise RuntimeError('Missing renderer dependency: ' + str(dependency))
            all_inputs[str(dependency)] = file_sha(dependency)
            if Path(row['file']).name in COMPILER_SOURCES:
                compiler_inputs[str(dependency)] = all_inputs[str(dependency)]
        all_inputs[str(compiler)] = record['compiler_sha256']
        if Path(row['file']).name in COMPILER_SOURCES:
            compiler_inputs[str(compiler)] = record['compiler_sha256']
    # Compiler implementation and its static C++/builtins dependencies are inputs,
    # not just declarations in shaderc.h or a human-readable version string.
    extras = [Path(shaderc).resolve(), Path(__file__).resolve(), *map(lambda p: Path(p).resolve(), toolchain_inputs)]
    for path in extras:
        if not path.is_file():
            raise RuntimeError('Missing GPU compiler/toolchain input: ' + str(path))
        all_inputs[str(path)] = compiler_inputs[str(path)] = file_sha(path)
    compiler_commands = [s for s in semantic if Path(s['source']).name in COMPILER_SOURCES]
    def digest(inputs, settings, kind):
        material = {'schema': SCHEMA, 'kind': kind,
                    'files': sorted((name(path), value) for path, value in inputs.items()),
                    'compile_commands': settings}
        return hashlib.sha256(json.dumps(material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return {'schema': SCHEMA, 'renderer_id': digest(all_inputs, semantic, 'renderer'),
            'compiler_id': digest(compiler_inputs, compiler_commands, 'spirv-compiler'),
            'source_root': str(source_root), 'repository_root': str(repository_root),
            'generated_root': str(generated_root), 'compile_commands': str(commands),
            'shaderc_archive': str(Path(shaderc).resolve()),
            'toolchain_inputs': list(map(lambda p: str(Path(p).resolve()), toolchain_inputs)),
            'input_sha256': dict(sorted(all_inputs.items())),
            'compiler_input_sha256': dict(sorted(compiler_inputs.items())),
            'semantic_compile_commands': semantic}

def write(record, header, report):
    content = ('#pragma once\n#define GPU_CACHE_BUILD_READY 1\n'
        '#define GPU_RENDERER_ID_HEX "' + record['renderer_id'] + '"\n'
        '#define GPU_COMPILER_ID_HEX "' + record['compiler_id'] + '"\n').encode()
    header, report = Path(header), Path(report)
    for path, data in [(header, content), (report, (json.dumps(record, indent=2) + '\n').encode())]:
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.is_file() or path.read_bytes() != data:
            temporary = path.with_suffix(path.suffix + '.tmp')
            temporary.write_bytes(data)
            temporary.replace(path)

def verify(record):
    if record.get('schema') != SCHEMA:
        raise RuntimeError('Unknown GPU cache build identity schema')
    current = build(record['compile_commands'], record['source_root'], record['repository_root'],
                    record['generated_root'], record['shaderc_archive'], record['toolchain_inputs'])
    if current != record:
        raise RuntimeError('GPU renderer/compiler identity inputs changed since the native build')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--commands', type=Path, required=True);p.add_argument('--source', type=Path, required=True)
    p.add_argument('--root', type=Path, required=True);p.add_argument('--generated', type=Path, required=True)
    p.add_argument('--shaderc', type=Path, required=True);p.add_argument('--toolchain-input', action='append', default=[])
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args();bootstrap(a.generated/'gpu_cache_build.h')
    record = build(a.commands, a.source, a.root, a.generated, a.shaderc, a.toolchain_input)
    write(record, a.generated/'gpu_cache_build.h', a.report)
    print(json.dumps({'renderer_id':record['renderer_id'],'compiler_id':record['compiler_id'],
                      'inputs':len(record['input_sha256'])}, indent=2))
if __name__ == '__main__':main()
