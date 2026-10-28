#!/usr/bin/env python3
"""Install/start only the separate asset-free Let's Go reference; never auto-close it."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.recomp.letsgo.test"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--screenshot", action="store_true")
    parser.add_argument("--observe-only", action="store_true", help="Read current process/logs without installing or launching")
    args = parser.parse_args()
    manual = ROOT / "evidence/manual-session.json"
    if not args.observe_only and manual.is_file() and json.loads(manual.read_text()).get("active"):
        parser.error("Owner-controlled manual session is active; no install or launch attempted")
    apk = ROOT / "build/letsgo-android/apk/outputs/apk/debug/app-debug.apk"
    digest = None
    receipt = ROOT / "evidence/letsgo-installed.json"
    if not args.observe_only:
        manifest = json.loads((ROOT / "build/letsgo-android/native-build.json").read_text())
        with apk.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != manifest["apk"]["sha256"] or manifest["apk"]["package"] != PACKAGE:
            raise RuntimeError("APK differs from the verified separate reference build")
    elif receipt.is_file():
        digest = json.loads(receipt.read_text()).get("apk_sha256")
    adb = ["adb", "-s", args.serial]

    def command(parts, check=True):
        return subprocess.run([*adb, *parts], check=check, capture_output=True)

    if not args.observe_only:
        if command(["shell", "pidof", PACKAGE], check=False).returncode == 0:
            raise RuntimeError("Let's Go reference already running; leaving its session untouched")
        command(["install", "-r", str(apk)])
        receipt.write_text(json.dumps({"package": PACKAGE, "apk_sha256": digest,
            "installed_utc": datetime.now(timezone.utc).isoformat()}, indent=2) + "\n")
        command(["shell", "input", "keyevent", "KEYCODE_WAKEUP"])
        command(["shell", "am", "start", "--display", "0", "-n", PACKAGE + "/.ReferenceActivity"])
        # Observation delay only. There is no stop, game input, or automatic cleanup.
        time.sleep(5)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = ROOT / "evidence/letsgo-launch" / stamp
    output.mkdir(parents=True, exist_ok=False)
    pid = command(["shell", "pidof", PACKAGE], check=False).stdout.decode().strip()
    native_read = command(["shell", "run-as", PACKAGE, "cat",
                           f"/sdcard/Android/data/{PACKAGE}/files/lasrecomp.log"], check=False)
    native = native_read.stdout.decode(errors="replace") if native_read.returncode == 0 else ""
    (output / "native.log").write_text(native)
    filters = ["--pid", pid] if pid.isdigit() else ["-s", "LetsGoRecomp", "SDL", "AndroidRuntime"]
    logcat = command(["logcat", "-d", "-t", "1500", *filters], check=False)
    (output / "logcat.log").write_bytes(logcat.stdout)
    if args.screenshot:
        with (output / "screen.png").open("wb") as stream:
            subprocess.run([*adb, "exec-out", "screencap", "-p"], check=True, stdout=stream)
    status = {"package": PACKAGE, "apk_sha256": digest, "observed_utc": stamp,
              "alive": bool(pid), "pids": pid.split(), "native_log_read_ok": native_read.returncode == 0,
              "complete_guest_window_reserved":
                  True if "[mem] reserved complete unshifted guest window 7000000000-7320000000" in native
                  else False if "FATAL: unshifted guest window" in native else None,
              "fatal_lines": [line for line in native.splitlines() if "FATAL" in line],
              "game_code_included": False, "game_code_packaged_in_apk": False,
              "guest_module_tables": [{"name": name, "functions": int(count)} for name, count in
                  re.findall(r"\[gamelib\].*: (main|sdk|subsdk0|subsdk1), (\d+) functions", native)],
              "read_only_gmem_mirror": "shader global memory: read-only bound-range snapshots" in native,
              "opus_decoder_open_events": [{"rate": int(rate), "channels": int(channels)} for rate, channels in
                  re.findall(r"\[hwopus\] decoder opened: (\d+) Hz, (\d+) ch", native)],
              "scripted_input": bool(re.search(r"^\[env\] LARECOMP_(?:INPUT|HOST_INPUT)=", native, re.MULTILINE)),
              "input_detection": "native startup environment; external input events are not detected",
              "automatic_close": False,
              "observation_only": args.observe_only}
    (output / "status.json").write_text(json.dumps(status, indent=2) + "\n")
    print(json.dumps(status, indent=2))


if __name__ == "__main__":
    main()
