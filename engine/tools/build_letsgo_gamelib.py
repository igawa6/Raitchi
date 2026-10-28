#!/usr/bin/env python3
"""Validate and optionally build the private four-module Let's Go Android AOT library.

Never downloads, installs, packages an APK, or contacts a device. All generated
code, objects, diagnostics and provenance remain in ignored local directories.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "work/letsgo-android"
CODEGEN = ROOT / "work/letsgo-baseline"
GENERATION = CODEGEN / "build/android-codegen/generation.json"
GENERATED = GENERATION.parent / "generated"
INPUTS = ROOT / "private/letsgo-baseline/extracted/exefs"
NATIVE = ROOT / "build/letsgo-android"
OUT = ROOT / "build/letsgo-gamelib"
MODULES = {"main": 0x7000000000, "sdk": 0x7004000000,
           "subsdk0": 0x7006000000, "subsdk1": 0x7008000000}
FLAGS = ["-O1", "-fPIC", "-fvisibility=hidden", "-fsigned-char", "-fno-strict-aliasing",
         "-Wno-return-type", "-Wno-unused-label"]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    os.replace(temporary, path)


def checked(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, **kwargs)


def native_contract():
    manifest = json.loads((NATIVE / "native-build.json").read_text())
    paths = [*sorted((WORK / "tools").glob("*.py")), *sorted((WORK / "runtime").glob("*.h")),
             WORK / "runtime/hle/hle_symbols.txt", WORK / "tools/gamelib_entry.c"]
    identity = {"source": manifest["source_commit"], "simd": "asm", "shift": 0, "abi": 3,
                "target": "aarch64-linux-android30", "cpu_size": 1024,
                "files": {str(p.relative_to(WORK)): sha(p) for p in paths}}
    code_id = manifest["game_code_identity"]
    if (not re.fullmatch(r"[0-9a-f]{16}", code_id)
            or fingerprint(identity)[:16] != code_id
            or manifest.get("guest_shift") != 0
            or manifest.get("target") != identity["target"]):
        raise RuntimeError("Native runtime source/options/code identity mismatch; rebuild its manifest first")
    library = NATIVE / "native/libmain.so"
    if sha(library) != manifest["library_sha256"]:
        raise RuntimeError("Native libmain.so does not match its build manifest")
    return manifest, identity


def validate():
    generation = json.loads(GENERATION.read_text())
    initial = {k: v for k, v in generation.items() if k not in ("fingerprint", "generated_sha256")}
    if fingerprint(initial) != generation["fingerprint"]:
        raise RuntimeError("Generation fingerprint is inconsistent")
    expected = {"target": "aarch64-linux-android30", "simd": "asm", "guest_shift": 0,
                "cpu_size": 1024, "upstream_gamelib_abi": 3}
    if any(generation.get(k) != v for k, v in expected.items()):
        raise RuntimeError("Generation is not the unshifted ARM64 SIMD-asm ABI3 profile")
    for relative, digest in generation["source_sha256"].items():
        if sha(CODEGEN / relative) != digest:
            raise RuntimeError(f"Codegen source changed since generation: {relative}")
        # Platform/gamelib headers are deliberately adapted in Android. Generated
        # code relies on the identical generator and CPU/SIMD headers below.
        compare = relative.startswith("tools/") or relative in (
            "runtime/cpu.h", "runtime/simd.h", "runtime/simd_fp.h", "runtime/hle/hle_symbols.txt")
        if compare and sha(WORK / relative) != digest:
            raise RuntimeError(f"Android/codegen source disagreement: {relative}")
    imported = json.loads((ROOT / "evidence/letsgo-inputs.json").read_text())["modules"]
    audited = json.loads((ROOT / "evidence/game-input-audit.json").read_text())["titles"]["letsgo"]["modules"]
    module_records = {}
    if set(generation["module_inputs"]) != set(MODULES):
        raise RuntimeError("Generation does not declare exactly four required modules")
    for name, base in MODULES.items():
        path = INPUTS / name
        digest = sha(path)
        with path.open("rb") as stream:
            header = stream.read(0x60)
        build_id = header[0x40:0x60].hex().upper()
        recorded = generation["module_inputs"][name]
        if (len(header) != 0x60 or header[:4] != b"NSO0" or recorded["canonical_base"] != base
                or digest != recorded["sha256"] or digest != imported[name]["sha256"]
                or digest != audited[name]["sha256"]
                or build_id != audited[name]["build_id_32_bytes"].upper()
                or not build_id.startswith(imported[name]["build_id"].upper())):
            raise RuntimeError(f"Private NSO hash/full build-ID/profile mismatch: {name}")
        module_records[name] = {"canonical_base": base, "sha256": digest,
                                "build_id_32_bytes": build_id, "bytes": path.stat().st_size}
    recorded = generation["generated_sha256"]
    files = sorted(GENERATED.glob("*/*.c"))
    if not recorded or set(recorded) != {str(p.relative_to(GENERATED)) for p in files}:
        raise RuntimeError("Generated C file inventory changed")
    for path in files:
        relative = path.relative_to(GENERATED)
        if relative.parts[0] not in MODULES or sha(path) != recorded[str(relative)]:
            raise RuntimeError(f"Generated file hash/profile mismatch: {relative}")
    for name in MODULES:
        if f"{name}/{name}_table.c" not in recorded:
            raise RuntimeError(f"Generated module table missing: {name}")
    native, identity = native_contract()
    if generation["source_commit"] != native["source_commit"]:
        raise RuntimeError("Native runtime and generation commits differ")
    return generation, module_records, files, native, identity


def verify_elf(toolchain, library):
    report = checked([toolchain / "llvm-readelf", "--file-header", "--program-headers", "--dynamic",
                      "--dyn-syms", "--wide", library], capture_output=True, text=True).stdout
    (OUT / "library-elf.txt").write_text(report)
    if "AArch64" not in report or not re.search(r"Type:\s+DYN", report):
        raise RuntimeError("Linked output is not an AArch64 shared object")
    if not re.search(r"SONAME.*\[liblasgame.so\]", report):
        raise RuntimeError("Game library SONAME is incorrect")
    needed = re.findall(r"NEEDED.*\[([^\]]+)\]", report)
    if "libmain.so" not in needed:
        raise RuntimeError("Game library has no runtime DT_NEEDED dependency")
    loads = [line.split() for line in report.splitlines() if line.lstrip().startswith("LOAD ")]
    if not loads or any(int(parts[-1], 16) < 16384 for parts in loads):
        raise RuntimeError("Game library PT_LOAD alignment is below 16 KiB")
    exports = []
    for line in report.splitlines():
        parts = line.split()
        if (len(parts) >= 8 and re.fullmatch(r"\d+:", parts[0])
                and parts[4] in ("GLOBAL", "WEAK") and parts[5] == "DEFAULT" and parts[6] != "UND"):
            exports.append(parts[7])
    if "lasgame_tables" not in exports or any(name.startswith("f_70") for name in exports):
        raise RuntimeError("Game entry missing or generated functions escaped hidden visibility")
    return {"needed": needed, "defined_dynamic_exports": exports, "pt_load_alignment_min":
            min(int(parts[-1], 16) for parts in loads)}


def build(args, validation):
    generation, modules, files, native, identity = validation
    toolchain = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    cc = toolchain / "aarch64-linux-android30-clang"
    compiler = {"path": str(cc), "sha256": sha(cc.resolve()),
                "version": checked([cc, "--version"], capture_output=True, text=True).stdout,
                "ndk_properties_sha256": sha(args.ndk / "source.properties")}
    alternate_compiler = None
    flags = [*FLAGS, "-I", str(WORK / "runtime"), f'-DGAMELIB_ID="{native["game_code_identity"]}"']
    abi = OUT / "abi-check.h"
    abi.write_text('#include "gamelib.h"\n_Static_assert(sizeof(Cpu) == 1024, "Cpu ABI");\n'
                   '_Static_assert(_Alignof(Cpu) == 16, "Cpu alignment");\n'
                   '_Static_assert(GAMELIB_ABI == 3 && GAMELIB_MAX_MODULES == 4, "module ABI");\n'
                   '_Static_assert(sizeof(GameLibTables) == 152, "table ABI");\n')
    common = {"schema": 1, "compiler": compiler, "flags": flags,
              "headers": {name: identity["files"][name] for name in (
                  "runtime/cpu.h", "runtime/simd.h", "runtime/simd_fp.h", "runtime/gamelib.h")}}
    units = [(str(p.relative_to(GENERATED)), p, generation["generated_sha256"][str(p.relative_to(GENERATED))])
             for p in files]
    entry = WORK / "tools/gamelib_entry.c"
    units.append(("entry/gamelib_entry.c", entry, sha(entry)))
    units.sort(key=lambda unit: unit[1].stat().st_size, reverse=True)
    objects, logs = OUT / "objects", OUT / "logs"
    objects.mkdir(exist_ok=True)
    logs.mkdir(exist_ok=True)
    stop = threading.Event()
    policy_path = OUT / "compile-policy.json"
    policy = json.loads(policy_path.read_text()) if policy_path.is_file() else {"schema": 1, "units": {}}
    policy_lock = threading.Lock()
    licm_flags = ["-mllvm", "-disable-licm-promotion"]
    retry_flags = [*licm_flags, "-mllvm", "-dag-maps-huge-region=500"]
    alternate_flags = ["--target=aarch64-linux-android30", "--sysroot=" + str(toolchain.parent / "sysroot"),
                       *retry_flags, "-mllvm", "-opt-disable=loop-load-elim",
                       "-mllvm", "-has-predecessor-max-steps=2048"]
    if not policy_path.is_file() and any(unit[0] == "subsdk0/subsdk0_289.c" for unit in units):
        # This upstream unit's huge PC switch has a measured Clang18 optimizer
        # pathology. Keep O1 and use the locally verified Android-target compiler
        # variant only for that unit; standard cached objects still win below.
        policy["units"]["subsdk0/subsdk0_289.c"] = {"compiler": "host-clang", "extra_flags": alternate_flags,
            "reason": "Known huge PC-switch Clang18 pathology; isolated Clang23 Android-target O1 probe passed"}
        atomic_json(policy_path, policy)
    if policy.get("schema") != 1 or any(
            not ((record.get("compiler") is None and record.get("extra_flags") in (licm_flags, retry_flags))
                 or (record.get("compiler") == "host-clang" and record.get("extra_flags") == alternate_flags))
            for record in policy.get("units", {}).values()):
        raise RuntimeError("Unsupported private per-unit compile policy")
    if any(record.get("compiler") == "host-clang" for record in policy["units"].values()):
        alternate_compiler = {"path": str(args.alternate_clang), "sha256": sha(args.alternate_clang.resolve()),
            "version": checked([args.alternate_clang, "--version"], capture_output=True, text=True).stdout,
            "ndk_properties_sha256": sha(args.ndk / "source.properties")}

    def cached_object(spec):
        key = fingerprint(spec)
        obj, meta = objects / f"{key}.o", objects / f"{key}.json"
        if obj.is_file() and meta.is_file():
            previous = json.loads(meta.read_text())
            if (previous.get("key") == key and sha(obj) == previous.get("object_sha256")
                    and previous.get("dependencies") and all(
                        Path(p).is_file() and sha(Path(p)) == value
                        for p, value in previous["dependencies"].items())):
                return {"unit": spec["unit"], "key": key, "object": str(obj), "cached": True,
                        "extra_flags": spec["extra_flags"], "compiler": spec["compiler"]}
        return None

    def compile_one(unit):
        if stop.is_set():
            return None
        relative, source, digest = unit
        entry_flags = ["-include", str(abi)] if source == entry else []
        base = {**common, "unit": relative, "source_sha256": digest, "extra_flags": entry_flags,
                "abi_check_sha256": sha(abi) if entry_flags else None}
        # Keep all original standard-object keys valid, including an expensive
        # unit that already completed normally before a mitigation was selected.
        existing = cached_object(base)
        if existing:
            return existing
        with policy_lock:
            unit_policy = policy.get("units", {}).get(relative, {})
            extra = unit_policy.get("extra_flags", [])
        spec = {**base, "extra_flags": [*entry_flags, *extra]}
        selected_cc = cc
        if unit_policy.get("compiler") == "host-clang":
            spec["compiler"] = alternate_compiler
            selected_cc = args.alternate_clang
        existing = cached_object(spec) if extra else None
        if existing:
            return existing
        log = logs / (relative.replace("/", "_") + ".log")
        timed_out = False
        elapsed_start = time.monotonic()
        for attempt in range(2):
            key = fingerprint(spec)
            obj, meta = objects / f"{key}.o", objects / f"{key}.json"
            temporary, depfile = objects / f"{key}.o.tmp", objects / f"{key}.d.tmp"
            timeout = args.retry_timeout if extra else args.unit_timeout
            with log.open("w" if attempt == 0 else "a") as stream:
                if attempt:
                    stream.write("\nRetrying only this unit with LICM promotion disabled and bounded scheduler memory chains.\n")
                    stream.flush()
                try:
                    # The NDK target driver is a shell wrapper that forks clang.
                    # Own a fresh group so timeout kills both, not only the shell.
                    with subprocess.Popen([str(selected_cc), *flags, *spec["extra_flags"], "-MD", "-MF", str(depfile),
                                           "-c", str(source), "-o", str(temporary)], cwd=WORK,
                                          stdout=stream, stderr=subprocess.STDOUT, start_new_session=True) as process:
                        try:
                            status = process.wait(timeout=timeout)
                        except subprocess.TimeoutExpired:
                            try:
                                os.killpg(process.pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                            process.wait()
                            raise
                    result = subprocess.CompletedProcess(process.args, status)
                    break
                except subprocess.TimeoutExpired:
                    temporary.unlink(missing_ok=True)
                    depfile.unlink(missing_ok=True)
                    if extra:
                        stop.set()
                        raise RuntimeError(f"Bounded targeted retry timed out: {relative}; inspect {log.name}") from None
                    timed_out = True
                    extra = retry_flags
                    spec = {**base, "extra_flags": [*entry_flags, *extra]}
                    with policy_lock:
                        policy["units"][relative] = {"extra_flags": retry_flags,
                            "reason": "standard compile exceeded bounded unit timeout",
                            "standard_timeout_seconds": args.unit_timeout}
                        atomic_json(policy_path, policy)
                    print(f"Unit timeout: {relative}; bounded LICM/scheduler retry, standard cache preserved.", flush=True)
                    existing = cached_object(spec)
                    if existing:
                        return existing
        if result.returncode:
            stop.set()
            raise RuntimeError(f"Compile failed: {relative}; inspect build/letsgo-gamelib/logs/{log.name}")
        dependencies = shlex.split(depfile.read_text().replace("\\\n", " ").split(":", 1)[1])
        hashes = {str(Path(p).resolve() if Path(p).is_absolute() else (WORK / p).resolve()):
                  sha(Path(p) if Path(p).is_absolute() else WORK / p) for p in dependencies}
        if sha(source) != digest:
            stop.set()
            raise RuntimeError(f"Source changed while compiling: {relative}")
        os.replace(temporary, obj)
        atomic_json(meta, {"key": key, "spec": spec, "dependencies": hashes, "object_sha256": sha(obj),
                           "elapsed_seconds": round(time.monotonic() - elapsed_start, 2), "timeout_retry": timed_out})
        depfile.unlink(missing_ok=True)
        return {"unit": relative, "key": key, "object": str(obj), "cached": False,
                "extra_flags": spec["extra_flags"], "compiler": spec["compiler"], "timeout_retry": timed_out}

    progress = {"total": len(units), "completed": 0, "compiled": 0, "reused": 0,
                "errors": [], "state": "compiling", "jobs": args.jobs, "timeout_retries": 0}
    atomic_json(OUT / "progress.json", progress)
    records, started, last_report = [], time.monotonic(), time.monotonic()
    pool = ThreadPoolExecutor(max_workers=args.jobs)
    futures = [pool.submit(compile_one, unit) for unit in units]
    try:
        for future in as_completed(futures):
            record = future.result()
            if record is None:
                continue
            records.append(record)
            progress["completed"] += 1
            progress["reused" if record["cached"] else "compiled"] += 1
            progress["timeout_retries"] += int(record.get("timeout_retry", False))
            atomic_json(OUT / "progress.json", progress)
            if progress["completed"] % 25 == 0 or time.monotonic() - last_report >= 30:
                print(f"Objects {progress['completed']}/{len(units)}; compiled {progress['compiled']}, "
                      f"reused {progress['reused']}; elapsed {time.monotonic() - started:.0f}s", flush=True)
                last_report = time.monotonic()
    except BaseException as error:
        stop.set()
        for future in futures:
            future.cancel()
        progress["state"], progress["errors"] = "failed", [str(error)]
        atomic_json(OUT / "progress.json", progress)
        raise
    finally:
        pool.shutdown(wait=True, cancel_futures=True)
    if len(records) != len(units):
        raise RuntimeError("Incomplete object set; refusing link")
    # Renderer-only rebuilds can replace libmain during the compile. Revalidate
    # its latest manifest and unchanged generated-code contract before linking.
    latest, latest_identity = native_contract()
    if latest_identity != identity or latest["game_code_identity"] != native["game_code_identity"]:
        raise RuntimeError("Runtime code contract changed during build; rerun to revalidate")
    progress["state"] = "linking"
    atomic_json(OUT / "progress.json", progress)
    response = OUT / "objects.rsp"
    response.write_text("\n".join(json.dumps(record["object"]) for record in sorted(records, key=lambda r: r["unit"])) + "\n")
    link_flags = ["-shared", "-fuse-ld=lld", "-Wl,--no-undefined", "-Wl,-soname,liblasgame.so",
                  "-Wl,-z,max-page-size=16384", "-Wl,-Bsymbolic"]
    temporary = OUT / "liblasgame.so.tmp"
    with (OUT / "link.log").open("w") as stream:
        result = subprocess.run([str(cc), *link_flags, "-o", str(temporary), "@" + str(response),
                                 "-L" + str(NATIVE / "native"), "-lmain", "-landroid", "-llog", "-ldl", "-lm"],
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        progress["state"], progress["errors"] = "failed", ["Link failed; inspect build/letsgo-gamelib/link.log"]
        atomic_json(OUT / "progress.json", progress)
        raise RuntimeError(progress["errors"][0])
    elf = verify_elf(toolchain, temporary)
    if sha(NATIVE / "native/libmain.so") != latest["library_sha256"]:
        raise RuntimeError("Runtime changed during link; rerun before publishing")
    library = OUT / "liblasgame.so"
    os.replace(temporary, library)
    sidecar = OUT / "liblasgame.so.id.tmp"
    sidecar.write_text(latest["game_code_identity"] + "\n")
    os.replace(sidecar, OUT / "liblasgame.so.id")
    manifest = {"schema": 1, "private_game_code": True, "device_executed": False,
                "source_commit": generation["source_commit"], "generation_fingerprint": generation["fingerprint"],
                "game_code_identity": latest["game_code_identity"], "target": generation["target"],
                "guest_shift": 0, "cpu_size": 1024, "gamelib_abi": 3, "modules": modules,
                "runtime_sha256": latest["library_sha256"], "compiler": compiler,
                "compile_flags": flags, "per_unit_policy": policy,
                "unit_timeout_seconds": args.unit_timeout, "retry_timeout_seconds": args.retry_timeout,
                "link_flags": link_flags, "objects": records,
                "generated_units": len(files), "library_sha256": sha(library),
                "library_bytes": library.stat().st_size, "elf": elf,
                "elapsed_seconds": round(time.monotonic() - started, 2)}
    atomic_json(OUT / "build.json", manifest)
    progress["state"] = "complete"
    atomic_json(OUT / "progress.json", progress)
    print(f"Linked private ABI3 four-module liblasgame.so: {len(records)} objects, "
          f"{library.stat().st_size} bytes; no APK or device action.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Compile/link all private units; default validates only")
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=4)
    parser.add_argument("--unit-timeout", type=int, default=120, help="Seconds before a standard unit gets a targeted LICM retry")
    parser.add_argument("--retry-timeout", type=int, default=180, help="Maximum seconds for a LICM-disabled unit")
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    parser.add_argument("--alternate-clang", type=Path, default=Path("/usr/bin/clang"),
                        help="Used only by an explicitly selected private per-unit host-clang policy")
    args = parser.parse_args()
    if args.unit_timeout <= 0 or args.retry_timeout <= 0:
        parser.error("Compile timeouts must be positive")
    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / "build.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another private game-library build holds the lock") from None
        validation = validate()
        print(f"Validated four NSOs/full build IDs, generation and runtime contract; "
              f"{len(validation[2])} generated C units plus fresh ABI3 entry.", flush=True)
        if args.build:
            build(args, validation)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Game-library build error: {error}", file=sys.stderr)
        sys.exit(1)
