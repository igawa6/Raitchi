#!/usr/bin/env python3
"""Audit existing private title ELF imports against baseline dependencies and shared host exports; offline only."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load(path):
    return json.loads(path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    toolchain = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64"
    readelf = toolchain / "bin/llvm-readelf"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = ROOT / "build/shared-aot/imports" / stamp
    output.mkdir(parents=True, exist_ok=False)
    parsed = {}

    def elf(path):
        path = path.resolve()
        if path in parsed:
            return parsed[path]
        report = subprocess.run([str(readelf), "--file-header", "--dynamic", "--dyn-syms", "--wide", str(path)],
                                capture_output=True, text=True, check=True).stdout
        symbols = []
        for line in report.splitlines():
            fields = line.split()
            if (len(fields) < 8 or not re.fullmatch(r"\d+:", fields[0])
                    or fields[4] not in ("GLOBAL", "WEAK") or fields[5] not in ("DEFAULT", "PROTECTED")):
                continue
            name = fields[7]
            base, separator, version = name.partition("@")
            symbols.append({"name": base, "version": version.lstrip("@") if separator else None,
                            "elf_name": name, "type": fields[3], "binding": fields[4],
                            "undefined": fields[6] == "UND"})
        record = {"path": str(path), "sha256": sha(path), "bytes": path.stat().st_size,
                  "needed": re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", report),
                  "soname": re.findall(r"\(SONAME\).*?\[([^\]]+)\]", report),
                  "machine": re.search(r"Machine:\s*(.+)", report).group(1).strip(),
                  "undefined": [p for p in symbols if p["undefined"]],
                  "exports": [p for p in symbols if not p["undefined"]]}
        (output / f"elf-{len(parsed):02d}.txt").write_text(report)
        parsed[path] = record
        return record

    las = ROOT / "work/las-baseline"
    las_linked_game = las / "build/android/gamelib/arm64-v8a/liblasgame.so"
    runtime_candidates = list((las / "android/app/build/intermediates/cxx").glob("*/*/obj/arm64-v8a/libmain.so"))
    if len(runtime_candidates) != 1:
        raise RuntimeError("Expected one existing LAS baseline runtime")
    las_runtime = runtime_candidates[0]
    lg_build = ROOT / "build/letsgo-gamelib/build.json"
    lg_native = ROOT / "build/letsgo-android/native-build.json"
    las_evidence = ROOT / "evidence/las-build.json"
    lg, native, legacy_las = load(lg_build), load(lg_native), load(las_evidence)
    # LAS evidence binds the packaged stripped ELF, not the larger link output.
    apk = las / "android/app/build/outputs/apk/debug/app-debug.apk"
    if sha(apk) != legacy_las["apk_sha256"]:
        raise RuntimeError("LAS baseline APK differs from recorded evidence")
    packaged = output / "las"
    packaged.mkdir()
    with zipfile.ZipFile(apk) as archive:
        for name in ("liblasgame.so", "libmain.so"):
            (packaged / name).write_bytes(archive.read("lib/arm64-v8a/" + name))
    las_game = packaged / "liblasgame.so"
    titles = [
        {"title": "las", "library": las_game, "runtime": packaged / "libmain.so", "old_abi": 1, "old_cpu_size": 66560,
         "expected_library_sha256": legacy_las["game_library_sha256"],
         "code_id": las_linked_game.with_name(las_linked_game.name + ".id").read_text().strip(),
         "provenance": {"baseline_manifest": str(las_evidence), "manifest_sha256": sha(las_evidence),
                        "apk_sha256": legacy_las["apk_sha256"], "library_source": "verified baseline APK; stripped ELF",
                        "unstripped_library_sha256": sha(las_linked_game),
                        "legacy_id_source": "observed sidecar; getter not invoked",
                        "abi_source": "baseline source declaration; getter not invoked",
                        "abi_header_sha256": sha(las / "runtime/gamelib.h"),
                        "legacy_generation_provenance": "legacy library ID/build record; no historical per-unit manifest"}},
        {"title": "letsgo", "library": ROOT / "build/letsgo-gamelib/liblasgame.so",
         "runtime": ROOT / "build/letsgo-android/native/libmain.so", "old_abi": lg["gamelib_abi"],
         "old_cpu_size": lg["cpu_size"], "code_id": lg["game_code_identity"],
         "expected_library_sha256": lg["library_sha256"],
         "expected_runtime_sha256": native["library_sha256"],
         "provenance": {"library_manifest": str(lg_build), "manifest_sha256": sha(lg_build),
                        "native_manifest": str(lg_native), "native_manifest_sha256": sha(lg_native),
                        "abi_source": "hash-bound legacy build manifest; getter not invoked",
                        "link_time_runtime_sha256": lg["runtime_sha256"],
                        "current_runtime_code_identity_matches": native["game_code_identity"] == lg["game_code_identity"]}}
    ]
    search = [las_runtime.parent, ROOT / "build/letsgo-android/apk-inputs/jniLibs/arm64-v8a",
              toolchain / "sysroot/usr/lib/aarch64-linux-android/30",
              toolchain / "sysroot/usr/lib/aarch64-linux-android"]

    def dependency_graph(game, runtime):
        queue = [*game["needed"], *runtime["needed"]]
        providers, missing = {}, []
        while queue:
            name = queue.pop(0)
            if name == "libmain.so" or name in providers or name in missing:
                continue
            path = next((directory / name for directory in search if (directory / name).is_file()), None)
            if not path:
                missing.append(name)
                continue
            record = elf(path)
            providers[name] = record
            queue.extend(record["needed"])
        return providers, missing

    def offers(record, symbol):
        return any(p["name"] == symbol["name"] and
                   (symbol["version"] is None or p["version"] == symbol["version"])
                   for p in record["exports"])

    result = {"schema": 1, "created_utc": stamp,
              "scope": "legacy Android ELFs; import discovery, not shared ABI acceptance",
              "readelf_sha256": sha(readelf), "titles": [], "runtime_import_union": [],
              "built": False, "generated": False, "device_executed": False}
    union = set()
    for title in titles:
        game, runtime = elf(title["library"]), elf(title["runtime"])
        if game["sha256"] != title["expected_library_sha256"]:
            raise RuntimeError("Existing private title library differs from its recorded build hash")
        if title.get("expected_runtime_sha256") and runtime["sha256"] != title["expected_runtime_sha256"]:
            raise RuntimeError("Let's Go current runtime differs from native-build manifest")
        if game["machine"] != "AArch64" or runtime["machine"] != "AArch64":
            raise RuntimeError("Expected ARM64 Android references")
        deps, missing = dependency_graph(game, runtime)
        imports = []
        for symbol in game["undefined"]:
            own = offers(runtime, symbol)
            supplied = [name for name, record in deps.items() if offers(record, symbol)]
            category = ("runtime_and_dependency" if supplied else "runtime") if own else (
                "dependency" if supplied else "unclassified")
            if own:
                union.add(symbol["name"])
            imports.append({**symbol, "category": category, "baseline_runtime_provides": own,
                            "dependency_providers": supplied})
        entry = {k: v for k, v in title.items() if k not in ("library", "runtime")}
        entry.update(library=game, baseline_runtime=runtime, imports=imports,
                     dependency_inventory={name: {"path": record["path"], "sha256": record["sha256"],
                                                  "needed": record["needed"],
                                                  "provider_kind": "NDK_API_stub_or_toolchain" if "sysroot" in record["path"] else "cached_actual_dependency"}
                                           for name, record in deps.items()},
                     unresolved_needed=missing,
                     unclassified=[p for p in imports if p["category"] == "unclassified"])
        result["titles"].append(entry)
    result["runtime_import_union"] = sorted(union)
    shared_path = ROOT / "build/shared-aot/host/libshared_aot.so"
    if shared_path.is_file():
        shared = elf(shared_path)
        names = {p["name"] for p in shared["exports"]}
        result["shared_host"] = {"path": str(shared_path), "sha256": shared["sha256"],
                                 "machine": shared["machine"], "needed": shared["needed"],
                                 "exports": shared["exports"], "runtime_union_present": sorted(union & names),
                                 "runtime_union_missing": sorted(union - names),
                                 "comparison": "symbol names only; host architecture/legacy ABI do not establish Android compatibility"}
    else:
        result["shared_host"] = {"available": False, "comparison_pending": True}
    manifest = output / "imports.json"
    temporary = manifest.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(result, indent=2) + "\n")
    temporary.replace(manifest)
    print(f'Runtime import union: {len(union)}; private report {manifest.relative_to(ROOT)}')
    for entry in result["titles"]:
        counts = {kind: sum(p["category"] == kind for p in entry["imports"])
                  for kind in ("runtime", "runtime_and_dependency", "dependency", "unclassified")}
        print(entry["title"], counts, "unresolved DT_NEEDED:", len(entry["unresolved_needed"]))
    if result["shared_host"].get("runtime_union_missing") is not None:
        print("Shared host missing", len(result["shared_host"]["runtime_union_missing"]), "legacy runtime imports")


if __name__ == "__main__":
    main()
