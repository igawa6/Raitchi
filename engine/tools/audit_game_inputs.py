#!/usr/bin/env python3
"""Inspect private NSOs read-only; write detailed metadata only under ignored evidence."""
import argparse
import collections
import ctypes
import ctypes.util
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import types

ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "las": {"source": "las-android", "modules": ("main", "sdk")},
    "letsgo": {"source": "letsgo", "modules": ("main", "sdk", "subsdk0", "subsdk1")},
}
BASES = dict(zip(("main", "sdk", "subsdk0", "subsdk1"),
                 (0x7000000000, 0x7004000000, 0x7006000000, 0x7008000000)))


def load_parser():
    """Use pinned parser unchanged; system liblz4 fallback avoids installation."""
    backend = "python-lz4.block"
    try:
        import lz4.block  # noqa: F401
    except ImportError:
        backend = "system-liblz4 via ctypes"
        lib = ctypes.CDLL(ctypes.util.find_library("lz4") or "liblz4.so.1")
        fn = lib.LZ4_decompress_safe
        fn.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        fn.restype = ctypes.c_int

        def decompress(data, uncompressed_size):
            if len(data) > 0x7fffffff or uncompressed_size > 0x7fffffff:
                raise ValueError("LZ4 input too large")
            src = ctypes.create_string_buffer(data)
            dst = ctypes.create_string_buffer(uncompressed_size)
            got = fn(src, dst, len(data), uncompressed_size)
            if got != uncompressed_size:
                raise ValueError("LZ4 output size mismatch")
            return dst.raw

        package, block = types.ModuleType("lz4"), types.ModuleType("lz4.block")
        block.decompress = decompress
        package.block = block
        sys.modules.update({"lz4": package, "lz4.block": block})
    path = ROOT / "sources/las-android/tools/nso.py"
    spec = importlib.util.spec_from_file_location("pinned_nso", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.Nso, backend, path


def inventory(source):
    literal, nvn, wrappers = set(), set(), []
    for path in (ROOT / "sources" / source / "runtime").rglob("*"):
        if path.suffix not in {".c", ".cpp", ".m", ".mm", ".h"}:
            continue
        text = path.read_text(errors="replace")
        if path.suffix != ".h":
            literal.update(re.findall(r'^HLE_FUNC\s*\(\s*"([^\"]+)"', text, re.M))
            nvn.update(re.findall(r'^NVN\s*\(\s*(\w+)\s*\)', text, re.M))
        for match in re.finditer(r'^#define\s+(\w+)\([^\n]*HLE_FUNC[^\n]*', text, re.M):
            wrappers.append({"path": str(path.relative_to(ROOT)), "macro": match[1]})
    return {"literal": literal, "nvn_macro": nvn, "wrappers": wrappers}


def sym_record(index, sym):
    name, info, shndx, value, size = sym
    return {"index": index, "name": name, "binding": info >> 4, "type": info & 15,
            "section_index": shndx, "value": value, "size": size}


def module_record(path, Nso, active):
    raw = path.read_bytes()
    flags = struct.unpack_from("<I", raw, 0x0c)[0]
    segments = []
    for i, name in enumerate(("text", "rodata", "data")):
        file_off, mem_off, size = struct.unpack_from("<III", raw, 0x10 + i * 0x10)
        compressed_size = struct.unpack_from("<I", raw, 0x60 + i * 4)[0]
        stored = compressed_size if flags & (1 << i) else size
        if file_off + stored > len(raw):
            raise ValueError(f"NSO segment outside file: {path.name}/{name}")
        segments.append({"name": name, "file_offset": file_off, "memory_offset": mem_off,
                         "uncompressed_bytes": size, "compressed_size_header": compressed_size,
                         "compressed": bool(flags & (1 << i)), "stored_bytes": stored})
    # Keep all 32 header bytes. The pinned parser strips trailing zero bytes from IDs.
    result = {"path": str(path.relative_to(ROOT)), "file_bytes": len(raw),
              "sha256": hashlib.sha256(raw).hexdigest(), "flags": flags,
              "build_id_32_bytes": raw[0x40:0x60].hex().upper(), "segments": segments,
              "selected_by_runtime": active, "canonical_base": BASES.get(path.name)}
    nso = Nso(path)
    imports = [sym_record(i, s) for i, s in enumerate(nso.syms) if s[0] and s[2] == 0]
    exports = [sym_record(i, s) for i, s in enumerate(nso.syms) if s[0] and s[2] != 0]
    referenced = collections.Counter(r[2] for r in nso.relocs if r[1] in {257, 1025, 1026})
    for item in imports:
        item["symbol_relocation_references"] = referenced[item["index"]]
    result.update({"parser_image_bytes_4k_aligned": nso.size,
                   "runtime_image_bytes_16k_aligned": (nso.data[0] + nso.data[1] + nso.bss_size + 0x3fff) & ~0x3fff,
                   "bss_bytes": nso.bss_size, "dynamic_offset": nso.dynamic,
                   "dt_needed": nso.needed, "dynamic_entries": {str(k): v for k, v in nso.dyn.items()},
                   "symbol_count": len(nso.syms), "imports": imports, "defined_exports": exports,
                   "relocation_types": dict(collections.Counter(r[1] for r in nso.relocs)),
                   "unsupported_relocation_types": sorted({r[1] for r in nso.relocs} - {257, 1025, 1026, 1027})})
    return result


def compare(modules, registry):
    providers, excluded = collections.defaultdict(list), collections.defaultdict(list)
    for module_name, module in modules.items():
        table = providers if module["selected_by_runtime"] else excluded
        for sym in module["defined_exports"]:
            table[sym["name"]].append(module_name)
    summaries, details = {}, {}
    for module_name, module in modules.items():
        if not module["selected_by_runtime"]:
            continue
        counts, used_counts, rows = collections.Counter(), collections.Counter(), []
        for sym in module["imports"]:
            name = sym["name"]
            if name in registry["literal"]:
                category = "source_literal_hle_candidate"
            elif name in registry["nvn_macro"]:
                category = "source_nvn_macro_hle_candidate"
            elif name in providers:
                category = "selected_guest_export"
            elif sym["binding"] == 2:
                category = "weak_undefined_zero"
            else:
                category = "potential_gap_or_unscanned_registration"
            counts[category] += 1
            if sym["symbol_relocation_references"]:
                used_counts[category] += 1
            rows.append({**sym, "category": category, "selected_guest_providers": providers.get(name, []),
                         "excluded_guest_providers": excluded.get(name, [])})
        details[module_name] = rows
        summaries[module_name] = {"all_import_entries": len(rows), "all_categories": dict(counts),
                                  "relocation_referenced_import_entries": sum(used_counts.values()),
                                  "relocation_referenced_categories": dict(used_counts),
                                  "guest_provider_sets": dict(collections.Counter(
                                      ",".join(dict.fromkeys(row["selected_guest_providers"]))
                                      for row in rows if row["category"] == "selected_guest_export"))}
    return {"summary": summaries, "imports": details}


def layout_check(modules):
    intervals = sorted((module["canonical_base"],
                        module["canonical_base"] + module["runtime_image_bytes_16k_aligned"], name)
                       for name, module in modules.items() if module["selected_by_runtime"])
    overlaps = [{"left": left[2], "right": right[2]}
                for left, right in zip(intervals, intervals[1:]) if left[1] > right[0]]
    return {"canonical_intervals": [{"module": name, "start": start, "end": end}
                                     for start, end, name in intervals],
            "canonical_overlaps": overlaps,
            "android_shift_proposal": 0x6000000000,
            "android_shifted_intervals": [{"module": name, "start": start - 0x6000000000,
                                            "end": end - 0x6000000000}
                                           for start, end, name in intervals],
            "ends_before_hle_region": all(end <= 0x7080000000 for _, end, _ in intervals),
            "note": "Arithmetic layout only; no reservation or Android load attempted"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "evidence/game-input-audit.json")
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "evidence"):
        parser.error("Detailed output must remain under this workspace's ignored evidence directory")
    ignored = subprocess.run(["git", "check-ignore", "-q", str(output)], cwd=ROOT)
    if ignored.returncode:
        parser.error("Output is not ignored by Git")
    Nso, backend, parser_path = load_parser()
    registries = {name: inventory(name) for name in ("las-android", "letsgo")}
    result = {"schema": 1, "method": "source-based candidate resolution; no execution or game-code output",
              "parser": str(parser_path.relative_to(ROOT)), "lz4_backend": backend,
              "source_commits": {}, "registries": {}, "titles": {}}
    for source, registry in registries.items():
        result["source_commits"][source] = subprocess.check_output(
            ["git", "-C", str(ROOT / "sources" / source), "rev-parse", "HEAD"], text=True).strip()
        result["registries"][source] = {k: sorted(v) if isinstance(v, set) else v for k, v in registry.items()}
    for title, profile in PROFILES.items():
        exefs = ROOT / "private" / f"{title}-baseline/extracted/exefs"
        modules = {}
        for path in sorted(exefs.iterdir()):
            if not path.is_file():
                continue
            with path.open("rb") as stream:
                magic = stream.read(4)
            if magic == b"NSO0":
                modules[path.name] = module_record(path, Nso, path.name in profile["modules"])
        if not set(profile["modules"]).issubset(modules):
            raise ValueError("Selected profile module missing")
        comparisons = {source: compare(modules, registry) for source, registry in registries.items()}
        deltas = []
        for module_name, rows in comparisons["letsgo"]["imports"].items():
            android_rows = {row["index"]: row for row in comparisons["las-android"]["imports"][module_name]}
            for row in rows:
                old = android_rows[row["index"]]
                if row["symbol_relocation_references"] and row["category"] != old["category"]:
                    deltas.append({"module": module_name, "symbol": row["name"],
                                   "las_android_category": old["category"], "letsgo_category": row["category"]})
        result["titles"][title] = {"selected_modules": profile["modules"], "modules": modules,
                                    "layout": layout_check(modules),
                                    "source_profile_resolution_deltas": deltas,
                                    "comparisons": comparisons}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n")
    # Console intentionally excludes IDs, hashes, symbols and input paths.
    for title, report in result["titles"].items():
        print(f"{title}: {len(report['modules'])} NSOs inventoried, {len(report['selected_modules'])} selected")
        for source, comparison in report["comparisons"].items():
            totals = collections.Counter()
            for row in comparison["summary"].values():
                totals.update(row["relocation_referenced_categories"])
            print(f"  {source}: relocation-referenced import entries {dict(totals)}")
    print("Detailed private evidence written to ignored evidence directory.")


if __name__ == "__main__":
    main()
