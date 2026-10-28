#!/usr/bin/env python3
"""Check the bounded native plan cache on host and cross-link for Android; no ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "native/native-game-backend"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    parser.add_argument("--capstone-include", type=Path, default=ROOT / "build/shared-ondevice/work/build/ondevice/src/capstone-5.0.9/src/include")
    parser.add_argument("--android-capstone", type=Path, default=ROOT / "build/shared-android/apk-inputs/jniLibs/arm64-v8a/libcapstone.so")
    args = parser.parse_args()
    out = ROOT / "build/native-plan-cache"; out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("clang")
    if not cc: parser.error("Installed host Clang required")
    paths = [SOURCE / name for name in ("plan_cache.c", "plan_cache.h", "block_plan.c", "block_plan.h")]
    paths += [ROOT / "tests/native_plan_cache_test.c", Path(__file__).resolve(),
              ROOT / "ports/shared-android/runtime/native_engine.c", ROOT / "ports/shared-android/runtime/native_engine.h",
              *[args.capstone_include / "capstone" / name for name in ("capstone.h", "arm64.h")]]
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in paths}

    def run(command, log):
        result = subprocess.run(list(map(str, command)), check=False, capture_output=True, text=True, timeout=30)
        (out / log).write_text(result.stdout + result.stderr)
        if result.returncode: raise RuntimeError("Check failed: " + str((out / log).relative_to(ROOT)))
        return result.stdout

    variants = {}
    includes = ["-I", ROOT / "native", "-I", args.capstone_include]
    for label, flags in (("optimized", ["-O2"]),
                         ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / label
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, *includes,
             SOURCE / "block_plan.c", SOURCE / "plan_cache.c", ROOT / "tests/native_plan_cache_test.c",
             "-lcapstone", "-o", binary], label + "-build.log")
        text = run([binary], label + "-run.log")
        variants[label] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", text)[1]), "stdout": text.strip()}
        print(label + ": " + text.strip(), flush=True)
    tools = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    library = out / "libnative_plan_cache.so"
    run([tools / "aarch64-linux-android30-clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-shared", "-fPIC", "-ffixed-x18", *includes, SOURCE / "block_plan.c", SOURCE / "plan_cache.c",
         args.android_capstone, "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384", "-o", library], "android-build.log")
    if hashes != {str(p.relative_to(ROOT)): sha(p) for p in paths}: raise RuntimeError("Source changed during verification")
    report = {"schema": 1, "host_tests": variants, "android_cross_link_passed": True,
              "source_sha256": hashes, "android_library_sha256": sha(library),
              "android_capstone_sha256": sha(args.android_capstone), "device_actions": False,
              "scope": "actual planner/Capstone + fixed256-entry cache; byte invalidation, hook limits, first exit, guarded text bounds, collisions and independent contexts; no guest execution"}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Android cache/planner cross-link passed; no device actions.")


if __name__ == "__main__":
    main()
