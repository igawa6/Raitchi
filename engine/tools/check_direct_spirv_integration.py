#!/usr/bin/env python3
"""Actual render integration for mixed direct SPIR-V and legacy shader stages."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'ports/shared-android/runtime/nvn/maxwell'
EXEC = ROOT / 'ports/shared-android/runtime/nvn/vulkan/exec.c'
TEST = ROOT / 'tests/direct_spirv_integration_test.c'
OUT = ROOT / 'build/direct-spirv-integration'


def function(source, name):
    clean = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                   lambda match: ''.join('\n' if c == '\n' else ' ' for c in match[0]), source)
    for match in re.finditer(r'\b' + re.escape(name) + r'\s*\(', clean):
        at = match.end()
        depth = 1
        while depth:
            depth += (clean[at] == '(') - (clean[at] == ')')
            at += 1
        while clean[at].isspace():
            at += 1
        if clean[at] != '{':
            continue
        start = clean.rfind('\n', 0, match.start()) + 1
        end, depth = at + 1, 1
        while depth:
            depth += (clean[end] == '{') - (clean[end] == '}')
            end += 1
        return source[start:end] + '\n'
    raise ValueError(name)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    paths = [EXEC, RT / 'shader_compiler.c', RT / 'shader_compiler.h', RT / 'shader.h',
             RT / 'translate.c', TEST, Path(__file__).resolve()]
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    pins = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    source = EXEC.read_text()
    functions = function((RT / 'shader_compiler.c').read_text(), 'shader_info_succeeded')
    functions += function((RT / 'translate.c').read_text(), 'maxwell_free')
    functions += ''.join(function(source, name) for name in
                         ['prog_code', 'spirv_strip_early_tests', 'program_spirv', 'program_legacy_module_retry', 'compile_program', 'translate_program_now'])
    (OUT / 'direct_spirv_integration_actual.inc').write_text(functions)
    results = {}
    for name, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined',
                         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]:
        executable = OUT / name
        commands = [('build', [shutil.which('clang'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                              '-Wno-unused-parameter', '-Wno-macro-redefined', *flags,
                              '-I', RT, '-I', OUT, TEST, '-pthread', '-o', executable]),
                    ('run', [executable])]
        for step, command in commands:
            result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=90)
            (OUT / (name + '-' + step + '.log')).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(name + '-' + step + ': ' + result.stdout + result.stderr)
            if step == 'run':
                results[name] = json.loads(result.stdout)
    assert pins == {str(p.relative_to(ROOT)): sha(p) for p in paths}, 'sources changed during check'
    report = {'state': 'passed', 'results': results, 'source_sha256': pins,
              'scope': 'Actual program_spirv, driver-rejection legacy-module retry, compile_program, translate_program_now, prog_code, early-test removal, success helper and maxwell_free; controlled compiler/driver/reflection/build hooks. Mixed per-stage backends, depth-only fallback, metadata/varying keys, finite immutable first-use spans, unknown-span legacy contract, shaderc isolation, retry success/failure/interface mismatch, final selected-word reflection, ownership transfer and error cleanup. No actual direct emitter/GPU/device/game.'}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
