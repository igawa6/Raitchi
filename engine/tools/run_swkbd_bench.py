#!/usr/bin/env python3
"""Run the software-keyboard encoding bench in both arms: original and fixed.

The bench compiles the real ShowKeyboard HLE against the pinned LAS Android
source and one host SDL3 build, so the conversion and serialisation logic
under test is the production one, not a rewrite.

All paths are relative to the repository root or passed on the command line;
nothing outside the checkout is assumed.
"""
import argparse
import pathlib
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--baseline", type=pathlib.Path, required=True,
               help="pinned LAS Android clone (the upstream source of swkbd.c)")
p.add_argument("--sdl-source", type=pathlib.Path, required=True,
               help="SDL3 source tree (include/ must be there)")
p.add_argument("--sdl-build", type=pathlib.Path, required=True,
               help="host SDL3 build directory (libSDL3.so must be there)")
p.add_argument("--cc", required=True, help="C compiler used for the bench")
p.add_argument("--out", type=pathlib.Path, default=pathlib.Path("build/swkbd-bench"),
               help="directory receiving red-full.log and green.log")
a = p.parse_args()

root = pathlib.Path(__file__).resolve().parents[2]  # engine/tools/<this file> -> repo root
args = [
    sys.executable, str(root / "engine/tools/check_swkbd.py"),
    "--baseline", str(a.baseline),
    "--sdl-source", str(a.sdl_source),
    "--sdl-build", str(a.sdl_build),
    "--cc", a.cc,
]
out = a.out
out.mkdir(parents=True, exist_ok=True)
status = 0
for label, extra, expect in [("red-full", ["--original"], 1), ("green", [], 0)]:
    p_run = subprocess.run(args + extra, capture_output=True, text=True, cwd=str(root))
    text = p_run.stdout + p_run.stderr + "rc=" + str(p_run.returncode) + "\n"
    (out / (label + ".log")).write_text(text)
    print("===", label, "rc", p_run.returncode)
    for line in text.splitlines():
        if line.startswith(("PASS", "FAIL", "rc=", "[swkbd]")):
            print(line)
    if p_run.returncode != expect:
        status = 1
print("expected status:", status)
raise SystemExit(status)
