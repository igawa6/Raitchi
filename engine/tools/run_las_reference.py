#!/usr/bin/env python3
"""Run an input fixture in the isolated Thor app, then restore its environment.

Only com.lasrecomp.android.test is controlled. Save files and other applications
are never copied or removed. Results and previous environment stay local.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.lasrecomp.android.test"
DATA = f"/sdcard/Android/data/{PACKAGE}/files"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--fixture", type=Path, default=ROOT / "fixtures/las/title-input.json")
    args = parser.parse_args()
    manual = ROOT / "evidence/manual-session.json"
    if manual.exists() and json.loads(manual.read_text()).get("active"):
        parser.error("Owner-controlled manual session is active; refusing timed input or automatic stops")
    args.fixture = args.fixture.resolve()
    fixture = json.loads(args.fixture.read_text())
    seconds = float(fixture["seconds"])
    checkpoints = sorted(float(x) for x in fixture["checkpoints"])
    if not 0 < seconds <= 600 or any(x < 0 or x > seconds for x in checkpoints):
        raise ValueError("Fixture duration/checkpoints out of range")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    result_dir = ROOT / "evidence/reference" / stamp
    result_dir.mkdir(parents=True, exist_ok=False)
    adb = ["adb", "-s", args.serial]

    def shell(argv, check=True):
        return subprocess.run([*adb, "shell", shlex.join([str(x) for x in argv])],
                              check=check, capture_output=True)

    def read(path):
        result = shell(["cat", path], check=False)
        if result.returncode:
            if shell(["test", "-e", path], check=False).returncode == 0:
                raise RuntimeError("Existing test-app file is unreadable")
            return None
        return result.stdout

    def push(content, name, remote):
        path = result_dir / name
        path.write_bytes(content)
        subprocess.run([*adb, "push", str(path), remote], check=True, capture_output=True)

    def private(argv, check=True, content=None):
        return subprocess.run([*adb, "shell", shlex.join(["run-as", PACKAGE, *[str(x) for x in argv]])],
                              check=check, input=content, capture_output=True)

    def private_write(path, content):
        private(["sh", "-c", 'cat >"$1"', "reference-write", path], content=content)

    def private_read(path):
        result = private(["cat", path], check=False)
        if result.returncode and private(["test", "-e", path], check=False).returncode == 0:
            raise RuntimeError("Existing private app file is unreadable")
        return result.stdout if result.returncode == 0 else None

    shell(["pm", "path", PACKAGE])
    previous = read(DATA + "/env.txt")
    if previous is not None:
        (result_dir / "previous-env.txt").write_bytes(previous)
    settings = (read(DATA + "/larecomp.ini") or b"").decode()
    for key, value in fixture["settings"].items():
        if not re.fullmatch(r"[a-z_]+", key) or "\n" in str(value):
            raise ValueError("Invalid fixture setting")
        line = f"{key} = {value}"
        if re.search(r"(?m)^" + re.escape(key) + r"\s*=.*$", settings):
            settings = re.sub(r"(?m)^" + re.escape(key) + r"\s*=.*$", line, settings)
        else:
            settings += "\n" + line + "\n"
    setting_name = f"reference-{stamp}.ini"
    environment = dict(fixture["environment"], LARECOMP_SETTINGS=setting_name)
    lines = []
    for key, value in environment.items():
        line = f"{key}={value}"
        if not re.fullmatch(r"LARECOMP_[A-Z_]+", key) or "\n" in str(value) or len(line.encode()) >= 1022:
            raise ValueError("Environment line incompatible with Android parser")
        lines.append(line)
    ours = ("\n".join(lines) + "\n").encode()
    manifest = {"fixture": args.fixture.relative_to(ROOT).as_posix(), "started_utc": stamp,
                "package": PACKAGE, "settings": fixture["settings"],
                "environment": environment, "checkpoints": [], "completed": False}
    driver = fixture.get("driver", "existing")
    if driver not in ("existing", "bundled"):
        raise ValueError("Unsupported reference driver selection")
    manifest["driver_selection"] = driver
    driver_active = False
    driver_previous = False
    driver_root = "files/gpu_driver"
    current = driver_root + "/current"
    cache = driver_root + "/reference-bundled"
    driver_backup = driver_root + "/reference-previous-" + stamp
    prefs_path = "shared_prefs/gpu_driver.xml"
    old_prefs = None
    shell(["am", "force-stop", PACKAGE])
    try:
        if driver == "bundled":
            old_prefs = private_read(prefs_path)
            if old_prefs is not None:
                (result_dir / "previous-gpu-driver.xml").write_bytes(old_prefs)
            bundle = ROOT / "work/las-baseline/build/android/deps/turnip"
            meta = json.loads((bundle / "meta.json").read_text())
            library = meta["libraryName"]
            if Path(library).name != library:
                raise ValueError("Invalid pinned driver library name")
            private(["mkdir", "-p", cache, "shared_prefs"])
            staging = "/data/local/tmp/switch-recomp-reference-driver"
            shell(["mkdir", "-p", staging])
            for name in ("meta.json", library):
                local = bundle / name
                digest = hashlib.sha256(local.read_bytes()).hexdigest()
                present = private(["sha256sum", cache + "/" + name], check=False).stdout.decode().split()
                if not present or present[0] != digest:
                    subprocess.run([*adb, "push", str(local), staging + "/" + name], check=True, capture_output=True)
                    private(["cp", staging + "/" + name, cache + "/" + name])
            private(["rm", "-f", cache + "/.probe"])
            if private(["test", "-e", current], check=False).returncode == 0:
                private(["mv", current, driver_backup])
                driver_previous = True
            private(["mv", cache, current])
            driver_active = True
            private_write(prefs_path, b'<?xml version="1.0" encoding="utf-8"?><map><boolean name="use" value="true" /></map>\n')
            manifest["driver_library_sha256"] = hashlib.sha256((bundle / library).read_bytes()).hexdigest()
        push(settings.encode(), "settings.ini", DATA + "/" + setting_name)
        push(ours, "env.txt", DATA + "/env.txt")
        shell(["input", "keyevent", "KEYCODE_WAKEUP"])
        shell(["am", "start", "--display", "0", "-n", PACKAGE + "/com.lasrecomp.android.LasActivity"])
        start = time.monotonic()
        for at in checkpoints + [seconds]:
            while time.monotonic() - start < at:
                time.sleep(max(0, min(1, at - (time.monotonic() - start))))
            pid = shell(["pidof", PACKAGE], check=False).stdout.decode().strip()
            entry = {"elapsed_seconds": round(time.monotonic() - start, 2), "alive": bool(pid)}
            manifest["checkpoints"].append(entry)
            if not pid:
                raise RuntimeError("Test process exited before the fixture completed")
            if at in checkpoints:
                with (result_dir / f"screen-{int(at):03d}.png").open("wb") as output:
                    subprocess.run([*adb, "exec-out", "screencap", "-p"], check=True, stdout=output)
                print(f"Reference checkpoint {int(at)}s: alive; screenshot saved.", flush=True)
        manifest["completed"] = True
    finally:
        # Preserve full native logs before stopping; logcat is a fallback only.
        native = read(DATA + "/lasrecomp.log")
        if native is not None:
            (result_dir / "native.log").write_bytes(native)
        logs = subprocess.run([*adb, "logcat", "-d", "-t", "2000", "-s", "LASRecomp", "AndroidRuntime"], capture_output=True)
        (result_dir / "logcat.log").write_bytes(logs.stdout)
        shell(["am", "force-stop", PACKAGE], check=False)
        if driver_active:
            private(["mv", current, cache])
            if driver_previous:
                private(["mv", driver_backup, current])
            if old_prefs is None:
                private(["rm", "-f", prefs_path])
            else:
                private_write(prefs_path, old_prefs)
            manifest["driver_selection_restored"] = True
        if read(DATA + "/env.txt") == ours:
            if previous is None:
                shell(["rm", "-f", DATA + "/env.txt"])
            else:
                push(previous, "restore-env.txt", DATA + "/env.txt")
            manifest["environment_restored"] = True
        else:
            manifest["environment_restored"] = False
            print("Test environment changed during session; preserving the newer file.", flush=True)
        shell(["rm", "-f", DATA + "/" + setting_name], check=False)
        (result_dir / "result.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print("Reference evidence:", result_dir.relative_to(ROOT), flush=True)


if __name__ == "__main__":
    main()
