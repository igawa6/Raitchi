#!/usr/bin/env python3
"""Select stock or the pinned bundled Turnip for only the isolated Let's Go app."""
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
PINNED_TURNIP = "3a1d3425a87cfd0ebc9cc87a03a387f4814f9e6f6084457f16756400c44e385e"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--driver", choices=("stock", "bundled"), required=True)
    args = parser.parse_args()
    manual = ROOT / "evidence/manual-session.json"
    if manual.is_file() and json.loads(manual.read_text()).get("active"):
        parser.error("Owner-controlled manual session active; no driver selection changed")
    adb = ["adb", "-s", args.serial]

    def shell(parts, check=True, **kwargs):
        return subprocess.run([*adb, "shell", "-T", shlex.join([str(p) for p in parts])],
                              check=check, capture_output=True, **kwargs)

    shell(["pm", "path", PACKAGE])
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    evidence = ROOT / "evidence/letsgo-driver" / stamp
    evidence.mkdir(parents=True, exist_ok=False)
    old = shell(["run-as", PACKAGE, "cat", DATA + "/env.txt"], check=False)
    if old.returncode == 0:
        (evidence / "previous-env.txt").write_bytes(old.stdout)
    lines = [line for line in old.stdout.decode().splitlines() if not line.startswith("LARECOMP_GPU_DRIVER_")]
    result = {"package": PACKAGE, "selection": args.driver, "applies_on_fresh_process": True}
    if args.driver == "bundled":
        bundle = ROOT / "work/las-baseline/build/android/deps/turnip"
        metadata = json.loads((bundle / "meta.json").read_text())
        name = metadata["libraryName"]
        if Path(name).name != name:
            raise RuntimeError("Invalid pinned driver soname")
        library = bundle / name
        if hashlib.sha256(library.read_bytes()).hexdigest() != PINNED_TURNIP:
            raise RuntimeError("Bundled driver differs from the verified Thor reference")
        relative = "files/gpu_driver/reference-bundled"
        shell(["run-as", PACKAGE, "mkdir", "-p", relative])
        target = relative + "/" + name
        present = shell(["run-as", PACKAGE, "sha256sum", target], check=False).stdout.decode().split()
        if not present or present[0] != PINNED_TURNIP:
            with library.open("rb") as stream:
                shell(["run-as", PACKAGE, "sh", "-c", 'umask 077; cat >"$1"', "stage-driver", target], stdin=stream)
        actual = shell(["run-as", PACKAGE, "sha256sum", target]).stdout.decode().split()[0]
        if actual != PINNED_TURNIP:
            raise RuntimeError("App-internal driver transfer hash mismatch")
        private_root = shell(["run-as", PACKAGE, "pwd"]).stdout.decode().strip()
        lines += [f"LARECOMP_GPU_DRIVER_DIR={private_root}/{relative}", f"LARECOMP_GPU_DRIVER_LIB={name}"]
        result.update(driver_sha256=actual, driver_metadata=metadata)
    incoming = DATA + "/env.txt.incoming-" + stamp
    shell(["run-as", PACKAGE, "sh", "-c", 'umask 077; cat >"$1"', "write-driver-env", incoming],
          input=("\n".join(lines) + "\n").encode())
    shell(["run-as", PACKAGE, "mv", incoming, DATA + "/env.txt"])
    (evidence / "selection.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Selected {args.driver} for the separate app's next fresh process; no system driver change.")


if __name__ == "__main__":
    main()
