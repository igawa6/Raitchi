#!/usr/bin/env python3
"""Verify conservative native-block planning; no game files or device actions."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "native/native-game-backend"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    parser.add_argument("--capstone-include", type=Path, default=ROOT / "build/shared-ondevice/work/build/ondevice/src/capstone-5.0.9/src/include")
    parser.add_argument("--android-capstone", type=Path, default=ROOT / "build/shared-android/apk-inputs/jniLibs/arm64-v8a/libcapstone.so")
    parser.add_argument("--only-atomics", action="store_true", help="Reuse unchanged planner/control checks and verify the new atomic implementation")
    args = parser.parse_args()
    out = ROOT / "build/native-game-backend"
    out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("clang")
    if not cc:
        parser.error("Installed host Clang required")
    headers = [args.capstone_include / "capstone" / name for name in ("capstone.h", "arm64.h", "platform.h")]
    for header in headers:
        if not header.is_file():
            parser.error("Missing cached Capstone header: " + str(header))
    source_files = [SOURCE / "block_plan.c", SOURCE / "block_plan.h", SOURCE / "control_step.c", SOURCE / "control_step.h",
                    SOURCE / "atomic_step.c", SOURCE / "atomic_step.h", ROOT / "tests/native_atomic_step_test.c",
                    ROOT / "tests/native_game_backend_test.c", ROOT / "tests/native_control_step_test.c",
                    ROOT / "native/runtime/aot_cpu.h", Path(__file__).resolve()]
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in source_files}

    def run(command, log):
        result = subprocess.run(list(map(str, command)), check=True, capture_output=True, text=True, timeout=30)
        (out / log).write_text(result.stdout + result.stderr)
        return result.stdout

    variants = {}; control_variants = {}; atomic_variants = {}; inherited = None
    modes = (("optimized", ["-O2"]), ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    if args.only_atomics:
        prior_path = out / "verification.json"
        prior = json.loads(prior_path.read_text())
        inherited_sources = [SOURCE / "block_plan.c", SOURCE / "block_plan.h", SOURCE / "control_step.c", SOURCE / "control_step.h",
                             ROOT / "tests/native_game_backend_test.c", ROOT / "tests/native_control_step_test.c", ROOT / "native/runtime/aot_cpu.h"]
        for path in inherited_sources:
            if prior["source_sha256"][str(path.relative_to(ROOT))] != sha(path):
                raise RuntimeError("Cannot reuse changed planner/control verification: " + str(path))
        inherited = {"prior_verification_sha256": sha(prior_path), "unchanged_source_files": [str(p.relative_to(ROOT)) for p in inherited_sources]}
        variants, control_variants = prior["host_tests"], prior["control_host_tests"]
    for name, flags in (() if args.only_atomics else modes):
        binary = out / name
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, "-I", SOURCE,
             "-I", args.capstone_include, SOURCE / "block_plan.c", ROOT / "tests/native_game_backend_test.c",
             "-lcapstone", "-o", binary], name + "-build.log")
        output = run([binary], name + "-run.log")
        variants[name] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", output)[1]), "binary_sha256": sha(binary)}
        control_binary = out / ("control-" + name)
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, "-I", ROOT / "native", "-I", SOURCE,
             SOURCE / "control_step.c", ROOT / "tests/native_control_step_test.c", "-o", control_binary], "control-" + name + "-build.log")
        output = run([control_binary], "control-" + name + "-run.log")
        control_variants[name] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", output)[1]), "binary_sha256": sha(control_binary)}
    atomic_host_flags = ["-mcx16"] if platform.machine() in ("x86_64", "AMD64") else []
    for name, flags in modes:
        binary = out / ("atomic-" + name)
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, *atomic_host_flags,
             "-I", ROOT / "native", "-I", SOURCE, SOURCE / "atomic_step.c", ROOT / "tests/native_atomic_step_test.c",
             "-pthread", "-o", binary], "atomic-" + name + "-build.log")
        output = run([binary], "atomic-" + name + "-run.log")
        atomic_variants[name] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", output)[1]), "stdout": output.strip(), "binary_sha256": sha(binary)}
    target = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    mapping = out / "exports.map"
    mapping.write_text("NATIVE_GAME_BLOCK_PLAN_1 { global: ngb_*; local: *; };\n")
    library = out / "libnative_game_block_plan.so"
    run([target / "aarch64-linux-android30-clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
         "-fPIC", "-ffixed-x18", "-shared", "-I", ROOT / "native", "-I", SOURCE, "-I", args.capstone_include,
         SOURCE / "block_plan.c", SOURCE / "control_step.c", SOURCE / "atomic_step.c", args.android_capstone, "-Wl,--no-undefined",
         "-Wl,--version-script=" + str(mapping), "-Wl,-z,max-page-size=16384", "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack",
         "-o", library], "android-build.log")
    exports = run([target / "llvm-nm", "--dynamic", "--defined-only", library], "exports.txt")
    symbols = [line.split()[-1].split("@")[0] for line in exports.splitlines()]
    expected = sorted(["ngb_decoder_init", "ngb_decoder_destroy", "ngb_classify", "ngb_plan_block", "ngb_plan_block_context", "ngb_status_string", "ngb_exit_string", "ngb_control_step", "ngb_atomic_step", "ngb_atomic_monitor_clear"])
    if sorted(symbols) != expected:
        raise RuntimeError("Unexpected native-block planner exports: " + repr(symbols))
    if hashes != {str(p.relative_to(ROOT)): sha(p) for p in source_files}:
        raise RuntimeError("Planner sources changed during verification")
    report = {"schema": 1, "host_tests": variants, "control_host_tests": control_variants, "atomic_host_tests": atomic_variants,
              "reused_verification": inherited, "atomic_host_flags": atomic_host_flags, "android_cross_link_passed": True,
              "source_sha256": hashes, "header_sha256": {str(p): sha(p) for p in headers},
              "android_capstone_sha256": sha(args.android_capstone), "android_library_sha256": sha(library),
              "exports": symbols, "device_execution": False, "game_execution": False,
              "scope": "actual Capstone decoder/register metadata; synthetic instruction matrix; conservative native prefix planning only"}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"host_tests_reused": bool(inherited), "host_tests": variants, "control_host_tests": control_variants, "atomic_host_tests": atomic_variants, "android_cross_link_passed": True, "device_execution": False}, indent=2))


if __name__ == "__main__":
    main()
