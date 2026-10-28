#!/usr/bin/env python3
"""Check actual host-pointer Unicorn mapping; build Android probe, no ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests/native_virtual_memory_probe.c"
INCLUDE = ROOT / "build/shared-ondevice/work/build/ondevice/src/unicorn-2.1.4/src/include"
HOST_LIBRARY = ROOT / "work/las-baseline/build/android/venv/lib/python3.14/site-packages/unicorn/lib/libunicorn.so.2"
ANDROID_LIBRARY = ROOT / "build/shared-android/apk-inputs/jniLibs/arm64-v8a/libunicorn.so"
RUNNER = '''import ctypes,hashlib,json,sys
config=json.load(open(sys.argv[1]));helper,unicorn=sys.argv[2:4]
for path,key in ((helper,'helper_sha256'),(unicorn,'unicorn_sha256')):
 with open(path,'rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
 if digest!=config[key]: raise SystemExit('SHA256 mismatch: '+key)
ctypes.CDLL(unicorn,mode=ctypes.RTLD_GLOBAL)
lib=ctypes.CDLL(helper,mode=ctypes.RTLD_LOCAL);probe=lib.native_virtual_memory_probe
probe.argtypes=[ctypes.c_void_p,ctypes.c_size_t];probe.restype=ctypes.c_int
output=ctypes.create_string_buffer(4096);status=probe(output,len(output))
report=json.loads(output.value);report.update(config);print(json.dumps(report),flush=True)
raise SystemExit(status!=0)
'''


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, log):
    result = subprocess.run(list(map(str, command)), check=False, capture_output=True, text=True, timeout=30)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("Probe command failed; inspect " + str(log.relative_to(ROOT)))
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    out = ROOT / "build/native-virtual-memory"; out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("clang")
    if not cc or not HOST_LIBRARY.is_file() or not ANDROID_LIBRARY.is_file():
        raise RuntimeError("Cached host Clang and host/Android Unicorn libraries required")
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", INCLUDE]
    host = out / "host-probe"
    run([cc, *flags, "-DNATIVE_VIRTUAL_MEMORY_MAIN", SOURCE, HOST_LIBRARY,
         "-Wl,-rpath," + str(HOST_LIBRARY.parent), "-o", host], out / "host-build.log")
    host_report = json.loads(run([host], out / "host-run.json"))
    tools = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    library = out / "libnative_virtual_memory_probe.so"
    exports = out / "exports.map"
    exports.write_text("NATIVE_VIRTUAL_MEMORY_1 { global: native_virtual_memory_probe; local: *; };\n")
    run([tools / "aarch64-linux-android30-clang", *flags, "-fPIC", "-fvisibility=hidden", "-shared",
         SOURCE, "-L", ANDROID_LIBRARY.parent, "-lunicorn", "-Wl,--no-undefined",
         "-Wl,--version-script=" + str(exports), "-Wl,-z,max-page-size=16384", "-o", library], out / "android-build.log")
    config = {"helper_sha256": sha(library), "unicorn_sha256": sha(ANDROID_LIBRARY)}
    (out / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    (out / "run_probe.py").write_text(RUNNER)
    record = {"schema": 1, "host": host_report, "android_cross_link_passed": True,
              "device_execution": "pending-coordinator", "device_actions": False,
              "source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in (SOURCE, Path(__file__).resolve(), INCLUDE / "unicorn/unicorn.h", INCLUDE / "unicorn/arm64.h")},
              "host_unicorn_sha256": sha(HOST_LIBRARY), "artifacts": config,
              "scope": "actual Unicorn with generic ARM64 code and actual shared host backing; observed pointer width reported per platform; no game or production kernel"}
    (out / "verification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"host_state": host_report["state"], "default_rejected_high_address": host_report["default_a72"]["rejected_high_address"],
                      "virtual_tlb_passed": host_report["virtual_tlb"]["completed"], "android_cross_link_passed": True,
                      "helper_sha256": config["helper_sha256"]}, indent=2))


if __name__ == "__main__":
    main()
