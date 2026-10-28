#!/usr/bin/env python3
"""Read the isolated LAS app's PID/logs; optionally capture the primary display."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.lasrecomp.android.test"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--screenshot", action="store_true")
    args = parser.parse_args()
    adb = ["adb", "-s", args.serial]
    evidence = ROOT / "evidence"
    evidence.mkdir(exist_ok=True)
    pid = subprocess.run([*adb, "shell", "pidof", PACKAGE], capture_output=True, text=True)
    pids = pid.stdout.strip().split()
    status = {"package": PACKAGE, "alive": bool(pids), "pids": pids}
    logargs = [*adb, "logcat", "-d", "-t", "1500"]
    if pids:
        if len(pids) != 1 or not pids[0].isdigit():
            raise RuntimeError("Expected one isolated app process")
        logargs += ["--pid", pids[0]]
    else:
        logargs += ["-s", "LASRecomp", "AndroidRuntime", "SDL"]
    logs = subprocess.run(logargs, check=True, capture_output=True, text=True).stdout
    (evidence / "las-app.log").write_text(logs)
    status["log_lines"] = len(logs.splitlines())
    status["fatal_lines"] = [line for line in logs.splitlines() if "FATAL EXCEPTION" in line or "FATAL:" in line]
    if args.screenshot:
        with (evidence / "las-screen.png").open("wb") as output:
            subprocess.run([*adb, "exec-out", "screencap", "-p"], stdout=output, check=True)
    (evidence / "las-app-status.json").write_text(json.dumps(status, indent=2) + "\n")
    print(json.dumps(status, indent=2))


if __name__ == "__main__":
    main()
