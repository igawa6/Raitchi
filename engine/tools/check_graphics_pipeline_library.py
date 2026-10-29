#!/usr/bin/env python3
"""Production GPL host/software-Vulkan checks; --android adds actual TU compilation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'ports/shared-android/runtime/nvn/vulkan/exec.c'
TEST = ROOT / 'tests/graphics_pipeline_library_test.c'
OUT = ROOT / 'build/graphics-pipeline-library'


def run(args, cwd=None, env=None):
    result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout.strip()


def function(text, name):
    start = text.rfind('\nstatic ', 0, text.index(name)) + 1
    body = text.index('{', text.index(name))
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android', action='store_true',
                        help='also compile actual exec/device TUs using a configured Android build')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'verification.json').unlink(missing_ok=True)
    compile_db = ROOT / 'build/shared-android/native/compile_commands.json'
    if args.android and not compile_db.is_file():
        parser.error('--android requires build/shared-android/native/compile_commands.json; '
                     'configure/build this checkout with tools/build_shared_android.py first')
    text = SOURCE.read_text()
    key = text[text.index('typedef struct {\n    Prog *prog;'):text.index('static uint64_t pipe_key_hash(')]
    dynamic = text[text.index('static const VkDynamicState DYN[]'):text.index('// Fixed-function state', text.index('static const VkDynamicState DYN[]'))]
    assembly = text[text.index('// A pipeline\'s create info from its key'):text.index('static void build_pipeline(void *arg)')]
    # The hash has an earlier forward declaration: select its actual definition explicitly.
    start = text.index('static uint64_t hash_words(uint64_t h, const void *p, size_t n) {')
    helpers = function(text[start - 1:], 'hash_words(') + ''.join(function(text, name) for name in ('topo_class_base(', 'blend_factor(', 'blend_op('))
    header = key + dynamic + helpers + assembly
    (OUT / 'gpl.production.h').write_text(header)
    cc = shutil.which('clang')
    assert cc
    results = {}
    for name, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
        exe = OUT / name
        run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', *flags, '-I', OUT, '-I', ROOT / 'work/las-baseline/build/android/deps/include', TEST, '-pthread', '-o', exe])
        results[name] = run([exe])
        print(name + ': ' + results[name], flush=True)
    # The local ICD is a CPU-only software driver; never touches Thor or a physical GPU.
    import check_direct_spirv
    config, driver = check_direct_spirv.software_driver()
    vertex = OUT / 'synthetic.vert'
    fragment = OUT / 'synthetic.frag'
    vertex.write_text('#version 450\ninvariant gl_Position;\nvoid main(){gl_Position=vec4(0.0,0.0,0.25,1.0);}')
    fragment.write_text('#version 450\nlayout(location=0) out vec4 color;\nvoid main(){color=vec4(0.25,0.5,0.75,1.0);}')
    for source in (vertex, fragment):
        run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', source, '-o', str(source) + '.spv'])
    gpu_test = ROOT / 'tests/graphics_pipeline_library_vulkan.c'
    runner = OUT / 'software-vulkan'
    run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-I', OUT,
         '-I', ROOT / 'work/las-baseline/build/android/deps/include', gpu_test, '-lvulkan', '-pthread', '-o', runner])
    env = dict(os.environ, VK_DRIVER_FILES=str(config.resolve()), VK_ICD_FILENAMES=str(config.resolve()), LP_NUM_THREADS='2')
    gpu = json.loads(run([runner, str(vertex) + '.spv', str(fragment) + '.spv'], env=env))
    gpu['driver'] = driver
    print('software Vulkan: ' + json.dumps(gpu), flush=True)
    compiled = []
    if args.android:
        # Compile actual canonical exec/device units through the existing Android compile commands.
        entries = json.loads(compile_db.read_text())
        for unit in ('exec.c', 'device.c'):
            entry = next(e for e in entries if e['file'].endswith('/runtime/nvn/vulkan/' + unit))
            overlay = OUT / (unit + '.overlay.json')
            staged_runtime = Path(entry['file']).parents[2]
            canonical_runtime = SOURCE.parents[2]
            roots = [{'type': 'file', 'name': entry['file'], 'external-contents': str(SOURCE.with_name(unit))}]
            roots += [{'type': 'file', 'name': str(staged_runtime / p.relative_to(canonical_runtime)),
                       'external-contents': str(p)} for p in canonical_runtime.rglob('*.h')]
            overlay.write_text(json.dumps({'version': 0, 'roots': roots}))
            command = shlex.split(entry['command'])
            command[command.index('-o') + 1] = str(OUT / (unit + '.android.o'))
            run(command + ['-ivfsoverlay', overlay], cwd=entry['directory'])
            compiled.append(unit)
    paths = [SOURCE, SOURCE.with_name('device.c'), SOURCE.with_name('vk.h'), TEST, gpu_test, Path(__file__)]
    report = {'schema': 1, 'scope': 'Actual production PipeCI/GPL keys, subset assembly, concurrent ownership/fallback assertions and CPU Vulkan creation/link smoke. Does not prove rendered images, game accuracy or hardware speedup.',
              'results': results, 'software_vulkan': gpu, 'android_requested': args.android,
              'android_actual_units_compiled': compiled,
              'production_helpers_sha256': hashlib.sha256(header.encode()).hexdigest(),
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
