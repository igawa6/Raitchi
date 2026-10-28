#!/usr/bin/env python3
"""Private shared-AOT producer: validate inputs, freshly generate LAS, compile/link locally; no ADB/downloads."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

from probe_shared_aot_codegen import validate_las, validate_lg

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build/shared-gamelib"
NATIVE = ROOT / "build/shared-android"
POLICIES = {
    "las": {"id": 1, "input": "las", "source": "las-android", "work": "las-baseline", "shift": 0x6000000000, "caps": 1,
            "modules": [("main", 1, 0x7000000000), ("sdk", 2, 0x7004000000)]},
    "eevee": {"id": 2, "input": "letsgo", "source": "letsgo", "work": "letsgo-baseline", "shift": 0, "caps": 2,
              "modules": [("main", 1, 0x7000000000), ("sdk", 2, 0x7004000000),
                          ("subsdk0", 3, 0x7006000000), ("subsdk1", 4, 0x7008000000)]}}


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def atomic_bytes(path, content):
    if path.is_file() and path.read_bytes() == content:
        return
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def checked(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, **kwargs)


def originals(policy):
    imported = json.loads((ROOT / f'evidence/{policy["input"]}-inputs.json').read_text())["modules"]
    audited = json.loads((ROOT / "evidence/game-input-audit.json").read_text())["titles"][policy["input"]]["modules"]
    directory = ROOT / f'private/{policy["input"]}-baseline/extracted/exefs'
    result = {}
    for name, module_id, base in policy["modules"]:
        path = directory / name
        digest = sha(path)
        with path.open("rb") as stream:
            header = stream.read(0x100)
        if len(header) != 0x100 or header[:4] != b"NSO0":
            raise RuntimeError("Original input is not a complete NSO header")
        build_id = header[0x40:0x60].hex()
        text = struct.unpack_from("<III", header, 0x10)
        data = struct.unpack_from("<III", header, 0x30)
        bss = struct.unpack_from("<I", header, 0x3c)[0]
        mapped = (data[1] + data[2] + bss + 0x3fff) & ~0x3fff
        if (digest != imported[name]["sha256"] or digest != audited[name]["sha256"]
                or build_id != audited[name]["build_id_32_bytes"].lower()
                or not build_id.startswith(imported[name]["build_id"].lower())
                or base != audited[name]["canonical_base"] or mapped != audited[name]["runtime_image_bytes_16k_aligned"]
                or text[1] != audited[name]["segments"][0]["memory_offset"]
                or text[2] != audited[name]["segments"][0]["uncompressed_bytes"]):
            raise RuntimeError(f"Original {name} differs from independent import/audit records")
        if not text[2] or text[1] + text[2] > mapped:
            raise RuntimeError("Original NSO has an invalid text/image range")
        result[name] = {"path": str(path), "module_id": module_id, "canonical_base": base,
                        "sha256": digest, "build_id": build_id, "mapped_size": mapped,
                        "text_offset": text[1], "text_size": text[2]}
    ordered = list(result.values())
    for index, module in enumerate(ordered):
        upper = ordered[index + 1]["canonical_base"] if index + 1 < len(ordered) else 0x7320000000
        if module["canonical_base"] + module["mapped_size"] > upper:
            raise RuntimeError("Original module images overlap or leave the guest window")
    return result


def table_metadata(path, module, shift, name):
    text = path.read_text()
    match = re.search(r"const uint64_t " + name + r"_fn_addrs\[\]\s*=\s*\{(.*?)\};", text, re.S)
    pointers = re.search(r"const GuestFn " + name + r"_fn_ptrs\[\]\s*=\s*\{(.*?)\};", text, re.S)
    count = re.search(r"const uint64_t " + name + r"_fn_count\s*=\s*(\d+)\s*;", text)
    if not match or not pointers or not count:
        raise RuntimeError("Generated table format/count is missing")
    addresses = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)ull", match.group(1))]
    functions = re.findall(r"\bf_([0-9a-fA-F]+)\b", pointers.group(1))
    expected = int(count.group(1))
    lo = module["canonical_base"] - shift + module["text_offset"]
    hi = lo + module["text_size"]
    if (not expected or len(addresses) != expected or len(functions) != expected
            or any(a % 4 or not lo <= a < hi for a in addresses)
            or any(a >= b for a, b in zip(addresses, addresses[1:]))
            or any(int(f, 16) != address + shift for f, address in zip(functions, addresses))):
        raise RuntimeError("Generated table count/order/bounds/function-address correspondence failed")
    return expected


def las_generation(out, modules, generate):
    legacy = validate_las()  # Historical ID is only an options/source cross-check, never production unit provenance.
    work = ROOT / "work/las-baseline"
    pinned = ROOT / "sources/las-android"
    files = set(legacy["source_sha256"])
    files.update(str(p.relative_to(work)) for p in (work / "tools").glob("*.py"))
    source_hashes = {}
    for relative in sorted(files):
        if sha(work / relative) != sha(pinned / relative):
            raise RuntimeError("LAS generator input differs from the pinned reference")
        source_hashes[relative] = sha(pinned / relative)
    python = work / "build/android/venv/bin/python"
    packages = json.loads(checked([python, "-c", "import importlib.metadata,json; print(json.dumps({n:importlib.metadata.version(n) for n in ['capstone','unicorn','lz4']}))"],
                                  capture_output=True, text=True).stdout)
    if packages != {"capstone": "5.0.9", "unicorn": "2.1.4", "lz4": "4.4.5"}:
        raise RuntimeError("Installed generator packages differ from pinned versions")
    options = shlex.split(legacy["legacy_values"]["GENARGS"])
    identity = {"source_commit": legacy["source_commit"], "source_sha256": source_hashes,
                "generator_packages": packages, "generator_args": options, "simd": "asm", "guest_shift": 0x6000000000,
                "originals": modules}
    request = fingerprint(identity)
    directory = out / "generation" / request
    manifest = directory / "generation.json"
    generated = directory / "generated"
    if generate and not manifest.is_file():
        directory.mkdir(parents=True, exist_ok=True)
        snapshot = directory / "source"
        for relative in source_hashes:
            target = snapshot / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(pinned / relative, target)
        inputs = directory / "inputs"
        inputs.mkdir(exist_ok=True)
        for name, module in modules.items():
            shutil.copy2(module["path"], inputs / name)
            if sha(inputs / name) != module["sha256"]:
                raise RuntimeError("LAS original changed during private snapshot")
        # One generator process at a time; no baseline generation output is ever reused here.
        for name in ("sdk", "main"):
            command = [str(python), str(snapshot / "tools/gen.py"), name, str(inputs / name),
                       hex(modules[name]["canonical_base"]), str(generated / name), *options]
            if name == "sdk":
                command += ["--hle", str(snapshot / "runtime/hle/hle_symbols.txt")]
            print(f"Generating fresh LAS {name}; private generation log recorded", flush=True)
            with (directory / f"generate-{name}.log").open("w") as log:
                checked(command, cwd=snapshot / "tools", stdout=log, stderr=subprocess.STDOUT)
        generated_files = sorted(generated.glob("*/*.c"))
        counts = {name: table_metadata(generated / name / f"{name}_table.c", module, identity["guest_shift"], name)
                  for name, module in modules.items()}
        if not generated_files or any(sha(snapshot / name) != value for name, value in source_hashes.items()):
            raise RuntimeError("Incomplete generation or source changed while generating LAS")
        record = {"identity": identity, "fingerprint": request, "fresh_generation": True,
                  "generated_sha256": {str(p.relative_to(generated)): sha(p) for p in generated_files}, "counts": counts}
        atomic_json(manifest, record)
    if not manifest.is_file():
        return None
    record = json.loads(manifest.read_text())
    if record.get("identity") != identity or record.get("fingerprint") != request or not record.get("fresh_generation"):
        raise RuntimeError("Fresh LAS generation manifest identity mismatch")
    return generated, manifest, record["generated_sha256"], record["counts"], identity


def eevee_generation(modules):
    profile = validate_lg()
    manifest = Path(profile["generation_manifest"])
    record = json.loads(manifest.read_text())
    for name, module in modules.items():
        original = record["module_inputs"][name]
        if original["sha256"] != module["sha256"] or original["canonical_base"] != module["canonical_base"]:
            raise RuntimeError("Eevee generated input declaration differs from verified NSO")
    generated = manifest.parent / "generated"
    counts = {name: table_metadata(generated / name / f"{name}_table.c", module, 0, name) for name, module in modules.items()}
    return generated, manifest, record["generated_sha256"], counts, {"generation_fingerprint": record["fingerprint"],
            "source_commit": record["source_commit"], "source_sha256": record["source_sha256"], "simd": "asm", "guest_shift": 0}


def runtime_contract(required):
    path = NATIVE / "native-build.json"
    if not path.is_file():
        if required:
            raise RuntimeError("Common Android runtime has no completed native-build manifest yet")
        return None
    record = json.loads(path.read_text())
    library = NATIVE / "native/libmain.so"
    cpu = ROOT / "native/runtime/aot_cpu.h"
    abi = ROOT / "native/module_abi.h"
    contract = hashlib.sha256(b"shared-aot-v1\0" + cpu.read_bytes() + abi.read_bytes()).hexdigest()
    if (sha(library) != record["library_sha256"] or record["shared_contract_sha256"] != contract
            or record.get("target") != "aarch64-linux-android30"):
        raise RuntimeError("Common runtime binary/contract/target differs from current native manifest")
    return record


def prepare(args, out, policy):
    checked([sys.executable, ROOT / "tools/verify_sources.py"])
    modules = originals(policy)
    generation = las_generation(out, modules, args.generate_las) if args.title == "las" else eevee_generation(modules)
    if generation is None:
        print("LAS originals verified; use --generate-las for a fresh production generation manifest. No compilation.")
        if args.build:
            raise RuntimeError("Fresh LAS generation is required before compilation")
        return None
    generated, manifest, hashes, counts, generation_identity = generation
    paths = sorted(generated.glob("*/*.c"))
    if not hashes or set(hashes) != {str(p.relative_to(generated)) for p in paths}:
        raise RuntimeError("Complete generated C inventory changed")
    for path in paths:
        if sha(path) != hashes[str(path.relative_to(generated))]:
            raise RuntimeError("Generated C differs from complete generation manifest")
    for name, module in modules.items():
        module["function_count"] = table_metadata(generated / name / f"{name}_table.c", module, policy["shift"], name)
        if module["function_count"] != counts[name]:
            raise RuntimeError("Generated function count differs from generation manifest")
    include = out / "include"
    include.mkdir(exist_ok=True)
    header_hashes = {}
    for name, source in {"aot_cpu.h": ROOT / "native/runtime/aot_cpu.h", "module_abi.h": ROOT / "native/module_abi.h",
                         "simd.h": ROOT / f'work/{policy["work"]}/runtime/simd.h',
                         "simd_fp.h": ROOT / f'work/{policy["work"]}/runtime/simd_fp.h'}.items():
        atomic_bytes(include / name, source.read_bytes())
        header_hashes[name] = sha(include / name)
    wrapper = (f'#define AOT_GUEST_SHIFT {policy["shift"]}ull\n#define AOT_PROFILE_CAPABILITIES {policy["caps"]}u\n'
               '#include "aot_cpu.h"\n_Static_assert(sizeof(Cpu)==66560 && _Alignof(Cpu)==16,"shared CPU3");\n')
    atomic_bytes(include / "cpu.h", wrapper.encode())
    header_hashes["cpu.h"] = sha(include / "cpu.h")
    contract = hashlib.sha256(b"shared-aot-v1\0" + (include / "aot_cpu.h").read_bytes() + (include / "module_abi.h").read_bytes()).hexdigest()
    semantic = {"execution_abi": "shared-aot-v1", "contract": contract, "title": args.title, "title_id": policy["id"],
                "target": "aarch64-linux-android30", "cpu_identity": 3, "cpu_size": 66560, "simd": "asm",
                "capabilities": policy["caps"], "guest_shift": policy["shift"], "generation": generation_identity,
                "generation_manifest_sha256": sha(manifest), "headers": header_hashes,
                "modules": modules, "generated_sha256": hashes}
    semantic_hash = fingerprint(semantic)
    record = {"semantic_code_sha256": semantic_hash, "semantic": semantic, "generated_directory": str(generated),
              "generation_manifest": str(manifest), "all_generated_units_verified": len(paths),
              "runtime_ready": runtime_contract(args.build) is not None, "game_code_private": True, "device_executed": False}
    atomic_json(out / "prepare.json", record)
    return record, modules, paths, hashes, include, contract


def entry_source(modules, policy):
    lines = ['#include "cpu.h"', '#include "module_abi.h"']
    for name in modules:
        lines += [f'extern const uint64_t {name}_fn_addrs[], {name}_fn_count;', f'extern const GuestFn {name}_fn_ptrs[];']
    lines += ['static const ModuleAbiModule modules[] = {']
    def byte_array(digest):
        return "{" + ",".join("0x" + digest[i:i + 2] for i in range(0, len(digest), 2)) + "}"
    for name, m in modules.items():
        lines += ['{.struct_size=sizeof(ModuleAbiModule),.reserved=0,.image={.struct_size=sizeof(ModuleAbiModuleSpec),',
                  f'.module_id={m["module_id"]},.nso_build_id={byte_array(m["build_id"])},.nso_sha256={byte_array(m["sha256"])},',
                  f'.canonical_base={m["canonical_base"]}ull,.mapped_size={m["mapped_size"]}ull,.text_offset={m["text_offset"]}ull,',
                  f'.text_size={m["text_size"]}ull,.function_count={m["function_count"]}ull}},.address_count={m["function_count"]}ull,',
                  f'.addresses={name}_fn_addrs,.functions={name}_fn_ptrs}},']
    lines += ['};', '__attribute__((visibility("default"))) const ModuleAbiDescriptor recomp_module_descriptor_v1={',
              f'.struct_size=sizeof(ModuleAbiDescriptor),.module_count={len(modules)},.identity={{.struct_size=sizeof(ModuleAbiIdentity),',
              '.abi_version=MODULE_ABI_VERSION,.target=MODULE_ABI_TARGET_ANDROID_ARM64_LP64,.cpu_identity=MODULE_ABI_CPU_A64_SHARED_66560,',
              f'.cpu_size=sizeof(Cpu),.cpu_alignment=_Alignof(Cpu),.guest_shift={policy["shift"]}ull,',
              f'.guest_window_lo={0x7000000000-policy["shift"]}ull,.guest_window_hi={0x7320000000-policy["shift"]}ull}},.modules=modules}};']
    return ("\n".join(lines) + "\n").encode()


def wire_manifest(modules, policy, contract, code, library):
    size = 176 + 112 * len(modules)
    data = struct.pack("<8s12I3Q32s32s32s", b"SAOTM001", size, 1, 1, policy["id"], 1, 30, 3, 66560, 16, 1,
                       policy["caps"], len(modules), policy["shift"], 0x7000000000-policy["shift"],
                       0x7320000000-policy["shift"], bytes.fromhex(contract), bytes.fromhex(code), bytes.fromhex(library))
    for m in modules.values():
        data += struct.pack("<II32s32s5Q", m["module_id"], 0, bytes.fromhex(m["build_id"]), bytes.fromhex(m["sha256"]),
                            m["canonical_base"], m["mapped_size"], m["text_offset"], m["text_size"], m["function_count"])
    if len(data) != size:
        raise RuntimeError("Internal sidecar size mismatch")
    return data


def build(args, out, policy, prepared):
    record, modules, sources, hashes, include, contract = prepared
    native = runtime_contract(True)
    (out / "build.json").unlink(missing_ok=True)
    tc = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64"
    cc = tc / "bin/aarch64-linux-android30-clang"
    if "Pkg.Revision = 27.2.12479018" not in (args.ndk / "source.properties").read_text():
        raise RuntimeError("Expected pinned NDK r27c")
    def compiler_identity(path):
        return {"path": str(path), "driver_sha256": sha(path),
                "version": checked([path, "--version"], capture_output=True, text=True).stdout,
                "ndk_clang_sha256": sha((tc / "bin/clang").resolve()), "ndk_properties_sha256": sha(args.ndk / "source.properties")}
    compiler = compiler_identity(cc)
    alternate = None
    entry = out / "gamelib_entry.c"
    atomic_bytes(entry, entry_source(modules, policy))
    units = [(str(p.relative_to(Path(record["generated_directory"]))), p, hashes[str(p.relative_to(Path(record["generated_directory"])))]) for p in sources]
    units.append(("entry/gamelib_entry.c", entry, sha(entry)))
    # Preserve the known per-unit O1 mitigations in a NEW cache; never borrow reference objects.
    licm = ["-mllvm", "-disable-licm-promotion"]
    retry = [*licm, "-mllvm", "-dag-maps-huge-region=500"]
    alternate_flags = ["--target=aarch64-linux-android30", "--sysroot=" + str(tc / "sysroot"), *retry,
                       "-mllvm", "-opt-disable=loop-load-elim", "-mllvm", "-has-predecessor-max-steps=2048"]
    mitigation = {}
    if args.title == "eevee":
        alternate = compiler_identity(args.alternate_clang)
        mitigation = {"main/main_320.c": {"extra": licm},
                      "subsdk0/subsdk0_289.c": {"extra": alternate_flags, "alternate": True}}
    policy_file = out / "compile-policy.json"
    if policy_file.is_file():
        stored = json.loads(policy_file.read_text())
        for name, variant in stored.items():
            if name not in {p[0] for p in units} or variant.get("extra") not in (licm, retry, alternate_flags):
                raise RuntimeError("Unrecognized per-unit optimization policy")
            if variant.get("alternate") and variant["extra"] != alternate_flags:
                raise RuntimeError("Unexpected alternate compiler policy")
        mitigation.update(stored)
    atomic_json(policy_file, mitigation)
    objects, logs = out / "objects", out / "logs"
    objects.mkdir(exist_ok=True); logs.mkdir(exist_ok=True)
    stop, lock = threading.Event(), threading.Lock()
    children = set()
    progress = {"state": "compiling", "total": len(units), "completed": 0, "compiled": 0, "reused": 0, "jobs": args.jobs, "errors": []}
    atomic_json(out / "progress.json", progress)

    def compile_unit(unit):
        relative, source, digest = unit
        if stop.is_set():
            return None
        flags = ["-O2" if args.title == "las" else "-O1", "-fPIC", "-fvisibility=hidden", "-fsigned-char",
                 "-fno-strict-aliasing", "-ffixed-x18", "-Wno-return-type", "-Wno-unused-label", "-I", str(include)]
        if args.title == "las":
            flags += ["-mllvm", "-dag-maps-huge-region=500"]
            marker = source.read_bytes()[:40]
            if b"lasgen: split-dispatch" in marker:
                flags += licm
            if b"lasgen: cold" in marker:
                flags[0] = "-O0"
        variant = mitigation.get(relative, {})
        extra = variant.get("extra", [])
        selected = args.alternate_clang if variant.get("alternate") else cc
        selected_identity = alternate if variant.get("alternate") else compiler
        log_path = logs / (relative.replace("/", "_") + ".log")
        for attempt in range(2):
            spec = {"unit": relative, "source_sha256": digest, "headers": record["semantic"]["headers"],
                    "compiler": selected_identity, "flags": flags, "extra_flags": extra}
            key = fingerprint(spec)
            obj, metadata = objects / (key + ".o"), objects / (key + ".json")
            if obj.is_file() and metadata.is_file():
                cached = json.loads(metadata.read_text())
                if (cached.get("key") == key and cached.get("object_sha256") == sha(obj)
                        and cached.get("dependencies") and all(Path(p).is_file() and sha(Path(p)) == h for p, h in cached["dependencies"].items())):
                    return {"unit": relative, "object": str(obj), "key": key, "cached": True, "spec": spec}
            temporary, depfile = objects / (key + ".o.tmp"), objects / (key + ".d.tmp")
            argv = [str(selected), *flags, *extra, "-MD", "-MF", str(depfile), "-c", str(source), "-o", str(temporary)]
            timed_out = False
            with log_path.open("w" if attempt == 0 else "a") as log:
                log.write(json.dumps(argv) + "\n"); log.flush()
                with subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True) as child:
                    with lock:
                        children.add(child.pid)
                    try:
                        status = child.wait(timeout=args.retry_timeout if extra else args.unit_timeout)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL); child.wait(); timed_out = True
                    finally:
                        with lock:
                            children.discard(child.pid)
            if timed_out:
                temporary.unlink(missing_ok=True); depfile.unlink(missing_ok=True)
                if extra or attempt:
                    raise RuntimeError(f"Targeted compile timed out; private log {log_path.name}")
                extra = retry
                with lock:
                    mitigation[relative] = {"extra": retry}
                    atomic_json(policy_file, mitigation)
                continue
            if status:
                raise RuntimeError(f"Compile failed; private log {log_path.name}")
            if sha(source) != digest:
                raise RuntimeError("Generated source changed during compilation")
            dependencies = shlex.split(depfile.read_text().replace("\\\n", " ").split(":", 1)[1])
            dependencies = {str(Path(p).resolve()): sha(Path(p)) for p in dependencies}
            temporary.replace(obj)
            atomic_json(metadata, {"key": key, "spec": spec, "dependencies": dependencies, "object_sha256": sha(obj)})
            depfile.unlink(missing_ok=True)
            return {"unit": relative, "object": str(obj), "key": key, "cached": False, "spec": spec}
        raise RuntimeError("No compile result")

    records = []
    pool = ThreadPoolExecutor(max_workers=args.jobs)
    futures = [pool.submit(compile_unit, unit) for unit in sorted(units, key=lambda p: p[1].stat().st_size, reverse=True)]
    try:
        for future in as_completed(futures):
            item = future.result()
            if item is None:
                continue
            records.append(item)
            progress["completed"] += 1
            progress["reused" if item["cached"] else "compiled"] += 1
            atomic_json(out / "progress.json", progress)
            if progress["completed"] % 25 == 0:
                print(f'Objects {progress["completed"]}/{progress["total"]}; compiled {progress["compiled"]}, reused {progress["reused"]}', flush=True)
    except BaseException as error:
        stop.set()
        with lock:
            for pid in children:
                try: os.killpg(pid, signal.SIGKILL)
                except ProcessLookupError: pass
        for future in futures:
            future.cancel()
        progress.update(state="failed", errors=[str(error)])
        atomic_json(out / "progress.json", progress)
        raise
    finally:
        pool.shutdown(wait=True, cancel_futures=True)
    if len(records) != len(units):
        raise RuntimeError("Incomplete object set")
    current = runtime_contract(True)
    if current["shared_contract_sha256"] != contract:
        raise RuntimeError("Runtime execution contract changed before link")
    progress["state"] = "linking"; atomic_json(out / "progress.json", progress)
    response = out / "objects.rsp"
    response.write_text("\n".join(json.dumps(p["object"]) for p in sorted(records, key=lambda p: p["unit"])) + "\n")
    version_script = out / "exports.map"
    atomic_bytes(version_script, b"{ global: recomp_module_descriptor_v1; local: *; };\n")
    temporary = out / "liblasgame.so.tmp"
    flags = ["-shared", "-fuse-ld=lld", "-Wl,--no-undefined", "-Wl,-Bsymbolic", "-Wl,-z,max-page-size=16384",
             "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-soname,liblasgame.so", "-Wl,--version-script=" + str(version_script)]
    command = [str(cc), *flags, "-o", str(temporary), "@" + str(response), "-L" + str(NATIVE / "native"),
               "-lmain", "-landroid", "-llog", "-ldl", "-lm"]
    with (out / "link.log").open("w") as log:
        checked(command, stdout=log, stderr=subprocess.STDOUT)
    elf = checked([tc / "bin/llvm-readelf", "--file-header", "--dynamic", "--program-headers", "--dyn-syms", "--wide", temporary],
                  capture_output=True, text=True).stdout
    (out / "library-elf.txt").write_text(elf)
    exports = []
    for line in elf.splitlines():
        parts = line.split()
        if len(parts) >= 8 and re.fullmatch(r"\d+:", parts[0]) and parts[4] in ("GLOBAL", "WEAK") and parts[5] == "DEFAULT" and parts[6] != "UND":
            exports.append((parts[7], parts[3]))
    loads = [line.split() for line in elf.splitlines() if line.lstrip().startswith("LOAD ")]
    needed = re.findall(r"NEEDED.*\[([^\]]+)\]", elf)
    if (exports != [("recomp_module_descriptor_v1", "OBJECT")] or "AArch64" not in elf or "libmain.so" not in needed
            or not re.search(r"SONAME.*\[liblasgame.so\]", elf) or not loads or any(int(p[-1], 16) < 16384 for p in loads)):
        raise RuntimeError("Shared producer ELF export/dependency/architecture/alignment check failed")
    if runtime_contract(True)["library_sha256"] != current["library_sha256"]:
        raise RuntimeError("Common runtime changed during link")
    library_hash = sha(temporary)
    sidecar = wire_manifest(modules, policy, contract, record["semantic_code_sha256"], library_hash)
    temporary.replace(out / "liblasgame.so")
    atomic_bytes(out / "liblasgame.so.saotm", sidecar)
    atomic_json(out / "build.json", {**record, "runtime_sha256": current["library_sha256"], "library_sha256": library_hash,
                "sidecar_sha256": hashlib.sha256(sidecar).hexdigest(), "sidecar_bytes": len(sidecar),
                "objects": records, "per_unit_policy": mitigation, "link_command": command,
                "dynamic_exports": exports, "needed": needed, "built_utc": datetime.now(timezone.utc).isoformat()})
    progress["state"] = "complete"; atomic_json(out / "progress.json", progress)
    print("Published private common-CPU title library, immutable descriptor and hash-bound binary sidecar; no device actions.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title", choices=("las", "eevee"), default="las")
    parser.add_argument("--generate-las", action="store_true", help="Fresh serial generation into a new ignored namespace")
    parser.add_argument("--build", action="store_true", help="Compile/link against the completed common Android runtime")
    parser.add_argument("--jobs", type=int, choices=(1, 2, 3, 4), default=4)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    parser.add_argument("--alternate-clang", type=Path, default=Path("/usr/bin/clang"))
    parser.add_argument("--unit-timeout", type=int, default=120)
    parser.add_argument("--retry-timeout", type=int, default=180)
    args = parser.parse_args()
    if args.generate_las and args.title != "las":
        parser.error("--generate-las requires --title las")
    if min(args.unit_timeout, args.retry_timeout) <= 0:
        parser.error("Compile timeouts must be positive")
    out = BASE / args.title
    out.mkdir(parents=True, exist_ok=True)
    with (out / "build.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        prepared = prepare(args, out, POLICIES[args.title])
        if args.build and prepared:
            build(args, out, POLICIES[args.title], prepared)
        elif prepared:
            print(f'Verified {prepared[0]["all_generated_units_verified"]} complete generated units and original NSO identities; no compilation.')


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Shared producer error: {error}", file=sys.stderr)
        raise SystemExit(1)
