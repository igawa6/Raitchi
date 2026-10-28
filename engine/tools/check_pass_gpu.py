#!/usr/bin/env python3
"""Asset-free checks of the live bounded render-frame GPU profiler; no device access."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'ports/shared-android/runtime/nvn/vulkan/exec.c'
TEST = ROOT / 'tests/pass_gpu_test.c'
CASES = ('disabled', 'zero', 'invalid', 'overflow', 'late', 'no_work', 'unsupported',
         'create_error', 'insufficient', 'cap', 'exact_cap', 'pending', 'timeout',
         'frame_timeout', 'read_error', 'wrap', 'groups', 'group_cap', 'draws_absent')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android', action='store_true')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/pass-gpu')
    args = parser.parse_args()
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    original = SOURCE.read_text()
    start = original.index('// ---- GPU time per pass (')
    end = original.index('\nstatic void vk_end_pass_at(int line);', start)
    fragment = original[start:end]
    (output / 'pass_gpu_fragment.h').write_text(fragment)
    assert 'VK_QUERY_RESULT_WAIT_BIT' not in fragment
    assert 'renderer_present_count()' not in fragment
    # Production draw counters must stop once pg_mark seals a full capture.
    assert re.search(r'if \(pg_state == 1 && pg_draws_on\(\) && pg_n > 0\) \{\s*uint64_t vc =[^;]+;\s*pg_verts \+= vc, pg_vc\[pg_n - 1\] = vc;', original)
    shared = (ROOT / 'ports/shared-android/apk/app/src/main/java/com/lasrecomp/android/SharedFiles.java').read_text()
    removal = shared[shared.index('static String withoutResearchEnvironment('):shared.index('private static String withoutKeys(')]
    for flag in ('LARECOMP_VK_PASS_GPU', 'LARECOMP_VK_PASS_GPU_DRAWS', 'LARECOMP_VK_PASS_GPU_ALL'):
        assert '"' + flag + '"' not in removal
    paths = (SOURCE, TEST, Path(__file__).resolve())
    pins = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    def run(command, name, cwd=None):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=60, cwd=cwd)
        (output / name).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + ': ' + result.stdout + result.stderr)
        return result.stdout
    results = {}
    for name, flags in (('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all'])):
        exe = output / name
        run([shutil.which('clang'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', *flags, '-I', output, TEST, '-o', exe], name + '-compile.log')
        results[name] = {case: json.loads(run([exe, case], name + '-' + case + '.log')) for case in CASES}
        print(name + ': passed ' + str(len(CASES)) + ' scenarios, ' + str(sum(x['assertions'] for x in results[name].values())) + ' assertions')
    android = {}
    if args.android:
        rows = json.loads((ROOT / 'build/shared-android/native/compile_commands.json').read_text())
        row = next(x for x in rows if x['file'].endswith('/runtime/nvn/vulkan/exec.c'))
        command = shlex.split(row['command'])
        command[command.index('-o') + 1] = str(output / 'exec-android.o')
        overlay = output / 'overlay.json'
        overlay.write_text(json.dumps({'version': 0, 'roots': [{'type': 'file', 'name': row['file'], 'external-contents': str(SOURCE)}, {'type': 'file', 'name': str(ROOT / 'work/shared-android/runtime/nvn/maxwell/shader.h'), 'external-contents': str(ROOT / 'ports/shared-android/runtime/nvn/maxwell/shader.h')}]}))
        run([*command, '-ivfsoverlay', overlay], 'android-production-compile.log', row['directory'])
        android = {'passed': True, 'object_sha256': sha(output / 'exec-android.o')}
    assert pins == {path: sha(ROOT / path) for path in pins}, 'Sources changed during tests'
    receipt = {'state': 'passed', 'source_sha256': pins, 'fragment_sha256': sha(output / 'pass_gpu_fragment.h'),
               'variants': results, 'android_production_compile': android,
               'scope': 'Live profiler region with controlled render identity, query results and clock; launcher flag retention checked. No driver/GPU/scene timing proof.'}
    (output / 'verification.json').write_text(json.dumps(receipt, indent=2) + '\n')

if __name__ == '__main__':
    main()
