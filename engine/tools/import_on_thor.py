#!/usr/bin/env python3
"""Build a LAS-only Android DEX importer; --serial explicitly imports on that device.

Default operation is host-only. Device paths come from ignored inventory evidence;
keys and game packages stay on the device. Existing extraction folders are refused.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.lasrecomp.android.test"
DESTINATION = f"/sdcard/Android/data/{PACKAGE}/files/extracted"
MAIN = "com.lasrecomp.android.LasDeviceImporter"

LAUNCHER = r'''package com.lasrecomp.android;
import java.io.*;
import java.nio.channels.FileChannel;
import java.util.Map;
public final class LasDeviceImporter {
    public static void main(String[] args) throws Exception {
        if (args.length != 3) throw new IllegalArgumentException("package, keys, destination required");
        File input = new File(args[0]), keys = new File(args[1]), dest = new File(args[2]);
        if (!input.isFile() || !input.canRead() || !keys.isFile() || !keys.canRead())
            throw new IOException("Importer inputs are not readable in this process context");
        Map<String, byte[]> parsed;
        try (InputStream stream = new FileInputStream(keys)) { parsed = NxPackage.parseKeys(stream); }
        // mkdir is exclusive: a completed or interrupted extraction is never overwritten.
        if (!dest.mkdir()) throw new IOException("Destination exists or cannot be created; refusing import");
        long start = System.nanoTime();
        try (FileChannel channel = FileChannel.open(input.toPath())) {
            String error = NxPackage.extract(channel, parsed, dest, "ae16f71e002af8cb",
                (done, total) -> System.out.printf("\r%.1f / %.1f GB", done / 1e9, total / 1e9));
            if (error != null) throw new IOException(error);
        }
        if (!new File(dest, "exefs/main").isFile() || !new File(dest, "exefs/sdk").isFile()
            || !new File(dest, "romfs").isDirectory()) throw new IOException("Incomplete extraction");
        System.out.printf("%nLAS import completed in %.1f s%n", (System.nanoTime() - start) / 1e9);
    }
}
'''


def run(argv, **kwargs):
    return subprocess.run([str(arg) for arg in argv], check=True, text=True, **kwargs)


def build(sdk, classes):
    run([sys.executable, ROOT / "tools/verify_sources.py"])
    if classes is None:
        preferred = ROOT / "build/host-importer/las"
        classes = preferred if preferred.is_dir() else ROOT / "build/host-importer"
    classes = classes.resolve()
    constants = run(["javap", "-classpath", classes, "-constants",
                     "com.lasrecomp.android.NxPackage"], capture_output=True).stdout
    title = re.search(r"TITLE_ID = (\d+)[lL];", constants)
    if not title or int(title.group(1)) != 0x01006BB00C6F0000:
        raise RuntimeError("Compiled host importer must select LAS; refusing another title")
    inputs = sorted(path for path in (classes / "com/lasrecomp/android").glob("*.class")
                    if path.name in ("LasActivity.class", "NxPackage.class")
                    or path.name.startswith("NxPackage$"))
    if not any(path.name == "LasActivity.class" for path in inputs):
        raise RuntimeError("Generated LasActivity helper missing; prepare host importer classes first")
    out = ROOT / "build/device-importer/las"
    out.mkdir(parents=True, exist_ok=True)
    launcher = out / "LasDeviceImporter.java"
    launcher.write_text(LAUNCHER)
    run(["javac", "--release", "17", "-classpath", classes, "-d", out, launcher])
    jar = out / "importer.jar"
    with zipfile.ZipFile(jar, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in inputs:
            archive.write(path, path.relative_to(classes).as_posix())
        compiled = out / "com/lasrecomp/android/LasDeviceImporter.class"
        archive.write(compiled, compiled.relative_to(out).as_posix())
    run([sdk / "build-tools/36.0.0/d8", "--release", "--min-api", "30", "--lib",
         sdk / "platforms/android-36/android.jar", "--output", out, jar])
    dex = out / "classes.dex"
    if not dex.read_bytes().startswith(b"dex\n"):
        raise RuntimeError("D8 did not produce a DEX file")
    print("Built build/device-importer/las/classes.dex; no game data or keys included.")
    return dex


def select_inputs(inventory):
    packages = [entry["path"] for entry in inventory["device_inputs"]
                if "[01006BB00C6F0000]" in entry["path"].upper() and "[v0]" in entry["path"]]
    keys = [entry["path"] for entry in inventory["key_file_presence_only"]
            if entry["path"].startswith(("/sdcard/", "/storage/"))]
    if len(packages) != 1 or not keys:
        raise RuntimeError("Inventory must identify one LAS base NSP and a device key path")
    return packages[0], keys[0]


def import_device(serial, dex, inventory_path):
    package, key = select_inputs(json.loads(inventory_path.read_text()))
    adb = ["adb", "-s", serial]

    def shell(argv, **kwargs):
        # adb joins shell operands; supply one correctly quoted remote command.
        return run([*adb, "shell", shlex.join([str(arg) for arg in argv])], **kwargs)

    preflight = '''set -eu
pm path "$1" >/dev/null
test -r "$2" || { echo 'Device NSP is not shell-readable' >&2; exit 1; }
test -r "$3" || { echo 'Device keys are not shell-readable' >&2; exit 1; }
test -d "$4" && test -w "$4" || { echo 'Install/prepare isolated app files directory first' >&2; exit 1; }
if test -e "$5" || test -L "$5"; then echo 'Extraction destination already exists; refusing import' >&2; exit 1; fi
'''
    shell(["sh", "-c", preflight, "las-import-preflight", PACKAGE, package, key,
           str(PurePosixPath(DESTINATION).parent), DESTINATION], capture_output=True)
    digest = hashlib.sha256(dex.read_bytes()).hexdigest()
    remote = f"/data/local/tmp/switch-recomp-las-importer-{digest[:16]}"
    shell(["mkdir", "-p", remote])
    run([*adb, "push", dex, remote + "/classes.dex"], capture_output=True)
    evidence = ROOT / "evidence/las-device-import.log"
    evidence.parent.mkdir(parents=True, exist_ok=True)
    # Keep source paths and any importer diagnostics in ignored local evidence.
    with evidence.open("w") as output:
        result = subprocess.run([*adb, "shell", shlex.join([
            "env", f"CLASSPATH={remote}/classes.dex", "app_process", "/system/bin", MAIN,
            package, key, DESTINATION])], text=True, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError("Device importer failed; inspect ignored evidence/las-device-import.log; partial extraction is retained")
    if "LAS import completed in " not in evidence.read_text():
        raise RuntimeError("Completion marker missing; inspect ignored evidence/las-device-import.log")
    (ROOT / "evidence/las-device-import.json").write_text(json.dumps({
        "execution_context": "adb shell app_process (not application UID)",
        "package": PACKAGE, "input": package, "key_path": key, "destination": DESTINATION,
        "dex_sha256": digest, "import_complete": True, "app_access_verified": False,
    }, indent=2) + "\n")
    print("Device import completed; app-UID file access and runtime loading remain unverified.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title", choices=("las",), default="las")
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("ANDROID_HOME")
                        or os.environ.get("ANDROID_SDK_ROOT") or Path.home() / "android-sdk"))
    parser.add_argument("--classes", type=Path, help="Existing generated host-importer classes directory")
    parser.add_argument("--inventory", type=Path, default=ROOT / "evidence/device-inventory.json")
    parser.add_argument("--serial", help="Explicitly push the DEX and import using this adb device")
    args = parser.parse_args()
    dex = build(args.sdk.resolve(), args.classes)
    if args.serial:
        import_device(args.serial, dex, args.inventory)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Importer error: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.strip(), file=sys.stderr)
        sys.exit(1)
