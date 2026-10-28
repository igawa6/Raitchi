#!/usr/bin/env python3
"""Build synthetic NDK fixtures; optionally run them through adb without waking the device.

This validates shell-process loading and CPU headers, not Android app namespaces,
the full guest address range, Vulkan, generated game code, or gameplay.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path(os.environ.get("ANDROID_NDK_HOME", Path.home() / "android-sdk/ndk/27.2.12479018")))
    parser.add_argument("--serial", help="Optional adb device for the synthetic native probe")
    args = parser.parse_args()
    run(["python3", ROOT / "tools/verify_sources.py"])
    cc = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
    out = ROOT / "build/native-probe"
    out.mkdir(parents=True, exist_ok=True)
    common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wl,-z,max-page-size=16384"]
    fixture = ROOT / "tools/android-smoke/fixture.c"
    libraries = []
    for source in ("las-android", "letsgo"):
        library = out / f"libprobe-{source}.so"
        run([*common, "-fPIC", "-shared", "-I", ROOT / f"sources/{source}/runtime", fixture, "-o", library])
        libraries.append(library)
    runner = out / "probe-runner"
    run([*common, ROOT / "tools/android-smoke/runner.c", "-ldl", "-o", runner])
    print("Built both ARM64 header fixtures and dynamic-loading runner.")
    if not args.serial:
        return
    adb = ["adb", "-s", args.serial]
    remote = "/data/local/tmp/switch-recomp-probe"
    run([*adb, "shell", "mkdir", "-p", remote])
    run([*adb, "push", runner, *libraries, remote + "/"], stdout=subprocess.PIPE)
    run([*adb, "shell", "chmod", "700", remote + "/probe-runner"])
    result = run([*adb, "shell", remote + "/probe-runner", *[remote + "/" + lib.name for lib in libraries]], stdout=subprocess.PIPE)
    evidence = json.loads(result.stdout)
    destination = ROOT / "evidence/native-probe.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
