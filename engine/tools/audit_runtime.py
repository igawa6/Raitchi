#!/usr/bin/env python3
"""Read-only, source-level runtime inventory; no game assets or device access."""
import argparse
import json
import pathlib
import re
import subprocess


def collect(root, pattern):
    found = set()
    for path in (root / "runtime").rglob("*"):
        if path.suffix in {".c", ".cpp", ".m", ".mm"}:
            found.update(re.findall(pattern, path.read_text(errors="replace"), re.M))
    return found


def inventory(root):
    cpu = (root / "runtime/cpu.h").read_text()
    gamelib = (root / "runtime/gamelib.h").read_text()
    return {
        "commit": subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
        ).strip(),
        "tracked_changes": subprocess.check_output(
            ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=no"],
            text=True,
        ).splitlines(),
        "gamelib_abi": int(re.search(r"^#define GAMELIB_ABI (\d+)", gamelib, re.M)[1]),
        "cpu_dispatch_cache": bool(re.search(r"\bdcache\[4096\]", cpu)),
        "guest_shift_macro": "#define GUEST_SHIFT" in cpu,
        "function_entry_hooks": "rt_fnhook" in cpu,
        "hle": collect(root, r'^HLE_FUNC\s*\(\s*"([^\"]+)"'),
        "nvn": collect(root, r"^NVN\s*\(\s*(\w+)\s*\)"),
        "services": collect(root, r'^SERVICE\s*\(\s*"([^\"]+)"'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1] / "sources")
    args = parser.parse_args()
    # Patterns deliberately count literal declarations, including disabled code.
    # They are not preprocessed registrations, import closure, or implemented features.
    data = {name: inventory(args.sources / name)
            for name in ("las-upstream", "las-android", "letsgo")}
    comparisons = {}
    for field in ("hle", "nvn", "services"):
        a, b = data["las-android"][field], data["letsgo"][field]
        comparisons[field] = {"common": len(a & b), "las_android_only": sorted(a - b),
                              "letsgo_only": sorted(b - a)}
    for item in data.values():
        for field in ("hle", "nvn", "services"):
            item[field + "_literal_count"] = len(item.pop(field))
    print(json.dumps({"sources": data, "las_android_vs_letsgo": comparisons}, indent=2))


if __name__ == "__main__":
    main()
