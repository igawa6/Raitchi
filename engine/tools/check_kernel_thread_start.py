#!/usr/bin/env python3
"""Test the actual production thread_start with controlled pthread failures.

No native/game builds or device actions. The test substitutes pthread attribute
and create calls, while using the production function body and private KThread
declaration. This checks start/error/publication policy, not guest execution.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "ports/shared-android/runtime/kernel.c"


def extract_function(source):
    signature = "static uint32_t thread_start(KThread *t) {"
    start = source.index(signature)
    depth = 0
    for index in range(source.index("{", start), len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise RuntimeError("Production thread_start body has no matching closing brace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/kernel-thread-start")
    args = parser.parse_args()
    if not args.clang:
        parser.error("Host Clang is required")
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / "build"):
        parser.error("Output must remain under the ignored workspace build directory")
    out.mkdir(parents=True, exist_ok=True)
    source = KERNEL.read_text()
    function = extract_function(source)
    constants = []
    for name in ("R_OK", "R_INVALID_STATE", "R_OUT_OF_RESOURCE"):
        match = re.search(r"^#define " + name + r"\s+(0x[0-9A-Fa-f]+|[0-9]+)\s*$", source, re.M)
        if not match:
            raise RuntimeError("Missing production result constant: " + name)
        constants += ["#undef " + name, "#define " + name + " " + match[1]]
    (out / "production_thread_start.inc").write_text("\n".join(constants) + "\n" + function + "\n")
    results = {}
    for label, flags in (("optimized", ["-O2"]),
                         ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / label
        command = [args.clang, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", *flags,
                   "-I", str(ROOT / "native"), "-I", str(ROOT / "ports/shared-android/runtime"),
                   "-I", str(ROOT / "sources/las-android/runtime"), "-I", str(out),
                   str(ROOT / "tests/kernel_thread_start_test.c"), "-pthread", "-o", str(binary)]
        built = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
        (out / (label + "-build.log")).write_text(built.stdout + built.stderr)
        ran = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
        (out / (label + "-run.log")).write_text(ran.stdout + ran.stderr)
        results[label] = {"passed": True, "stdout": ran.stdout.strip()}
        print(label + ": " + ran.stdout.strip(), flush=True)
    report = {"kernel_sha256": hashlib.sha256(source.encode()).hexdigest(),
              "production_function_sha256": hashlib.sha256(function.encode()).hexdigest(),
              "scope": "verbatim production thread_start; controlled pthread attributes/create; actual KThread header",
              "guest_execution": False, "device_actions": False, "results": results}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
