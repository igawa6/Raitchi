#!/usr/bin/env python3
"""Verify private two-title shared-runtime artifacts offline; never load game code or use ADB."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def inspect_elf(readelf, path):
    text = subprocess.run([str(readelf), "--file-header", "--program-headers", "--dynamic",
                           "--dyn-syms", "--wide", str(path)], check=True, capture_output=True, text=True).stdout
    exports, undefined = [], []
    for line in text.splitlines():
        parts = line.split()
        if (len(parts) < 8 or not re.fullmatch(r"\d+:", parts[0]) or
                parts[4] not in ("GLOBAL", "WEAK") or parts[5] not in ("DEFAULT", "PROTECTED")):
            continue
        name, separator, version = parts[7].partition("@")
        record = {"name": name, "version": version.lstrip("@") if separator else None,
                  "type": parts[3], "binding": parts[4]}
        (undefined if parts[6] == "UND" else exports).append(record)
    loads = [line.split() for line in text.splitlines() if line.lstrip().startswith("LOAD ")]
    return {"path": str(path), "sha256": sha(path), "needed": re.findall(r"NEEDED.*\[([^\]]+)\]", text),
            "soname": re.findall(r"SONAME.*\[([^\]]+)\]", text), "exports": exports, "undefined": undefined,
            "alignment": min((int(p[-1], 16) for p in loads), default=0),
            "aarch64": "AArch64" in text and "ELF64" in text and "little endian" in text}


def offers(provider, symbol):
    return any(item["name"] == symbol["name"] and
               (symbol["version"] is None or symbol["version"] == item["version"])
               for item in provider["exports"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title", choices=("both", "las", "eevee"), default="both")
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    toolchain = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64"
    readelf = toolchain / "bin/llvm-readelf"
    native_path = ROOT / "build/shared-android/native-build.json"
    native = json.loads(native_path.read_text())
    runtime_path = ROOT / "build/shared-android/native/libmain.so"
    runtime = inspect_elf(readelf, runtime_path)
    require(runtime["sha256"] == native["library_sha256"] and runtime["soname"] == ["libmain.so"] and
            runtime["alignment"] >= 16384 and runtime["aarch64"], "Common runtime file/ELF does not match its build")
    for relative, digest in native["overlay_and_native_sha256"].items():
        require(sha(ROOT / relative) == digest, "Common runtime source changed after build")
    require(sha(ROOT / "ports/shared-android/exports.map") == native["exports_map_sha256"], "Runtime export policy changed")
    require(sha(ROOT / "ports/shared-android/CMakeLists.txt") == native["cmake_sha256"], "Runtime build configuration changed")
    contract = hashlib.sha256(b"shared-aot-v1\0" + (ROOT / "native/runtime/aot_cpu.h").read_bytes() +
                              (ROOT / "native/module_abi.h").read_bytes()).hexdigest()
    require(contract == native["shared_contract_sha256"], "Runtime execution contract changed")
    cached_dsos = {}
    for relative, digest in native["dependencies"].items():
        path = ROOT / relative
        require(sha(path) == digest, "Cached runtime dependency changed after build")
        if path.suffix == ".so":
            cached_dsos[path.name] = path
    search = [toolchain / "sysroot/usr/lib/aarch64-linux-android/30",
              toolchain / "sysroot/usr/lib/aarch64-linux-android"]
    parsed = {"libmain.so": runtime}

    def graph(needed):
        result = {"libmain.so": runtime}
        queue = list(needed)
        while queue:
            name = queue.pop(0)
            if name in result:
                continue
            path = cached_dsos.get(name) or next((directory / name for directory in search if (directory / name).is_file()), None)
            require(path is not None, "Missing declared Android dependency: " + name)
            if name not in parsed:
                parsed[name] = inspect_elf(readelf, path)
            result[name] = parsed[name]
            queue.extend(result[name]["needed"])
        return result

    report = {"schema": 1, "verified_utc": datetime.now(timezone.utc).isoformat(),
              "runtime_sha256": runtime["sha256"], "runtime_manifest_sha256": sha(native_path),
              "contract_sha256": contract, "device_execution": False, "titles": {}}
    for title in (("las", "eevee") if args.title == "both" else (args.title,)):
        directory = ROOT / "build/shared-gamelib" / title
        record = json.loads((directory / "build.json").read_text())
        game = inspect_elf(readelf, directory / "liblasgame.so")
        sidecar_path = directory / "liblasgame.so.saotm"
        wire = sidecar_path.read_bytes()
        header = struct.unpack_from("<8s12I3Q32s32s32s", wire)
        count = header[12]
        require(header[:5] == (b"SAOTM001", len(wire), 1, 1, 1 if title == "las" else 2) and
                header[5:12] == (1, 30, 3, 66560, 16, 1, 1 if title == "las" else 2) and
                count == (2 if title == "las" else 4) and len(wire) == 176 + 112 * count,
                "Sidecar header/profile is not the selected shared contract")
        require(header[16].hex() == contract and header[17].hex() == record["semantic_code_sha256"] and
                header[18].hex() == game["sha256"] == record["library_sha256"] and
                sha(sidecar_path) == record["sidecar_sha256"] and record["runtime_sha256"] == runtime["sha256"],
                "Title sidecar/library/runtime hash binding failed")
        require(game["aarch64"] and game["alignment"] >= 16384 and game["soname"] == ["liblasgame.so"] and
                "libmain.so" in game["needed"] and
                [(item["name"], item["type"]) for item in game["exports"]] == [("recomp_module_descriptor_v1", "OBJECT")],
                "Title ELF export/architecture/dependency policy failed")
        for index, module in enumerate(sorted(record["semantic"]["modules"].values(), key=lambda item: item["module_id"])):
            fields = struct.unpack_from("<II32s32s5Q", wire, 176 + 112 * index)
            require(fields == (module["module_id"], 0, bytes.fromhex(module["build_id"]), bytes.fromhex(module["sha256"]),
                               module["canonical_base"], module["mapped_size"], module["text_offset"],
                               module["text_size"], module["function_count"]) and sha(Path(module["path"])) == module["sha256"],
                    "Original module metadata/file binding failed")
        providers = graph([*game["needed"], *runtime["needed"]])
        imports = []
        for symbol in game["undefined"]:
            supplied = [name for name, provider in providers.items() if offers(provider, symbol)]
            require(supplied or symbol["binding"] == "WEAK", "Unresolved strong title import: " + symbol["name"])
            imports.append({**symbol, "providers": supplied})
        require(sha(Path(record["generation_manifest"])) == record["semantic"]["generation_manifest_sha256"],
                "Title generation manifest changed")
        report["titles"][title] = {"library_sha256": game["sha256"], "sidecar_sha256": sha(sidecar_path),
            "runtime_sha256": record["runtime_sha256"], "objects": len(record["objects"]),
            "module_count": count, "imports": imports, "needed": game["needed"],
            "unresolved_strong_imports": [], "generation": record["semantic"]["generation"]}
        print(f"{title}: {count} modules, one common runtime, descriptor/data-only export and all strong imports verified")
    require(sha(runtime_path) == runtime["sha256"], "Common runtime changed during verification")
    report["two_title_host_gate"] = len(report["titles"]) == 2
    output = ROOT / "build/shared-android/artifact-verification.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print("Offline artifact checks pass; Android execution and gameplay remain unverified.")


if __name__ == "__main__":
    main()
