#!/usr/bin/env python3
"""Compile twelve cached private AOT units against the shared CPU; never regenerate, link or use ADB."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "native/runtime/aot_cpu.h"
MODULES = {"main": 0x7000000000, "sdk": 0x7004000000,
           "subsdk0": 0x7006000000, "subsdk1": 0x7008000000}


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def checked(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, **kwargs)


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def select(generated, names):
    units = []
    for name in names:
        candidates = sorted((generated / name).glob(f"{name}_[0-9]*.c"))
        if not candidates:
            raise RuntimeError(f"No cached function unit for {name}")
        for kind, path in (("functions", candidates[0]), ("table", generated / name / f"{name}_table.c")):
            if not path.is_file():
                raise RuntimeError(f"Missing cached {name} {kind}")
            units.append({"module": name, "kind": kind, "source": str(path), "source_sha256": sha(path)})
    return units


def validate_lg():
    work = ROOT / "work/letsgo-baseline"
    manifest = work / "build/android-codegen/generation.json"
    original = json.loads(manifest.read_text())
    identity = {k: v for k, v in original.items() if k not in ("fingerprint", "generated_sha256")}
    if fingerprint(identity) != original["fingerprint"]:
        raise RuntimeError("Let's Go generation fingerprint is inconsistent")
    expected = {"target": "aarch64-linux-android30", "simd": "asm", "guest_shift": 0,
                "cpu_size": 1024, "upstream_gamelib_abi": 3}
    if any(original.get(k) != v for k, v in expected.items()):
        raise RuntimeError("Let's Go cached generation has another ABI/SIMD/address profile")
    pinned = next(p["commit"] for p in json.loads((ROOT / "sources.lock.json").read_text())["sources"]
                  if p["id"] == "letsgo")
    actual = checked(["git", "-C", work, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if actual != pinned or original["source_commit"] != pinned:
        raise RuntimeError("Let's Go source/generation differs from the pinned revision")
    for relative, digest in original["source_sha256"].items():
        if sha(work / relative) != digest:
            raise RuntimeError(f"Let's Go generator/header changed: {relative}")
    if set(original["module_inputs"]) != set(MODULES):
        raise RuntimeError("Let's Go generation module inventory mismatch")
    for name, base in MODULES.items():
        record = original["module_inputs"][name]
        if record["canonical_base"] != base or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"]):
            raise RuntimeError("Let's Go generation module profile is invalid")
    generated = manifest.parent / "generated"
    units = select(generated, MODULES)
    for unit in units:
        relative = str(Path(unit["source"]).relative_to(generated))
        if original["generated_sha256"].get(relative) != unit["source_sha256"]:
            raise RuntimeError("Let's Go selected generated unit changed since generation")
    return {"title": "letsgo", "capabilities": 2, "guest_shift": 0, "simd": "asm",
            "provenance": "original_generation_manifest_selected_units_verified",
            "generation_manifest": str(manifest), "generation_manifest_sha256": sha(manifest),
            "generation_fingerprint": original["fingerprint"], "source_commit": pinned,
            "original_input_files_rehashed": False, "units": units}


def validate_las():
    work = ROOT / "work/las-baseline"
    script = work / "tools/android/gamelib_id.sh"
    text = script.read_text()
    values = {}
    for name in ("GENFLAGS", "SPLITFLAGS", "GENARGS", "CC22FLAGS"):
        match = re.search(r"^" + name + r'="([^"\n]*)"$', text, re.M)
        if not match:
            raise RuntimeError("Unknown LAS legacy identity-script format")
        values[name] = match.group(1)
    # Reproduce the pinned legacy hash without running repository shell scripts.
    files = ["tools/game_addresses.py", "runtime/game_addresses/versions.h",
             *[str(p.relative_to(work)) for p in sorted((work / "runtime/game_addresses").glob("patches_v*.h"))],
             "tools/gen.py", "tools/lifter.py", "tools/analyze.py", "tools/nso.py", "tools/passes.py",
             "tools/patches.py", "tools/simd_c.py", "tools/rando/patch_variants.json", "tools/gamelib_entry.c",
             "runtime/hle/hle_symbols.txt", "runtime/cpu.h", "runtime/simd.h", "runtime/simd_fp.h",
             "runtime/gamelib.h", "tools/hot_functions.txt"]
    hashed = hashlib.sha256()
    for relative in files:
        hashed.update((work / relative).read_bytes())
    hashed.update((values["GENFLAGS"] + " android shift=0x6000000000\n").encode())
    hashed.update((f'split {values["SPLITFLAGS"]} gen {values["GENARGS"]} cc22 {values["CC22FLAGS"]}\n').encode())
    legacy = hashed.hexdigest()[:16]
    library_id_file = work / "build/android/gamelib/arm64-v8a/liblasgame.so.id"
    library_id = library_id_file.read_text().strip()
    build_id_file = work / "build/android/gen/main/main_build_id.txt"
    build_id = build_id_file.read_text().strip().upper()
    if not re.fullmatch(r"[0-9A-F]{40,64}", build_id) or library_id != legacy + "-" + build_id[:16]:
        raise RuntimeError("LAS current generators/headers/options differ from the legacy cache identity")
    build_log = ROOT / "evidence/las-game-build.log"
    if f"== generating C (game code id {library_id})" not in build_log.read_text():
        raise RuntimeError("LAS existing build log has no matching generation identity")
    pinned = next(p["commit"] for p in json.loads((ROOT / "sources.lock.json").read_text())["sources"]
                  if p["id"] == "las-android")
    actual = checked(["git", "-C", work, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if actual != pinned:
        raise RuntimeError("LAS owned source differs from its pinned revision")
    if not all(value in values["GENARGS"] for value in ("--simd asm", "--shift 0x6000000000")):
        raise RuntimeError("LAS legacy generation options mismatch")
    generated = work / "build/android/gen"
    units = select(generated, ("main", "sdk"))
    return {"title": "las", "capabilities": 1, "guest_shift": 0x6000000000, "simd": "asm",
            "provenance": "observed_legacy_cache_snapshot_not_historical_unit_certification",
            "source_commit": pinned, "legacy_library_id": library_id,
            "legacy_script_sha256": sha(script), "legacy_values": values,
            "source_sha256": {name: sha(work / name) for name in files},
            "build_id_file_sha256": sha(build_id_file), "library_id_file_sha256": sha(library_id_file),
            "existing_build_log_sha256": sha(build_log), "original_input_files_rehashed": False,
            "historical_generated_unit_hashes_available": False, "units": units}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile", action="store_true", help="Compile the selected twelve objects after provenance checks")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=1)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    profiles = [validate_las(), validate_lg()]
    toolchain = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    compiler = toolchain / "aarch64-linux-android30-clang"
    properties = (args.ndk / "source.properties").read_text()
    if "Pkg.Revision = 27.2.12479018" not in properties:
        raise RuntimeError("Expected pinned NDK r27c")
    version = checked([compiler, "--version"], capture_output=True, text=True).stdout
    if not args.compile:
        print("Validated selected LG units against generation manifest and LAS legacy identity/cache snapshot; no compilation.")
        return
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = ROOT / "build/shared-aot-probes" / stamp
    output.mkdir(parents=True, exist_ok=False)
    common_copy = output / "aot_cpu.h"
    common_copy.write_bytes(HEADER.read_bytes())
    common_hash = sha(common_copy)
    result = {"schema": 1, "created_utc": stamp, "cpu_identity": 3, "cpu_size": 66560,
              "cpu_alignment": 16, "common_header_sha256": common_hash,
              "compiler": {"path": str(compiler), "driver_sha256": sha(compiler),
                           "binary_sha256": sha((toolchain / "clang").resolve()),
                           "version": version, "ndk_properties_sha256": sha(args.ndk / "source.properties")},
              "profiles": profiles, "objects": [], "complete": False,
              "generated_or_original_source_modified": False, "linked": False, "device_executed": False}
    manifest = output / "probes.json"
    atomic_json(manifest, result)
    tasks = []
    for profile in profiles:
        include = output / profile["title"] / "include"
        include.mkdir(parents=True)
        wrapper = (f'#define AOT_GUEST_SHIFT {profile["guest_shift"]}ull\n'
                   f'#define AOT_PROFILE_CAPABILITIES {profile["capabilities"]}u\n'
                   '#include "../../aot_cpu.h"\n'
                   '_Static_assert(sizeof(Cpu) == 66560 && _Alignof(Cpu) == 16, "shared CPU");\n'
                   '_Static_assert(offsetof(Cpu, excl_hi) == 336 && offsetof(Cpu, dcache) == 1024, "shared identity3");\n')
        # Wrapper lives title/include, so ../../ resolves to this immutable run's copied header.
        (include / "cpu.h").write_text(wrapper)
        profile["wrapper_sha256"] = sha(include / "cpu.h")
        for unit in profile["units"]:
            flags = ["-O2" if profile["title"] == "las" else "-O1", "-fPIC", "-fvisibility=hidden",
                     "-fsigned-char", "-fno-strict-aliasing", "-ffixed-x18",
                     "-Wno-return-type", "-Wno-unused-label", "-I", str(include)]
            if profile["title"] == "las":
                flags += ["-mllvm", "-dag-maps-huge-region=500"]
                marker = Path(unit["source"]).read_bytes()[:40]
                if b"lasgen: split-dispatch" in marker:
                    flags += ["-mllvm", "-disable-licm-promotion"]
                if b"lasgen: cold" in marker:
                    flags[0] = "-O0"
            tasks.append((profile, unit, flags))

    def compile_unit(task):
        profile, unit, flags = task
        source = Path(unit["source"])
        destination = output / profile["title"] / (source.stem + ".o")
        log_path = destination.with_suffix(".log")
        if sha(source) != unit["source_sha256"]:
            raise RuntimeError("Selected source changed after validation")
        command = [str(compiler), *flags, "-c", str(source), "-o", str(destination) + ".tmp"]
        record = {**unit, "title": profile["title"], "flags": flags, "command": command,
                  "log": str(log_path), "cpu_identity": 3, "common_header_sha256": common_hash}
        with log_path.open("wb") as log:
            try:
                process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=180)
                record["returncode"] = process.returncode
            except subprocess.TimeoutExpired:
                record["returncode"] = None
                record["timed_out_seconds"] = 180
        temporary = destination.with_name(destination.name + ".tmp")
        if record["returncode"] == 0:
            temporary.replace(destination)
            inventory = checked([toolchain / "llvm-nm", "--undefined-only", "--format=posix", destination],
                                capture_output=True, text=True).stdout
            inventory_path = destination.with_suffix(".undefined.txt")
            inventory_path.write_text(inventory)
            record.update(object_sha256=sha(destination), object_bytes=destination.stat().st_size,
                          undefined_inventory=str(inventory_path), undefined_inventory_sha256=sha(inventory_path),
                          undefined_symbol_count=len(inventory.splitlines()))
        else:
            temporary.unlink(missing_ok=True)
        if sha(source) != unit["source_sha256"]:
            raise RuntimeError("Selected source changed during compilation")
        return record

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in as_completed([pool.submit(compile_unit, task) for task in tasks]):
            record = future.result()
            result["objects"].append(record)
            atomic_json(manifest, result)
            print(f'{record["title"]}/{record["module"]} {record["kind"]}: ' +
                  ("compiled" if record["returncode"] == 0 else "failed; private log recorded"), flush=True)
    if sha(HEADER) != common_hash:
        raise RuntimeError("Shared header changed during probe; results not marked complete")
    result["complete"] = len(result["objects"]) == 12 and all(p["returncode"] == 0 for p in result["objects"])
    atomic_json(manifest, result)
    print(f'Compiled {sum(p["returncode"] == 0 for p in result["objects"])}/12 objects; private manifest {manifest.relative_to(ROOT)}')
    if not result["complete"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
