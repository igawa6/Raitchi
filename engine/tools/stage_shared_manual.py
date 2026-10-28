#!/usr/bin/env python3
"""Stage verified LAS/Eevee copies in the separate shared app; no scripted input/timers.

Default remains Eevee. LAS preserves a valid installed custom driver, seeding
the pinned reference Turnip only when its custom-driver directory is empty.
Both titles retain the app-wide driver preference and startup failure marker.
Eevee uses its recorded packed RomFS. Reference data stays read-only.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.recomp.shared.test"
REFERENCES = {"eevee": "com.recomp.letsgo.test", "las": "com.lasrecomp.android.test"}
PINNED_TURNIP = "3a1d3425a87cfd0ebc9cc87a03a387f4814f9e6f6084457f16756400c44e385e"
DATA = "/data/user/0/" + PACKAGE + "/files"
PROFILE = """fullscreen = 1
resolution = 1
upscaler = bilinear
antialias = off
frame_rate = 2
frame_gen = 0
aspect = 1
show_fps = 1
power_mode = 0
blur_removal = 0
mirror_mode = 0
"""


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()



def las_asset_manifest():
    """Expected immutable copies, derived from the owner's local imported tree.

    The private cache is reusable only while the complete path/size/mtime/inode
    inventory and original import receipt match. Thor copies are always hashed.
    """
    imported_path = ROOT / "evidence/las-inputs.json"
    imported = json.loads(imported_path.read_text())
    original = (ROOT / "private/las-baseline/extracted").resolve()
    if Path(imported["destination"]).resolve() != original:
        raise RuntimeError("LAS original tree differs from its import receipt")
    files = [original / "exefs/main.npdm"]
    entries = sorted((original / "romfs").rglob("*"))
    if any(p.is_symlink() for p in entries) or (original / "romfs").is_symlink():
        raise RuntimeError("Refusing symlinks in the LAS original asset tree")
    files += [p for p in entries if p.is_file()]
    if len(files) - 1 != imported["romfs_file_count"]:
        raise RuntimeError("LAS RomFS inventory differs from import receipt")
    inventory = []
    for path in files:
        name = str(path.relative_to(original))
        if not path.is_file() or path.is_symlink() or any(c in name for c in "\\\r\n"):
            raise RuntimeError("Unsafe or missing original asset path")
        stat = path.stat()
        inventory.append([name, stat.st_size, stat.st_mtime_ns, stat.st_ino])
    identity = hashlib.sha256(json.dumps([sha(imported_path), inventory], separators=(",", ":")).encode()).hexdigest()
    cache = ROOT / "build/shared-staging/las-assets.json"
    if cache.is_file():
        record = json.loads(cache.read_text())
        if record.get("inventory_identity") == identity:
            hashes = record["sha256"]
            if set(hashes) == {row[0] for row in inventory}:
                return hashes, record
    print("Hashing the local imported LAS asset tree once for copy verification...", flush=True)
    hashes = {str(path.relative_to(original)): sha(path) for path in files}
    # Re-stat after hashing, so a changing original tree cannot seed the cache.
    if inventory != [[str(p.relative_to(original)), p.stat().st_size, p.stat().st_mtime_ns, p.stat().st_ino] for p in files]:
        raise RuntimeError("LAS originals changed during host asset hashing")
    record = {"schema": 1, "source": str(original), "import_receipt_sha256": sha(imported_path),
              "inventory_identity": identity, "romfs_files": len(files) - 1,
              "romfs_bytes": sum(p.stat().st_size for p in files[1:]), "sha256": hashes}
    cache.parent.mkdir(parents=True, exist_ok=True)
    temporary = cache.with_suffix(".incoming")
    temporary.write_text(json.dumps(record, indent=2) + "\n")
    temporary.replace(cache)
    return hashes, record


def baseline_environment(title, diagnostics=False):
    if title not in REFERENCES:
        raise ValueError("Unknown shared title")
    state = "sessions/" + title + "-1.0.0"
    lines = ["LARECOMP_TITLE=" + title, "LARECOMP_GAME_ROOT=" + DATA + "/games/" + title + "/extracted",
             "LARECOMP_GAME_LIB=" + DATA + "/code/" + title + "/liblasgame.so",
             "LARECOMP_SHARED_MANIFEST=" + DATA + "/code/" + title + "/liblasgame.so.saotm",
             "LARECOMP_SETTINGS=" + DATA + "/" + state + "/shared-baseline.ini",
             "LARECOMP_NPAD=" + ("handheld" if title == "eevee" else "fullkey"), "LARECOMP_FONT_SCALE=1",
             "LARECOMP_SCALE=0.6666666666666666", "LARECOMP_NO_LAUNCHER=1", "LARECOMP_VK_DYN_VERTEX=0"]
    if title == "eevee":
        # Proven Thor stock-driver path: GPU-semaphore acquire and static layouts.
        lines += ["LARECOMP_VK_ACQUIRE_FENCE=0"]
    if diagnostics:
        lines += ["LARECOMP_DEBUG_FRAME=120", "LARECOMP_DEBUG_DATA=1", "LARECOMP_SAVE_MSL=1", "LARECOMP_SAVE_BLOB=1"]
    return "\n".join(lines) + "\n"


def driver_library_name(metadata):
    name = metadata.get("libraryName") if isinstance(metadata, dict) else None
    if (not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", name)
            or name in ("meta.json", ".probe")):
        raise RuntimeError("Custom driver libraryName must be a plain safe basename")
    return name


def stage_las_driver(shell, app_sha, copy_reference, reference):
    """Preserve a verified installed driver; never repair/overwrite a nonempty one."""
    target = "files/gpu_driver/current"
    inspect = ('[ ! -L files/gpu_driver ] && [ ! -L "$1" ] || exit 2; '
               'if [ -e "$1" ]; then [ -d "$1" ] || exit 2; ls -A "$1"; fi')
    inventory = shell(["run-as", PACKAGE, "sh", "-c", inspect, "inspect-driver", target], check=False)
    if inventory.returncode:
        raise RuntimeError("Refusing unsafe or unreadable custom-driver directory")

    def observe():
        def regular(path):
            result = shell(["run-as", PACKAGE, "sh", "-c", '[ -f "$1" ] && [ ! -L "$1" ]',
                            "check-driver-file", path], check=False)
            if result.returncode:
                raise RuntimeError("Custom driver requires regular metadata and library files")
        regular(target + "/meta.json")
        raw = shell(["run-as", PACKAGE, "cat", target + "/meta.json"]).stdout
        try:
            metadata = json.loads(raw)
        except (ValueError, UnicodeDecodeError) as error:
            raise RuntimeError("Invalid installed custom-driver metadata; preserving directory") from error
        name = driver_library_name(metadata)
        regular(target + "/" + name)
        metadata_hash = hashlib.sha256(raw).hexdigest()
        library_hash = app_sha(target + "/" + name)
        if (app_sha(target + "/meta.json") != metadata_hash or not isinstance(library_hash, str)
                or not re.fullmatch(r"[0-9a-f]{64}", library_hash)):
            raise RuntimeError("Custom driver metadata/library hash readback failed")
        return {"library_sha256": library_hash, "metadata_sha256": metadata_hash, "metadata": metadata}

    if inventory.stdout.strip():
        return {"selection": "selected-existing", **observe()}

    bundle = ROOT / "work/las-baseline/build/android/deps/turnip"
    metadata = json.loads((bundle / "meta.json").read_text())
    name = driver_library_name(metadata)
    if sha(bundle / name) != PINNED_TURNIP:
        raise RuntimeError("Pinned LAS reference driver provenance mismatch")
    source = "/data/user/0/" + reference + "/files/gpu_driver/current"
    for filename, digest in [(name, PINNED_TURNIP), ("meta.json", sha(bundle / "meta.json"))]:
        actual = shell(["run-as", reference, "sha256sum", source + "/" + filename]).stdout.decode().split()[0]
        if actual != digest:
            raise RuntimeError("LAS device reference driver differs from pinned baseline")
    shell(["run-as", PACKAGE, "mkdir", "-p", target])
    copy_reference(["meta.json", name], source, target, "driver-copy.log")
    observed = observe()
    if observed["library_sha256"] != PINNED_TURNIP or observed["metadata_sha256"] != sha(bundle / "meta.json"):
        raise RuntimeError("Copied LAS driver hash mismatch")
    return {"selection": "pinned-reference-turnip", **observed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--title", choices=("las", "eevee"), default="eevee")
    parser.add_argument("--launch", action="store_true", help="Open once for owner play; never inject input or close it")
    parser.add_argument("--diagnostics", action="store_true", help="Record one renderer debug frame and translated shaders")
    args = parser.parse_args()
    marker = ROOT / "evidence/manual-session.json"
    if marker.is_file() and json.loads(marker.read_text()).get("active"):
        parser.error("Protected manual session is active; no staging or restart attempted")
    receipt = json.loads((ROOT / "build/shared-android/apk.json").read_text())
    verified = json.loads((ROOT / "build/shared-android/artifact-verification.json").read_text())
    selected = args.title
    reference = REFERENCES[selected]
    title = json.loads((ROOT / f"build/shared-gamelib/{selected}/build.json").read_text())
    library = ROOT / f"build/shared-gamelib/{selected}/liblasgame.so"
    sidecar = library.with_name("liblasgame.so.saotm")
    if title["semantic"]["title"] != selected or not verified["two_title_host_gate"] or sha(library) != title["library_sha256"] or sha(sidecar) != title["sidecar_sha256"]:
        raise RuntimeError("Private shared title artifacts differ from the verified host build")
    if title["runtime_sha256"] != verified["runtime_sha256"]:
        raise RuntimeError("Title library and common runtime receipts differ")
    if receipt["runtime_sha256"] != verified["runtime_sha256"] or sha(ROOT / receipt["apk"]) != receipt["apk_sha256"]:
        raise RuntimeError("APK and verified common runtime receipts differ")
    adb = [str(Path.home() / "android-sdk/platform-tools/adb"), "-s", args.serial]

    def shell(parts, check=True, **kwargs):
        return subprocess.run([*adb, "shell", "-T", shlex.join([str(p) for p in parts])],
                              check=check, capture_output=True, **kwargs)

    model = shell(["getprop", "ro.product.model"]).stdout.decode().strip()
    if "Thor" not in model:
        raise RuntimeError("The selected device is not the owner's Thor")
    for package in (PACKAGE, reference):
        if shell(["pidof", package], check=False).stdout.strip():
            raise RuntimeError("An affected app is running; leave owner play untouched")
    apk_path = shell(["pm", "path", PACKAGE]).stdout.decode().strip()
    if not apk_path.startswith("package:") or "\n" in apk_path:
        raise RuntimeError("Install the verified separate shared APK first")
    if shell(["sha256sum", apk_path[8:]]).stdout.decode().split()[0] != receipt["apk_sha256"]:
        raise RuntimeError("Installed shared APK differs from its verified receipt")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = ROOT / "evidence/shared-manual" / stamp
    output.mkdir(parents=True)
    root = "files/games/" + selected + "/extracted"
    state = "files/sessions/" + selected + "-1.0.0"
    shell(["run-as", PACKAGE, "mkdir", "-p", root + "/exefs", "files/code/" + selected, state + "/save"])

    def app_sha(path):
        result = shell(["run-as", PACKAGE, "sha256sum", path], check=False)
        return result.stdout.decode().split()[0] if result.returncode == 0 else None

    def write(path, contents=None, source=None, digest=None):
        if digest and app_sha(path) == digest:
            return
        temporary = path + ".incoming-" + stamp
        command = ["run-as", PACKAGE, "sh", "-c", 'umask 077; cat > "$1"', "shared-stage", temporary]
        if source:
            with source.open("rb") as stream:
                shell(command, stdin=stream)
        else:
            shell(command, input=contents)
        if digest and app_sha(temporary) != digest:
            raise RuntimeError("Transferred title file hash mismatch")
        shell(["run-as", PACKAGE, "chmod", "444" if digest else "600", temporary])
        shell(["run-as", PACKAGE, "mv", temporary, path])

    write("files/code/" + selected + "/liblasgame.so", source=library, digest=title["library_sha256"])
    write("files/code/" + selected + "/liblasgame.so.saotm", source=sidecar, digest=title["sidecar_sha256"])
    source_root = "/sdcard/Android/data/" + reference + "/files"
    modules = title["semantic"]["modules"]
    original_key = "las" if selected == "las" else "letsgo"
    npdm_digest = sha(ROOT / f"private/{original_key}-baseline/extracted/exefs/main.npdm")
    if list(modules) != (["main", "sdk"] if selected == "las" else ["main", "sdk", "subsdk0", "subsdk1"]):
        raise RuntimeError("Unexpected selected module inventory")
    for name, module in modules.items():
        source_path = source_root + "/extracted/exefs/" + name
        actual = shell(["run-as", reference, "sha256sum", source_path]).stdout.decode().split()[0]
        if actual != module["sha256"]:
            raise RuntimeError("Reference NSO differs from verified title producer: " + name)
    if shell(["run-as", reference, "sha256sum", source_root + "/extracted/exefs/main.npdm"]).stdout.decode().split()[0] != npdm_digest:
        raise RuntimeError("Reference NPDM differs from the imported original")

    def copy_reference(names, source_dir, destination, log_name):
        source = ["run-as", reference, "tar", "-cf", "-", "-C", source_dir, *names]
        target = ["run-as", PACKAGE, "tar", "-xf", "-", "-C", destination]
        # Both UIDs retain their scoped-storage access. No world-readable copy,
        # original modification, or host relay of multi-GB game assets.
        command = "set -o pipefail; " + shlex.join(source) + " | " + shlex.join(target)
        with (output / log_name).open("wb") as log:
            subprocess.run([*adb, "shell", "-T", command], check=True, stdout=log, stderr=subprocess.STDOUT)

    assets = {}
    if selected == "eevee":
        bin_record = json.loads((ROOT / "evidence/letsgo-romfs/20261024T215945.820858Z/result.json").read_text())
        bin_digest = bin_record["romfs_sha256"]
        complete = (all(app_sha(root + "/exefs/" + name) == data["sha256"] for name, data in modules.items())
                    and app_sha(root + "/exefs/main.npdm") == npdm_digest)
        existing_bin_hash = app_sha(root + "/romfs.bin") if complete else None
        complete = complete and existing_bin_hash == bin_digest
        if not complete:
            print("Copying original NSOs and packed RomFS on Thor into the separate app...", flush=True)
            copy_reference(["exefs/" + name for name in modules] + ["exefs/main.npdm", "romfs.bin"],
                           source_root + "/extracted", root, "data-copy.log")
        print("Verifying the copied RomFS SHA-256...", flush=True)
        if (existing_bin_hash if complete else app_sha(root + "/romfs.bin")) != bin_digest:
            raise RuntimeError("Copied RomFS differs from the reference builder record")
        assets = {"format": "packed", "romfs_sha256": bin_digest, "romfs_sha256_verified": True}
    else:
        expected, asset_record = las_asset_manifest()
        checksum_text = "".join(digest + "  " + name + "\n" for name, digest in sorted(expected.items())).encode()
        checksum_path = state + "/las-assets.sha256"
        write(checksum_path, contents=checksum_text, digest=hashlib.sha256(checksum_text).hexdigest())

        def check_assets(check=True):
            command = 'cd "$1" && sha256sum -c "$2"'
            result = shell(["run-as", PACKAGE, "sh", "-c", command, "verify-las-assets", root,
                            DATA + "/sessions/las-1.0.0/las-assets.sha256"], check=False)
            (output / "las-assets-check.log").write_bytes(result.stdout + result.stderr)
            count = shell(["run-as", PACKAGE, "sh", "-c", 'find "$1" -type f | wc -l',
                           "count-las-assets", root + "/romfs"], check=False)
            valid = result.returncode == 0 and count.returncode == 0 and int(count.stdout) == asset_record["romfs_files"]
            if check and not valid:
                raise RuntimeError("LAS copied asset SHA256/inventory verification failed")
            return valid

        complete = all(app_sha(root + "/exefs/" + name) == data["sha256"] for name, data in modules.items())
        if not complete or not check_assets(check=False):
            print("Copying original LAS NSOs and directory RomFS on Thor into the separate app...", flush=True)
            copy_reference(["exefs/" + name for name in modules] + ["exefs/main.npdm", "romfs"],
                           source_root + "/extracted", root, "data-copy.log")
            check_assets()
        assets = {"format": "directory", "romfs_files": asset_record["romfs_files"],
                  "romfs_bytes": asset_record["romfs_bytes"], "per_file_sha256_verified": True,
                  "host_asset_inventory_identity": asset_record["inventory_identity"],
                  "manifest_sha256": hashlib.sha256(checksum_text).hexdigest()}
    for name, data in modules.items():
        if app_sha(root + "/exefs/" + name) != data["sha256"]:
            raise RuntimeError("Copied original module hash differs: " + name)
    if app_sha(root + "/exefs/main.npdm") != npdm_digest:
        raise RuntimeError("Copied NPDM hash differs")
    # Copy the stopped reference's save only into an empty test namespace.
    cloned = False
    save_verified = False
    existing_save = shell(["run-as", PACKAGE, "ls", "-A", state + "/save"]).stdout.strip()
    if not existing_save and shell(["run-as", reference, "test", "-d", source_root + "/save"], check=False).returncode == 0:
        links = shell(["run-as", reference, "find", source_root + "/save", "-type", "l"]).stdout.strip()
        if links:
            raise RuntimeError("Refusing linked reference save entries")
        checksum = shell(["run-as", reference, "sh", "-c",
                          'cd "$1" && find save -type f -exec sha256sum {} +', "hash-save-copy", source_root]).stdout
        # Only hashes/names are recorded; save contents never leave Thor.
        if b"\\\n" in checksum or b"\r" in checksum:
            raise RuntimeError("Unsupported save checksum filename encoding")
        (output / "save-clone.sha256").write_bytes(checksum)
        incoming_save = state + "/.save-incoming-" + stamp
        shell(["run-as", PACKAGE, "mkdir", incoming_save])
        copy_reference(["save"], source_root, incoming_save, "save-clone.log")
        if checksum.strip():
            manifest = state + "/save-clone.sha256"
            write(manifest, contents=checksum, digest=hashlib.sha256(checksum).hexdigest())
            result = shell(["run-as", PACKAGE, "sh", "-c", 'cd "$1" && sha256sum -c "$2"',
                            "check-save-copy", incoming_save, DATA + "/sessions/" + selected + "-1.0.0/save-clone.sha256"])
            (output / "save-clone-check.log").write_bytes(result.stdout + result.stderr)
            source_count = len(checksum.splitlines())
            copied_count = int(shell(["run-as", PACKAGE, "sh", "-c", 'find "$1" -type f | wc -l',
                                      "count-save-copy", incoming_save + "/save"]).stdout)
            if copied_count != source_count:
                raise RuntimeError("Save clone file count differs")
            save_verified = True
        # rmdir refuses any destination that acquired data; only an empty test
        # namespace may be replaced. Failed partial copies are never published.
        shell(["run-as", PACKAGE, "rmdir", state + "/save"])
        shell(["run-as", PACKAGE, "mv", incoming_save + "/save", state + "/save"])
        shell(["run-as", PACKAGE, "rmdir", incoming_save])
        cloned = True
    # Keep the app-wide preference and .probe exactly as found. Preparing a title
    # must not enable a driver, select stock, or reset a failed-start attempt.
    driver = {"selection": "preserved-app-preference", "preference_modified": False,
              "failure_marker_modified": False}
    if selected == "las":
        driver["installation"] = stage_las_driver(shell, app_sha, copy_reference, reference)
    write(state + "/shared-baseline.ini", contents=PROFILE.encode())
    environment = baseline_environment(selected, args.diagnostics)
    write("files/env.txt", contents=environment.encode())
    status = {"package": PACKAGE, "title": selected, "apk_sha256": receipt["apk_sha256"],
        "runtime_sha256": verified["runtime_sha256"], "library_sha256": title["library_sha256"],
        "assets": assets, "driver": driver, "original_modules_verified": True, "npdm_sha256_verified": True,
        "save_cloned_to_separate_namespace": cloned, "save_clone_sha256_verified": save_verified,
        "environment": environment, "scripted_input": False, "automatic_close": False,
        "reference_data_modified": False, "renderer_diagnostics": args.diagnostics, "output": str(output)}
    (output / "staging.json").write_text(json.dumps(status, indent=2) + "\n")
    (ROOT / "evidence/shared-staging-current.json").write_text(json.dumps(status, indent=2) + "\n")
    if args.launch:
        launched = shell(["am", "start", "--display", "0", "-n", PACKAGE + "/com.lasrecomp.android.LasActivity"])
        (output / "activity-start.txt").write_bytes(launched.stdout + launched.stderr)
        # Startup may fail before owner handoff. The coordinator verifies the
        # readable game screen before marking a protected manual session active.
        (output / "handoff-pending.json").write_text(json.dumps({**status,
            "owner_handoff_completed": False, "startup_qualified": False}, indent=2) + "\n")
    print("Shared " + selected + " staged" + (" and opened for owner play" if args.launch else "") + "; no game input or automatic close.")


if __name__ == "__main__":
    main()
