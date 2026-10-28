#!/usr/bin/env python3
"""Test actual sampled texture-handle and pool resolvers with bounded host memory."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ports/shared-android/runtime/nvn/vulkan/exec.c"
TEST = ROOT / "tests/sampled_texture_handle_test.c"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function(text, name):
    match = re.search(r"(?m)^static\s+[^;\n]*\b" + re.escape(name) + r"\(", text)
    if not match:
        raise RuntimeError("Missing production function " + name)
    start = match.start()
    brace = text.index("{", match.end())
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise RuntimeError("Unclosed production function " + name)


def main():
    clang = shutil.which("clang")
    if not clang:
        raise RuntimeError("Host Clang is required")
    out = ROOT / "build/sampled-texture-handle"
    out.mkdir(parents=True, exist_ok=True)
    text = SOURCE.read_text()
    extracted = "\n\n".join(function(text, name) for name in
                               ("sampled_texture_handle", "shader_texture_object"))
    feedback = function(text, "resolve_feedback")
    binding = function(text, "texture_info")
    if "shader_texture_object(sg, &infos[sg]->tex[i])" not in feedback:
        raise RuntimeError("Feedback discovery no longer uses the sampled texture resolver")
    if "sampled_texture_handle(stage, st_, &h)" not in binding:
        raise RuntimeError("Descriptor binding no longer uses the sampled handle resolver")
    (out / "production_sampled_texture_helpers.inc").write_text(extracted + "\n")

    reports = {}
    for mode, flags in (("optimized", ["-O2"]),
                        ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / mode
        cmd = [clang, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", *flags,
               "-I", str(out), str(TEST), "-o", str(binary)]
        built = subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=45)
        (out / (mode + "-build.log")).write_text(built.stdout + built.stderr)
        ran = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=20)
        (out / (mode + "-run.log")).write_text(ran.stdout + ran.stderr)
        result = json.loads(ran.stdout)
        if result.get("state") != "passed":
            raise RuntimeError(mode + " reported failure")
        reports[mode] = result
        print(mode + ": " + ran.stdout.strip(), flush=True)

    report = {"schema": 1, "state": "passed", "source_sha256": {str(p.relative_to(ROOT)): sha(p)
              for p in (SOURCE, TEST, Path(__file__))}, "results": reports,
              "scope": "Verbatim production sampled_texture_handle/shader_texture_object; feedback and descriptor call-site source checks; six stages, bound/CB2/UBO handles, guard-page read, invalid bounds, live mutation, pool-zero distinction, controlled float-view rejection. No game shaders, Vulkan driver, device, or app build."}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
