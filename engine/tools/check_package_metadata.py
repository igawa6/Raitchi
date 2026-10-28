#!/usr/bin/env python3
"""Host regression for production nxpkg metadata scan and adapter build matching.

All private inputs are explicit CLI paths. Reports under build/package-metadata/
omit the keys path and key contents; package bytes are only read.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/package-metadata"
ADAPTERS = [
    ROOT / "native/adapters/las/profile.c",
    ROOT / "native/adapters/letsgo/profile.c",
    ROOT / "native/adapters/echoes/profile.c",
    ROOT / "native/adapters/wonder/profile.c",
]
# Production launcher classification, including generic-startup tryability.
CLASSIFIER = [
    ROOT / "ports/shared-android/runtime/native_package_metadata.c",
    ROOT / "native/adapters/generic/generic.c",
    ROOT / "native/runtime/session_layout.c",
    ROOT / "native/runtime/native_manifest.c",
    ROOT / "native/module_abi.c",
]
SOURCES = [
    ROOT / "ports/shared-android/runtime/nxpkg.c",
    ROOT / "ports/shared-android/runtime/nxpkg.h",
    ROOT / "ports/shared-android/runtime/native_package_prepare_android.c",
    ROOT / "ports/shared-android/apk/app/src/main/java/com/lasrecomp/android/RaitchiMetadata.java",
    ROOT / "tests/package_metadata_probe.c",
    pathlib.Path(__file__).resolve(),
    *ADAPTERS,
    *CLASSIFIER,
]


def sha(path):
    h = hashlib.sha256()
    with pathlib.Path(path).open("rb") as f:
        for block in iter(lambda: f.read(4 << 20), b""):
            h.update(block)
    return h.hexdigest()


def parse_pairs(items, sep="="):
    result = {}
    for item in items:
        if sep not in item:
            raise argparse.ArgumentTypeError(f"expected NAME{sep}VALUE")
        name, value = item.split(sep, 1)
        if not name or not value or name in result:
            raise argparse.ArgumentTypeError(f"invalid or duplicate name in {item!r}")
        result[name] = pathlib.Path(value)
    return result


def parse_strings(items):
    result = {}
    for item in items:
        if "=" not in item:
            raise argparse.ArgumentTypeError("expected NAME=VALUE")
        name, value = item.split("=", 1)
        if not name or not value or name in result:
            raise argparse.ArgumentTypeError(f"invalid or duplicate name in {item!r}")
        result[name] = value
    return result


def run(command, log):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=300)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"{pathlib.Path(command[0]).name} failed; see {log.relative_to(ROOT)}")
    return result.stdout


def parse_nacp(path):
    data = pathlib.Path(path).read_bytes()
    if len(data) < 0x3070:
        raise ValueError("reference NACP is truncated")
    title = developer = ""
    for i in range(16):
        record = data[i * 0x300:(i + 1) * 0x300]
        if record[:1] and record[0]:
            title = record[:0x200].split(b"\0", 1)[0].decode("utf-8")
            developer = record[0x200:0x300].split(b"\0", 1)[0].decode("utf-8")
            break
    version = data[0x3060:0x3070].split(b"\0", 1)[0].decode("utf-8")
    return {"title": title, "developer": developer, "version": version}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keys", required=True, type=pathlib.Path)
    parser.add_argument("--case", action="append", default=[], metavar="NAME=NSP_OR_XCI")
    parser.add_argument("--expect-id", action="append", default=[], metavar="NAME=16_HEX_DIGIT_PROGRAM_ID")
    parser.add_argument("--expect-adapter", action="append", default=[], metavar="NAME=ADAPTER_KEY")
    parser.add_argument("--reference-dir", action="append", default=[], metavar="NAME=EXTRACTED_CONTROL_DIR")
    args = parser.parse_args()
    cases = parse_pairs(args.case)
    expected_ids = parse_strings(args.expect_id)
    expected_adapters = parse_strings(args.expect_adapter)
    references = parse_pairs(args.reference_dir)
    if not cases:
        parser.error("provide at least one --case NAME=NSP_OR_XCI")
    if not args.keys.is_file():
        parser.error("keys file is unavailable")
    if not all(p.is_file() for p in cases.values()):
        parser.error("a package input is unavailable")
    OUT.mkdir(parents=True, exist_ok=True)
    icon_dir = OUT / "icons"
    icon_dir.mkdir(exist_ok=True)
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("clang is required")
    common = ["-std=c11", "-ffunction-sections", "-fdata-sections",
              "-I", ROOT / "native", "-I", ROOT / "ports/shared-android/runtime",
              ROOT / "tests/package_metadata_probe.c",
              ROOT / "native/adapters/title_adapter.c", *ADAPTERS, *CLASSIFIER,
              "-pthread", "-Wl,--gc-sections"]
    optimized = OUT / "metadata-probe"
    run([compiler, "-O2", *common, "-o", optimized], OUT / "optimized-build.log")
    run([optimized, "--synthetic"], OUT / "synthetic-optimized.log")
    sanitized = OUT / "metadata-probe-asan"
    run([compiler, "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         *common, "-o", sanitized], OUT / "sanitized-build.log")
    env = os.environ.copy()
    env.update({"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"})
    san = subprocess.run([str(sanitized), "--synthetic"], capture_output=True, text=True, timeout=120, env=env)
    (OUT / "synthetic-sanitized.log").write_text(san.stdout + san.stderr)
    if san.returncode:
        raise RuntimeError("sanitized synthetic metadata probe failed")

    scans = {}
    for name, package in cases.items():
        icon = icon_dir / f"{name}.dat"
        raw = run([optimized, package, args.keys, icon], OUT / f"{name}-scan.log")
        result = json.loads(raw)
        if name in expected_ids and result["programIdHex"].lower() != expected_ids[name].lower().removeprefix("0x"):
            raise RuntimeError(f"{name}: package program ID differs from expected ID")
        if name in expected_adapters and (not result["compatible"] or result["adapterId"] != expected_adapters[name]):
            raise RuntimeError(f"{name}: expected adapter compatibility was not confirmed")
        if name not in expected_adapters and not result["adapterId"] and result["compatible"]:
            raise RuntimeError(f"{name}: unknown adapter reported compatible")
        if result["tryable"] and (result["compatible"] or result["adapterId"] != "generic"):
            raise RuntimeError(f"{name}: tryable is only valid for generic startup of an unmatched program")
        icon_bytes = icon.read_bytes() if icon.exists() else b""
        image_type = "jpeg" if icon_bytes.startswith(b"\xff\xd8\xff") else "png" if icon_bytes.startswith(b"\x89PNG\r\n\x1a\n") else "unknown"
        if icon_bytes and image_type == "unknown":
            raise RuntimeError(f"{name}: icon bytes are not a PNG or JPEG")
        san_icon = icon_dir / f"{name}-asan.dat"
        san_run = subprocess.run([str(sanitized), str(package), str(args.keys), str(san_icon)],
                                 capture_output=True, text=True, timeout=300, env=env)
        (OUT / f"{name}-scan-sanitized.log").write_text(san_run.stdout + san_run.stderr)
        if san_run.returncode:
            raise RuntimeError(f"{name}: ASan/UBSan package scan failed")
        san_result = json.loads(san_run.stdout)
        if any(san_result.get(k) != result.get(k) for k in ("programId", "programIdHex", "title", "developer", "version", "adapterId", "compatible", "tryable", "reason", "mainBuildId", "iconBytes")):
            raise RuntimeError(f"{name}: optimized and ASan/UBSan scan results differ")
        result["input"] = {"bytes": package.stat().st_size, "sha256": sha(package)}
        result["icon"] = {"bytes": len(icon_bytes), "sha256": hashlib.sha256(icon_bytes).hexdigest() if icon_bytes else "", "image_type": image_type}
        result["sanitized_result_matches"] = True
        reference = references.get(name)
        if reference:
            nacp = parse_nacp(reference / "control.nacp")
            for field, value in nacp.items():
                if value and result[field] != value:
                    raise RuntimeError(f"{name}: {field} differs from extracted NACP reference")
            ref_icon = reference / "icon_AmericanEnglish.dat"
            icon_match = not ref_icon.exists() or icon_bytes == ref_icon.read_bytes()
            if not icon_match:
                raise RuntimeError(f"{name}: icon differs from extracted reference")
            result["reference_match"] = {**nacp, "icon_matches": icon_match}
        scans[name] = result

    report = {
        "state": "passed",
        "synthetic": {"optimized": json.loads((OUT / "synthetic-optimized.log").read_text()),
                      "asan_ubsan": json.loads((OUT / "synthetic-sanitized.log").read_text())},
        "real_packages": scans,
        "source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in SOURCES},
        "scope": "Production nxpkg API, owned package inputs are read-only; no keys or key paths are recorded.",
    }
    dest = OUT / "verification.json"
    dest.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"metadata check failed: {error}", file=sys.stderr)
        sys.exit(1)
