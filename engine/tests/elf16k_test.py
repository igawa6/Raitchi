#!/usr/bin/env python3
"""Tests for the 16 KiB page-size check of Android ELF binaries.

Adapted from ptichalouf, Raitchi PR #4, commit c5712c813ecc6484d624c81be9a75b34c55a8588.

The check must accept a library whose segments are 16 KiB compatible and
reject one that is only 4 KiB compatible, without ever requiring the file
offset itself to be a multiple of 16384.
"""
import struct
import contextlib
import io
import os
from unittest.mock import patch
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
    struct.pack_into("<HH", header, 52, 64, 56)  # e_ehsize, e_phentsize
    struct.pack_into("<HH", header, 54, 56, len(segments))  # e_phentsize, e_phnum
    body = bytearray()
    for index, (offset, vaddr, align) in enumerate(segments):
        entry = bytearray(56)
        struct.pack_into("<II", entry, 0, 1, 4)  # PT_LOAD, PF_R
        struct.pack_into("<QQ", entry, 8, offset, vaddr)
        struct.pack_into("<QQ", entry, 32, 0x1000, 0x1000)
        struct.pack_into("<Q", entry, 48, align)
        body += entry
    data = bytearray(header + body)
    extent = max([len(data), *(offset + 0x1000 for offset, _, _ in segments)])
    data.extend(bytes(extent - len(data)))
    path.write_bytes(data)
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


class GateRegressionTests(unittest.TestCase):
    def apk(self, path, library):
        import zipfile
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("lib/arm64-v8a/lib.so", library.read_bytes())
        return path

    def cli(self, arguments):
        with patch.object(sys, "argv", ["check_elf16k", *map(str, arguments)]), contextlib.redirect_stdout(io.StringIO()) as output:
            status = check_elf16k.main()
        import json
        return status, json.loads(output.getvalue())

    def test_mixed_inputs_all_affect_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good = write_elf(root / "good.so", [(0, 0, 16384)])
            bad = write_elf(root / "bad.so", [(0, 0, 4096)])
            apk = self.apk(root / "good.apk", good)
            with patch.object(check_elf16k, "zipalign_report", return_value={"checked": True, "returncode": 0}):
                status, report = self.cli(["--apk", apk, "--lib", bad])
            self.assertEqual(status, 1)
            self.assertFalse(report["aligned16k"])

    def test_zip_alignment_failure_affects_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good = write_elf(root / "good.so", [(0, 0, 16384)])
            apk = self.apk(root / "good.apk", good)
            with patch.object(check_elf16k, "zipalign_report", return_value={"checked": True, "returncode": 1}):
                status, report = self.cli(["--apk", apk, "--zipalign", "/synthetic/zipalign"])
            self.assertEqual(status, 1)
            self.assertFalse(report["aligned16k"])

    def test_temp_cleanup_success_and_exception(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            real_temporary = tempfile.TemporaryDirectory
            created = []
            def temporary(*args, **kwargs):
                result = real_temporary(*args, dir=root, **kwargs)
                created.append(Path(result.name))
                return result
            good = write_elf(root / "good.so", [(0, 0, 16384)])
            apk = self.apk(root / "good.apk", good)
            before = len(os.listdir("/proc/self/fd")) if Path("/proc/self/fd").exists() else None
            with patch.object(check_elf16k.tempfile, "TemporaryDirectory", side_effect=temporary):
                check_elf16k.audit_apk(apk)
                good.write_bytes(b"bad ELF" + bytes(64))
                self.apk(apk, good)
                with self.assertRaises(ValueError):
                    check_elf16k.audit_apk(apk)
            self.assertEqual(len(created), 2)
            self.assertTrue(all(not path.exists() for path in created))
            if before is not None:
                self.assertEqual(before, len(os.listdir("/proc/self/fd")))

    def test_malformed_tables_and_extents_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lib.so"
            cases = [
                ("empty table", 56, "<H", 0),
                ("huge table", 56, "<H", 65535),
                ("short entry", 54, "<H", 8),
                ("table offset", 32, "<Q", 0x10000),
                ("no load", 64, "<I", 4),
                ("file overrun", 64 + 8, "<Q", 0x10000),
                ("filesz overrun", 64 + 32, "<Q", 0x10000),
                ("filesz exceeds memsz", 64 + 40, "<Q", 1),
                ("memory wrap", 64 + 16, "<Q", (1 << 64) - 1),
            ]
            for label, offset, format_, value in cases:
                with self.subTest(label=label):
                    write_elf(path, [(0, 0, 16384)])
                    raw = bytearray(path.read_bytes())
                    struct.pack_into(format_, raw, offset, value)
                    path.write_bytes(raw)
                    with self.assertRaises(ValueError):
                        check_elf16k.audit_library(path)
            for size in (0, 63, 80):
                with self.subTest(truncated=size):
                    write_elf(path, [(0, 0, 16384)])
                    path.write_bytes(path.read_bytes()[:size])
                    with self.assertRaises(ValueError):
                        check_elf16k.audit_library(path)

    def test_wrong_machine_and_non_power_alignment_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lib.so"
            write_elf(path, [(0, 0, 24576)])
            self.assertFalse(check_elf16k.audit_library(path)["aligned16k"])
            write_elf(path, [(0, 0, 16384)])
            data = bytearray(path.read_bytes())
            struct.pack_into("<H", data, 18, 62)
            path.write_bytes(data)
            self.assertFalse(check_elf16k.audit_library(path)["aligned16k"])

    def test_invalid_library_cli_fails_cleanly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.so"
            path.write_bytes(bytes(64))
            status, report = self.cli(["--lib", path])
            self.assertEqual(status, 1)
            self.assertIn("error", report)

    def test_final_map_includes_optional_libraries(self):
        import package_shared_android as package
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good = write_elf(root / "good.so", [(0, 0, 16384)])
            bad = write_elf(root / "bad.so", [(0, 0, 4096)])
            for optional in ("libpython3.14.so", "libVkLayer_khronos_validation.so"):
                with self.subTest(optional=optional):
                    libraries = {"libmain.so": good, optional: bad}
                    reports = {name: {} for name in libraries}
                    with self.assertRaises(RuntimeError):
                        package.verify_library_alignment(libraries, reports)
            reports = {"libmain.so": {}, "libpython3.14.so": {}}
            package.verify_library_alignment({name: good for name in reports}, reports)
            self.assertTrue(all(report["aligned16k"] for report in reports.values()))

    def test_final_apk_gate_rejects_bad_native_bytes(self):
        import package_shared_android as package
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = write_elf(root / "lib.so", [(0, 0, 4096)])
            apk = self.apk(root / "app.apk", library)
            with patch.object(check_elf16k, "zipalign_report", return_value={"checked": True, "returncode": 0}):
                with self.assertRaises(RuntimeError):
                    package.verify_apk_alignment(apk, root)
                write_elf(library, [(0, 0, 16384)])
                self.apk(apk, library)
                self.assertTrue(package.verify_apk_alignment(apk, root)["aligned16k"])

    def test_empty_and_duplicate_apks_rejected(self):
        import zipfile
        import warnings
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = write_elf(root / "lib.so", [(0, 0, 16384)])
            apk = root / "app.apk"
            with zipfile.ZipFile(apk, "w"):
                pass
            with self.assertRaises(ValueError):
                check_elf16k.audit_apk(apk)
            self.apk(apk, library)
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                with zipfile.ZipFile(apk, "a") as archive:
                    archive.writestr("lib/arm64-v8a/lib.so", library.read_bytes())
            with self.assertRaises(ValueError):
                check_elf16k.audit_apk(apk)


if __name__ == "__main__":
    unittest.main()
