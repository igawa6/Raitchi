#!/usr/bin/env python3
"""Exercise current compute cache parser and replay helpers with controlled queues."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / "ports/shared-android/runtime/nvn"
OUT = ROOT / "build/compute-headstart"


def function(text, name):
    match = re.search(r"(?m)^(?:static )?[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", text)
    if not match:
        raise RuntimeError(f"Cannot locate production definition: {name}")
    opening = text.index("{", match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end] + "\n"


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
    source = NVN / "vulkan/exec.c"
    hash_source = ROOT / "sources/las-android/runtime/nvn/shader_prep.c"
    kernel = NVN.parent / "kernel.h"
    stage_header = NVN / "nvn.h"
    paths = [source, hash_source, kernel, stage_header, NVN / "maxwell/shader.h", ROOT / "tests/compute_headstart_test.c", Path(__file__).resolve()]
    pins = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    text = source.read_text()
    prefix = text[text.index("#define HS_MAGIC"):text.index("static uint64_t hash_words", text.index("#define HS_MAGIC"))]
    start = text.index("typedef struct {\n    uint32_t magic, header_size, pipe_size, shader_key_size;")
    types = text[start:text.index("} HsHeader;", start) + len("} HsHeader;")]
    limit = re.search(r"(?m)^#define KERNEL_READABLE_COPY_MAX[^\n]+", kernel.read_text()).group(0)
    declarations = "\nstatic uint64_t hs_cache_file_bytes;\nstatic uint32_t hs_cache_record_count;\nstatic uint64_t hs_cs_recorded[HS_CS_DEDUP_TAB];\nstatic size_t hs_cs_recorded_n;\n"
    functions = ["hash_words", "hs_program_size_valid", "hs_cs_size_valid", "hs_cs_content_key", "hs_cs_recorded_clear_locked", "hs_cs_recorded_find_locked", "hs_cs_recorded_add_locked", "hs_cache_open"]
    (OUT / "compute_cache.inc").write_text(limit + "\n" + prefix + types + declarations + function(hash_source.read_text(), "shader_code_hash") + "\n".join(function(text, n) for n in functions))
    start = text.index("typedef struct { CsProg *cp; uint8_t *snapshot; size_t code_bytes, constant_bytes; uint64_t stage_hash; } HsCsTranslate;")
    typedef = text[start:text.index("\n", start)]
    (OUT / "compute_replay.inc").write_text(typedef + "\n" + "\n".join(function(text, n) for n in ["hs_cs_translate_job", "hs_cs_local_supported", "hs_cs_replay", "hs_cs_take", "hs_cs_state"]))
    stages = stage_header.read_text()
    start = stages.index("typedef struct NvnShaderStage {")
    stop = stages.index("} NvnProgram;", start) + len("} NvnProgram;")
    (OUT / "compute_stage_types.inc").write_text(stages[start:stop] + "\n" + function(hash_source.read_text(), "shader_stage_hash"))
    (OUT / "compute_program.inc").write_text(function(text, "cs_content_key") + function(text, "compute_program"))
    results = {}
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    for mode, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"])):
        binary = OUT / mode
        run([cc, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", *flags, "-I", NVN, "-I", OUT, ROOT / "tests/compute_headstart_test.c", "-pthread", "-o", binary])
        with tempfile.TemporaryDirectory(prefix="cache-", dir=OUT) as tmp:
            results[mode] = run([binary, Path(tmp) / "headstart.bin"], env)
        print(f"{mode}: {results[mode]}", flush=True)
    assert pins == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}, "Sources changed during verification"
    (OUT / "verification.json").write_text(json.dumps({"state": "passed", "results": results, "source_sha256": pins,
        "scope": "Exact current production cache types, parser, content key/hash, device-local-size admission, replay queue and ready-only lookup helpers. Synthetic bytes, immutable queue copy, pending-byte cap, in-flight/failed/mismatched misses, matching ready reuse, corrupt/oversized/torn/identity-mismatched cache reset. Translation, pipeline creation and queue scheduling are controlled stand-ins; no real translator, Vulkan, Android or guest execution. Actual compute_program exercised for cold foreground translation, failed/pending replay fallback and ready reuse; prepared-cache/capture/recording/fingerprint helpers are controlled."}, indent=2) + "\n")


if __name__ == "__main__":
    main()
