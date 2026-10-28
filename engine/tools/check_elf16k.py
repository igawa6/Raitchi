#!/usr/bin/env python3
"""Check the 16 KiB page-size compatibility of Android ELF binaries.

Android 15+ devices may use 16 KiB memory pages. A native library is loadable
on such a device when every PT_LOAD segment can be placed on a 16 KiB page
boundary in memory and in the file. The ELF constraint is:

  1. p_align >= 16384 for every PT_LOAD segment;
  2. p_vaddr ≡ p_offset (mod 16384) for every PT_LOAD segment.

The second condition is a congruence, not a divisibility: a segment file
offset of 0x215e90 is fine as long as the virtual address differs from it by
a multiple of 16384. Requiring the offset itself to be a multiple of 16384
would reject correct binaries.

Usage:
  check_elf16k.py --lib lib/arm64-v8a/libfoo.so [--lib ...] [--json report.json]
  check_elf16k.py --apk app.apk [--zipalign /path/to/zipalign] [--json report.json]

Read-only on the binaries. The only file written is the optional --json report.
Exit status is 0 when every checked library is 16 KiB compatible, 1 otherwise.
"""
import argparse
import json
import struct
import subprocess
import sys
import tempfile
import zipfile

PT_LOAD = 1
PAGE = 16384


def elf_load_segments(path):
    """Return (machine, PT_LOAD segments) read straight from the ELF64 header."""
    with open(path, "rb") as stream:
        header = stream.read(64)
        if len(header) < 64 or header[:4] != b"\x7fELF":
            raise ValueError(f"{path}: not an ELF file")
        if header[4] != 2 or header[5] != 1:
            raise ValueError(f"{path}: expected a 64-bit little-endian ELF")
        machine, = struct.unpack_from("<H", header, 18)
        e_phoff, = struct.unpack_from("<Q", header, 32)
        e_phentsize, e_phnum = struct.unpack_from("<HH", header, 54)
        stream.seek(e_phoff)
        segments = []
        for index in range(e_phnum):
            data = stream.read(e_phentsize)
            p_type, p_flags = struct.unpack_from("<II", data, 0)
            p_offset, p_vaddr = struct.unpack_from("<QQ", data, 8)
            p_filesz, p_memsz = struct.unpack_from("<QQ", data, 32)
            p_align, = struct.unpack_from("<Q", data, 48)
            if p_type == PT_LOAD:
                segments.append({"index": index, "offset": p_offset, "vaddr": p_vaddr,
                                 "filesz": p_filesz, "memsz": p_memsz, "align": p_align,
                                 "flags": p_flags})
        return machine, segments


def audit_library(path):
    """Check one ELF file; returns a report entry with the problems found."""
    machine, segments = elf_load_segments(path)
    problems = []
    for segment in segments:
        if segment["align"] < PAGE:
            problems.append(f"PT_LOAD#{segment['index']} p_align={segment['align']} < {PAGE}")
        if (segment["vaddr"] - segment["offset"]) % PAGE:
            problems.append(
                f"PT_LOAD#{segment['index']} vaddr-offset=0x{segment['vaddr'] - segment['offset']:x} "
                f"not congruent to {PAGE}")
    return {"library": path, "machine": machine, "alignments": [s["align"] for s in segments],
            "segments": segments, "aligned16k": not problems, "problems": problems}


def audit_apk(apk):
    """Check every native library packaged in an APK."""
    result = {"apk": apk, "libraries": []}
    with zipfile.ZipFile(apk) as archive:
        names = [n for n in archive.namelist() if n.startswith("lib/") and n.endswith(".so")]
        for name in sorted(names):
            handle, target = tempfile.mkstemp(suffix=".so")
            with open(target, "wb") as stream:
                stream.write(archive.read(name))
            entry = audit_library(target)
            entry["path_in_apk"] = name
            result["libraries"].append(entry)
    result["aligned16k"] = all(entry["aligned16k"] for entry in result["libraries"])
    return result


def zipalign_report(apk, zipalign):
    """Report the -P 16 alignment state of an APK when zipalign is available."""
    if not zipalign:
        return {"checked": False, "reason": "zipalign not provided"}
    completed = subprocess.run([zipalign, "-c", "-P", "16", "-v", "4", apk],
                               capture_output=True, text=True)
    return {"checked": True, "returncode": completed.returncode,
            "output": (completed.stdout + completed.stderr).strip().splitlines()[-5:]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lib", action="append", default=[])
    parser.add_argument("--apk")
    parser.add_argument("--zipalign")
    parser.add_argument("--json")
    args = parser.parse_args()
    report = {"libraries": []}  # type: dict
    if args.apk:
        report["apk"] = audit_apk(args.apk)
        report["apk"]["zipalign"] = zipalign_report(args.apk, args.zipalign)
        report["aligned16k"] = report["apk"]["aligned16k"] and \
            (report["apk"]["zipalign"].get("returncode", 1) == 0)
    for path in args.lib:
        report["libraries"].append(audit_library(path))
    report["aligned16k"] = report.get("aligned16k", all(e["aligned16k"] for e in report["libraries"]))
    text = json.dumps(report, indent=2)
    print(text)
    if args.json:
        with open(args.json, "w") as stream:
            stream.write(text + "\n")
    return 0 if report["aligned16k"] else 1


if __name__ == "__main__":
    sys.exit(main())
