#!/usr/bin/env python3
"""Start an uncapped manual LAS session; no scripted input or automatic stop."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.lasrecomp.android.test"
DATA = f"/sdcard/Android/data/{PACKAGE}/files"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    args = parser.parse_args()
    adb = ["adb", "-s", args.serial]

    def shell(parts, check=True, content=None):
        return subprocess.run([*adb, "shell", shlex.join([str(x) for x in parts])],
                              check=check, input=content, capture_output=True)

    def private(parts, check=True, content=None):
        return shell(["run-as", PACKAGE, *parts], check, content)

    if shell(["pidof", PACKAGE], check=False).returncode == 0:
        raise RuntimeError("Test app is already running; leaving its session untouched")
    existing_env = shell(["cat", DATA + "/env.txt"], check=False)
    if existing_env.returncode == 0 and existing_env.stdout.strip():
        raise RuntimeError("Existing env.txt requires review before manual play; nothing changed")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    evidence = ROOT / "evidence/manual" / stamp
    evidence.mkdir(parents=True, exist_ok=False)
    settings = shell(["cat", DATA + "/larecomp.ini"]).stdout
    (evidence / "previous-settings.ini").write_bytes(settings)
    changes = {"frame_gen": "0", "frame_rate": "0", "resolution": "1", "upscaler": "bilinear", "show_fps": "1"}
    text = settings.decode()
    for key, value in changes.items():
        pattern = r"(?m)^" + key + r"\s*=.*$"
        text = re.sub(pattern, f"{key} = {value}", text) if re.search(pattern, text) else text + f"\n{key} = {value}\n"
    driver_root = "files/gpu_driver"
    current = driver_root + "/current"
    cache = driver_root + "/reference-bundled"
    if private(["test", "-e", current], check=False).returncode == 0:
        raise RuntimeError("A current driver already exists; leaving its selection untouched")
    private(["test", "-f", cache + "/meta.json"])
    old_prefs = private(["cat", "shared_prefs/gpu_driver.xml"], check=False)
    if old_prefs.returncode == 0:
        (evidence / "previous-gpu-driver.xml").write_bytes(old_prefs.stdout)
    private(["rm", "-f", cache + "/.probe"])
    private(["mv", cache, current])
    private(["mkdir", "-p", "shared_prefs"])
    private(["sh", "-c", 'cat >"$1"', "manual-pref", "shared_prefs/gpu_driver.xml"],
            content=b'<?xml version="1.0" encoding="utf-8"?><map><boolean name="use" value="true" /></map>\n')
    shell(["sh", "-c", 'cat >"$1"', "manual-settings", DATA + "/larecomp.ini"], content=text.encode())
    marker = {"active": True, "started_utc": stamp, "package": PACKAGE, "automatic_close": False,
              "scripted_input": False, "owner_controls_device": True,
              "driver": "previously verified bundled Turnip", "settings": changes,
              "previous_configuration": evidence.relative_to(ROOT).as_posix()}
    (ROOT / "evidence/manual-session.json").write_text(json.dumps(marker, indent=2) + "\n")
    shell(["input", "keyevent", "KEYCODE_WAKEUP"])
    shell(["am", "start", "--display", "0", "-n", PACKAGE + "/com.lasrecomp.android.LasActivity"])
    print("Manual session launched: no scripted input, no timer, no automatic close.")
    print("Choose Play, then Autosave to resume the latest committed test progress.")


if __name__ == "__main__":
    main()
