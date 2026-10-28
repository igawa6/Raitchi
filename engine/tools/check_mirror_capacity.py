#!/usr/bin/env python3
"""Production mirror allocation/retirement regression with controlled Vulkan recorder."""
import argparse, hashlib, json, pathlib, shlex, shutil, subprocess
ROOT = pathlib.Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android', action='store_true')
    args = parser.parse_args()
    out = ROOT / 'build/mirror-capacity'
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'ports/shared-android/runtime/nvn/vulkan/exec.c'
    test = ROOT / 'tests/mirror_capacity_test.c'
    fixture = ROOT / 'tests/graphics_mirror_test.c'
    text = source.read_text()
    start = text.index('#define MIR_SIZE ')
    end = text.index('// (guest_direct) a range the GPU holds the current copy of: that copy', start)
    fragment = text[start:end]
    # The observer has its own actual-body tests; this fixture exercises allocation/merge only.
    probe = fragment.index('static void projection_probe_mirror_cb4(')
    probe_end = fragment.index('\n}\n', probe) + 3
    fragment = fragment[:probe] + fragment[probe_end:]
    header = out / 'graphics_mirror_fragments.h'
    header.write_text(fragment)
    paths = [source, test, fixture, pathlib.Path(__file__).resolve()]
    pins = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    def run(command, log, cwd=None, expect_failure=False):
        result = subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True, text=True, timeout=90)
        (out / log).write_text(result.stdout + result.stderr)
        if (result.returncode == 0) == expect_failure:
            raise RuntimeError(log + ': ' + result.stdout + result.stderr)
        return result.stdout
    reports = {}
    cc = shutil.which('clang')
    common = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', out, test, '-pthread']
    for name, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
        exe = out / name
        run([*common, *flags, '-o', exe], name + '-build.log')
        reports[name] = json.loads(run([exe], name + '-run.log').splitlines()[-1])
    # A shortened victim loop reproduces the old arbitrary eight-attempt bug.
    mutant = fragment.replace('progress <= 2 * MIR_SLOTS', 'progress < 8')
    assert mutant != fragment
    header.write_text(mutant)
    exe = out / 'eight-victim-mutant'
    run([*common, '-O2', '-o', exe], 'mutant-build.log')
    run([exe], 'mutant-run.log', expect_failure=True)
    assert 'mirror_slot_reserve(128 * 1024, &k, &off)' in (out / 'mutant-run.log').read_text()
    header.write_text(fragment)
    android = {}
    if args.android:
        rows = json.loads((ROOT / 'build/shared-android/native/compile_commands.json').read_text())
        row = next(r for r in rows if r['file'].endswith('/runtime/nvn/vulkan/exec.c'))
        command = shlex.split(row['command'])
        command[command.index('-o') + 1] = str(out / 'exec-android.o')
        overlay = out / 'overlay.json'
        stage = ROOT / 'work/shared-android/runtime/nvn/vulkan/exec.c'
        overlay.write_text(json.dumps({'version': 0, 'roots': [{'type': 'file', 'name': str(stage), 'external-contents': str(source)}]}))
        run([*command, '-ivfsoverlay', overlay], 'android-production-compile.log', row['directory'])
        android = {'passed': True, 'object_sha256': sha(out / 'exec-android.o')}
    assert pins == {str(p.relative_to(ROOT)): sha(p) for p in paths}, 'Source changed during checks'
    receipt = {'state': 'passed', 'host_tests': reports, 'eight_victim_mutant_rejected': True,
               'android_production_compile': android, 'source_sha256': pins,
               'scope': 'Actual production mirror functions, synthetic storage and controlled command recorder; includes graphics visibility/merge suite. No Vulkan hardware or device claim.'}
    (out / 'verification.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))
if __name__ == '__main__': main()
