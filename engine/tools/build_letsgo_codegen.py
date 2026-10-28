#!/usr/bin/env python3
"""Generate private Let's Go AOT code and compile Android probes; never touch a device."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "sources/letsgo"
WORK = ROOT / "work/letsgo-baseline"
BUILD = WORK / "build/android-codegen"
INPUTS = ROOT / "private/letsgo-baseline/extracted/exefs"
MODULES = {"main": 0x7000000000, "sdk": 0x7004000000,
           "subsdk0": 0x7006000000, "subsdk1": 0x7008000000}


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--compile-probes", action="store_true",
                        help="Compile first code unit and address table of each module; no runtime link")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--python", type=Path,
                        default=ROOT / "work/las-baseline/build/android/venv/bin/python")
    parser.add_argument("--ndk", type=Path,
                        default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error("--jobs must be between 1 and 8")
    run([sys.executable, ROOT / "tools/verify_sources.py"])
    if not WORK.exists():
        WORK.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--shared", SOURCE, WORK])
    revision = run(["git", "-C", SOURCE, "rev-parse", "HEAD"],
                   capture_output=True, text=True).stdout.strip()
    actual = run(["git", "-C", WORK, "rev-parse", "HEAD"],
                 capture_output=True, text=True).stdout.strip()
    if actual != revision:
        raise RuntimeError("Owned Let's Go checkout differs from the pinned reference")
    if not args.generate and not args.compile_probes:
        print("Prepared owned Let's Go source; no generation, compilation or device action.")
        return

    # Reuse the already installed generator environment. No pip or downloads.
    generator_python = args.python.absolute()
    packages = json.loads(run([generator_python, "-c",
        "import importlib.metadata,json; print(json.dumps({n:importlib.metadata.version(n) "
        "for n in ['capstone','unicorn','lz4']}))"], capture_output=True, text=True).stdout)
    expected = {"capstone": "5.0.9", "unicorn": "2.1.4", "lz4": "4.4.5"}
    if packages != expected:
        raise RuntimeError(f"Generator environment differs from the audited versions: {packages}")
    BUILD.mkdir(parents=True, exist_ok=True)
    identity_files = [*sorted((WORK / "tools").glob("*.py")),
                      *sorted((WORK / "runtime").glob("*.h")),
                      WORK / "runtime/hle/hle_symbols.txt", WORK / "tools/gamelib_entry.c"]
    identity = {"source_commit": revision, "target": "aarch64-linux-android30",
                "simd": "asm", "guest_shift": 0, "cpu_size": 1024,
                "upstream_gamelib_abi": 3, "generator_packages": packages,
                "source_sha256": {str(p.relative_to(WORK)): digest(p) for p in identity_files},
                "module_inputs": {name: {"canonical_base": base,
                    "sha256": digest(INPUTS / name)} for name, base in MODULES.items()}}
    fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    generated = BUILD / "generated"
    previous_path = BUILD / "generation.json"
    if args.generate:
        previous = json.loads(previous_path.read_text()) if previous_path.is_file() else {}
        cache_files = previous.get("generated_sha256", {})
        reusable = (previous.get("fingerprint") == fingerprint and cache_files and
                    all((generated / f).is_file() and digest(generated / f) == value
                        for f, value in cache_files.items()))
        if reusable:
            print("All four generated modules match source/input/options fingerprints; reused.")
        else:
            # An interrupted generator cannot leave output eligible for compilation.
            previous_path.unlink(missing_ok=True)
            generated.mkdir(parents=True, exist_ok=True)
            for name, base in MODULES.items():
                log = BUILD / f"generate-{name}.log"
                command = [generator_python, WORK / "tools/gen.py", name, INPUTS / name,
                           hex(base), generated / name, "--simd", "asm"]
                if name == "sdk":
                    command += ["--hle", WORK / "runtime/hle/hle_symbols.txt"]
                print(f"Generating {name}; private log: {log.relative_to(ROOT)}", flush=True)
                with log.open("w") as stream:
                    run(command, cwd=WORK / "tools", stdout=stream, stderr=subprocess.STDOUT)
            files = sorted(generated.glob("*/*.c"))
            for name in MODULES:
                if not (generated / name / f"{name}_table.c").is_file():
                    raise RuntimeError(f"Missing generated table for {name}")
            previous = {**identity, "fingerprint": fingerprint,
                        "generated_sha256": {str(p.relative_to(generated)): digest(p) for p in files}}
            previous_path.write_text(json.dumps(previous, indent=2) + "\n")
            print(f"Generated {len(files)} private C units across four modules.", flush=True)
    if args.compile_probes:
        (BUILD / "compile-probes.json").unlink(missing_ok=True)
        if not previous_path.is_file():
            raise RuntimeError("Generate all modules first with --generate")
        previous = json.loads(previous_path.read_text())
        if previous.get("fingerprint") != fingerprint:
            raise RuntimeError("Generation is stale; rerun --generate")
        toolchain = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
        clang = toolchain / "aarch64-linux-android30-clang"
        flags = ["-O1", "-fPIC", "-fvisibility=hidden", "-fsigned-char",
                 "-fno-strict-aliasing", "-Wno-return-type", "-Wno-unused-label",
                 "-I", WORK / "runtime"]
        probes = []
        for name in MODULES:
            candidates = sorted((generated / name).glob(f"{name}_[0-9]*.c"))
            if not candidates:
                raise RuntimeError(f"No generated function units for {name}")
            probes += [candidates[0], generated / name / f"{name}_table.c"]
        for source in probes:
            relative = str(source.relative_to(generated))
            if digest(source) != previous["generated_sha256"].get(relative):
                raise RuntimeError(f"Generated unit changed: {relative}")
        probes.append(WORK / "tools/gamelib_entry.c")
        output = BUILD / "probe-objects"
        output.mkdir(exist_ok=True)

        def compile_one(source):
            destination = output / (source.stem + ".o")
            with (output / (source.stem + ".log")).open("w") as log:
                run([clang, *flags, "-c", source, "-o", destination],
                    cwd=WORK, stdout=log, stderr=subprocess.STDOUT)
            return {"unit": source.name, "object_sha256": digest(destination)}

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            compiled = list(pool.map(compile_one, probes))
        result = {"generation_fingerprint": fingerprint,
                  "gamelib_id": "dev (header default; compile-only entry probe)",
                  "clang_version": run([clang, "--version"], capture_output=True, text=True).stdout,
                  "compile_flags": [str(x) for x in flags], "objects": compiled,
                  "linked": False, "device_executed": False}
        (BUILD / "compile-probes.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"Compiled {len(compiled)} Android ARM64 probe units; no runtime link or device execution.")


if __name__ == "__main__":
    main()
