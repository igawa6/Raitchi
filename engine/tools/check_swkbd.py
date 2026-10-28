#!/usr/bin/env python3
"""Compile the real ShowKeyboard HLE with SDL conversion, no game or Android required.

--baseline is the pinned LAS Android clone. --sdl-source/--sdl-build are one
host SDL3 build (include/ and libSDL3.so). --original omits the new overlay.
"""
import argparse
import io
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PIN = "cca791a3e7218e1dac95e7d4a11b8f97dba3ea51"
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--baseline", type=Path, required=True)
p.add_argument("--sdl-source", type=Path, required=True)
p.add_argument("--sdl-build", type=Path, required=True)
p.add_argument("--cc", required=True)
p.add_argument("--original", action="store_true")
a = p.parse_args()
build = ROOT / "build/swkbd" / ("original" if a.original else "fixed")
build.mkdir(parents=True, exist_ok=True)
archive = subprocess.check_output(["git", "-C", str(a.baseline), "archive", PIN, "runtime"])
with tarfile.open(fileobj=io.BytesIO(archive)) as t:
    t.extractall(build, filter="data")
shutil.copytree(ROOT / "ports/shared-android/runtime", build / "runtime", dirs_exist_ok=True)
if a.original:
    original = subprocess.check_output(["git", "-C", str(a.baseline), "show", PIN + ":runtime/hle/swkbd.c"])
    (build / "runtime/hle/swkbd.c").write_bytes(original)
source = build / "runtime/hle/swkbd.c"
cmd = [a.cc, "--target=x86_64-linux-gnu", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
       "-Wno-unused-parameter", "-I" + str(ROOT / "native"), "-I" + str(build / "runtime"),
       "-I" + str(a.sdl_source / "include"), '-DSWKBD_SOURCE="' + str(source) + '"',
       str(ROOT / "tests/swkbd_test.c"), "-L" + str(a.sdl_build),
       "-Wl,-rpath," + str(a.sdl_build), "-lSDL3", "-o", str(build / "test")]
print("compile:", " ".join(cmd), flush=True)
subprocess.run(cmd, check=True)
raise SystemExit(subprocess.run([str(build / "test")]).returncode)
