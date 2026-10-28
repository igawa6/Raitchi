#!/usr/bin/env python3
"""Sample the running Let's Go reference without input, restarts or configuration changes."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.recomp.letsgo.test"
SENSORS = r'''
for zone in /sys/class/thermal/thermal_zone*; do
    [ -d "$zone" ] || continue
    sensor_type=$(cat "$zone/type" 2>/dev/null)
    sensor_temp=$(cat "$zone/temp" 2>/dev/null)
    printf 'thermal\t%s\t%s\t%s\n' "$zone" "$sensor_type" "$sensor_temp"
done
for node in /sys/class/kgsl/kgsl-3d0/gpuclk /sys/class/kgsl/kgsl-3d0/gpubusy /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq /sys/class/kgsl/kgsl-3d0/devfreq/gpu_load; do
    [ -e "$node" ] || continue
    sensor_value=$(cat "$node" 2>/dev/null)
    printf 'gpu\t%s\t%s\n' "$node" "$sensor_value"
done
'''


def parse_stat(raw):
    # comm is parenthesized and may contain spaces or ')'; split after its last ')'.
    close = raw.rfind(")")
    fields = raw[close + 1:].split()
    if close < 0 or len(fields) < 20:
        raise RuntimeError("Cannot parse process stat")
    return {"pid": int(raw.split("(", 1)[0]), "state": fields[0],
            "utime_ticks": int(fields[11]), "stime_ticks": int(fields[12]),
            "starttime_ticks": int(fields[19])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--duration", type=int, default=12, help="Interval seconds, 4..30 (default: 12)")
    parser.add_argument("--perf", action="store_true", help="Also record user CPU cycles with on-device simpleperf")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,47}", args.label):
        parser.error("--label must contain 1..48 safe letters/digits/underscores/hyphens")
    if not 4 <= args.duration <= 30:
        parser.error("--duration must be between 4 and 30")
    adb = shutil.which("adb")
    if not adb:
        parser.error("Cached adb must be on PATH")
    adb_args = [adb, "-s", args.serial]

    def remote(argv, timeout=20):
        return subprocess.run([*adb_args, "shell", shlex.join([str(x) for x in argv])],
                              capture_output=True, timeout=timeout)

    def pids():
        result = remote(["pidof", PACKAGE])
        return result.stdout.decode().split() if result.returncode == 0 else []

    running = pids()
    if len(running) != 1 or not running[0].isdigit():
        raise RuntimeError("Require exactly one running reference process; no launch attempted")
    pid = int(running[0])
    focus_result = remote(["dumpsys", "window", "displays"])
    focus_result.check_returncode()
    focus = focus_result.stdout.decode(errors="replace")
    blocks = list(re.finditer(r"(?m)^\s*Display:\s*mDisplayId=(\d+)\b", focus))
    display_zero = ""
    for index, block in enumerate(blocks):
        if block.group(1) == "0":
            display_zero = focus[block.end():blocks[index + 1].start() if index + 1 < len(blocks) else len(focus)]
            break
    current = re.search(r"(?m)^\s*mCurrentFocus=(.+)$", display_zero)
    top = re.search(r"\bmTopFocusedDisplayId=(\d+)\b", focus)
    if not current or PACKAGE + "/" not in current.group(1) or (top and top.group(1) != "0"):
        raise RuntimeError("Cannot verify the reference has focus on display0; no sample attempted")
    tick_result = remote(["getconf", "CLK_TCK"])
    tick_result.check_returncode()
    ticks = int(tick_result.stdout.strip())
    if ticks <= 0:
        raise RuntimeError("Invalid device CLK_TCK")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = ROOT / "evidence/letsgo-thermal" / f"{stamp}-{args.label}"
    output.mkdir(parents=True, exist_ok=False)
    status = {"package": PACKAGE, "pid": pid, "label": args.label, "started_utc": stamp,
              "duration_requested_seconds": args.duration, "clock_ticks_per_second": ticks,
              "focus_before": current.group(1), "display": 0, "perf_requested": args.perf,
              "app_control_performed": False}
    receipt = ROOT / "evidence/letsgo-installed.json"
    if receipt.is_file():
        installed = json.loads(receipt.read_text())
        if installed.get("package") == PACKAGE:
            status["installed_receipt"] = {key: installed.get(key) for key in
                                           ("package", "apk_sha256", "installed_utc")}
            status["installed_hash_source"] = "local receipt; not rehashed on device"

    def save(name, command, timeout=20):
        result = remote(command, timeout=timeout)
        (output / name).write_bytes(result.stdout)
        (output / (name + ".stderr")).write_bytes(result.stderr)
        return result

    def snapshot(label):
        sensors = save(f"sensors-{label}.txt", ["sh", "-c", SENSORS])
        thermal, gpu = [], []
        for line in sensors.stdout.decode(errors="replace").splitlines():
            parts = line.split("\t")
            if len(parts) == 4 and parts[0] == "thermal":
                thermal.append({"path": parts[1], "type": parts[2], "raw_temperature": parts[3] or None})
            elif len(parts) == 3 and parts[0] == "gpu":
                gpu.append({"path": parts[1], "raw_value": parts[2] or None})
        battery = save(f"battery-{label}.txt", ["dumpsys", "battery"])
        native = save(f"native-{label}.log", ["run-as", PACKAGE, "cat",
                      f"/sdcard/Android/data/{PACKAGE}/files/lasrecomp.log"])
        return {"observed_utc": datetime.now(timezone.utc).isoformat(),
                "thermal_sensors": thermal, "gpu_nodes": gpu,
                "sensor_units": "raw sysfs values; no uniform temperature/frequency unit assumed",
                "battery_read_ok": battery.returncode == 0,
                "native_log_read_ok": native.returncode == 0}

    status["before"] = snapshot("before")
    perf_path = f"files/letsgo-load-{uuid.uuid4().hex}.data"
    try:
        # Keep slow sensor/log/receipt operations outside these two stat reads.
        raw_before = remote(["run-as", PACKAGE, "cat", f"/proc/{pid}/stat"])
        raw_before.check_returncode()
        begin = time.monotonic()
        stat_before = parse_stat(raw_before.stdout.decode())
        if args.perf:
            recording = save("record.stdout", ["run-as", PACKAGE, "/system/bin/simpleperf", "record",
                             "-p", pid, "-e", "cpu-cycles:u", "-f", "200", "--duration", args.duration,
                             "--call-graph", "fp", "-o", perf_path], timeout=args.duration + 20)
            status["perf_record_returncode"] = recording.returncode
        else:
            time.sleep(args.duration)
        raw_after = remote(["run-as", PACKAGE, "cat", f"/proc/{pid}/stat"])
        end = time.monotonic()
        raw_after.check_returncode()
        stat_after = parse_stat(raw_after.stdout.decode())
        (output / "proc-stat-before.txt").write_bytes(raw_before.stdout)
        (output / "proc-stat-after.txt").write_bytes(raw_after.stdout)
        elapsed = end - begin
        status.update(proc_before=stat_before, proc_after=stat_after, host_monotonic_elapsed_seconds=elapsed)
        if stat_before["pid"] != pid or stat_after["pid"] != pid or stat_before["starttime_ticks"] != stat_after["starttime_ticks"]:
            raise RuntimeError("Process changed during sample; CPU interval is invalid")
        user = stat_after["utime_ticks"] - stat_before["utime_ticks"]
        system = stat_after["stime_ticks"] - stat_before["stime_ticks"]
        if min(user, system) < 0:
            raise RuntimeError("Process CPU counters went backwards")
        status["cpu"] = {"user_ticks_delta": user, "system_ticks_delta": system,
                         "user_percent": 100 * user / ticks / elapsed,
                         "system_percent": 100 * system / ticks / elapsed,
                         "total_percent": 100 * (user + system) / ticks / elapsed,
                         "basis": "100% equals one CPU core; process total may exceed 100%",
                         "timing": "host monotonic interval includes ending ADB stat-read latency"}
        # Copy/report only after ending the CPU interval. Keep the remote record if copying fails.
        if args.perf:
            copied = False
            with (output / "perf.data").open("wb") as stream:
                result = subprocess.run([*adb_args, "exec-out", shlex.join(
                    ["run-as", PACKAGE, "cat", perf_path])], stdout=stream,
                    stderr=subprocess.PIPE, timeout=40)
            copied = result.returncode == 0 and (output / "perf.data").stat().st_size > 0
            status["perf_copied"] = copied
            if copied:
                report = save("report.txt", ["run-as", PACKAGE, "/system/bin/simpleperf", "report",
                              "-i", perf_path, "--sort", "comm,dso,symbol"], timeout=40)
                status["perf_report_returncode"] = report.returncode
                cleanup = remote(["run-as", PACKAGE, "rm", "-f", perf_path])
                status["perf_record_cleanup_ok"] = cleanup.returncode == 0
            else:
                status["retained_perf_record"] = perf_path
                (output / "perf-copy.stderr").write_bytes(result.stderr)
    finally:
        status["pids_after"] = pids()
        status["after"] = snapshot("after")
        (output / "status.json").write_text(json.dumps(status, indent=2) + "\n")
    total = status["cpu"]["total_percent"]
    print(f"{output.relative_to(ROOT)}: CPU {total:.1f}% over {elapsed:.3f}s (one core = 100%)")


if __name__ == "__main__":
    main()
