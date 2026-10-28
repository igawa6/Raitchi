#!/usr/bin/env python3
"""Exercise actual driver-probe/fatal routing with temporary files and no GPU."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/driver-probe-recovery"
DEVICE = ROOT / "ports/shared-android/runtime/nvn/vulkan/device.c"
CRASH = ROOT / "ports/shared-android/runtime/crash_explain.c"


def function(text, name):
    match = re.search(r"(?m)^(?:static )?[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", text)
    if not match:
        raise RuntimeError("Missing production definition: " + name)
    start = text.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end] + "\n"


def run(command, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=60, env=env)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout.strip()


def main():
    cc = shutil.which("clang")
    if not cc:
        raise RuntimeError("clang is required")
    OUT.mkdir(parents=True, exist_ok=True)
    test = ROOT / "tests/driver_probe_recovery_test.c"
    paths = [DEVICE, CRASH, test, Path(__file__).resolve()]
    pins = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    device, crash = DEVICE.read_text(), CRASH.read_text()
    (OUT / "driver_probe_production.inc").write_text(function(device, "vk_gpu_driver_runtime_failure") + function(device, "vk_present_result") + function(crash, "rt_fatal_msg") + function(crash, "rt_crash_fault"))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    results = {}
    for mode, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"])):
        binary = OUT / mode
        run([cc, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", *flags, "-I", OUT, test, "-pthread", "-o", binary])
        for case in ("known", "signal", "unknown", "gpu", "init", "system", "similar", "missing", "unlink-failure", "present"):
            with tempfile.TemporaryDirectory(prefix="probe-", dir=OUT) as directory:
                result = run([binary, directory, case], env)
            results[f"{mode}/{case}"] = result
            print(mode + "/" + result, flush=True)
    assert pins == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}, "Sources changed during verification"
    (OUT / "verification.json").write_text(json.dumps({"state": "passed", "results": results, "source_sha256": pins,
        "scope": "Actual Android helper, fatal/first-fault routing and 60-successful-present handling; real temporary marker files. Only exact controlled NVN self-chain rejection after initialized active custom driver acknowledges marker. Unknown/signal/GPU/init/system/similar-message/unlink-failure cases preserve recovery. No Java preferences, driver load, Vulkan or device execution."}, indent=2) + "\n")


if __name__ == "__main__":
    main()
