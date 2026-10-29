#!/usr/bin/env python3
"""Check AArch64 ELF load alignment and optionally APK ZIP alignment.

Adapted from ptichalouf's Raitchi PR #4, commit
c5712c813ecc6484d624c81be9a75b34c55a8588. PT_LOAD file offsets need not
be divisible by 16 KiB: their virtual addresses must have the same remainder.
APK checks inspect every packaged native library; --zipalign additionally
checks the ZIP layout. No input binaries are modified.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

PT_LOAD = 1
PAGE = 16384
MAX_LIBRARY_BYTES = 1 << 30
MAX_PROGRAM_HEADERS = 4096


def elf_load_segments(path):
    """Read a bounded ELF64 little-endian program table, rejecting bad extents."""
    with open(path, "rb") as stream:
        size = stream.seek(0, 2)
        stream.seek(0)
        header = stream.read(64)
        if size > MAX_LIBRARY_BYTES or len(header) != 64 or header[:4] != b"\x7fELF":
            raise ValueError(f"{path}: invalid or oversized ELF header")
        if header[4:7] != b"\x02\x01\x01":
            raise ValueError(f"{path}: expected ELF64 little-endian version 1")
        kind, machine, version = struct.unpack_from("<HHI", header, 16)
        phoff, = struct.unpack_from("<Q", header, 32)
        ehsize, phsize, count = struct.unpack_from("<HHH", header, 52)
        if (kind not in (2, 3) or version != 1 or ehsize != 64 or phsize < 56 or
                not 0 < count <= MAX_PROGRAM_HEADERS or phoff < 64 or
                phoff > size or count * phsize > size - phoff):
            raise ValueError(f"{path}: invalid program-header table")
        segments = []
        for index in range(count):
            stream.seek(phoff + index * phsize)
            data = stream.read(56)
            if len(data) != 56:
                raise ValueError(f"{path}: truncated program header")
            p_type, flags, offset, vaddr, _, filesz, memsz, align = struct.unpack("<IIQQQQQQ", data)
            if p_type != PT_LOAD:
                continue
            if (filesz > memsz or offset > size or filesz > size - offset or
                    memsz > (1 << 64) - 1 - vaddr):
                raise ValueError(f"{path}: PT_LOAD#{index} extent is invalid")
            segments.append({"index": index, "offset": offset, "vaddr": vaddr,
                             "filesz": filesz, "memsz": memsz, "align": align, "flags": flags})
        if not segments:
            raise ValueError(f"{path}: no PT_LOAD segments")
        return machine, segments


def audit_library(path):
    machine, segments = elf_load_segments(path)
    problems = []
    if machine != 183:
        problems.append(f"ELF machine {machine} is not AArch64")
    for segment in segments:
        alignment = segment["align"]
        difference = segment["vaddr"] - segment["offset"]
        if alignment < PAGE or alignment & (alignment - 1):
            problems.append(f"PT_LOAD#{segment['index']} p_align={alignment} is not a power of two >= {PAGE}")
        elif difference % alignment:
            problems.append(f"PT_LOAD#{segment['index']} vaddr-offset is not congruent to p_align={alignment}")
        if difference % PAGE:
            problems.append(f"PT_LOAD#{segment['index']} vaddr-offset is not congruent to {PAGE}")
    return {"library": str(path), "machine": machine,
            "alignments": [s["align"] for s in segments], "segments": segments,
            "aligned16k": not problems, "problems": problems}


def audit_apk(apk):
    result = {"apk": str(apk), "libraries": []}
    with zipfile.ZipFile(apk) as archive, tempfile.TemporaryDirectory(prefix="elf16k-") as directory:
        entries = [item for item in archive.infolist()
                   if item.filename.startswith("lib/") and item.filename.endswith(".so")]
        if not entries or len({item.filename for item in entries}) != len(entries):
            raise ValueError("APK has no native libraries or duplicate native entries")
        for item in sorted(entries, key=lambda entry: entry.filename):
            if not 64 <= item.file_size <= MAX_LIBRARY_BYTES:
                raise ValueError(f"{item.filename}: invalid or oversized native library")
            target = Path(directory) / "library.so"
            with archive.open(item) as source, target.open("wb") as destination:
                shutil.copyfileobj(source, destination, length=1024 * 1024)
            entry = audit_library(target)
            entry["library"] = entry["path_in_apk"] = item.filename
            result["libraries"].append(entry)
    result["aligned16k"] = all(entry["aligned16k"] for entry in result["libraries"])
    return result


def zipalign_report(apk, zipalign):
    if not zipalign:
        return {"checked": False, "reason": "zipalign not provided"}
    completed = subprocess.run([str(zipalign), "-c", "-P", "16", "-v", "4", str(apk)],
                               capture_output=True, text=True, timeout=120)
    return {"checked": True, "returncode": completed.returncode,
            "output": (completed.stdout + completed.stderr).strip().splitlines()[-5:]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lib", action="append", default=[])
    parser.add_argument("--apk")
    parser.add_argument("--zipalign")
    parser.add_argument("--json")
    args = parser.parse_args()
    if not args.apk and not args.lib:
        parser.error("provide at least one --lib or --apk")
    if args.zipalign and not args.apk:
        parser.error("--zipalign requires --apk")
    report = {"libraries": [], "aligned16k": True}
    try:
        if args.apk:
            report["apk"] = audit_apk(args.apk)
            alignment = zipalign_report(args.apk, args.zipalign)
            report["apk"]["zipalign"] = alignment
            report["aligned16k"] &= report["apk"]["aligned16k"] and (
                not alignment["checked"] or alignment["returncode"] == 0)
        for path in args.lib:
            entry = audit_library(path)
            report["libraries"].append(entry)
            report["aligned16k"] &= entry["aligned16k"]
    except (OSError, ValueError, struct.error, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        report["aligned16k"] = False
        report["error"] = str(error)
    text = json.dumps(report, indent=2)
    print(text)
    if args.json:
        Path(args.json).write_text(text + "\n")
    return 0 if report["aligned16k"] else 1


if __name__ == "__main__":
    sys.exit(main())
