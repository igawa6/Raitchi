#!/usr/bin/env python3
"""Stream owned extracted Eevee files into the separate reference app; refuse overwrite."""
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


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    args = parser.parse_args()
    manual = ROOT / "evidence/manual-session.json"
    if manual.is_file() and json.loads(manual.read_text()).get("active"):
        parser.error("Owner-controlled manual session is active; no input transfer attempted")
    source = ROOT / "private/letsgo-baseline/extracted"
    generation = json.loads((ROOT / "work/letsgo-baseline/build/android-codegen/generation.json").read_text())
    for name, info in generation["module_inputs"].items():
        if sha(source / "exefs" / name) != info["sha256"]:
            raise RuntimeError(f"Private NSO differs from generated code: {name}")
    files = []
    for folder in (source / "exefs", source / "romfs"):
        if not folder.is_dir() or folder.is_symlink():
            raise RuntimeError("Expected ordinary private exefs/romfs directories")
        for path in folder.rglob("*"):
            if path.is_symlink() or "\n" in str(path):
                raise RuntimeError("Refusing links or newline-containing private input paths")
            if path.is_file():
                files.append(path)
    total_bytes = sum(p.stat().st_size for p in files)
    adb = ["adb", "-s", args.serial]

    def shell(parts, check=True):
        return subprocess.run([*adb, "shell", shlex.join([str(p) for p in parts])],
                              check=check, capture_output=True, text=True)

    shell(["pm", "path", PACKAGE])
    shell(["test", "-w", DATA])
    destination = DATA + "/extracted"
    if shell(["test", "-e", destination], check=False).returncode == 0:
        raise RuntimeError("Extracted destination exists; refusing overwrite")
    free_kib = int(shell(["df", "-k", DATA]).stdout.splitlines()[-1].split()[3])
    if free_kib * 1024 < total_bytes + 64 * 1024 * 1024:
        raise RuntimeError("Insufficient device space for new extracted inputs")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    staging = DATA + "/.extracted-incoming-" + stamp
    shell(["mkdir", staging])
    evidence = ROOT / "evidence/letsgo-input-transfer" / stamp
    evidence.mkdir(parents=True, exist_ok=False)
    print(f"Streaming {len(files)} private files ({total_bytes / 1e9:.2f} GB); no keys or intermediate archive.", flush=True)
    with (evidence / "transfer.log").open("wb") as log:
        producer = subprocess.Popen(["tar", "-C", str(source), "-cf", "-", "exefs", "romfs"],
                                    stdout=subprocess.PIPE, stderr=log)
        try:
            consumer = subprocess.Popen([*adb, "shell", shlex.join(["tar", "-xf", "-", "-C", staging])],
                                        stdin=producer.stdout, stdout=log, stderr=log)
        except BaseException:
            producer.stdout.close()
            producer.terminate()
            producer.wait()
            raise
        producer.stdout.close()
        consumer_status = consumer.wait()
        producer_status = producer.wait()
    if consumer_status or producer_status:
        raise RuntimeError("Transfer failed; own partial staging and private log retained")
    # Run checks as the application UID, verifying its access as well as module content.
    for name, info in generation["module_inputs"].items():
        actual = shell(["run-as", PACKAGE, "sha256sum", staging + "/exefs/" + name]).stdout.split()[0]
        if actual != info["sha256"]:
            raise RuntimeError(f"Application-UID NSO hash mismatch: {name}")
    count_script = 'find "$1" -type f | wc -l'
    actual_count = int(shell(["run-as", PACKAGE, "sh", "-c", count_script, "count-inputs", staging]).stdout)
    if actual_count != len(files):
        raise RuntimeError("Application-UID transferred file count differs from source")
    publish = 'test ! -e "$2" && test ! -L "$2" && mv "$1" "$2"'
    shell(["sh", "-c", publish, "publish-inputs", staging, destination])
    result = {"package": PACKAGE, "completed_utc": datetime.now(timezone.utc).isoformat(),
              "files": len(files), "bytes": total_bytes, "app_uid_access_verified": True,
              "module_sha256": {n: i["sha256"] for n, i in generation["module_inputs"].items()},
              "romfs_verification": "file count and successful tar transfer; no per-file content hashes",
              "destination": destination, "game_executed": False}
    (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Private inputs transferred; four NSO hashes and file count verified as app UID.")


if __name__ == "__main__":
    main()
