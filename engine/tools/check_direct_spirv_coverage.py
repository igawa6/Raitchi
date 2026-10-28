#!/usr/bin/env python3
"""Survey an explicitly supplied private journal; emit metadata only, no device actions."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'ports/shared-android/runtime'
OUT = ROOT / 'build/direct-spirv-coverage'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(eden_source=None):
    files = [Path(__file__).resolve(), ROOT / 'tools/direct_spirv_coverage.c',
             RT / 'nvn/maxwell/direct_spirv.c', RT / 'nvn/maxwell/direct_spirv.h',
             RT / 'nvn/maxwell/translate.c', RT / 'nvn/maxwell/shader.h',
             RT / 'nvn/shader_discovery.h', RT / 'shared_hash.cpp',
             ROOT / 'sources/las-android/runtime/nvn/shader_prep.h']
    pins = {str(p.relative_to(ROOT)): sha(p) for p in files}
    reference = None
    if eden_source:
        reference = eden_source / 'src/shader_recompiler/frontend/maxwell/maxwell.inc'
        if not reference.is_file():
            raise RuntimeError('selected Eden reference has no Maxwell opcode table')
        pins['reference_opcode_table'] = sha(reference)
    executable, receipt = OUT / 'coverage', OUT / 'build.json'
    if executable.is_file() and receipt.is_file() and json.loads(receipt.read_text()).get('sources') == pins:
        return executable, pins
    OUT.mkdir(parents=True, exist_ok=True)
    cc, cxx = shutil.which('clang'), shutil.which('clang++')
    if not cc or not cxx:
        raise RuntimeError('clang and clang++ are required')
    includes = [RT / 'nvn', RT / 'nvn/maxwell', ROOT / 'sources/las-android/runtime/nvn', RT, ROOT / 'native']
    objects = []
    for source in [files[1], files[2], files[4]]:
        obj = OUT / (source.stem + '.o')
        command = [cc, '-std=gnu11', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
                   *sum((['-I', str(p)] for p in includes), []), '-c', str(source), '-o', str(obj)]
        if reference and source == files[1]:
            command.insert(1, '-DDIRECT_SPIRV_COVERAGE_EDEN_OPCODES=' + json.dumps(str(reference)))
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr)
        objects.append(obj)
    hash_obj = OUT / 'hash.o'
    compiled = subprocess.run([cxx, '-std=c++17', '-O2', '-I', str(ROOT / 'sources/las-android/runtime'),
                              '-I', str(RT), '-I', str(ROOT / 'native'), '-c', str(files[7]), '-o', str(hash_obj)],
                             capture_output=True, text=True, timeout=120)
    if compiled.returncode:
        raise RuntimeError(compiled.stderr)
    linked = subprocess.run([cxx, '-O2', *map(str, objects), str(hash_obj), '-lm', '-o', str(executable)],
                            capture_output=True, text=True, timeout=120)
    if linked.returncode:
        raise RuntimeError(linked.stderr)
    receipt.write_text(json.dumps({'sources': pins}, indent=2) + '\n')
    return executable, pins


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--journal', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--include-hashes', action='store_true', help='include one-way stage hashes in per-record metadata')
    parser.add_argument('--eden-source', type=Path, help='optional local read-only reference for public opcode-name metadata')
    args = parser.parse_args()
    if not args.journal:
        print(json.dumps({'state': 'skipped', 'reason': 'no private journal explicitly supplied', 'device_actions': False}))
        return 0
    if not args.output:
        parser.error('--output is required when --journal is supplied')
    validator = shutil.which('spirv-val')
    if not validator:
        raise RuntimeError('spirv-val is required for admitted modules')
    journal = args.journal.resolve()
    output = args.output.resolve()
    if output == journal or (output.exists() and output.samefile(journal)):
        raise RuntimeError('metadata output must not replace the input journal')
    original_sha = sha(journal)
    eden_source = args.eden_source.resolve() if args.eden_source else None
    executable, pins = build(eden_source)
    env = {k: v for k, v in os.environ.items() if not k.startswith('LARECOMP_')}
    stages = {'vertex': Counter(), 'fragment': Counter(), 'compute': Counter()}
    reasons = Counter()
    rejected_instructions = Counter()
    rejected_modifiers = Counter()
    reference_instructions = Counter()
    rows = []
    unique_inputs = {stage: set() for stage in stages}
    unique_direct = {stage: set() for stage in stages}
    unique_modules = {stage: set() for stage in stages}
    with tempfile.TemporaryDirectory(prefix='private-', dir=OUT) as private:
        result = subprocess.run([str(executable), str(journal), private, '1' if args.include_hashes else '0'],
                                capture_output=True, text=True, timeout=300, env=env)
        if result.returncode:
            raise RuntimeError(result.stderr)
        lines = [json.loads(line) for line in result.stdout.splitlines() if line]
        summary = lines.pop(0)
        for row in lines:
            stage = {0: 'vertex', 1: 'fragment', 5: 'compute'}[row['stage']]
            stats = stages[stage]
            stats['evaluated'] += 1
            if 'input_identity' not in row:
                raise RuntimeError('compile-input identity could not be computed')
            unique_inputs[stage].add(row['input_identity'])
            status = {0: 'direct', 1: 'unsupported', 2: 'invalid', 3: 'oom'}[row['status']]
            stats[status] += 1
            if row['reason']:
                reasons[stage + ': ' + row['reason']] += 1
            if row.get('first_rejected_instruction'):
                rejected_instructions[stage + ': ' + row['first_rejected_instruction']] += 1
            for modifier, value in row.get('rejected_modifiers', {}).items():
                if value:
                    rejected_modifiers[f"{stage}: {row['first_rejected_instruction']} {modifier}={value}"] += 1
            if row.get('reference_instruction'):
                reference_instructions[stage + ': ' + row['reference_instruction']] += 1
            if status == 'direct':
                if not row['saved']:
                    raise RuntimeError('admitted module could not be privately validated')
                path = Path(private) / f"{row['record']}-{row['stage']}.spv"
                validation = subprocess.run([validator, '--target-env', 'vulkan1.1', str(path)],
                                            capture_output=True, text=True, timeout=30)
                path.unlink()
                if validation.returncode:
                    # Driver/compiler messages can contain literal operands. Keep
                    # those private and report only the failed module's index.
                    raise RuntimeError(f"SPIR-V validation failed for record {row['record']} stage {row['stage']}")
                stats['spirv_validated'] += 1
                unique_direct[stage].add(row['input_identity'])
                if 'spirv_identity' not in row:
                    raise RuntimeError('admitted SPIR-V identity could not be computed')
                unique_modules[stage].add(row['spirv_identity'])
            rows.append(row)
    if sha(journal) != original_sha:
        raise RuntimeError('input journal changed while surveying')
    current = {relative: sha(ROOT / relative) for relative in pins if relative != 'reference_opcode_table'}
    if eden_source:
        current['reference_opcode_table'] = sha(eden_source / 'src/shader_recompiler/frontend/maxwell/maxwell.inc')
    if pins != current:
        raise RuntimeError('compiler source changed during survey')
    for stage, stats in stages.items():
        stats['unique_compile_inputs'] = len(unique_inputs[stage])
        stats['unique_direct_compile_inputs'] = len(unique_direct[stage])
        stats['unique_produced_spirv_modules'] = len(unique_modules[stage])
    report = {'state': 'passed', 'input_sha256': original_sha, **summary, 'stages': stages,
              'reasons': dict(reasons.most_common()), 'rejected_instructions': dict(rejected_instructions.most_common()),
              'rejected_modifiers': dict(rejected_modifiers.most_common()),
              'source_sha256': {k: v for k, v in pins.items() if k != 'reference_opcode_table'},
              'reference_instructions': dict(reference_instructions.most_common()),
              'input_preserved': True, 'temporary_modules_removed': True, 'device_actions': False,
              'scope': 'Actual validated journal, direct emitter and legacy VS metadata for FS context; admission and spirv-val only. Per-recipe stages include repeated shader/key variants. Not runtime coverage, Vulkan execution, performance evidence or whole-game support.'}
    if args.include_hashes:
        report['records'] = rows
    if eden_source:
        revision = subprocess.run(['git', '-C', str(eden_source), 'rev-parse', 'HEAD'],
                                  capture_output=True, text=True, timeout=10)
        report['reference_opcode_table'] = {'sha256': pins['reference_opcode_table'],
                                           'revision': revision.stdout.strip() if revision.returncode == 0 else None,
                                           'runtime_dependency': False}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('source_sha256', 'records')}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
