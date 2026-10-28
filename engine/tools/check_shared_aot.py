#!/usr/bin/env python3
"""Verify the shared AOT execution seam offline; no games or device actions."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PROFILES = (("two-module", 2, 1), ("four-module", 4, 2), ("profiling", 4, 6))
EXECUTION_CASES = ("execution", "cache", "overrides", "hle", "hooks", "instruction",
                   "profiling", "threads", "helpers", "gate-caps", "gate-legacy",
                   "gate-metadata", "second-bind", "dynamic-hle", "backend",
                   "host-lookup-capacity", "host-lookup-zero", "host-lookup-concurrent", "host-lookup-workload",
                   "host-lookup-collisions")
SESSION_CASES = ("execution", "layout", "profile", "caps", "legacy", "mapping", "metadata", "missing",
                 "profile-text", "profile-digest", "profile-order", "concurrent", "reserve-ok", "reserve-fail")
CORE = ["native/module_abi.c", "native/module_loader.c", "native/runtime/aot_dispatch.c", "native/runtime/native_manifest.c",
        "native/runtime/aot_hooks.c", "native/runtime/aot_libc.c",
        "native/runtime/session_layout.c", "native/runtime/shared_aot.c",
        "native/runtime/nso_image.c", "native/runtime/title_manifest.c", "native/runtime/guest_path.c",
        "native/adapters/title_adapter.c", "native/adapters/las/profile.c",
        "native/adapters/letsgo/profile.c", "native/adapters/echoes/profile.c", "native/adapters/wonder/profile.c", "native/adapters/letsgo/scene.c"]
COMPONENTS = (("libc", "tests/aot_libc_test.c", ((),)),
              ("nso", "tests/nso_image_test.c", ((),)),
              ("manifest", "tests/title_manifest_test.c", ((),)),
              ("path", "tests/guest_path_test.c", ((),)),
              ("adapter", "tests/title_adapter_test.c", (("las",), ("eevee",), ("concurrent",))))


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, timeout=45, **kwargs)


def build(compiler, output, sanitized=False, android=False):
    output.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-I", ROOT / "native",
             "-O1" if sanitized else "-O2"]
    if sanitized:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    page = ["-Wl,-z,max-page-size=16384"] if android else []
    strict = [] if sanitized else ["-Wl,--no-undefined"]
    shared = output / "libshared_aot.so"
    # There are deliberately no title/profile defines in the common artifact.
    run([compiler, *flags, "-shared", "-fPIC", "-fvisibility=hidden", *[ROOT / p for p in CORE],
         "-Wl,-soname,libshared_aot.so", *strict, *page, "-ldl", "-pthread", "-o", shared])
    products = []
    for name, modules, caps in PROFILES:
        selected = [f"-DFIXTURE_MODULE_COUNT={modules}", f"-DAOT_PROFILE_CAPABILITIES={caps}"]
        fixture = output / f"{name}.so"
        link = ["-L", output, "-lshared_aot", "-Wl,-rpath,$ORIGIN", *page, "-ldl", "-pthread"]
        run([compiler, *flags, *selected, "-shared", "-fPIC", "-fvisibility=hidden",
             ROOT / "tests/aot_execution_fixture.c", *strict, *link, "-o", fixture])
        runners = {}
        for label, source in (("execution", "tests/aot_execution_test.c"),
                              ("session", "tests/shared_aot_session_test.c")):
            runner = output / f"{name}-{label}"
            run([compiler, *flags, *selected, ROOT / source, *link, "-o", runner])
            runners[label] = runner
        products.append((name, fixture, runners))
    layout = output / "session-layout-test"
    run([compiler, *flags, ROOT / "tests/session_layout_test.c", "-L", output,
         "-lshared_aot", "-Wl,-rpath,$ORIGIN", *page, "-pthread", "-o", layout])
    components = []
    for name, source, arguments in COMPONENTS:
        runner = output / f"{name}-test"
        run([compiler, *flags, ROOT / source, "-L", output,
             "-lshared_aot", "-Wl,-rpath,$ORIGIN", *page, "-pthread", "-o", runner])
        components.append((name, runner, arguments))
    return shared, products, layout, components


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--ndk", type=Path, default=Path(os.environ.get("ANDROID_NDK_HOME") or
        Path.home() / "android-sdk/ndk/27.2.12479018"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/shared-aot")
    args = parser.parse_args()
    if not args.clang:
        parser.error("Host Clang is required")
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Output must stay under the ignored workspace build directory")
    toolchain = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    android = toolchain / "aarch64-linux-android30-clang"
    if not android.is_file():
        parser.error("Offline Android arm64/API30 compiler not found")
    report = {"verified_utc": datetime.now(timezone.utc).isoformat(),
              "game_execution": False, "synthetic_execution": True, "device_actions": False, "variants": {}}
    for name, sanitized in (("host", False), ("sanitized", True)):
        shared, products, layout, components = build(args.clang, output / name, sanitized=sanitized)
        digest = sha(shared)
        cases = []
        for profile, fixture, runners in products:
            for label, names in (("execution", EXECUTION_CASES), ("session", SESSION_CASES)):
                for case in names:
                    result = run([runners[label], fixture, case], capture_output=True, text=True)
                    print(f"{name}/{profile}/{label}/{case}: {result.stdout.strip()}", flush=True)
                    cases.append({"profile": profile, "suite": label, "case": case,
                                  "stdout": result.stdout.strip()})
        for profile in ("las", "letsgo", "future"):
            result = run([layout, profile], capture_output=True, text=True)
            print(f"{name}/layout/{profile}: {result.stdout.strip()}", flush=True)
            cases.append({"suite": "layout", "case": profile, "stdout": result.stdout.strip()})
        for suite, runner, arguments in components:
            for selected in arguments:
                result = run([runner, *selected], capture_output=True, text=True)
                print(f"{name}/{suite}/{'/'.join(selected)}: {result.stdout.strip()}", flush=True)
                cases.append({"suite": suite, "arguments": selected, "stdout": result.stdout.strip()})
        if sha(shared) != digest:
            raise RuntimeError("Common core changed during profile tests")
        report["variants"][name] = {"shared_core_sha256": digest, "cases": cases}

    shared, products, layout, components = build(android, output / "android-arm64", android=True)
    elf = run([toolchain / "llvm-readelf", "--file-header", "--program-headers", "--dynamic",
               "--dyn-syms", "--wide", shared], capture_output=True, text=True).stdout
    (output / "android-arm64/core-elf.txt").write_text(elf)
    if "AArch64" not in elf or "libshared_aot.so" not in elf:
        raise RuntimeError("Unexpected shared-core ELF target/SONAME")
    report["variants"]["android-arm64"] = {"shared_core_sha256": sha(shared),
        "execution": "not run", "linked_fixture_profiles": [p[0] for p in products],
        "layout_runner_sha256": sha(layout),
        "component_runner_sha256": {suite: sha(runner) for suite, runner, _ in components}}
    sources = [*map(lambda p: ROOT / p, CORE), *sorted((ROOT / "native/runtime").glob("*.h")),
               ROOT / "tests/aot_execution_fixture.c", ROOT / "tests/aot_execution_test.c",
               ROOT / "tests/shared_aot_session_test.c", ROOT / "tests/session_layout_test.c",
               *[ROOT / source for _, source, _ in COMPONENTS],
               *sorted((ROOT / "native/adapters").rglob("*.h")),
               Path(__file__).resolve(), ROOT / "native/module_abi.h", ROOT / "native/module_loader.h"]
    report["source_sha256"] = {str(p.relative_to(ROOT)): sha(p) for p in sources}
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Android shared core, three fixture profiles and all runners link; no Android execution.")
    print("Synthetic component execution only; full Android runtime/artifacts are checked separately, with device gameplay pending.")


if __name__ == "__main__":
    main()
