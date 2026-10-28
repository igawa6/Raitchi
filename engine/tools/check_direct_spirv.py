#!/usr/bin/env python3
"""Synthetic direct Maxwell/SPIR-V validation and software Vulkan differential execution."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/direct-spirv'
RT = ROOT / 'ports/shared-android/runtime/nvn/maxwell'
FILES = [RT / 'direct_spirv.c', RT / 'direct_spirv.h', RT / 'translate.c', RT / 'shader.h',
         ROOT / 'tests/direct_spirv_fixtures.c', ROOT / 'tests/direct_spirv_vulkan.c', Path(__file__).resolve()]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, name, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=env, timeout=180)
    (OUT / name).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(name + ':\n' + result.stdout + result.stderr)
    return result.stdout


def software_driver():
    local = OUT / 'software-driver/usr/lib/libvulkan_lvp.so'
    if local.exists():
        config = OUT / 'software-driver/lvp.json'
        config.write_text(json.dumps({'file_format_version': '1.0.1', 'ICD': {
            'library_path': str(local.resolve()), 'api_version': '1.4.354'}}))
        return config, {'library': str(local.relative_to(ROOT)), 'sha256': sha(local), 'system_modified': False}
    for path in (Path('/usr/share/vulkan/icd.d/lvp_icd.json'), Path('/usr/share/vulkan/icd.d/lvp_icd.x86_64.json')):
        if path.exists():
            return path, {'icd': str(path), 'sha256': sha(path), 'system_modified': False}
    raise RuntimeError('Software Vulkan ICD unavailable; the GPU execution gate cannot be passed. '
                       'Provide an existing lavapipe ICD or extract matching vulkan-swrast under build/direct-spirv/software-driver.')


def module_metadata(path):
    words = struct.unpack('<' + 'I' * (path.stat().st_size // 4), path.read_bytes())
    assert words[0] == 0x07230203 and words[1] <= 0x00010300
    decorations, variables, execution = {}, {}, []
    at = 5
    while at < len(words):
        wc, opcode = words[at] >> 16, words[at] & 0xffff
        assert wc and at + wc <= len(words)
        args = words[at + 1:at + wc]
        if opcode == 71:
            decorations.setdefault(args[0], {})[args[1]] = args[2:]
        if opcode == 59:
            variables[args[1]] = args[2]
        if opcode == 15:
            execution.append(args[0])
        at += wc
    descriptors, locations = [], []
    for identifier, decorations_ in decorations.items():
        if 33 in decorations_:
            assert 34 in decorations_ and identifier in variables
            descriptors.append((decorations_[34][0], decorations_[33][0], variables[identifier]))
        if 30 in decorations_:
            locations.append((variables[identifier], decorations_[30][0], tuple(x for x in (13, 14, 16, 17) if x in decorations_)))
    return {'execution_models': sorted(execution), 'descriptors': sorted(descriptors), 'locations': sorted(locations)}


def compare(a, b, kind, mask, depth, expected=None, known_signed_zero=False, independent=None):
    assert len(a) == len(b) == 64 * 20
    values_a, values_b = struct.unpack('<256I', a[:1024]), struct.unpack('<256I', b[:1024])
    checks = 0
    for pixel in range(64):
        for component in range(4):
            if not mask & (1 << component):
                continue
            x, y = values_a[4 * pixel + component], values_b[4 * pixel + component]
            if kind:
                if independent is not None:
                    reference = independent[component]
                    if (reference & 0x7fffffff) > 0x7f800000:
                        assert (y & 0x7fffffff) > 0x7f800000 and y & 0x00400000, (pixel, component, hex(y), 'quiet NaN required')
                    else:
                        assert y == reference, (pixel, component, hex(y), hex(reference))
                elif known_signed_zero and component == 0:
                    assert x in (0, 0x80000000) and y == 0x80000000, (pixel, component, hex(x), hex(y))
                    reference = struct.unpack('<I', struct.pack('<f', 0.25 * -0.0))[0]
                    assert y == reference == 0x80000000
                else:
                    assert x == y, (pixel, component, hex(x), hex(y))
            else:
                fx, fy = struct.unpack('<f', struct.pack('<I', x))[0], struct.unpack('<f', struct.pack('<I', y))[0]
                assert (math.isnan(fx) and math.isnan(fy)) or math.isclose(fx, fy, rel_tol=1e-6, abs_tol=1e-6), (pixel, component, fx, fy)
            if expected is not None:
                target = expected[component]
                if kind:
                    assert x == target, (pixel, component, hex(x), hex(target))
                else:
                    assert math.isclose(struct.unpack('<f', struct.pack('<I', x))[0], target, abs_tol=1e-6)
                checks += 1
            checks += 1
    for x, y in zip(struct.unpack('<64f', a[1024:]), struct.unpack('<64f', b[1024:])):
        assert math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-6)
        assert math.isclose(x, depth, abs_tol=1e-6), (x, depth)  # Also rejects equal blank renders.
        checks += 2
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-gpu', action='store_true', help='Run syntax/front-end checks only; never counts as complete validation')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    for tool in ('clang', 'spirv-val', 'glslangValidator'):
        if not shutil.which(tool):
            raise RuntimeError('Missing ' + tool)
    pins = {str(p.relative_to(ROOT)): sha(p) for p in FILES}
    common = ['clang', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-I', RT, '-I', ROOT / 'native']
    modes = {}
    for mode, flags in (('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined',
                       '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])):
        directory = OUT / mode
        directory.mkdir(exist_ok=True)
        exe = OUT / (mode + '-fixtures')
        run([*common, *flags, ROOT / 'tests/direct_spirv_fixtures.c', RT / 'direct_spirv.c', RT / 'translate.c',
             '-lshaderc_shared', '-lm', '-o', exe], mode + '-build.log')
        modes[mode] = json.loads(run([exe, directory], mode + '-fixtures.log'))
    optimized = OUT / 'optimized'
    rows = []
    for line in (optimized / 'index.txt').read_text().splitlines():
        name, *fields = line.split()
        stage, kind, vertex_kind, mask, writes_depth, flip_x, flip_y, depth_mode = map(int, fields)
        rows.append({'name': name, 'stage': stage, 'kind': kind, 'vertex_kind': vertex_kind,
                     'mask': mask, 'writes_depth': writes_depth, 'flip_x': flip_x, 'flip_y': flip_y, 'depth_mode': depth_mode})
        direct = optimized / (name + '.direct.spv')
        assert direct.read_bytes() == (OUT / 'sanitized' / direct.name).read_bytes(), 'nondeterministic module: ' + name
        run(['spirv-val', '--target-env', 'vulkan1.1', direct], name + '-direct-val.log')
        source = optimized / (name + ('.vert' if stage == 0 else '.frag'))
        legacy = optimized / (name + '.legacy.spv')
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', source, '-o', optimized / (name + '.glslang.spv')], name + '-legacy-build.log')
        run(['spirv-val', '--target-env', 'vulkan1.1', legacy], name + '-legacy-val.log')
        assert module_metadata(direct) == module_metadata(legacy), (name, module_metadata(direct), module_metadata(legacy))
    gpu = {'state': 'not_run'}
    if not args.no_gpu:
        config, driver = software_driver()
        env = dict(os.environ, VK_DRIVER_FILES=str(config.resolve()), VK_ICD_FILENAMES=str(config.resolve()), LP_NUM_THREADS='2')
        include = ROOT / 'work/las-baseline/build/android/deps/include'
        runner = OUT / 'vulkan-oracle'
        run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-I', include,
             ROOT / 'tests/direct_spirv_vulkan.c', '-lvulkan', '-o', runner], 'vulkan-oracle-build.log')
        ubo = OUT / 'ubo.bin'
        ubo.write_bytes(struct.pack('<4f', 0.5, 1.5, -2.0, 1.0) + bytes(65536 - 16))
        checks, routes, differences, known_differences = 0, [], [], []
        bits = lambda value: struct.unpack('<I', struct.pack('<f', value))[0]
        for row in rows:
            name, stage = row['name'], row['stage']
            vertex = name if stage == 0 else 'vertex-transform-0'
            fragment = 'fragment-varying' if stage == 0 else name
            kind = 0 if stage == 0 else row['kind']
            vertex_kind = row['vertex_kind'] if stage == 0 else 0
            a, b = OUT / (name + '-legacy.output'), OUT / (name + '-direct.output')
            legacy_vs = optimized / (vertex + '.legacy.spv')
            legacy_fs = optimized / (fragment + '.legacy.spv')
            direct_vs = optimized / (vertex + '.direct.spv')
            direct_fs = optimized / (fragment + '.direct.spv')
            selected_ubo = optimized / (name + '.ubo')
            if not selected_ubo.exists():
                selected_ubo = ubo
            device = json.loads(run([runner, legacy_vs, legacy_fs, selected_ubo, kind, vertex_kind, a], name + '-legacy-render.log', env))
            run([runner, direct_vs, direct_fs, selected_ubo, kind, vertex_kind, b], name + '-direct-render.log', env)
            if stage == 0:
                expected, mask = [0.125, 0.75, 0, 0], 3
                depth = 0.5 if name == 'vertex-cbuf' else 0.625 if row['depth_mode'] else 0.25
            else:
                mask, depth = row['mask'] & 15, 0.25
                if name == 'fragment-depth':
                    depth = 0.375
                    expected = None
                elif name == 'fragment-missing-varying':
                    expected = [0.0, 0.0, 0.0, 0.0]
                elif name.startswith('fragment-ipa-'):
                    mode = int(name.rsplit('-', 1)[1])
                    expected = [0.25, 1.5, 0, 0] if mode == 1 else [0.125, 0.75, 0, 0]
                elif name == 'fragment-varying':
                    expected = [0.125, 0.75, 0, 0]
                elif name.startswith('fragment-alu-'):
                    op = int(name.rsplit('-', 1)[1])
                    expected = [bits([1.0, 0.1875, -0.3125, 0.5][op]), bits(1.5 if op == 3 else 0.75), bits(-0.5), bits(1.0)]
                else:
                    expected = None
            oracle_path = optimized / (name + '.oracle')
            independent = struct.unpack('<4I', oracle_path.read_bytes()) if oracle_path.exists() else None
            try:
                assertions = compare(a.read_bytes(), b.read_bytes(), kind, mask, depth, expected, name == 'fragment-extra-24', independent)
                if name == 'fragment-extra-24' and a.read_bytes()[:4] != b.read_bytes()[:4]:
                    known_differences.append({'name': name, 'operation': 'FMUL +0.25 * -0.0, no FTZ or SAT',
                        'legacy_shaderc_software_driver_raw': hex(struct.unpack('<I', a.read_bytes()[:4])[0]), 'direct_software_driver_raw': '0x80000000',
                        'independent_ieee_reference_raw': '0x80000000', 'classification': 'signed_zero',
                        'maxwell_hardware_accuracy_verified': False})
                if independent is not None:
                    legacy_raw = struct.unpack('<4I', a.read_bytes()[:16])
                    direct_raw = struct.unpack('<4I', b.read_bytes()[:16])
                    for component, (actual, reference) in enumerate(zip(direct_raw, independent)):
                        if actual != reference and (reference & 0x7fffffff) > 0x7f800000:
                            known_differences.append({'name': name, 'classification': 'nan_payload', 'component': component,
                                'direct_raw': hex(actual), 'independent_nan_reference_raw': hex(reference),
                                'criterion': 'quiet NaN class; payload/sign not hardware-qualified'})
                    if legacy_raw != direct_raw:
                        known_differences.append({'name': name, 'classification': 'explicit_ftz',
                            'legacy_raw': [hex(x) for x in legacy_raw], 'direct_raw': [hex(x) for x in direct_raw],
                            'independent_reference_raw': [hex(x) for x in independent],
                            'maxwell_hardware_accuracy_verified': False})
            except AssertionError as exc:
                differences.append({'name': name, 'difference': str(exc)})
                assertions = 0
            checks += assertions
            routes.append({'name': name, 'assertions': assertions})
        gpu = {'state': 'passed' if not differences else 'failed', 'differences': differences,
               'known_differences': known_differences, 'bitwise_parity_all_routes': not known_differences and not differences, 'driver': driver, 'device': device, 'routes': routes, 'assertions': checks,
               'scope': '8x8 offscreen float/uint attachments + depth readback; exact direct/legacy comparison except explicitly recorded signed-zero difference verified against independent IEEE arithmetic' }
    assert pins == {str(p.relative_to(ROOT)): sha(p) for p in FILES}, 'Sources changed during tests'
    report = {'state': 'partial' if args.no_gpu else gpu['state'], 'source_sha256': pins, 'frontends': modes,
              'validated_modules': len(rows) * 2, 'gpu': gpu,
              'scope': 'Synthetic Maxwell only. Software Vulkan numerics are not Thor coverage or device performance. '
                       'Front-end timing includes translation+in-process shaderc without driver builds; unsupported operations fail closed.'}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if report['state'] == 'failed':
        raise SystemExit(1)


if __name__ == '__main__':
    main()
