#!/usr/bin/env python3
"""Check request-only depth replay policy, actual command emitter and Android TU."""
import os
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android', action='store_true')
    args = parser.parse_args()
    out = ROOT / 'build/depth-replay'
    out.mkdir(parents=True, exist_ok=True)
    runtime = ROOT / 'ports/shared-android/runtime'
    source = runtime / 'nvn/vulkan/exec.c'
    header = source.with_name('depth_replay.h')
    test = ROOT / 'tests/depth_replay_test.c'
    analyzer = ROOT / 'tools/analyze_depth_replay.py'
    paths = [source, header, test, analyzer, Path(__file__).resolve()]
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    pins = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    h = header.read_text()
    start = h.index('static void depth_replay_emit(')
    end = h.index('\n}\n', start) + 3
    emitter = h[start:end]
    (out / 'depth_replay.emitter.h').write_text(emitter)
    start_capture = h.index('static void depth_replay_capture(')
    end_capture = h.index('\n}\n', start_capture) + 3
    capture = h[start_capture:end_capture]
    assert not re.search(r'\b(?:renderer_\w+|vk_wait\w*|vk_cmd|vk_upload_cmd|draw_|stage_set|get_program|mirror_\w+)\s*\(', emitter)
    assert 'VK_COMPARE_OP_ALWAYS' in emitter and 'rec_CmdSetDepthWriteEnable(cb, VK_FALSE)' in emitter
    assert 'j->recorded.ds' in emitter and 'j->recorded.dyn' in emitter
    assert 'VK_BLEND_OP_MIN' in h and 'VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT' in h
    assert 'atomic_store_explicit(&j->state, 1, memory_order_release)' in h
    assert 'j->seq <= vk_completed_seq()' in h
    assert 'if (frame == depth_replay.frame) return;' in h
    assert 'depth_replay_extent(pass_w, pass_h, &bytes)' in h
    s = source.read_text()
    forget = re.search(r'static void ss_forget\(void\) \{[^\n]+\}', s).group()
    (out / 'depth_replay.capture.h').write_text(forget + '\n' + capture)
    draw = s[s.index('static void draw_(CmdRec *r, bool indexed)'):s.index('static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {')]
    assert draw.index('depth_replay_select(') > draw.index('material_probe_draw(')
    assert draw.index('depth_replay_capture(') > draw.index('else rec_CmdDrawIndexed(cb, icount')
    assert draw.index('depth_replay_capture(') < draw.index('mirror_finish_graphics(cb)')
    assert 'if (depth_replay_slot >= 0)' in draw
    assert 'pass_depth_attachment_view = has_d ? da.imageView : VK_NULL_HANDLE;' in s
    def run(command, name, cwd=None):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=90, cwd=cwd)
        (out / name).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + ': ' + result.stdout + result.stderr)
        return result.stdout
    # Use only Vulkan definitions from the cached NDK. Host libc remains first.
    ndk = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home() / 'android-sdk')) / 'ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include'
    host = {}
    for name, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]:
        exe = out / name
        run([shutil.which('clang'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', *flags,
             '-idirafter', ndk, '-I', out, '-I', header.parent, test, '-o', exe], name + '-build.log')
        host[name] = json.loads(run([exe], name + '-run.log'))
    spec = importlib.util.spec_from_file_location('depth_analyzer', analyzer)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    f32 = lambda x: struct.pack('<f', x)
    half = struct.unpack('<I', f32(.5))[0]
    near = f32(.5) + struct.pack('<I', half + 1) + f32(math.inf) + f32(float('nan'))
    reference = f32(.5) * 4
    result, pixels = module.compare(near, reference, 130)
    assert result['covered'] == 2 and result['histogram_abs_delta']['0'] == 1 and result['histogram_abs_delta']['1'] == 1
    assert result['uncovered'] == 1 and result['invalid'] == 1 and len(pixels) == 4
    result24, _ = module.compare(f32(1) + f32(0), struct.pack('<II', 0xffffff, 0), 129)
    assert result24['histogram_abs_delta']['0'] == 2
    for n, r, fmt in [(b'1', b'1', 130), (b'', b'', 999), (f32(0), b'', 130)]:
        try:
            module.compare(n, r, fmt)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid analyzer input accepted')
    android = {}
    if args.android:
        rows = json.loads((ROOT / 'build/shared-android/native/compile_commands.json').read_text())
        row = next(r for r in rows if r['file'].endswith('/runtime/nvn/vulkan/exec.c'))
        owned = [source, header, source.with_name('uniform_forward.h'), source.with_name('mirror_normalize.h'),
                 runtime / 'nvn/vulkan/vk.h', runtime / 'nvn/maxwell/shader.h']
        overlay = out / 'overlay.json'
        overlay.write_text(json.dumps({'version': 0, 'roots': [{'type': 'file',
            'name': str(ROOT / 'work/shared-android/runtime' / p.relative_to(runtime)), 'external-contents': str(p)} for p in owned]}))
        command = shlex.split(row['command'])
        obj = out / 'exec-android.o'
        command[command.index('-o') + 1] = str(obj)
        run([*command, '-ivfsoverlay', overlay], 'android-production-compile.log', row['directory'])
        android = {'passed': True, 'object_sha256': sha(obj)}
    assert pins == {str(p.relative_to(ROOT)): sha(p) for p in paths}, 'source changed during checks'
    report = {'state': 'passed', 'host': host, 'analyzer_fixtures': 5, 'android': android, 'source_sha256': pins,
              'scope': 'Actual request parser/extent policy and actual Vulkan command emitter with controlled recorder: exact input bindings/draw args/raster, ALWAYS read-only depth/stencil off, async copy ordering and immutable captured state. Analyzer synthetic IEEE32 and packed-D24 checks. No device/GPU depth-accuracy claim.'}
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
