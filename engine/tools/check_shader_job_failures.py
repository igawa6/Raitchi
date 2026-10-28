#!/usr/bin/env python3
"""Check real shader job failure paths without a GPU or immortal workers."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/shader-job-failures"
RUNTIME = ROOT / "ports/shared-android/runtime"


def function(source, name):
    start = source.index("static void " + name + "(")
    end = source.index("\n}", start) + 2
    return source[start:end] + "\n"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = RUNTIME / "nvn/vulkan/exec.c"
    paths = [source, RUNTIME / "nvn/shader_job_queue.c",
             RUNTIME / "nvn/shader_job_queue.h",
             ROOT / "tests/shader_job_failure_test.c", Path(__file__).resolve()]
    pins = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    text = source.read_text()
    (OUT / "shader_job_wrappers.inc").write_text(
        function(text, "queue_push") + function(text, "start_translator"))
    results = {}
    for name, flags in [("optimized", ["-O2"]),
                        ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined",
                                       "-fno-sanitize-recover=all"])]:
        binary = OUT / name
        subprocess.run([shutil.which("clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        *flags, "-I", str(RUNTIME), "-I", str(OUT), str(paths[3]),
                        "-pthread", "-o", str(binary)], check=True)
        results[name] = subprocess.check_output([binary], text=True, timeout=20).strip()
    assert pins == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    report = {"state": "passed", "results": results, "source_sha256": pins}
    (OUT / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
