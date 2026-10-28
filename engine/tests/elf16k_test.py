#!/usr/bin/env python3
"""Tests for the 16 KiB page-size check of Android ELF binaries.

The check must accept a library whose segments are 16 KiB compatible and
reject one that is only 4 KiB compatible, without ever requiring the file
offset itself to be a multiple of 16384.
"""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import check_elf16k


def write_elf(path, segments):
    """Write a minimal ELF64 AArch64 file with the given PT_LOAD segments."""
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2  # ELFCLASS64
    header[5] = 1  # ELFDATA2LSB
    header[6] = 1  # EV_CURRENT
    struct.pack_into("<H", header, 16, 2)  # ET_EXEC
    struct.pack_into("<H", header, 18, 183)  # EM_AARCH64
    struct.pack_into("<I", header, 20, 1)
    struct.pack_into("<Q", header, 32, 64)  # e_phoff
    struct.pack_into("<HH", header, 52, 56, 56)  # e_ehsize, e_phentsize
    struct.pack_into("<HH", header, 54, 56, len(segments))  # e_phentsize, e_phnum
    body = bytearray()
    for index, (offset, vaddr, align) in enumerate(segments):
        entry = bytearray(56)
        struct.pack_into("<II", entry, 0, 1, 4)  # PT_LOAD, PF_R
        struct.pack_into("<QQ", entry, 8, offset, vaddr)
        struct.pack_into("<QQ", entry, 32, 0x1000, 0x1000)
        struct.pack_into("<Q", entry, 48, align)
        body += entry
    path.write_bytes(bytes(header) + bytes(body))
    return path


class Elf16kTests(unittest.TestCase):
    def test_16k_library_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write_elf(Path(directory) / "lib.so",
                             [(0, 0, 16384), (0x1000, 0x5000, 16384)])
            entry = check_elf16k.audit_library(str(path))
            self.assertTrue(entry["aligned16k"], entry["problems"])
            self.assertEqual(entry["problems"], [])

    def test_4k_library_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write_elf(Path(directory) / "lib.so",
                             [(0, 0, 4096), (0x1000, 0x2000, 4096)])
            entry = check_elf16k.audit_library(str(path))
            self.assertFalse(entry["aligned16k"])
            self.assertTrue(any("p_align=4096" in problem for problem in entry["problems"]))

    def test_offset_need_not_be_a_multiple_of_16k(self):
        # 0x215e90 is not a multiple of 16384, but vaddr - offset = 0x8000 is,
        # so the segment can still be placed on a 16 KiB page boundary.
        with tempfile.TemporaryDirectory() as directory:
            path = write_elf(Path(directory) / "lib.so",
                             [(0, 0, 16384), (0x215e90, 0x21de90, 16384)])
            entry = check_elf16k.audit_library(str(path))
            self.assertTrue(entry["aligned16k"], entry["problems"])

    def test_broken_congruence_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = write_elf(Path(directory) / "lib.so",
                             [(0, 0, 16384), (0x215e90, 0x21fe90, 16384)])
            entry = check_elf16k.audit_library(str(path))
            self.assertFalse(entry["aligned16k"])
            self.assertTrue(any("not congruent" in problem for problem in entry["problems"]))

    def test_apk_check_reports_every_packaged_library(self):
        with tempfile.TemporaryDirectory() as directory:
            import zipfile
            apk = Path(directory) / "app.apk"
            library = write_elf(Path(directory) / "libgood.so", [(0, 0, 16384)])
            with zipfile.ZipFile(apk, "w") as archive:
                archive.writestr("lib/arm64-v8a/libgood.so", library.read_bytes())
                archive.writestr("assets/text.txt", b"x")
            entry = check_elf16k.audit_apk(str(apk))
            self.assertEqual([e["path_in_apk"] for e in entry["libraries"]],
                             ["lib/arm64-v8a/libgood.so"])
            self.assertTrue(entry["aligned16k"], entry["libraries"])

    def test_command_line_exit_status_reflects_the_check(self):
        with tempfile.TemporaryDirectory() as directory:
            good = write_elf(Path(directory) / "good.so", [(0, 0, 16384)])
            bad = write_elf(Path(directory) / "bad.so", [(0, 0, 4096)])
            good_run = subprocess.run(
                [sys.executable, str(ROOT / "tools/check_elf16k.py"), "--lib", str(good)],
                capture_output=True, text=True)
            bad_run = subprocess.run(
                [sys.executable, str(ROOT / "tools/check_elf16k.py"), "--lib", str(bad)],
                capture_output=True, text=True)
            self.assertEqual(good_run.returncode, 0, good_run.stdout + good_run.stderr)
            self.assertEqual(bad_run.returncode, 1)


if __name__ == "__main__":
    unittest.main()
