#!/usr/bin/env python3
"""Portable private shared-AOT producer for Android/Linux (Python 3.10+).

Consumes a trusted, independently packaged shared-build-kit.json and the owner's
original extracted NSOs. Generates through the pinned upstream Python tools,
compiles real AArch64 objects, links against the same common libmain.so, checks
ELF/CPU3/sidecar contracts, and publishes an immutable pair via output/current.
No downloads, installation, runtime selection, save access, or guest execution.
Toolkit metadata is trusted packaging input, not an untrusted-code sandbox.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
import fcntl
import hashlib
import json
import mmap
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
import uuid

POLICIES = {
    "las": {"id": 1, "shift": 0x6000000000, "caps": 1,
            "program_id": 0x01006BB00C6F0000,
            "main_prefix": "ae16f71e002af8cb059a9a74c4d90f34ba984892",
            "commit": "cca791a3e7218e1dac95e7d4a11b8f97dba3ea51",
            "args": ["--simd", "asm", "--shift", "0x6000000000", "--split-dispatch", "512", "--hot", "hot_functions.txt"],
            "modules": [("main", 1, 0x7000000000), ("sdk", 2, 0x7004000000)]},
    "eevee": {"id": 2, "shift": 0, "caps": 2,
              "program_id": 0x0100187003A36000, "main_prefix": "0a3f0d21e67dad80",
              "commit": "96894fd8e9ea20c1b38cdea53105ec2a335df620",
              "args": ["--simd", "asm"],
              "modules": [("main", 1, 0x7000000000), ("sdk", 2, 0x7004000000),
                          ("subsdk0", 3, 0x7006000000), ("subsdk1", 4, 0x7008000000)]},
}
PACKAGES = {"capstone": "5.0.9", "unicorn": "2.1.4", "lz4": "4.4.5"}
# Capstone's pinned 5.0.9 distribution intentionally reports binding 5.0.7.
# Android trims dist-info; payload SHA256 pins the distribution, imports check
# the actual native bindings rather than requiring absent package metadata.
BINDINGS = {"capstone": "5.0.7", "unicorn": "2.1.4", "lz4": "4.4.5"}

class BuildError(RuntimeError):
    pass

class Cancelled(BuildError):
    pass


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def atomic_bytes(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
    try:
        with temporary.open("xb") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_json(path, value):
    atomic_bytes(path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode())


# These three functions preserve the existing verified host producer's table,
# immutable DATA descriptor, and SAOTM001 wire encoding exactly.
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


def checked_digest(value, label):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value) or value == "0" * 64:
        raise BuildError(f"Invalid {label} digest")
    return value


def within(path, root):
    return path == root or root in path.parents


def kit_path(root, relative):
    if not isinstance(relative, str) or not relative or "\\" in relative:
        raise BuildError("Invalid toolkit relative path")
    part = Path(relative)
    if part.is_absolute() or any(x in (".", "..") for x in part.parts):
        raise BuildError("Toolkit path escapes its root")
    result = root / part
    if not within(result.resolve(), root.resolve()):
        raise BuildError("Toolkit symlink escapes its root")
    return result


class Progress:
    def __init__(self, path, title):
        self.path = path
        self.value = {"schema": 1, "title": title, "state": "checking", "completed": 0,
                      "total": 0, "compiled": 0, "reused": 0}
        self.lock = threading.Lock()

    def update(self, **values):
        with self.lock:
            self.value.update(values)
            self.value["updated_unix"] = time.time()
            atomic_json(self.path, self.value)


class Runner:
    def __init__(self, cancel_file=None):
        self.cancel_file = cancel_file
        self.stop = threading.Event()
        self.children = set()
        self.lock = threading.Lock()

    def check(self):
        if self.stop.is_set() or (self.cancel_file and self.cancel_file.exists()):
            self.stop.set()
            raise Cancelled("Build cancelled; completed objects remain resumable")

    @staticmethod
    def terminate(child):
        if child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        child.wait()

    def close(self):
        self.stop.set()
        with self.lock:
            for child in self.children:
                self.terminate(child)

    def run(self, argv, log, *, cwd=None, env=None, timeout=0, tick=None):
        self.check()
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w") as stream:
            stream.write(json.dumps([str(x) for x in argv]) + "\n")
            stream.flush()
            child = subprocess.Popen([str(x) for x in argv], cwd=cwd, env=env,
                                     stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            with self.lock:
                self.children.add(child)
            start, last = time.monotonic(), 0.0
            try:
                while child.poll() is None:
                    self.check()
                    now = time.monotonic()
                    if timeout and now - start > timeout:
                        raise BuildError(f"Helper timed out; see {log.name}")
                    if tick and now - last >= 1:
                        tick(log)
                        last = now
                    time.sleep(0.1)
                self.check()
                if child.returncode:
                    raise BuildError(f"Helper exited {child.returncode}; see {log.name}")
            except BaseException:
                self.terminate(child)
                raise
            finally:
                with self.lock:
                    self.children.discard(child)
        return log.read_text(errors="replace")


@contextmanager
def build_lock(directory):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "build.lock").open("a") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise BuildError("Another build owns this work/output directory") from error
        yield


def verify_kit(args, runner):
    kit = args.kit
    record = json.loads((kit / "shared-build-kit.json").read_text())
    if record.get("schema") != 1 or record.get("compiler_kind") not in ("zig", "clang"):
        raise BuildError("Unsupported shared build kit schema/compiler")
    checked_digest(record.get("contract_sha256"), "contract")
    checked_digest(record.get("runtime_sha256"), "runtime")
    files = record.get("files")
    if not isinstance(files, dict) or not files:
        raise BuildError("Toolkit has no complete file inventory")
    for relative, digest in files.items():
        runner.check()
        path = kit_path(kit, relative)
        if not path.is_file() or sha(path) != checked_digest(digest, "toolkit file"):
            raise BuildError(f"Toolkit file hash mismatch: {relative}")
    native_files = record.get("native_files", {})
    if not isinstance(native_files, dict) or native_files.get("libmain.so") != record["runtime_sha256"]:
        raise BuildError("Toolkit runtime identity mismatch")
    for name, digest in native_files.items():
        if Path(name).name != name or not name:
            raise BuildError("Invalid native-tool filename")
        path = args.native_lib_dir / name
        if not path.is_file() or sha(path) != checked_digest(digest, "native tool"):
            raise BuildError(f"Native tool hash mismatch: {name}")
    required = ["cckit/include/runtime/aot_cpu.h", "cckit/include/module_abi.h"]
    required += [f"cckit/include/profiles/{args.title}/{n}" for n in ("simd.h", "simd_fp.h")]
    required += [f"generators/{args.title}/tools/{n}" for n in ("gen.py", "nso.py", "lifter.py", "analyze.py", "patches.py")]
    if any(name not in files for name in required):
        raise BuildError("Missing required hashed generator/common headers")
    contract = hashlib.sha256(b"shared-aot-v1\0" + (kit / required[0]).read_bytes() +
                              (kit / required[1]).read_bytes()).hexdigest()
    if contract != record["contract_sha256"]:
        raise BuildError("Shared CPU/header execution contract mismatch")
    policy = POLICIES[args.title]
    title = record.get("titles", {}).get(args.title, {})
    if title.get("source_commit") != policy["commit"]:
        raise BuildError("Generator revision differs from the supported pinned title")
    modules = title.get("modules")
    if not isinstance(modules, list) or len(modules) != len(policy["modules"]):
        raise BuildError("Title module inventory mismatch")
    for expected, (name, module_id, base) in zip(modules, policy["modules"]):
        if (expected.get("name"), expected.get("module_id"), expected.get("canonical_base")) != (name, module_id, base):
            raise BuildError("Title module order/base/identity mismatch")
        checked_digest(expected.get("sha256"), "original NSO")
        checked_digest(expected.get("build_id"), "full NSO ID")
    if not modules[0]["build_id"].startswith(policy["main_prefix"]):
        raise BuildError("Unsupported title build ID")
    return record, policy


def recognize_inputs(root, modules, policy):
    exefs = root / "exefs"
    expected_names = {m[0] for m in policy["modules"]}
    found = {p.name for p in exefs.iterdir() if p.name in ("main", "sdk") or re.fullmatch(r"subsdk\d+", p.name)}
    # LAS's original dump also contains subsdk0/1, but its established runtime
    # and producer select only main/sdk. These inert originals are never read,
    # generated, relocated or loaded. Unknown extra active-module names still fail.
    inactive = {"subsdk0", "subsdk1"} if len(policy["modules"]) == 2 else set()
    if not expected_names <= found or found - expected_names - inactive:
        raise BuildError("Original selected 2/4-module inventory does not match the title")
    result = {}
    for expected, (name, module_id, base) in zip(modules, policy["modules"]):
        path = exefs / name
        with path.open("rb") as stream:
            header = stream.read(0x100)
        if len(header) != 0x100 or header[:4] != b"NSO0":
            raise BuildError("Original is not a complete NSO header")
        text = struct.unpack_from("<III", header, 0x10)
        data = struct.unpack_from("<III", header, 0x30)
        bss = struct.unpack_from("<I", header, 0x3c)[0]
        mapped = (data[1] + data[2] + bss + 0x3fff) & ~0x3fff
        actual = {"module_id": module_id, "canonical_base": base, "sha256": sha(path),
                  "build_id": header[0x40:0x60].hex(), "mapped_size": mapped,
                  "text_offset": text[1], "text_size": text[2]}
        if any(expected.get(key) != value for key, value in actual.items()):
            raise BuildError(f"Unsupported/changed original NSO: {name}")
        if not text[2] or text[1] + text[2] > mapped:
            raise BuildError("Invalid NSO text/image range")
        result[name] = actual
    ordered = list(result.values())
    for index, module in enumerate(ordered):
        upper = ordered[index + 1]["canonical_base"] if index + 1 < len(ordered) else 0x7320000000
        if module["canonical_base"] + module["mapped_size"] > upper:
            raise BuildError("Original module ranges overlap or leave the guest window")
    npdm = (exefs / "main.npdm").read_bytes()
    if len(npdm) < 0x80 or npdm[:4] != b"META":
        raise BuildError("main.npdm META header missing")
    off = struct.unpack_from("<I", npdm, 0x70)[0]
    if off > len(npdm) - 0x18 or npdm[off:off + 4] != b"ACI0":
        raise BuildError("main.npdm ACI0 header missing")
    if struct.unpack_from("<Q", npdm, off + 0x10)[0] != policy["program_id"]:
        raise BuildError("main.npdm belongs to another title")
    return result


def _verify_elf_data(data):
    def unpack(fmt, off):
        size = struct.calcsize(fmt)
        if off < 0 or off > len(data) - size:
            raise BuildError("Truncated linked ELF metadata")
        return struct.unpack_from(fmt, data, off)
    def region(off, size):
        if off < 0 or size < 0 or off > len(data) - size:
            raise BuildError("Linked ELF range outside file")
        return data[off:off + size]
    def string(blob, offset):
        if not 0 <= offset < len(blob) or b"\0" not in blob[offset:]:
            raise BuildError("Invalid linked ELF string")
        return blob[offset:blob.index(b"\0", offset)].decode("utf-8")
    h = unpack("<16sHHIQQQIHHHHHH", 0)
    if h[0][:7] != b"\x7fELF\x02\x01\x01" or h[1] != 3 or h[2] != 183 or h[3] != 1:
        raise BuildError("Expected an AArch64 ELF64 little-endian shared library")
    if h[9] != 56 or h[11] != 64 or not h[10] or not h[12]:
        raise BuildError("Invalid linked ELF header tables")
    loads = []
    for i in range(h[10]):
        p = unpack("<IIQQQQQQ", h[5] + i * h[9])
        if p[0] == 1:
            region(p[2], p[5])
            if p[5] > p[6] or p[7] < 16384 or p[7] & (p[7] - 1) or (p[2] - p[3]) % p[7]:
                raise BuildError("Linked ELF load alignment/size failed")
            loads.append(p)
    sections = [unpack("<IIQQQQIIQQ", h[6] + i * h[11]) for i in range(h[12])]
    exports, needed, soname = [], [], []
    for s in sections:
        if s[1] not in (6, 11):
            continue
        if s[6] >= len(sections):
            raise BuildError("Invalid ELF linked string table")
        strings = region(sections[s[6]][4], sections[s[6]][5])
        if s[1] == 11:
            if s[9] != 24 or s[5] % 24:
                raise BuildError("Invalid ELF dynamic symbol table")
            for off in range(s[4], s[4] + s[5], 24):
                name, info, other, index, value, size = unpack("<IBBHQQ", off)
                if info >> 4 in (1, 2) and other & 3 == 0 and index:
                    exports.append((string(strings, name), info & 15, size))
        else:
            if s[9] != 16 or s[5] % 16:
                raise BuildError("Invalid ELF dynamic table")
            for off in range(s[4], s[4] + s[5], 16):
                tag, value = unpack("<qQ", off)
                if tag == 1:
                    needed.append(string(strings, value))
                if tag == 14:
                    soname.append(string(strings, value))
    if (not loads or len(exports) != 1 or exports[0][0:2] != ("recomp_module_descriptor_v1", 1)
            or not exports[0][2] or "libmain.so" not in needed or soname != ["liblasgame.so"]):
        raise BuildError("Linked ELF descriptor DATA export/SONAME/runtime dependency failed")
    return {"exports": exports, "needed": needed, "load_segments": len(loads)}


def verify_elf(path):
    with Path(path).open("rb") as stream:
        if os.fstat(stream.fileno()).st_size < 64:
            raise BuildError("Truncated linked ELF header")
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            return _verify_elf_data(data)


def generator_environment(args, record, tools):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("LARECOMP_", "LASGEN_", "PYTHON"))}
    env["PYTHONHASHSEED"] = "0"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    home = record.get("python_home")
    if home:
        env["LASPY_HOME"] = str(kit_path(args.kit, home))
    paths = [str(tools)] + [str(kit_path(args.kit, p)) for p in record.get("python_paths", [])]
    env["LASPY_PATH"] = os.pathsep.join(paths)
    # Normal host Python uses PYTHONPATH; the packaged isolated launcher uses LASPY_PATH.
    env["PYTHONPATH"] = os.pathsep.join(paths)
    return env


def generated_inventory(directory, name):
    paths = sorted(directory.glob("*.c"))
    if not paths or any(not re.fullmatch(re.escape(name) + r"_(?:\d+|table)\.c", p.name) for p in paths):
        raise BuildError("Unexpected generated C inventory")
    if not (directory / f"{name}_table.c").is_file() or not any(re.fullmatch(re.escape(name) + r"_\d+\.c", p.name) for p in paths):
        raise BuildError("Incomplete generated module")
    return {p.name: sha(p) for p in paths}


def generate(args, record, policy, modules, runner, progress):
    python = args.python or Path(sys.executable)
    source = args.kit / "generators" / args.title
    env = generator_environment(args, record, source / "tools")
    query = "import capstone,unicorn,lz4,json; print(json.dumps({m.__name__:m.__version__ for m in (capstone,unicorn,lz4)}))"
    text = runner.run([python, "-B", "-c", query], args.work / "python-version.log", env=env, timeout=60)
    try:
        packages = json.loads(text.splitlines()[-1])
    except (ValueError, IndexError) as error:
        raise BuildError("Cannot identify generator Python packages") from error
    if packages != BINDINGS:
        raise BuildError(f"Pinned generator bindings required: {BINDINGS}")
    prefix = f"generators/{args.title}/"
    source_hashes = {key[len(prefix):]: value for key, value in record["files"].items() if key.startswith(prefix)}
    identity = {"producer_sha256": sha(Path(__file__)), "title": args.title, "source_commit": policy["commit"],
                "source_sha256": source_hashes, "packages": PACKAGES, "bindings": packages, "args": policy["args"], "modules": json.loads(json.dumps(modules))}
    request = fingerprint(identity)
    directory = args.work / "generation" / request
    snapshot, inputs, generated = directory / "source", directory / "inputs", directory / "generated"
    inputs.mkdir(parents=True, exist_ok=True)
    # Snapshot only trusted files, not arbitrary stale Python files in an unpack directory.
    if snapshot.exists():
        actual = {str(p.relative_to(snapshot)): sha(p) for p in snapshot.rglob("*") if p.is_file()}
        if actual != source_hashes:
            raise BuildError("Generator snapshot changed; use a fresh work directory")
    else:
        snapshot.mkdir()
        for relative, digest in source_hashes.items():
            target = kit_path(snapshot, relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / relative, target)
            if sha(target) != digest:
                raise BuildError("Generator changed during snapshot")
    for name, module in modules.items():
        path = inputs / name
        if not path.is_file() or sha(path) != module["sha256"]:
            shutil.copyfile(args.game_root / "exefs" / name, path)
        if sha(path) != module["sha256"]:
            raise BuildError("Original NSO changed during snapshot")
    env = generator_environment(args, record, snapshot / "tools")
    complete = {}
    for name in ["sdk", *[name for name in modules if name != "sdk"]]:
        runner.check()
        module = modules[name]
        dest = generated / name
        manifest = directory / (name + ".json")
        cached = None
        if manifest.exists():
            cached = json.loads(manifest.read_text())
            if cached.get("request") != request or generated_inventory(dest, name) != cached.get("sha256"):
                raise BuildError("Generated module cache changed; refusing unverified C")
        if cached is None:
            # An interrupted module has no completion record; discard only that partial output.
            if dest.exists():
                shutil.rmtree(dest)
            progress.update(state="generating", module=name, generated_functions=0, total_functions=0)
            def tick(log):
                with log.open("rb") as stream:
                    stream.seek(max(0, log.stat().st_size - 16384))
                    tail = stream.read().decode(errors="replace")
                matches = re.findall(r": progress (\d+)/(\d+) files (\d+)", tail)
                if matches:
                    done, total, units = map(int, matches[-1])
                    progress.update(generated_functions=done, total_functions=total, generated_units=units)
            cmd = [python, "-B", snapshot / "tools/gen.py", name, inputs / name,
                   hex(module["canonical_base"]), dest, *policy["args"]]
            if args.title == "las" and name == "sdk":
                cmd += ["--hle", snapshot / "runtime/hle/hle_symbols.txt"]
            runner.run(cmd, directory / ("generate-" + name + ".log"), cwd=snapshot / "tools", env=env, tick=tick)
            hashes = generated_inventory(dest, name)
            count = table_metadata(dest / f"{name}_table.c", module, policy["shift"], name)
            cached = {"request": request, "sha256": hashes, "function_count": count}
            atomic_json(manifest, cached)
        count = table_metadata(dest / f"{name}_table.c", module, policy["shift"], name)
        if count != cached["function_count"]:
            raise BuildError("Generated function-table count changed")
        module["function_count"] = count
        complete.update({name + "/" + k: v for k, v in cached["sha256"].items()})
    # identity was serialized before function_count was added; save independent metadata.
    atomic_json(directory / "generation.json", {"request": request, "identity": identity, "sha256": complete})
    return generated, complete, request


def dependency_hashes(path, cwd):
    text = path.read_text().replace("\\\n", " ")
    if ":" not in text:
        raise BuildError("Compiler did not emit a dependency rule")
    deps = shlex.split(text.split(":", 1)[1])
    if not deps:
        raise BuildError("Compiler emitted an empty dependency rule")
    return {str((cwd / p).resolve()): sha(cwd / p) for p in deps}


def object_reusable(obj, metadata, spec):
    if not obj.is_file() or not metadata.is_file():
        return False
    try:
        cached = json.loads(metadata.read_text())
        dependencies = cached.get("dependencies")
        return (cached.get("spec") == spec and cached.get("key") == fingerprint(spec)
                and cached.get("object_sha256") == sha(obj) and isinstance(dependencies, dict) and bool(dependencies)
                and all(Path(p).is_file() and sha(Path(p)) == digest for p, digest in dependencies.items()))
    except (OSError, ValueError, TypeError):
        return False


def compile_flags(title, source, relative, llvm_major):
    flags = ["-O2" if title == "las" else "-O1", "-fPIC", "-fvisibility=hidden", "-fsigned-char",
             "-fno-strict-aliasing", "-ffixed-x18", "-Wno-return-type", "-Wno-unused-label"]
    with source.open("rb") as stream:
        marker = stream.read(40)
    if title == "las":
        flags += ["-mllvm", "-dag-maps-huge-region=500"]
        if b"lasgen: split-dispatch" in marker:
            flags += ["-mllvm", "-disable-licm-promotion"]
        if b"lasgen: cold" in marker:
            flags[0] = "-O0"
    elif relative in ("main/main_320.c", "subsdk0/subsdk0_289.c"):
        flags += ["-mllvm", "-disable-licm-promotion"]
        if relative.startswith("subsdk0/"):
            flags += ["-mllvm", "-dag-maps-huge-region=500"]
    if llvm_major >= 22:
        flags += ["-mllvm", "-opt-disable=loop-load-elim", "-mllvm", "-has-predecessor-max-steps=2048"]
    return flags


def compiler_tools(args, record, runner):
    kind = args.compiler_kind or record["compiler_kind"]
    compiler = args.compiler or args.native_lib_dir / ("libzig.so" if kind == "zig" else "libclang.so")
    compiler = compiler.resolve()
    prefix = [str(compiler)] + (["clang"] if kind == "zig" else [])
    version = runner.run([*prefix, "--version"], args.work / "compiler-version.log", timeout=60)
    version = "\n".join(version.splitlines()[1:])
    matches = re.findall(r"clang version (\d+)", version)
    if not matches:
        raise BuildError("Packaged compiler must expose a Clang version")
    major = int(matches[-1])
    common = ["-target", "aarch64-linux-android30"]
    if kind == "zig":
        common += ["-nostdinc"]
        for relative in ("clang/include", "sysroot/usr/include", "sysroot/usr/include/aarch64-linux-android"):
            common += ["-isystem", str(args.kit / "cckit" / relative)]
    identity = {"sha256": sha(compiler), "version": version, "kind": kind,
                "native_files": {k: v for k, v in record["native_files"].items() if k != "libmain.so"}, "toolkit_sha256": fingerprint(record["files"])}
    return kind, prefix, common, major, identity


def compile_objects(args, record, policy, modules, generated, hashes, request, runner, progress):
    include = args.work / "include"
    include.mkdir(exist_ok=True)
    sources = {"aot_cpu.h": args.kit / "cckit/include/runtime/aot_cpu.h",
               "module_abi.h": args.kit / "cckit/include/module_abi.h"}
    sources.update({name: args.kit / f"cckit/include/profiles/{args.title}" / name for name in ("simd.h", "simd_fp.h")})
    for name, path in sources.items():
        atomic_bytes(include / name, path.read_bytes())
    wrapper = (f'#define AOT_GUEST_SHIFT {policy["shift"]}ull\n#define AOT_PROFILE_CAPABILITIES {policy["caps"]}u\n'
               '#include "aot_cpu.h"\n_Static_assert(sizeof(Cpu)==66560 && _Alignof(Cpu)==16,"shared CPU3");\n'
               '_Static_assert(offsetof(Cpu,excl_hi)==336 && offsetof(Cpu,dcache)==1024,"shared identity3");\n')
    atomic_bytes(include / "cpu.h", wrapper.encode())
    headers = {p.name: sha(p) for p in include.iterdir() if p.is_file()}
    kind, prefix, common, major, compiler = compiler_tools(args, record, runner)
    entry = args.work / "gamelib_entry.c"
    atomic_bytes(entry, entry_source(modules, policy))
    units = [(relative, generated / relative, digest) for relative, digest in sorted(hashes.items())]
    units.append(("entry/gamelib_entry.c", entry, sha(entry)))
    # Validate flags before spending time on any generated C.
    probe = args.work / "compiler-probe.c"
    atomic_bytes(probe, b'#include "cpu.h"\nvoid probe(Cpu*c){c->x[0]++;}\n')
    runner.run([*prefix, *common, *compile_flags(args.title, probe, "probe.c", major),
                "-I", include, "-c", probe, "-o", args.work / "compiler-probe.o"],
               args.work / "compiler-probe.log", timeout=args.unit_timeout)
    objects = args.work / "objects"
    objects.mkdir(exist_ok=True)
    logs = args.work / "logs"
    logs.mkdir(exist_ok=True)
    progress.update(state="compiling", completed=0, total=len(units), compiled=0, reused=0)
    def compile_one(unit):
        runner.check()
        relative, source, digest = unit
        flags = [*common, *compile_flags(args.title, source, relative, major), "-I", str(include)]
        spec = {"generation": request, "unit": relative, "source_sha256": digest, "headers": headers,
                "compiler": compiler, "flags": flags}
        key = fingerprint(spec)
        obj, meta = objects / (key + ".o"), objects / (key + ".json")
        if object_reusable(obj, meta, spec):
            return {"unit": relative, "key": key, "object": str(obj), "sha256": sha(obj), "reused": True}
        temporary, dependencies = objects / (key + ".o.tmp"), objects / (key + ".d.tmp")
        try:
            runner.run([*prefix, *flags, "-MD", "-MF", dependencies, "-c", source, "-o", temporary],
                       logs / (relative.replace("/", "_") + ".log"), cwd=args.work, timeout=args.unit_timeout)
            if sha(source) != digest:
                raise BuildError("Generated source changed during compilation")
            deps = dependency_hashes(dependencies, args.work)
            if not all(sha(include / name) == digest for name, digest in headers.items()):
                raise BuildError("Shared headers changed during compilation")
            temporary.replace(obj)
            atomic_json(meta, {"key": key, "spec": spec, "dependencies": deps, "object_sha256": sha(obj)})
            return {"unit": relative, "key": key, "object": str(obj), "sha256": sha(obj), "reused": False}
        finally:
            temporary.unlink(missing_ok=True)
            dependencies.unlink(missing_ok=True)
    records = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, unit) for unit in sorted(units, key=lambda u: u[1].stat().st_size, reverse=True)]
        try:
            for future in as_completed(futures):
                item = future.result()
                records.append(item)
                progress.update(completed=len(records), compiled=sum(not r["reused"] for r in records),
                                reused=sum(r["reused"] for r in records))
        except BaseException:
            runner.close()
            for future in futures:
                future.cancel()
            raise
    runner.check()
    semantic = {"execution_abi": "shared-aot-v1", "contract": record["contract_sha256"], "title": args.title,
                "target": "aarch64-linux-android30", "cpu_identity": 3, "cpu_size": 66560,
                "cpu_alignment": 16, "capabilities": policy["caps"], "guest_shift": policy["shift"],
                "simd": "asm", "generation": request, "generated_sha256": hashes, "headers": headers,
                "compiler": compiler, "objects": [{k: v for k, v in r.items() if k not in ("object", "reused")} for r in sorted(records, key=lambda r: r["unit"])],
                "modules": modules}
    return records, fingerprint(semantic), (kind, prefix, common), semantic


def link_library(args, record, records, tools, runner, progress):
    kind, prefix, common = tools
    progress.update(state="linking", message="Linking verified objects")
    response = args.work / "objects.rsp"
    for item in records:
        if sha(Path(item["object"])) != item["sha256"]:
            raise BuildError("Object changed before link")
    atomic_bytes(response, ("\n".join(json.dumps(item["object"]) for item in sorted(records, key=lambda r: r["unit"])) + "\n").encode())
    exports = args.work / "exports.map"
    atomic_bytes(exports, b"{ global: recomp_module_descriptor_v1; local: *; };\n")
    temporary = args.work / "liblasgame.so.candidate"
    flags = ["--no-undefined", "-Bsymbolic", "-z", "max-page-size=16384", "-z", "relro", "-z", "now",
             "-soname", "liblasgame.so", "--version-script=" + str(exports)]
    if kind == "zig":
        kit = args.kit / "cckit"
        syslib = kit / "sysroot/usr/lib/aarch64-linux-android/30"
        builtins = kit / "clang/lib/libclang_rt.builtins-aarch64-android.a"
        cmd = [prefix[0], "ld.lld", "-EL", "--fix-cortex-a53-843419", "--hash-style=gnu", "--eh-frame-hdr",
               "-m", "aarch64linux", "-shared", *flags, "-o", temporary,
               syslib / "crtbegin_so.o", "@" + str(response), "-L" + str(args.native_lib_dir), "-lmain",
               "-L" + str(syslib), builtins, "-ldl", "-lm", "-lc", builtins, syslib / "crtend_so.o"]
    else:
        # A host NDK driver or packaged Android clang supplies its own resource/sysroot configuration.
        cmd = [*prefix, *common, "-shared", "-fuse-ld=lld", *["-Wl," + value for value in flags],
               "-o", temporary, "@" + str(response), "-L" + str(args.native_lib_dir), "-lmain", "-ldl", "-lm"]
    runner.run(cmd, args.work / "link.log", timeout=args.link_timeout)
    elf = verify_elf(temporary)
    if sha(args.native_lib_dir / "libmain.so") != record["runtime_sha256"]:
        raise BuildError("Common runtime changed while compiling/linking")
    return temporary, elf


def publish(output, library, sidecar, metadata, runner):
    runner.check()
    digest = sha(library)
    release = digest + "-" + hashlib.sha256(sidecar).hexdigest()[:16]
    releases = output / "releases"
    releases.mkdir(exist_ok=True)
    target = releases / release
    temporary = releases / (".staging-" + uuid.uuid4().hex)
    if target.exists():
        if (not (target / "liblasgame.so").is_file() or sha(target / "liblasgame.so") != digest
                or not (target / "liblasgame.so.saotm").is_file()
                or (target / "liblasgame.so.saotm").read_bytes() != sidecar):
            raise BuildError("Existing immutable artifact was modified; refusing overwrite")
    else:
        temporary.mkdir()
        try:
            with library.open("rb") as src, (temporary / "liblasgame.so").open("xb") as dst:
                shutil.copyfileobj(src, dst, 1024 * 1024)
                dst.flush()
                os.fsync(dst.fileno())
            atomic_bytes(temporary / "liblasgame.so.saotm", sidecar)
            atomic_json(temporary / "build.json", metadata)
            runner.check()
            os.rename(temporary, target)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
    current = output / "current"
    # Never replace a real caller-owned directory/file with a symlink.
    if current.exists() and not current.is_symlink():
        raise BuildError("Output/current must be absent or a producer symlink")
    pointer = output / (".current-" + uuid.uuid4().hex)
    try:
        pointer.symlink_to(Path("releases") / release, target_is_directory=True)
        runner.check()
        os.replace(pointer, current)  # commit point: one immutable library/sidecar pair
        fd = os.open(output, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        pointer.unlink(missing_ok=True)
    return target


def execute(args, runner, progress):
    progress.update(message="Verifying packaged toolchain and supported title")
    record, policy = verify_kit(args, runner)
    modules = recognize_inputs(args.game_root, record["titles"][args.title]["modules"], policy)
    runner.check()
    generated, hashes, request = generate(args, record, policy, modules, runner, progress)
    records, code, tools, semantic = compile_objects(args, record, policy, modules, generated, hashes, request, runner, progress)
    library, elf = link_library(args, record, records, tools, runner, progress)
    # Recheck originals/toolkit before publication; an unrelated runtime build
    # cannot silently swap the contract while this process has been running.
    current, _ = verify_kit(args, runner)
    actual = recognize_inputs(args.game_root, current["titles"][args.title]["modules"], policy)
    if any({k: v for k, v in m.items() if k != "function_count"} != actual[name] for name, m in modules.items()):
        raise BuildError("Original input identity changed before publication")
    library_hash = sha(library)
    sidecar = wire_manifest(modules, policy, record["contract_sha256"], code, library_hash)
    metadata = {"schema": 1, "title": args.title, "semantic_code_sha256": code, "semantic": semantic,
                "runtime_sha256": record["runtime_sha256"], "library_sha256": library_hash,
                "sidecar_sha256": hashlib.sha256(sidecar).hexdigest(), "elf": elf,
                "objects": records, "device_executed": False}
    progress.update(state="publishing", message="Publishing an immutable library and sidecar pair")
    target = publish(args.output, library, sidecar, metadata, runner)
    progress.update(state="complete", message="Game code built; ready for a fresh runtime process",
                    library=str(target / "liblasgame.so"), sidecar=str(target / "liblasgame.so.saotm"),
                    library_sha256=library_hash, code_sha256=code)
    return metadata


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title", required=True, choices=tuple(POLICIES))
    for name in ("game-root", "work", "output", "kit", "native-lib-dir", "progress"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--cancel-file", type=Path)
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=1)
    parser.add_argument("--python", type=Path, help="Host-only explicit Python override; default current packaged interpreter")
    parser.add_argument("--compiler", type=Path, help="Explicit host/toolchain compiler override")
    parser.add_argument("--compiler-kind", choices=("clang", "zig"))
    parser.add_argument("--unit-timeout", type=int, default=1200)
    parser.add_argument("--link-timeout", type=int, default=1200)
    args = parser.parse_args(argv)
    for name in ("game_root", "work", "output", "kit", "native_lib_dir", "progress", "cancel_file", "python", "compiler"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, Path(os.path.abspath(value)) if name == "python" else value.resolve())
    if args.unit_timeout <= 0 or args.link_timeout <= 0:
        parser.error("Timeouts must be positive")
    # All mutation lives in separate build/output trees, never an original game,
    # a toolkit, or Android's installed nativeLibraryDir. Save roots are not read.
    for mutable in (args.work, args.output):
        for original in (args.game_root, args.kit, args.native_lib_dir):
            if within(mutable, original) or within(original, mutable):
                parser.error("Build/output directories must be separate from game/toolkit/native inputs")
    if within(args.work, args.output) or within(args.output, args.work):
        parser.error("Work and output directories must be separate")
    if not within(args.progress, args.work) or (args.cancel_file and not within(args.cancel_file, args.work)):
        parser.error("Progress/cancellation files must be inside the selected work directory")
    return args


def main(argv=None):
    args = parse_args(argv)
    runner, progress = Runner(args.cancel_file), Progress(args.progress, args.title)
    def cancel(signum, frame):
        runner.stop.set()
    previous = {s: signal.signal(s, cancel) for s in (signal.SIGINT, signal.SIGTERM)}
    locked = False
    try:
        with build_lock(args.work), build_lock(args.output):
            locked = True
            execute(args, runner, progress)
        return 0
    except Cancelled as error:
        progress.update(state="cancelled", message=str(error))
        return 130
    except (BuildError, OSError, ValueError, KeyError, RuntimeError) as error:
        if locked:
            progress.update(state="failed", message=str(error))
        print(f"Shared on-device producer: {error}", file=sys.stderr)
        return 1
    finally:
        runner.close()
        for s, handler in previous.items():
            signal.signal(s, handler)


if __name__ == "__main__":
    raise SystemExit(main())
