#!/usr/bin/env python3
"""Install verified private title code and boot Eevee in its isolated app, without input/timers."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.recomp.letsgo.test"
DATA = f"/sdcard/Android/data/{PACKAGE}/files"
PROFILE = """fullscreen = 1
fps = 30
render_height = 720
aa = off
frame_gen = 0
upscaler = bilinear
ui2x = 0
ssao_level = 0
gi_level = 0
show_fps = 1
skip_controller = 0
wild_battles = 0
"""


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    args = parser.parse_args()
    manual = ROOT / "evidence/manual-session.json"
    if manual.is_file() and json.loads(manual.read_text()).get("active"):
        parser.error("Owner-controlled manual session is active; no title-code install or restart attempted")
    native = json.loads((ROOT / "build/letsgo-android/native-build.json").read_text())
    game = json.loads((ROOT / "build/letsgo-gamelib/build.json").read_text())
    library = ROOT / "build/letsgo-gamelib/liblasgame.so"
    if (game["game_code_identity"] != native["game_code_identity"] or game["gamelib_abi"] != 3
            or game["guest_shift"] != 0 or game["cpu_size"] != 1024
            or sha(library) != game["library_sha256"]):
        raise RuntimeError("Private library or runtime generated-code contract mismatch")
    adb = ["adb", "-s", args.serial]

    def shell(parts, check=True, **kwargs):
        return subprocess.run([*adb, "shell", "-T", shlex.join([str(p) for p in parts])],
                              check=check, capture_output=True, **kwargs)

    installed = shell(["pm", "path", PACKAGE]).stdout.decode().strip()
    if not installed.startswith("package:") or "\n" in installed:
        raise RuntimeError("Expected the isolated single-APK reference installation")
    installed_sha = shell(["sha256sum", installed.removeprefix("package:")]).stdout.decode().split()[0]
    if installed_sha != native["apk"]["sha256"]:
        raise RuntimeError("Installed runtime APK differs from verified build; update separate reference first")
    for name, info in game["modules"].items():
        actual = shell(["run-as", PACKAGE, "sha256sum", DATA + "/extracted/exefs/" + name]).stdout.decode().split()[0]
        if actual != info["sha256"]:
            raise RuntimeError(f"Installed NSO differs from generated title library: {name}")
    shell(["run-as", PACKAGE, "test", "-r", DATA + "/extracted/romfs.bin"])
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    evidence = ROOT / "evidence/letsgo-boot" / stamp
    evidence.mkdir(parents=True, exist_ok=False)
    # A new native process is mandatory when replacing generated title code.
    shell(["am", "force-stop", PACKAGE])
    driver_environment = []
    for name in ("env.txt", "reference-baseline.ini"):
        previous = shell(["run-as", PACKAGE, "cat", DATA + "/" + name], check=False)
        if previous.returncode == 0:
            (evidence / ("previous-" + name)).write_bytes(previous.stdout)
            if name == "env.txt":
                driver_environment = [line for line in previous.stdout.decode().splitlines()
                                      if line.startswith("LARECOMP_GPU_DRIVER_")]
    incoming = "files/liblasgame.so.incoming-" + stamp
    writer = 'umask 077; cat >"$1"'
    with library.open("rb") as stream:
        shell(["run-as", PACKAGE, "sh", "-c", writer, "install-title-code", incoming], stdin=stream)
    actual = shell(["run-as", PACKAGE, "sha256sum", incoming]).stdout.decode().split()[0]
    if actual != game["library_sha256"]:
        raise RuntimeError("App-internal title library transfer hash mismatch")
    shell(["run-as", PACKAGE, "mv", incoming, "files/liblasgame.so"])
    shell(["run-as", PACKAGE, "sh", "-c", writer, "install-title-id", "files/liblasgame.so.id"],
          input=(game["game_code_identity"] + "\n").encode())
    shell(["run-as", PACKAGE, "sh", "-c", writer, "write-profile", DATA + "/reference-baseline.ini"],
          input=PROFILE.encode())
    environment = ("\n".join(["LARECOMP_NO_LAUNCHER=1", "LARECOMP_SETTINGS=reference-baseline.ini",
        "LARECOMP_NPAD=handheld", "LARECOMP_FONT_SCALE=1", *driver_environment]) + "\n").encode()
    shell(["run-as", PACKAGE, "sh", "-c", writer, "write-environment", DATA + "/env.txt"], input=environment)
    (evidence / "launch.json").write_text(json.dumps({
        "package": PACKAGE, "library_sha256": game["library_sha256"],
        "game_code_identity": game["game_code_identity"], "apk_sha256": installed_sha,
        "baseline_profile": PROFILE, "environment": environment.decode(),
        "scripted_input": False, "automatic_close": False,
        "gameplay_verified": False}, indent=2) + "\n")
    shell(["input", "keyevent", "KEYCODE_WAKEUP"])
    shell(["am", "start", "--display", "0", "-n", PACKAGE + "/.ReferenceActivity"])
    print("Boot requested with verified private four-module code; no scripted input or automatic close.")


if __name__ == "__main__":
    main()
