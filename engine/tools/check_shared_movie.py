#!/usr/bin/env python3
"""Asset-free movie overlay checks: real C include, mock external interfaces.

Needs clang, the pinned LAS checkout (or staged shared runtime), cached FFmpeg
headers, and an Android NDK. Does not fetch dependencies or touch a device.
The host probe omits HLE registration sections and mocks cleanup/render/callback
interfaces; it does not exercise FFmpeg decoding or establish title support.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(args: list[str], label: str, output: Path, cwd: Path = ROOT) -> str:
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    log = result.stdout + result.stderr
    (output / (label + '.log')).write_text(log)
    if result.returncode:
        raise SystemExit(f'{label} failed ({result.returncode}):\n{log}')
    print(f'{label}: PASS' + (f'\n{log.rstrip()}' if log else ''))
    return log


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/shared-movie')
    parser.add_argument('--ffmpeg-include', type=Path,
                        default=ROOT / 'build/letsgo-ffmpeg/prefix/include')
    parser.add_argument('--ndk', type=Path, help='NDK root; uses compile_commands.json when available')
    args = parser.parse_args()
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    ffmpeg = args.ffmpeg_include.resolve()
    if not (ffmpeg / 'libavcodec/avcodec.h').is_file():
        raise SystemExit('Cached FFmpeg headers missing; pass --ffmpeg-include.')
    base = ROOT / 'sources/las-android/runtime'
    if not (base / 'hle.h').is_file():
        base = ROOT / 'work/shared-android/runtime'
    if not (base / 'hle.h').is_file():
        raise SystemExit('Pinned LAS/staged shared runtime headers are required.')

    # One coherent header tree avoids mixing separate pragma-once copies of
    # overlay/staged rt.h. The host test includes the live production movie.c.
    tree = output / 'runtime'
    if tree.exists():
        shutil.rmtree(tree)
    for src in (base, ROOT / 'ports/shared-android/runtime'):
        for header in src.rglob('*.h'):
            target = tree / header.relative_to(src)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(header, target)
    (tree / 'hle').mkdir(exist_ok=True)
    cc = shutil.which(os.environ.get('CC', 'clang'))
    if not cc:
        raise SystemExit('Host clang/CC not found.')
    flags = ['-std=gnu11', '-Wno-format', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-I' + str(tree), '-I' + str(tree / 'hle'),
             '-I' + str(ROOT / 'native'), '-I' + str(ffmpeg)]
    test = ROOT / 'tests/shared_movie_test.c'
    results = {}
    for name, variant in [('optimized', ['-O2']),
                          ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined'])]:
        binary = output / ('shared_movie_' + name)
        run([cc, *flags, *variant, str(test), '-o', str(binary)], name + '-compile', output)
        results[name] = run([str(binary)], name + '-run', output)

    commands = ROOT / 'build/shared-android/native/compile_commands.json'
    entries = json.loads(commands.read_text()) if commands.exists() else []
    ndk = args.ndk
    if ndk is None and os.environ.get('ANDROID_NDK_HOME'):
        ndk = Path(os.environ['ANDROID_NDK_HOME'])
    if ndk is None:
        cached = sorted((ROOT.parent / 'android-sdk/ndk').glob('*'))
        ndk = cached[-1] if cached else None
    hashes = {}
    for name in ('movie.c', 'vi.c'):
        source = ROOT / 'ports/shared-android/runtime/hle' / name
        hashes[name] = digest(source)
        # Exact full-TU snapshot for NDK: vi.c directly includes an owned kernel.h
        # whereas hle.h is inherited, so all quoted headers must share one tree.
        snapshot = tree / 'hle' / name
        shutil.copyfile(source, snapshot)
        entry = next((e for e in entries if e['file'].endswith('/hle/' + name)), None)
        target = output / (name + '.o')
        if entry and args.ndk is None:
            cmd = shlex.split(entry['command'])
            cmd[cmd.index('-o') + 1] = str(target)
            cmd[cmd.index('-c') + 1] = str(snapshot)
            cmd[1:1] = ['-I' + str(tree), '-I' + str(ROOT / 'native')]
            cwd = Path(entry['directory'])
        else:
            if ndk is None:
                raise SystemExit('NDK required: pass --ndk or set ANDROID_NDK_HOME.')
            compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang'
            cmd = [str(compiler), '-std=gnu11', '-O2', '-fPIC', '-DANDROID',
                   '-Werror=implicit-function-declaration', '-Wno-format',
                   '-I' + str(tree), '-I' + str(ROOT / 'native'), '-I' + str(ffmpeg),
                   '-c', str(snapshot), '-o', str(target)]
            cwd = ROOT
        run(cmd, 'ndk-' + name, output, cwd)
        if digest(snapshot) != hashes[name] or digest(source) != hashes[name]:
            raise SystemExit('Source changed during checks; rerun against a frozen overlay.')
    (output / 'verification.json').write_text(json.dumps({
        'scope': 'synthetic direct calls; mocked external interfaces; no media decode/device proof',
        'host': results, 'ndk_api': 30, 'ndk_arch': 'aarch64', 'sha256': hashes,
    }, indent=2) + '\n')
    print('Movie probes and both full Android translation units passed.')


if __name__ == '__main__':
    main()
