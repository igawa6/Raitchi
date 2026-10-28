#!/usr/bin/env python3
"""Verify the actual production hash wrapper; vectors only, no game files or device actions."""
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/shared-hash"


def main():
    compiler = shutil.which("clang++")
    if not compiler:
        raise RuntimeError("Host clang++ is required")
    OUT.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / "ports/shared-android/runtime/shared_hash.cpp", ROOT / "tests/shared_hash_test.cpp"]
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", ROOT / "ports/shared-android/runtime",
             "-I", ROOT / "native", "-I", ROOT / "sources/las-android/runtime"]
    report = {}
    for name, extra in (("optimized", ["-O2"]),
                        ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        runner = OUT / name
        subprocess.run([str(x) for x in [compiler, *flags, *extra, *sources, "-o", runner]], check=True, timeout=45)
        result = subprocess.run([str(runner), str(OUT)], check=True, capture_output=True, text=True, timeout=45)
        print(name + ": " + result.stdout.strip())
        report[name] = result.stdout.strip()
    (OUT / "verification.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
