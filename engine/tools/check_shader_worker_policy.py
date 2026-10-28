#!/usr/bin/env python3
"""Affinity-aware compiler limits for loading and gameplay, without device access."""
import hashlib
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/shader-worker-policy"
SOURCE = ROOT / "ports/shared-android/runtime/nvn/shader_worker_policy.h"
TEST = ROOT / "tests/shader_worker_policy_test.c"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    results = {}
    for name, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"])):
        executable = OUT / name
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I", str(ROOT / "ports/shared-android/runtime"), str(TEST), "-o", str(executable)], check=True)
        results[name] = subprocess.check_output([executable], text=True).strip()
    report = {"state": "passed", "results": results, "source_sha256": {
        str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in (SOURCE, TEST, pathlib.Path(__file__).resolve())}}
    (OUT / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
