#!/usr/bin/env python3
"""Exercise native module loading with game-free DSOs; cross-link for Android offline."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ("good2", "good4", "missing", "bad_version", "bad_hash",
            "null_function", "unresolved")
CASES = (*FIXTURES, "missing_file", "invalid_args", "profile_mismatch",
         "second_open", "second_open_after_reject", "second_open_after_open_failure",
         "reentrant", "concurrent")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def build(compiler, output, extra, android=False):
    output.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", ROOT / "native",
             "-O1" if extra else "-O2", *extra]
    gate = output / "libmodule_gate.so"
    link = ["-Wl,-z,max-page-size=16384"] if android else []
    run([compiler, *flags, "-fPIC", "-fvisibility=hidden", "-shared",
         ROOT / "native/module_abi.c", ROOT / "native/module_loader.c",
         "-Wl,-soname,libmodule_gate.so", *link, "-ldl", "-pthread", "-o", gate])
    for kind, name in enumerate(FIXTURES):
        run([compiler, *flags, "-fPIC", "-shared", f"-DFIXTURE_KIND={kind}",
             ROOT / "tests/module_loader_fixture.c", *link, "-o", output / f"{name}.so"])
    runner = output / "module_loader_test"
    run([compiler, *flags, ROOT / "tests/module_loader_test.c", "-L", output,
         "-lmodule_gate", "-Wl,-rpath,$ORIGIN", "-Wl,--export-dynamic", *link,
         "-ldl", "-pthread", "-o", runner])
    return gate, runner


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--ndk", type=Path, default=Path(
        os.environ.get("ANDROID_NDK_HOME") or
        Path.home() / "android-sdk/ndk/27.2.12479018"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/module-loader")
    args = parser.parse_args()
    if not args.clang:
        parser.error("Host Clang was not found")
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Build output must stay under the ignored workspace build directory")
    toolchain = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    android_compiler = toolchain / "aarch64-linux-android30-clang"
    if not android_compiler.is_file():
        parser.error("Offline Android arm64/API30 compiler not found; pass --ndk")
    results = {}
    for name, extra in (("host", []), ("sanitized", ["-g", "-fsanitize=address,undefined",
                                                   "-fno-omit-frame-pointer"])):
        directory = output / name
        gate, runner = build(args.clang, directory, extra)
        before = sha(gate)
        cases = []
        for case in CASES:
            result = run([runner, directory, case], capture_output=True, text=True)
            print(f"{name}/{case}: {result.stdout.strip()}")
            cases.append({"case": case, "stdout": result.stdout.strip()})
        if sha(gate) != before:
            raise RuntimeError("Shared gate artifact changed between profile tests")
        results[name] = {"gate_sha256": before, "cases": cases}
    directory = output / "android-arm64"
    gate, runner = build(android_compiler, directory, [], android=True)
    elf = run([toolchain / "llvm-readelf", "--file-header", "--dynamic", "--wide", gate],
              capture_output=True, text=True).stdout
    if "AArch64" not in elf or "libmodule_gate.so" not in elf:
        raise RuntimeError("Android gate ELF target or SONAME mismatch")
    (directory / "gate-elf.txt").write_text(elf)
    results["android"] = {"gate_sha256": sha(gate), "runner_sha256": sha(runner),
                          "execution": "not run", "target": "aarch64-linux-android30"}
    sources = [ROOT / "native/module_abi.h", ROOT / "native/module_abi.c",
               ROOT / "native/module_loader.h", ROOT / "native/module_loader.c",
               ROOT / "tests/module_loader_fixture.c", ROOT / "tests/module_loader_test.c",
               Path(__file__).resolve()]
    report = {"verified_utc": datetime.now(timezone.utc).isoformat(),
              "source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources},
              "results": results, "synthetic_only": True, "game_execution": False}
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Android arm64/API30 gate, runner and seven DSOs cross-link; not executed.")
    print("Synthetic loader plumbing only; real title metadata and app integration remain pending.")


if __name__ == "__main__":
    main()
