#!/usr/bin/env python3
"""Prepare validated native-only metadata from an already extracted owner ExeFS.

Uses actual C title adapters and NSO/LZ4 decoder; no games, ADB, CPU translation
or game DSO compilation. This compiles a small host metadata-checking utility.
Whole-file hashes bind supplied originals, not publisher authenticity. Inputs must
remain unchanged during preparation; production rechecks hashes before loading.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CONTRACT_INPUTS = ("native/runtime/aot_cpu.h", "native/module_abi.h", "native/runtime/native_manifest.h")
HELPER_SOURCES = ("tools/native_prepare_check.c", "native/module_abi.c", "native/runtime/native_manifest.c",
                  "native/runtime/nso_image.c", "native/runtime/session_layout.c", "native/adapters/title_adapter.c",
                  "native/adapters/las/profile.c", "native/adapters/letsgo/profile.c", "native/adapters/echoes/profile.c", "native/adapters/wonder/profile.c")
HELPER_HEADERS = (*CONTRACT_INPUTS, "native/runtime/nso_image.h", "native/runtime/session_layout.h", "native/adapters/title_adapter.h")


def contract_digest():
    return hashlib.sha256(b"shared-native-v1\0" + b"".join((ROOT / p).read_bytes() for p in CONTRACT_INPUTS)).digest()


def run(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "Native metadata helper failed")
    return result.stdout


def build_helper():
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("Host clang is required for the original-image validation helper")
    out = ROOT / "build/native-prepare"
    out.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(b"native-prepare-helper-v1\0" + str(compiler).encode() +
        b"".join((ROOT / p).read_bytes() for p in (*HELPER_SOURCES, *HELPER_HEADERS))).hexdigest()
    binary = out / ("check-" + digest[:20])
    if not binary.is_file():
        fd, name = tempfile.mkstemp(prefix=".check-", dir=out)
        os.close(fd)
        temporary = Path(name)
        try:
            run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", ROOT / "native",
                 *(ROOT / p for p in HELPER_SOURCES), "-pthread", "-o", temporary])
            os.replace(temporary, binary)
        finally:
            temporary.unlink(missing_ok=True)
    return binary


def manifest_bytes(exefs, policy, contract):
    modules = policy["modules"]
    length = 128 + 104 * len(modules)
    wire = bytearray(length)
    wire[:8] = b"NATVM001"
    struct.pack_into("<IIIIQIIIIIIQQQ", wire, 8, length, 1, policy["title_id"], len(modules),
        policy["program_id"], policy["target"], policy["cpu_identity"], policy["cpu_size"], policy["cpu_alignment"],
        policy["mapping_alignment"], 0, policy["guest_shift"], policy["window_lo"], policy["window_hi"])
    wire[80:112] = contract
    alignment = policy["mapping_alignment"]
    for i, module in enumerate(modules):
        path = exefs / module["name"]
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            if before.st_size > 512 << 20:
                raise RuntimeError("Original NSO exceeds bounded preparation limit")
            header = stream.read(256)
            if len(header) != 256 or header[:4] != b"NSO0":
                raise RuntimeError("Invalid original NSO header: " + module["name"])
            stream.seek(0)
            digest = hashlib.file_digest(stream, "sha256").digest()
            after = os.fstat(stream.fileno())
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise RuntimeError("Original NSO changed during hashing")
        segments = [struct.unpack_from("<III", header, 0x10 + j * 16) for j in range(3)]
        bss = struct.unpack_from("<I", header, 0x3c)[0]
        end = max(offset + size + (bss if j == 2 else 0) for j, (_, offset, size) in enumerate(segments))
        mapped = (end + alignment - 1) & ~(alignment - 1)
        at = 128 + i * 104
        struct.pack_into("<II", wire, at, module["module_id"], 0)
        wire[at + 8:at + 40] = header[0x40:0x60]
        wire[at + 40:at + 72] = digest
        struct.pack_into("<QQQQ", wire, at + 72, module["canonical_base"], mapped, *segments[0][1:])
    return bytes(wire)


def prepare(title, exefs, output):
    helper = build_helper()
    policy = json.loads(run([helper, "policy", title]))
    contract = contract_digest()
    wire = manifest_bytes(exefs, policy, contract)
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".native-", dir=output.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(wire)
            stream.flush()
            os.fsync(stream.fileno())
        validation = json.loads(run([helper, "validate", title, exefs, temporary, contract.hex()]))
        if validation.get("state") != "validated":
            raise RuntimeError("Native metadata validation did not complete")
        # A hard link publishes the entire validated file atomically, refusing
        # replacement even if another preparer races this process.
        try:
            os.link(temporary, output)
            state = "created"
        except FileExistsError:
            if not output.is_file() or output.is_symlink() or output.read_bytes() != wire:
                raise RuntimeError("Refusing to overwrite conflicting existing metadata")
            state = "already-identical"
        return {"state": state, "title": title, "modules": len(policy["modules"]),
                "manifest_bytes": len(wire), "manifest_sha256": hashlib.sha256(wire).hexdigest(),
                "native_contract_sha256": contract.hex(), "generated_functions": False,
                "game_dso_required": False}
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title", required=True, choices=("las", "eevee", "echoes", "wonder"))
    parser.add_argument("--exefs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = prepare(args.title, args.exefs.resolve(strict=True), args.output.absolute())
    except (OSError, RuntimeError, ValueError, struct.error) as error:
        parser.exit(1, "Native preparation failed: " + str(error) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
