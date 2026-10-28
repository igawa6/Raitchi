#!/usr/bin/env python3
"""Check shared NSO/LZ4 decoder via both AOT and native-only expectations."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    out = ROOT / "build/nso-image"
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / "native/runtime/nso_image.c"
    test = ROOT / "tests/nso_image_test.c"
    paths = [source, source.with_suffix(".h"), test, ROOT / "native/module_abi.h",
             ROOT / "native/runtime/native_manifest.h", Path(__file__).resolve()]
    pinned = {str(p.relative_to(ROOT)): sha(p) for p in paths}

    def run(command, log):
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=60)
        (out / log).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(log + ": " + result.stdout + result.stderr)
        return result.stdout

    cc = shutil.which("clang")
    assert cc, "host clang is required"
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", ROOT / "native"]
    reports = {}
    for name, flags in [("optimized", ["-O2"]),
                        ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]:
        executable = out / name
        # Intentionally do not link native_manifest.c or module_abi.c. Both
        # public decode APIs require only their immutable metadata declarations.
        run([cc, *common, *flags, source, test, "-o", executable], name + "-build.log")
        stdout = run([executable], name + "-run.log")
        count = re.search(r"NSO image: (\d+) synthetic checks passed", stdout)
        assert count and int(count[1]) > 8000, stdout
        reports[name] = {"state": "passed", "checks": int(count[1])}
    android = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
    target = out / "nso-image-test"
    run([android, *common, "-O2", "-ffixed-x18", source, test, "-Wl,--no-undefined",
         "-Wl,-z,max-page-size=16384", "-o", target], "android-link.log")
    assert pinned == {str(p.relative_to(ROOT)): sha(p) for p in paths}, "Sources changed during verification"
    report = {"schema": 1, "host_tests": reports, "android_cross_link_passed": True,
              "source_sha256": pinned, "android_test_sha256": sha(target),
              "native_manifest_implementation_linked": False,
              "scope": "Actual shared decoder, synthetic compressed/raw NSO fixtures, native-only metadata and retained AOT table/legacy-CPU rules; no game assets, mappings or device execution"}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
