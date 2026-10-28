#!/usr/bin/env python3
"""Record an already focused Let's Go reference; never launch, stop or control it."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.recomp.letsgo.test"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--label", required=True, help="Short evidence label: letters, digits, underscores or hyphens")
    parser.add_argument("--duration", type=int, default=8, help="Recording seconds, 4 through 20 (default: 8)")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,47}", args.label):
        parser.error("--label must be 1..48 safe letters/digits/underscores/hyphens")
    if not 4 <= args.duration <= 20:
        parser.error("--duration must be between 4 and 20")
    adb, ffmpeg = shutil.which("adb"), shutil.which("ffmpeg")
    if not adb or not ffmpeg:
        parser.error("Cached host adb and ffmpeg must be available on PATH")
    adb_args = [adb, "-s", args.serial]

    def remote(argv, check=True, timeout=20):
        # adb shell passes through a remote shell even when subprocess uses argv.
        return subprocess.run([*adb_args, "shell", shlex.join([str(x) for x in argv])],
                              check=check, capture_output=True, timeout=timeout)

    def pids():
        result = remote(["pidof", PACKAGE], check=False)
        values = result.stdout.decode().strip().split()
        return values if result.returncode == 0 and all(x.isdigit() for x in values) else []

    before = pids()
    if not before:
        raise RuntimeError("Reference app is not alive; no launch attempted")
    focus = remote(["dumpsys", "window", "displays"]).stdout.decode(errors="replace")
    # Require the focused window specifically inside logical display0's block.
    blocks = list(re.finditer(r"(?m)^\s*Display:\s*mDisplayId=(\d+)\b", focus))
    display_zero = ""
    for index, block in enumerate(blocks):
        if block.group(1) == "0":
            display_zero = focus[block.end():blocks[index + 1].start() if index + 1 < len(blocks) else len(focus)]
            break
    current = re.search(r"(?m)^\s*mCurrentFocus=(.+)$", display_zero)
    top = re.search(r"\bmTopFocusedDisplayId=(\d+)\b", focus)
    if not current or PACKAGE + "/" not in current.group(1) or (top and top.group(1) != "0"):
        raise RuntimeError("Cannot verify the reference window is focused on display0; no capture attempted")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = ROOT / "evidence/letsgo-blinking" / f"{stamp}-{args.label}"
    output.mkdir(parents=True, exist_ok=False)
    temporary = f"/data/local/tmp/letsgo-blinking-{uuid.uuid4().hex}.mp4"
    status = {"package": PACKAGE, "label": args.label, "started_utc": stamp,
              "duration_requested_seconds": args.duration, "pids_before": before,
              "display": 0, "focus_before": current.group(1),
              "recording_display": "screenrecord default primary display",
              "app_control_performed": False, "device_executed": True,
              "temporary_recording": temporary}
    receipt = ROOT / "evidence/letsgo-installed.json"
    if receipt.is_file():
        installed = json.loads(receipt.read_text())
        if installed.get("package") == PACKAGE:
            status["installed_receipt"] = {key: installed.get(key) for key in
                                           ("package", "apk_sha256", "installed_utc")}
            status["installed_hash_source"] = "local installation receipt; not rehashed on device"
    try:
        result = remote(["screenrecord", "--time-limit", args.duration,
                         "--bit-rate", "8000000", temporary], check=False,
                        timeout=args.duration + 30)
        (output / "record.stdout").write_bytes(result.stdout)
        (output / "record.stderr").write_bytes(result.stderr)
        status["record_returncode"] = result.returncode
        result.check_returncode()
        subprocess.run([*adb_args, "pull", temporary, str(output / "screen.mp4")],
                       check=True, capture_output=True, timeout=40)
    finally:
        # The exact UUID path belongs to this invocation. Never remove app files or stop a process.
        cleanup = remote(["rm", "-f", temporary], check=False)
        status["temporary_cleanup_ok"] = cleanup.returncode == 0
        status["pids_after"] = pids()
        native = remote(["run-as", PACKAGE, "cat",
                         f"/sdcard/Android/data/{PACKAGE}/files/lasrecomp.log"], check=False)
        status["native_log_read_ok"] = native.returncode == 0
        if native.returncode == 0:
            (output / "native.log").write_bytes(native.stdout)
        (output / "status.json").write_text(json.dumps(status, indent=2) + "\n")

    # Sample the complete display, including the overlay. No numpy/image package is needed.
    video = output / "screen.mp4"
    raw = output / "samples.rgb"
    common = [ffmpeg, "-hide_banner", "-loglevel", "error", "-threads", "1", "-i", str(video),
              "-filter_threads", "1"]
    sample_command = [*common, "-vf", "fps=30,scale=192:108:flags=area", "-pix_fmt", "rgb24",
                      "-f", "rawvideo", str(raw)]
    tile_command = [*common, "-vf", "fps=4,scale=480:270:flags=area,tile=4x4:nb_frames=16",
                    "-frames:v", "1", "-update", "1", str(output / "contact.png")]
    with (output / "analysis.stderr").open("wb") as log:
        subprocess.run(sample_command, check=True, stdout=subprocess.DEVNULL, stderr=log, timeout=60)
        subprocess.run(tile_command, check=True, stdout=subprocess.DEVNULL, stderr=log, timeout=60)
    frame_bytes = 192 * 108 * 3
    frames = []
    with raw.open("rb") as stream:
        while pixels := stream.read(frame_bytes):
            if len(pixels) != frame_bytes:
                raise RuntimeError("Incomplete RGB sample frame")
            maximum = max(pixels)
            frames.append({"frame": len(frames), "time_seconds": len(frames) / 30,
                           "mean": sum(pixels) / frame_bytes,
                           "mean_rgb": [sum(pixels[channel::3]) / (192 * 108) for channel in range(3)],
                           "max_pixel": maximum, "fully_black": maximum == 0})
    if not frames:
        raise RuntimeError("Recording contains no decoded sample frames")
    black = sum(frame["fully_black"] for frame in frames)
    report = {"sample_fps": 30, "sample_size": [192, 108], "pixel_format": "rgb24",
              "overlay_included": True, "black_rule": "max pixel channel == 0",
              "sampled_frames": len(frames), "fully_black_frames": black,
              "fully_black_fraction": black / len(frames), "frames": frames,
              "sample_command": sample_command, "contact_command": tile_command}
    (output / "frame-stats.json").write_text(json.dumps(report, indent=2) + "\n")
    status["analysis_complete"] = True
    status["ffmpeg_version"] = subprocess.run([ffmpeg, "-version"], check=True,
                                              capture_output=True, text=True).stdout.splitlines()[0]
    (output / "status.json").write_text(json.dumps(status, indent=2) + "\n")
    print(f"{output.relative_to(ROOT)}: {black}/{len(frames)} fully black samples ({black / len(frames):.1%})")


if __name__ == "__main__":
    main()
