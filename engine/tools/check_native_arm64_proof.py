#!/usr/bin/env python3
"""Build/check an asset-free native ARM64 bridge proof; never access ADB or game files."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "native/native-arm64-proof"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(arguments, output):
    result = subprocess.run([str(x) for x in arguments], check=False, capture_output=True, text=True, timeout=60)
    output.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Check failed ({result.returncode}); inspect {output.relative_to(ROOT)}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    out = ROOT / "build/native-arm64-proof"
    out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("clang")
    if not cc:
        raise RuntimeError("Installed clang is required; nothing is downloaded")
    reports = {}
    for kind, flags in (("optimized", ["-O2"]),
                        ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / ("rewrite-" + kind)
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, "-I", SOURCE,
             SOURCE / "rewrite.c", ROOT / "tests/native_arm64_proof_test.c", "-o", binary], out / (kind + "-compile.log"))
        report = run([binary], out / (kind + "-run.log"))
        reports[kind] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", report)[1])}

    toolchain = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    android_cc = toolchain / "aarch64-linux-android30-clang"
    if not android_cc.is_file() or not (args.ndk / "source.properties").is_file():
        raise RuntimeError("Installed NDK r27c is required; nothing is downloaded")
    if "27.2.12479018" not in (args.ndk / "source.properties").read_text():
        raise RuntimeError("Use the already qualified NDK r27c")
    library = out / "libnative_arm64_proof.so"
    common = [android_cc, "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fPIC", "-ffixed-x18",
              "-fvisibility=hidden", "-mbranch-protection=none", "-pthread", "-I", SOURCE,
              SOURCE / "rewrite.c", SOURCE / "proof.c", SOURCE / "bridge.S",
              "-Wl,-z,max-page-size=16384", "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack"]
    run([*common, "-shared", "-Wl,--no-undefined", "-Wl,--version-script=" + str(SOURCE / "exports.map"),
         "-Wl,-soname,libnative_arm64_proof.so", "-o", library], out / "android-library-build.log")
    executable = out / "native_arm64_proof"
    run([*common, SOURCE / "runner.c", "-pie", "-o", executable], out / "android-executable-build.log")
    exported = run([toolchain / "llvm-nm", "--dynamic", "--defined-only", library], out / "android-exports.txt")
    symbols = [line.split() for line in exported.splitlines() if line.split()]
    callable_exports = [parts[-1].split("@")[0] for parts in symbols if parts[-2] != "A"]
    if callable_exports != ["native_arm64_proof_run"]:
        raise RuntimeError("Unexpected DSO export surface")
    disassembly = run([toolchain / "llvm-objdump", "--no-show-raw-insn", "-d", library], out / "android-disassembly.txt")
    # Review the actual compiled code for writes to the host's platform register
    # and TLS system registers; reads are intentionally allowed.
    instructions = []
    for line in disassembly.splitlines():
        matched = re.match(r"\s*[0-9a-f]+:\s+([a-z0-9.]+)\s+(.*)", line)
        if matched:
            instructions.append(tuple(x.lower() for x in matched.groups()))
    if not instructions or not any(op == "mrs" and "tpidr_el0" in operands for op, operands in instructions):
        raise RuntimeError("Disassembly parser did not find the expected host TLS reads")
    bad = [(op, operands) for op, operands in instructions
           if (op == "msr" and re.match(r"tpidr(?:ro)?_el0\b", operands))
           or (re.match(r"[xw]18(?:,|$)", operands) and op not in ("str", "stur", "stlr", "stp", "stnp"))
           or (op in ("ldp", "ldnp") and re.match(r"[^,]+,\s*[xw]18\b", operands))
           or re.search(r"\[x18(?:,[^]]*)?\](?:!|,\s*#)", operands)]
    if bad:
        raise RuntimeError("Compiled proof writes actual x18/TLS: " + repr(bad))
    # This launcher is an artifact for coordinator execution with an existing
    # Android Python. It is never run by this host build utility.
    launcher = out / "run_probe.py"
    launcher.write_text('''import ctypes,json,sys\nlib=ctypes.CDLL(sys.argv[1])\nprobe=lib.native_arm64_proof_run\nprobe.argtypes=[ctypes.POINTER(ctypes.c_char),ctypes.c_size_t]\nprobe.restype=ctypes.c_int\noutput=ctypes.create_string_buffer(2048)\nstatus=probe(output,len(output))\nprint(output.value.decode(),flush=True)\nraise SystemExit(status!=0)\n''')
    record = {"schema": 1, "host_whitelist_tests": reports,
              "android_ndk": "27.2.12479018", "android_api": 30,
              "android_cross_link_passed": True, "android_execution": "pending-coordinator-device-run",
              "exported_api": callable_exports, "compiled_host_x18_tls_writes": bad,
              "sources_sha256": {str(path.relative_to(ROOT)): sha(path) for path in
                                  sorted([*SOURCE.glob("*"), ROOT / "tests/native_arm64_proof_test.c", Path(__file__).resolve()]) if path.is_file()},
              "artifacts_sha256": {str(path.relative_to(ROOT)): sha(path) for path in (library, executable, launcher)},
              "limits": ["synthetic whitelist only", "no game code or arbitrary ARM64 rewriting",
                         "atomic kernels are original assembly, not transformed exclusive sequences",
                         "signal handler is specific to Linux arm64 ucontext", "no GPU or power/performance conclusion"]}
    (out / "verification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"verification": str((out / "verification.json").relative_to(ROOT)),
                      "library_sha256": sha(library), "host_tests": reports,
                      "android_execution": record["android_execution"]}, indent=2))


if __name__ == "__main__":
    main()
