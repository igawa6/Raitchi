#!/usr/bin/env python3
"""Check the production headless-session API using controlled dependencies.

Uses the real guest_session.c and private KThread header with controlled loader,
hash, session and kernel stand-ins. Fresh processes exercise each lifecycle
case. This does not establish production kernel/SVC/TLS or game execution.
No Android builds or device actions.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = ("lifecycle", "identity", "no-session")
SOURCES = ("tests/shared_guest_session_test.c",
           "ports/shared-android/runtime/guest_session.c",
           "ports/shared-android/runtime/guest_session.h",
           "ports/shared-android/runtime/kernel.h",
           "ports/shared-android/runtime/rt.h",
           "ports/shared-android/runtime/cpu.h",
           "ports/shared-android/runtime/common_session.h",
           "native/runtime/aot_cpu.h", "native/runtime/aot_dispatch.h",
           "native/runtime/session_layout.h", "native/runtime/title_manifest.h",
           "native/adapters/title_adapter.h", "native/module_abi.h",
           "sources/las-android/runtime/platform.h",
           "sources/las-android/runtime/platform/linux_compat.h",
           "tools/check_shared_guest_session.py")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/shared-guest-session")
    args = parser.parse_args()
    if not args.clang:
        parser.error("Host Clang is required")
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Output must remain under the ignored workspace build directory")
    output.mkdir(parents=True, exist_ok=True)
    source_hashes = {name: sha(ROOT / name) for name in SOURCES}
    report = {"verified_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "actual guest_session.c and private KThread header; controlled loader/hash/session/kernel stand-ins",
              "production_kernel_execution": False, "guest_execution": False,
              "device_actions": False, "source_sha256": source_hashes, "variants": {}}
    for label, flags in (("optimized", ["-O2"]),
                         ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = output / label
        command = [args.clang, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", *flags,
                   "-I", str(ROOT / "native"), "-I", str(ROOT / "ports/shared-android/runtime"),
                   "-I", str(ROOT / "sources/las-android/runtime"),
                   str(ROOT / "tests/shared_guest_session_test.c"),
                   str(ROOT / "ports/shared-android/runtime/guest_session.c"),
                   "-pthread", "-o", str(binary)]
        built = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
        (output / (label + "-build.log")).write_text(built.stdout + built.stderr)
        cases = []
        for case in CASES:
            # Each invocation gets a fresh process and one headless setup attempt.
            ran = subprocess.run([str(binary), case], cwd=ROOT, check=True,
                                 capture_output=True, text=True, timeout=10)
            (output / (label + "-" + case + ".log")).write_text(ran.stdout + ran.stderr)
            cases.append({"case": case, "passed": True, "stdout": ran.stdout.strip()})
            print(label + "/" + case + ": " + ran.stdout.strip(), flush=True)
        report["variants"][label] = {"binary_sha256": sha(binary), "cases": cases}
    if source_hashes != {name: sha(ROOT / name) for name in SOURCES}:
        raise RuntimeError("Source files changed while API-contract tests were running")
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
