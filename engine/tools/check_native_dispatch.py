#!/usr/bin/env python3
"""Verify genuine native binding and shared dispatch behavior in fresh processes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = ("unbound", "invalid", "native-first", "aot-first", "execution", "hle", "hooks", "no-hooks", "threads",
         *(f"order-{i}" for i in range(24)))


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    out = ROOT / "build/native-dispatch"
    out.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / p for p in ("native/module_abi.c", "native/runtime/native_manifest.c",
               "native/runtime/aot_dispatch.c", "native/runtime/aot_hooks.c", "tests/native_dispatch_test.c")]
    paths = [*sources, ROOT / "native/module_abi.h", ROOT / "native/runtime/native_manifest.h",
             ROOT / "native/runtime/aot_dispatch.h", ROOT / "native/runtime/aot_hooks.h",
             ROOT / "native/runtime/aot_cpu.h", Path(__file__).resolve()]
    pinned = {str(p.relative_to(ROOT)): sha(p) for p in paths}

    def run(command, log):
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=60)
        (out / log).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(log + ": " + result.stdout + result.stderr)
        return result.stdout

    cc = shutil.which("clang")
    assert cc
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", ROOT / "native"]
    reports = {}
    for name, flags in [("optimized", ["-O2"]),
                        ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]:
        executable = out / name
        run([cc, *common, *flags, *sources, "-pthread", "-o", executable], name + "-build.log")
        reports[name] = {}
        for case in CASES:
            report = json.loads(run([executable, case], name + "-" + case + ".log"))
            assert report["state"] == "passed"
            reports[name][case] = report
    android = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
    target = out / "native-dispatch-test"
    run([android, *common, "-O2", "-ffixed-x18", *sources, "-pthread", "-Wl,--no-undefined",
         "-Wl,-z,max-page-size=16384", "-o", target], "android-link.log")
    assert pinned == {str(p.relative_to(ROOT)): sha(p) for p in paths}, "Sources changed during verification"
    report = {"schema": 1, "host_tests": reports, "android_cross_link_passed": True,
              "source_sha256": pinned, "android_test_sha256": sha(target),
              "scope": "Actual native manifest/registry/hooks, synthetic metadata and host backend fixture, fresh process per case; no game assets, DSO requirement or device execution"}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"state": "passed", "fresh_process_cases_per_build": len(CASES),
                      "assertions_per_build": {name: sum(case["checks"] for case in cases.values()) for name, cases in reports.items()},
                      "android_cross_link_passed": True}, indent=2))


if __name__ == "__main__":
    main()
