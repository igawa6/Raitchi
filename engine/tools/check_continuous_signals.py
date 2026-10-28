#!/usr/bin/env python3
"""Check continuous signal C routing on host and compile AArch64 callback code; no ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "ports/shared-android/runtime"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    out = ROOT / "build/continuous-signals"; out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("clang")
    if not cc: parser.error("Host Clang required")
    includes = ["-I", ROOT / "native", "-I", RUNTIME, "-I", ROOT / "sources/las-android/runtime"]
    sources = [RUNTIME / "continuous_signals.c", RUNTIME / "continuous_signals.h", RUNTIME / "continuous_context.h",
               ROOT / "native/continuous-nce/patch_plan.h", ROOT / "native/runtime/aot_cpu.h",
               ROOT / "tests/continuous_signals_test.c", Path(__file__).resolve()]
    hashes = {str(path.relative_to(ROOT)): sha(path) for path in sources}

    def run(command, log):
        process = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=30)
        (out / log).write_text(process.stdout + process.stderr)
        if process.returncode: raise RuntimeError("Check failed: " + str((out / log).relative_to(ROOT)))
        return process.stdout

    results = {}
    for label, flags in (("optimized", ["-O2"]),
                         ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / label
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, *includes,
             RUNTIME / "continuous_signals.c", ROOT / "tests/continuous_signals_test.c", "-pthread", "-o", binary], label + "-build.log")
        # Deliberately faulting children reset signal dispositions to obtain
        # real kernel SIGSEGV/SIGTRAP outcomes, rather than sanitizer handlers.
        text = run([binary], label + "-run.log")
        results[label] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", text)[1]), "stdout": text.strip()}
        print(label + ": " + text.strip(), flush=True)
    target = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
    run([target, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-ffixed-x18", *includes,
         "-c", RUNTIME / "continuous_signals.c", "-o", out / "continuous_signals.o"], "android-compile.log")
    if hashes != {str(path.relative_to(ROOT)): sha(path) for path in sources}: raise RuntimeError("Source changed during check")
    report = {"schema": 1, "host_tests": results, "android_aarch64_compile_passed": True,
              "device_actions": False, "guest_execution": False, "source_sha256": hashes,
              "scope": "actual POSIX actions, masks, guarded altstacks and default-fatal delivery; explicit assembly/kernel stand-ins, no guest TLS or production KThread claim"}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("AArch64 signal callback compiled; real guest-state/assembly integration requires coordinator probe.")


if __name__ == "__main__":
    main()
