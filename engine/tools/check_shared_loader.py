#!/usr/bin/env python3
"""Offline synthetic checks of the actual shared NSO loader; no games/devices.

The harness uses current overlay/pinned headers and fake platform/HLE/dispatch
callbacks. Its SHA provider is a labeled test stub, not a cryptographic test.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ("valid", "host-peer", "order", "hash", "mod0", "dynamic", "name", "unknown",
             "packed", "destination", "symbol", "strong", "array")
NATIVE_SCENARIOS = ("valid", "order", "hash", "mod0", "dynamic", "name", "unknown",
    "packed", "destination", "symbol", "strong", "array", "init-outside", "init-unaligned",
    "fini-outside", "array-hle", "fini-array-outside", "fini-array-unaligned", "decode", "build", "range",
    "last-fini-outside", "last-array-outside", "last-hash", "last-decode", "late-relocation")
# Every tag rejected by the production relocation-format guard. Values include
# zero (still rejected) and a full-width word (must survive diagnostics intact).
REJECTED_DYNAMIC_TAGS = (17, 18, 19,
    0x6000000f, 0x60000010, 0x60000011, 0x60000012,
    0x6fffe000, 0x6fffe001, 0x6fffe002, 0x6fffe003)
RELR_SUCCESS = ("relr-valid", "relr-last-valid", "relr-empty", "relr-only",
                "relr-metadata-snapshot", "relr-none-overlap", "relr-empty-bitmap", "relr-max-addend")
RELR_DIAGNOSTICS = {
    "relr-conflicting-tag": "conflicting dynamic tag 36",
    **dict.fromkeys(("relr-missing-address", "relr-missing-size", "relr-missing-entsize",
                    "relr-bad-entsize", "relr-table-unaligned", "relr-bad-size"),
                   "invalid/missing ELF64 RELR metadata"),
    **dict.fromkeys(("relr-table-unbacked", "relr-table-cross-end"), "metadata outside initialized image"),
    "relr-table-count-limit": "RELR table entry count exceeds limit",
    "relr-count-limit": "RELR decoded relocation count exceeds limit",
    **dict.fromkeys(("relr-table-rela-overlap", "relr-table-plt-overlap"), "overlapping RELR/RELA tables"),
    "relr-bitmap-first": "RELR bitmap before address",
    **dict.fromkeys(("relr-target-unaligned", "relr-target-gap", "relr-target-padding",
                    "relr-target-outside", "relr-last-bad"), "unaligned/out-of-image RELR destination"),
    "relr-cursor-overflow": "RELR address cursor overflow",
    "relr-addend-overflow": "RELR implicit addend overflow",
    **dict.fromkeys(("relr-duplicate", "relr-bitmap-duplicate"), "duplicate RELR destination"),
    **dict.fromkeys(("relr-rela-overlap", "relr-plt-overlap", "relr-partial-before", "relr-partial-after"),
                   "overlapping RELR/RELA relocation destinations"),
}
GNU_SUCCESS = ("gnu-valid", "gnu-all-valid", "gnu-last-valid", "gnu-empty", "gnu-zero-buckets",
               "gnu-repeat", "gnu-coexist", "gnu-sysv-authoritative", "gnu-metadata-snapshot", "gnu-relr-valid")
GNU_DIAGNOSTICS = {
    "gnu-conflicting-tag": "conflicting DT_GNU_HASH dynamic tag",
    **dict.fromkeys(("gnu-missing-hash", "gnu-bad-syment"), "missing/unsupported dynsym metadata"),
    **dict.fromkeys(("gnu-header-unaligned", "gnu-symbol-unaligned"), "unaligned GNU hash/dynsym metadata"),
    **dict.fromkeys(("gnu-header-unbacked", "gnu-header-truncated"), "metadata outside initialized image"),
    **dict.fromkeys(("gnu-bloom-zero", "gnu-bloom-not-power2", "gnu-bloom-cap", "gnu-shift", "gnu-buckets-cap"),
                   "invalid/oversized GNU hash header"),
    **dict.fromkeys(("gnu-symbol-unbacked", "gnu-symoffset-zero", "gnu-symoffset-capacity"),
                   "GNU hash symoffset outside dynsym capacity"),
    **dict.fromkeys(("gnu-bloom-truncated", "gnu-buckets-truncated"), "GNU hash bloom/buckets outside initialized metadata"),
    **dict.fromkeys(("gnu-bucket-prefix", "gnu-bucket-capacity"), "GNU hash bucket outside dynsym capacity"),
    **dict.fromkeys(("gnu-chain-unbacked", "gnu-chain-unterminated", "gnu-chain-dynsym-capacity", "gnu-last-bad"),
                   "GNU hash chain has no bounded terminator"),
    "gnu-chain-boundary": "GNU hash bucket starts inside a chain",
    "gnu-name": "unterminated/out-of-range symbol name",
    "gnu-strong": "unresolved strong import lost",
    "gnu-symbol": "relocation symbol index out of range",
}
DIAGNOSTICS = {
    **RELR_DIAGNOSTICS,
    **GNU_DIAGNOSTICS,
    "order": "unsupported NSO segment order",
    "hash": "complete original NSO SHA256 mismatch",
    "mod0": "invalid MOD0",
    "dynamic": "dynamic table has no bounded DT_NULL",
    "name": "unterminated/out-of-range symbol name",
    "unknown": "unsupported AArch64 relocation type 999",
    "packed": "unsupported REL/RELR/packed dynamic relocation format",
    "destination": "relocation destination outside module",
    "symbol": "relocation symbol index out of range",
    "strong": "unresolved strong import lost",
    "array": "init/fini array target outside admitted code",
    "init-outside": "initializer outside admitted code",
    "init-unaligned": "initializer outside admitted code",
    "fini-outside": "initializer outside admitted code",
    "array-hle": "init/fini array target outside admitted code",
    "fini-array-outside": "init/fini array target outside admitted code",
    "fini-array-unaligned": "init/fini array target outside admitted code",
    "decode": "NSO segment outside file payload",
    "build": "full NSO build ID mismatch",
    "range": "independent module expectations rejected",
    "last-fini-outside": "initializer outside admitted code",
    "last-array-outside": "init/fini array target outside admitted code",
    "last-hash": "complete original NSO SHA256 mismatch",
    "last-decode": "NSO segment outside file payload",
    "late-relocation": "unsupported AArch64 relocation type 999",
}
SOURCES = ("tests/shared_nso_loader_test.c", "native/module_abi.c", "native/runtime/nso_image.c", "native/runtime/native_manifest.c")
INPUTS = (*SOURCES, "ports/shared-android/runtime/loader.c", "ports/shared-android/runtime/rt.h",
          "ports/shared-android/runtime/cpu.h", "ports/shared-android/runtime/common_session.h",
          "native/module_abi.h", "native/runtime/aot_cpu.h", "native/runtime/session_layout.h",
          "native/runtime/native_manifest.h", "native/runtime/title_manifest.h", "native/runtime/nso_image.h", "native/adapters/title_adapter.h",
          "sources/las-android/runtime/platform.h", "tools/check_shared_loader.py")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, timeout=45, **kwargs)


def build(compiler, output, sanitized=False, android=False):
    output.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-O1" if sanitized else "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-format", "-I", ROOT, "-I", ROOT / "ports/shared-android/runtime",
             "-I", ROOT / "sources/las-android/runtime", "-I", ROOT / "native"]
    if sanitized:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if android:
        flags += ["-Wl,--no-undefined", "-Wl,-z,max-page-size=16384"]
    executable = output / "shared_nso_loader_test"
    run([compiler, *flags, *[ROOT / source for source in SOURCES], "-o", executable])
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--ndk", type=Path, default=Path(os.environ.get("ANDROID_NDK_HOME") or
        Path.home() / "android-sdk/ndk/27.2.12479018"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/shared-loader")
    parser.add_argument("--host-only", action="store_true",
                        help="Run optimized and sanitized synthetic host checks without an Android build")
    args = parser.parse_args()
    if not args.clang:
        parser.error("Host Clang required")
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Output must remain under the ignored workspace build directory")
    toolchain = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    target = toolchain / "aarch64-linux-android30-clang"
    if not args.host_only and not target.is_file():
        parser.error("Offline NDK ARM64/API30 compiler required")
    missing = [name for name in INPUTS if not (ROOT / name).is_file()]
    if missing:
        parser.error("Missing staged/pinned source inputs: " + ", ".join(missing))
    before = {name: sha(ROOT / name) for name in INPUTS}
    report = {"verified_utc": datetime.now(timezone.utc).isoformat(), "game_inputs": False,
              "device_actions": False, "synthetic_execution": True, "sha_provider": "test stub",
              "source_sha256": before, "variants": {}}
    for name, sanitized in (("host", False), ("sanitized", True)):
        executable = build(args.clang, output / name, sanitized=sanitized)
        results = []
        cases=[(False,modules,scenario) for modules in (2,4) for scenario in SCENARIOS]
        cases += [(True,4,scenario) for scenario in NATIVE_SCENARIOS]
        for native, modules in ((False, 2), (False, 4), (True, 4)):
            cases.extend((native, modules, scenario) for scenario in
                         (*RELR_SUCCESS, *RELR_DIAGNOSTICS, *GNU_SUCCESS, *GNU_DIAGNOSTICS))
            for index, tag in enumerate(REJECTED_DYNAMIC_TAGS):
                value = 0xfedcba9876543210 if index % 2 else 0
                module = modules - 1 if index % 2 else 0
                cases.append((native, modules, f"reject-tag-{tag:x}-{value:x}-{module}"))
        for native,modules,scenario in cases:
            requested=("native-" if native else "")+scenario
            fixtures=output/name/f"{modules}-{requested}"
            fixtures.mkdir(parents=True,exist_ok=True)
            result=subprocess.run([str(executable),requested,str(modules),str(fixtures)],capture_output=True,text=True,timeout=30)
            wanted=0 if scenario in ("valid","host-peer",*RELR_SUCCESS,*GNU_SUCCESS) else 42
            if result.returncode!=wanted:
                raise RuntimeError(f"{name}/{modules}/{requested}: expected {wanted}, got {result.returncode}\n{result.stdout}\n{result.stderr}")
            if scenario in DIAGNOSTICS and DIAGNOSTICS[scenario] not in result.stderr:
                raise RuntimeError(f"{name}/{modules}/{requested}: rejected for unexpected reason:\n"+result.stderr)
            if scenario.startswith("reject-tag-"):
                tag_hex, value_hex, module_text = scenario.removeprefix("reject-tag-").split("-")
                module = int(module_text)
                names = ("main", "subsdk0", "subsdk1", "sdk") if native else ("main", "sdk", "subsdk0", "subsdk1")
                # The appended entry follows 11 AOT or 14 native fixture tags.
                entry = 0x1e0 if native else 0x1b0
                expected = (f"{names[module]}: unsupported REL/RELR/packed dynamic relocation format "
                            f"(tag=0x{tag_hex} value=0x{value_hex} entry=0x{entry:x})")
                if expected not in result.stderr:
                    raise RuntimeError(f"{name}/{modules}/{requested}: missing exact relocation diagnostic:\n"+result.stderr)
            if native and wanted==0 and "generated_dispatch_calls=0" not in result.stdout:
                raise RuntimeError("Native case did not establish absent generated admission")
            print(f"{name}/{modules}/{requested}: pass (exit {wanted})",flush=True)
            results.append({"native_only":native,"module_count":modules,"scenario":requested,"exit":result.returncode,"stdout":result.stdout,"stderr":result.stderr})
        report["variants"][name] = {"runner_sha256": sha(executable), "scenarios": results}
    if not args.host_only:
        executable = build(target, output / "android-arm64", android=True)
        elf = run([toolchain / "llvm-readelf", "--file-header", "--program-headers", "--wide", executable],
                  capture_output=True, text=True).stdout
        if "AArch64" not in elf:
            raise RuntimeError("Unexpected Android ELF target")
        (output / "android-arm64/runner-elf.txt").write_text(elf)
        report["variants"]["android-arm64"] = {"runner_sha256": sha(executable), "execution": "not run"}
    if before != {name: sha(ROOT / name) for name in INPUTS}:
        raise RuntimeError("Source changed during verification; rerun against a stable snapshot")
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{len(SCENARIOS) * 2} AOT + {len(NATIVE_SCENARIOS)} native-only + "
          f"{len(REJECTED_DYNAMIC_TAGS) * 3} relocation-diagnostic + "
          f"{(len(RELR_SUCCESS) + len(RELR_DIAGNOSTICS)) * 3} RELR + "
          f"{(len(GNU_SUCCESS) + len(GNU_DIAGNOSTICS)) * 3} GNU hash scenarios pass per host variant.")
    if not args.host_only:
        print("Actual loader + current headers link for Android ARM64/API30.")
    print("Synthetic buffers/fake platform-HLE-SHA only; no real title or Android execution established.")


if __name__ == "__main__":
    main()
