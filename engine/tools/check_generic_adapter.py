#!/usr/bin/env python3
"""Synthetic generic-adapter checks (derivation, rejection, preparation, package
metadata JSON, once-per-process install/selection). No game files or keys.

Builds below build/generic-adapter/ or --out. With --android, also compiles the
changed launcher/runtime translation units with the Android NDK when present.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/generic-adapter"
C_SOURCES = ["tests/generic_adapter_test.c", "native/adapters/generic/generic.c",
             "native/adapters/title_adapter.c", "native/adapters/las/profile.c",
             "native/adapters/letsgo/profile.c", "native/adapters/echoes/profile.c",
             "native/adapters/wonder/profile.c", "native/module_abi.c",
             "native/runtime/native_manifest.c", "native/runtime/session_layout.c",
             "native/runtime/nso_image.c", "native/runtime/native_prepare.c",
             "ports/shared-android/runtime/native_package_metadata.c"]
HASH = "ports/shared-android/runtime/shared_hash.cpp"
INCLUDES = ["-I", ROOT / "native", "-I", ROOT / "ports/shared-android/runtime"]
VARIANTS = [("optimized", ["-O2"]),
            ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
NDK = Path.home() / "android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"


def run(command, env=None):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=300, env=env)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command[:2])) + "\n" + result.stdout + result.stderr)
    return result.stdout.strip()


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--android", action="store_true", help="also compile changed TUs with NDK r27c if installed")
    parser.add_argument("--out", type=Path, default=OUT, help="directory for synthetic build and verification artifacts")
    args = parser.parse_args()
    OUT = args.out.resolve()
    cc, cxx = shutil.which("clang"), shutil.which("clang++")
    if not cc or not cxx:
        raise RuntimeError("clang and clang++ are required")
    OUT.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("prepare", ROOT / "tools/prepare_native_game.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    contract = module.contract_digest().hex()
    env = os.environ.copy()
    env.update({"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
    results = {}
    for name, flags in VARIANTS:
        objects = []
        for index, source in enumerate(C_SOURCES):
            obj = OUT / f"{name}-{index}.o"
            run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, *INCLUDES, "-c", ROOT / source, "-o", obj])
            objects.append(obj)
        obj = OUT / f"{name}-hash.o"
        run([cxx, "-std=c++17", *flags, "-I", ROOT / "native", "-I", ROOT / "sources/las-android/runtime",
             "-c", ROOT / HASH, "-o", obj])
        objects.append(obj)
        binary = OUT / f"generic-{name}"
        run([cxx, *flags, *objects, "-pthread", "-o", binary])
        for mode, extra in (("install", {}), ("select-first", {"GENERIC_TEST_SELECT_FIRST": "1"})):
            with tempfile.TemporaryDirectory(prefix="generic-", dir=OUT) as tmp:
                results[f"{name}/{mode}"] = run([binary, tmp, contract], env={**env, **extra})
                print(f"{name}/{mode}: {results[f'{name}/{mode}']}", flush=True)
    android = "not requested"
    if args.android:
        if not NDK.is_file():
            android = "skipped: NDK r27c not installed"
        else:
            generated = ROOT / "build/shared-android/include"
            units = [("native/adapters/generic/generic.c", []), ("native/adapters/title_adapter.c", []),
                     ("native/runtime/native_prepare.c", []),
                     ("ports/shared-android/runtime/native_package_metadata.c", []),
                     ("ports/shared-android/runtime/nxpkg.c", [])]
            if (generated / "shared_contract.h").is_file():
                units.append(("ports/shared-android/runtime/native_package_prepare_android.c", ["-I", generated]))
            for source, extra in units:
                run([NDK, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-O2", *INCLUDES, *extra,
                     "-c", ROOT / source, "-o", OUT / (Path(source).stem + ".android.o")])
            android = "compiled: " + ", ".join(s for s, _ in units)
        print("android: " + android, flush=True)
    paths = [ROOT / p for p in [*C_SOURCES, HASH, "native/adapters/generic/generic.h",
                                "native/adapters/title_adapter.h", "ports/shared-android/runtime/native_package_metadata.h",
                                "ports/shared-android/runtime/nxpkg.c"]] + [Path(__file__).resolve()]
    report = {"schema": 1, "state": "passed", "results": results, "android": android,
              "scope": "Synthetic NSO/NPDM/PFS0 metadata only: generic derivation and rejections, shared native "
                       "layout preflight, native preparation/manifest, nxpkg ExeFS view, metadata JSON tryable flag, "
                       "install/selection once per process. No game files, keys, device or guest execution.",
              "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
    (OUT / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Generic adapter checks passed.")


if __name__ == "__main__":
    main()
