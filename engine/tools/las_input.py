#!/usr/bin/env python3
"""Send supervised test input only while the isolated LAS window is focused."""
import argparse
import json
from pathlib import Path
import subprocess
import time

PACKAGE = "com.lasrecomp.android.test"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--interval", type=float, default=0.4)
    parser.add_argument("--longpress", action="store_true")
    parser.add_argument("key", help="Android KEYCODE_* name")
    args = parser.parse_args()
    manual = Path(__file__).resolve().parents[1] / "evidence/manual-session.json"
    if manual.exists() and json.loads(manual.read_text()).get("active"):
        parser.error("Owner-controlled manual session is active; no automated input sent")
    if not args.key.startswith("KEYCODE_") or not args.key.replace("_", "").isalnum():
        parser.error("Expected a KEYCODE_* name")
    if not 1 <= args.repeat <= 30 or not 0.1 <= args.interval <= 2:
        parser.error("Repeat or interval outside supervised-test limits")
    adb = ["adb", "-s", args.serial]
    for _ in range(args.repeat):
        pid = subprocess.run([*adb, "shell", "pidof", PACKAGE], capture_output=True, text=True).stdout.strip()
        windows = subprocess.run([*adb, "shell", "dumpsys", "window", "displays"], check=True, capture_output=True, text=True).stdout
        focused = [line for line in windows.splitlines() if "mCurrentFocus=" in line]
        if not pid or not any(PACKAGE + "/" in line for line in focused):
            raise RuntimeError("Isolated LAS window is not focused; no further input sent")
        command = [*adb, "shell", "input", "-d", "0", "keyevent"]
        if args.longpress:
            command.append("--longpress")
        subprocess.run([*command, args.key], check=True)
        time.sleep(args.interval)
    print(f"Sent {args.repeat} guarded {args.key} event(s).")


if __name__ == "__main__":
    main()
