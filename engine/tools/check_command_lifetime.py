#!/usr/bin/env python3
"""Synthetic production command allocator/ownership core checks; no device or NDK."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / "ports/shared-android/runtime/nvn"
OUT = ROOT / "build/command-lifetime"


def run(command, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120, env=env)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout.strip()


def main():
    cc = shutil.which("clang")
    if not cc:
        raise RuntimeError("clang is required")
    OUT.mkdir(parents=True, exist_ok=True)
    header = (NVN / "nvn.h").read_text()
    start = header.index("typedef struct {", header.index("// One recorded command:"))
    end = header.index("} CmdRec;", start) + len("} CmdRec;")
    enum = header.index("enum {", end)
    stop = header.index("\n};", enum) + 3
    (OUT / "production_record_schema.h").write_text("#include <stdint.h>\n#include <stddef.h>\n" + header[start:end] + "\n" + header[enum:stop] + "\n")
    sources = [NVN / name for name in ("command_lifetime.c", "command_lifetime.h", "command_body_arena.c", "command_body_arena.h", "nvn.h")]
    tests = [ROOT / "tests/command_lifetime_test.c", ROOT / "tests/command_body_arena_test.c"]
    paths = sources + tests + [Path(__file__).resolve()]
    pins = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    results = {}
    for mode, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"])):
        for name, test, extra in (("arena", tests[1], []), ("lifetime", tests[0], [NVN / "command_lifetime.c"])):
            binary = OUT / f"{name}-{mode}"
            run([cc, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", *flags, "-I", NVN, "-I", OUT, test, NVN / "command_body_arena.c", *extra, "-pthread", "-o", binary])
            result = run([binary], env)
            results[f"{name}/{mode}"] = result
            print(f"{name}/{mode}: {result}", flush=True)
    assert pins == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}, "Sources changed during verification"
    (OUT / "verification.json").write_text(json.dumps({"state": "passed", "results": results, "source_sha256": pins,
        "scope": "Production allocator and lifetime core linked directly, actual recorder ABI/opcodes extracted from current nvn.h. Packing, stale tokens, preserved bytes, allocation rollback, CURRENT CALL closures, COPY snapshots, pinned body retirement, explicit completion, cached deterministic handles after structural collection, and 2048 overlapping publications with bounded live slots. Recorder/pool wrappers, queue routing, GPU completion and guest execution are outside this host fixture."}, indent=2) + "\n")


if __name__ == "__main__":
    main()
