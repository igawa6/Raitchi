#!/usr/bin/env python3
"""Offline host tests and Android arm64 compile-only checks of provisional ABI."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(command):
    subprocess.run([str(arg) for arg in command], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--ndk", type=Path, default=Path(
        os.environ.get("ANDROID_NDK_HOME") or
        Path.home() / "android-sdk/ndk/27.2.12479018"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/module-abi")
    args = parser.parse_args()
    if not args.clang:
        parser.error("Host clang was not found")
    arm64 = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
    if not arm64.is_file():
        parser.error("Offline NDK arm64/API30 compiler not found; pass --ndk")
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Build output must stay under the ignored workspace build directory")
    output.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", ROOT / "native"]
    sources = [ROOT / "native/module_abi.c", ROOT / "tests/module_abi_test.c"]
    executable = output / "module_abi_test"
    run([args.clang, *flags, "-O2", *sources, "-o", executable])
    run([executable])
    sanitized = output / "module_abi_test_sanitized"
    run([args.clang, *flags, "-O1", "-g", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", *sources, "-o", sanitized])
    run([sanitized])
    # Compile the same contract assertions and test TU for the actual target.
    # No Android execution, native shared library, linking or game build occurs.
    for source in sources:
        run([arm64, *flags, "-O2", "-c", source, "-o", output / (source.stem + ".android-arm64.o")])
    # nm verifies the validator remains standalone: no hidden allocation/libc calls.
    nm = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
    undefined = subprocess.check_output([str(nm), "--undefined-only", str(output / "module_abi.android-arm64.o")], text=True)
    if undefined.strip():
        raise RuntimeError("Validator unexpectedly needs external symbols:\n" + undefined)
    print("Android arm64/API30: validator and synthetic tests compile; validator has no undefined symbols.")
    print("Compile-only target check; real runtime integration and Android execution remain pending.")


if __name__ == "__main__":
    main()
