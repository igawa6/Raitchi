#!/usr/bin/env python3
"""Asset-free production audio mix-splitter parsing, routing and PCM rendering checks."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'ports/shared-android/runtime/services/audio.c'
TEST = ROOT / 'tests/audio_splitter_test.c'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android', action='store_true')
    args = parser.parse_args()
    out = ROOT / 'build/audio-splitter'
    out.mkdir(parents=True, exist_ok=True)
    text = SOURCE.read_text()
    # Verbatim production state, decode/resample/effects/render code, and update
    # parser. Exclude only SDL output/thread plumbing; no mixer stand-ins.
    pieces = [text[text.index('#define WB_COUNT'):text.index('// Output gain:')],
              text[text.index('static void update_voice('):text.index('static void renderer_open_output(')]]
    (out / 'production_audio_splitter.inc').write_text('\n'.join(pieces))
    shutil.copyfile(ROOT / 'sources/las-android/runtime/ipc.h', out / 'ipc.h')
    (out / 'kernel.h').write_text('#pragma once\n#include <stdint.h>\n#include <stdbool.h>\ntypedef struct { int type; } KObject;\ntypedef struct KEvent KEvent;\n')
    results = {}
    for variant, flags in [('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
        binary = out / variant
        command = ['clang', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-parameter', *flags, '-I', str(out), str(TEST), '-pthread', '-lm', '-o', str(binary)]
        built = subprocess.run(command, text=True, capture_output=True, timeout=60)
        (out / f'{variant}-build.log').write_text(built.stdout + built.stderr)
        built.check_returncode()
        ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
        (out / f'{variant}-run.log').write_text(ran.stdout + ran.stderr)
        ran.check_returncode()
        results[variant] = {'passed': True, 'output': ran.stdout.strip()}
        print(variant + ': ' + ran.stdout.strip(), flush=True)
    android = {}
    if args.android:
        rows = json.loads((ROOT / 'build/shared-android/native/compile_commands.json').read_text())
        rows = [row for row in rows if row['file'].endswith('/runtime/services/audio.c')]
        if len(rows) != 1:
            raise RuntimeError('Missing unique staged Android audio compile command')
        row = rows[0]
        original = shlex.split(row['command'])
        command = []
        index = 0
        while index < len(original):
            item = original[index]
            if item in ('-o', '-MF', '-MT', '-MQ'):
                index += 2
                continue
            if item in ('-MD', '-MMD', '-MP') or item == row['file']:
                index += 1
                continue
            command.append(item)
            index += 1
        obj = out / 'audio-android.o'
        command += ['-I', str(ROOT / 'work/shared-android/runtime/services'), str(SOURCE), '-o', str(obj)]
        built = subprocess.run(command, cwd=row['directory'], check=True, text=True, capture_output=True, timeout=90)
        (out / 'android-build.log').write_text(built.stdout + built.stderr)
        android = {'passed': True, 'object_sha256': digest(obj)}
        print('Android production audio overlay compiled', flush=True)
    report = {'state': 'passed', 'results': results, 'android': android,
              'source_sha256': {str(path.relative_to(ROOT)): digest(path) for path in (SOURCE, TEST, Path(__file__))},
              'scope': 'Verbatim production splitter/mix parser, graph sort, voice decoding, resampling, mixing and final stereo render with synthetic PCM data; controlled SDL/thread/IPC envelope only. No playback/device/BGM identity claim.'}
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
