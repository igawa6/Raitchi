#!/usr/bin/env python3
"""Build Eden's actual Android app with Raitchi NCE; no device or publishing actions."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import zipfile

import package_shared_android as engine

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/eden-shell"
PACKAGE = "dev.igawa6.raitchi"
LAUNCHER = "org.yuzu.yuzu_emu.ui.main.MainActivity"
EDEN_BASE = "989768aa46"
SHELL_LOCK = ROOT / "shell.lock.json"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, text=True, **kwargs)


def shell_inputs(shell):
    paths = [shell / "src/android/gradle/wrapper/gradle-wrapper.jar",
             shell / "src/android/gradle/wrapper/gradle-wrapper.properties"]
    for directory in ("raitchi-build", "raitchi-engine", "app/src/main", "app/src/test"):
        paths += [p for p in (shell / "src/android" / directory).rglob("*") if p.is_file()
                  and not set(p.relative_to(shell / "src/android" / directory).parts)
                  .intersection({".gradle", "build", "local.properties"})]
    paths += [shell / "src/android/app" / name for name in
              ("build.gradle.kts", "raitchi-sources.txt", "raitchi-proguard-rules.pro")]
    paths += list((shell / "LICENSES").glob("*.txt"))
    return {str(p.relative_to(shell)): sha(p) for p in sorted(paths)}


def test_signing(env):
    """Dedicated local test identity, independent of any other app key and Android's default key."""
    directory = Path(env.get("RAITCHI_TEST_SIGNING_DIR", ROOT / "private/raitchi-shell-signing"))
    directory.mkdir(parents=True, exist_ok=True)
    directory.chmod(0o700)
    password_file, keystore = directory / "password.txt", directory / "test.jks"
    if not password_file.exists():
        with password_file.open("x") as stream:
            stream.write(secrets.token_urlsafe(32))
        password_file.chmod(0o600)
    password = password_file.read_text().strip()
    if not password or keystore.is_symlink():
        raise RuntimeError("Invalid private Raitchi signing material")
    env.update(RAITCHI_TEST_KEYSTORE=str(keystore), RAITCHI_TEST_PASSWORD=password)
    if not keystore.exists():
        run([Path(env["JAVA_HOME"]) / "bin/keytool", "-genkeypair", "-keystore", keystore,
             "-alias", "raitchi-test", "-keyalg", "RSA", "-keysize", "3072", "-validity", "10000",
             "-dname", "CN=Raitchi Local Test", "-storepass:env", "RAITCHI_TEST_PASSWORD",
             "-keypass:env", "RAITCHI_TEST_PASSWORD", "-noprompt"], env=env, capture_output=True)
        keystore.chmod(0o600)
    return {"identity": "dedicated local Raitchi test key", "keystore_sha256": sha(keystore)}


def strip_libraries(sdk, libraries, reports):
    llvm = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin"
    destination = BUILD / "packaged-native"
    destination.mkdir(parents=True, exist_ok=True)
    packaged, packaged_reports = {}, {}
    for name, source in libraries.items():
        digest = reports[name]["sha256"]
        symbols = BUILD / "symbols" / digest / name
        symbols.parent.mkdir(parents=True, exist_ok=True)
        if not symbols.exists():
            shutil.copy2(source, symbols)
        if sha(symbols) != digest:
            raise RuntimeError("Unstripped symbol archive differs: " + name)
        notes = run([llvm / "llvm-readelf", "--notes", source], capture_output=True).stdout
        match = re.search(r"Build ID:\s*(\w+)", notes)
        build_id = match.group(1) if match else None
        symbols.with_suffix(".build-id.txt").write_text((build_id or "not present") + "\n")
        target = destination / name
        shutil.copyfile(source, target)
        run([llvm / "llvm-strip", "--strip-unneeded", target])
        packaged[name] = target
        packaged_reports[name] = dict(reports[name], source=str(target.relative_to(ROOT)),
                sha256=sha(target), bytes=target.stat().st_size, build_id=build_id,
                unstripped_sha256=digest, unstripped_bytes=source.stat().st_size,
                symbol_archive=str(symbols.relative_to(ROOT)))
    return packaged, packaged_reports


def inspect_apk(apk, sdk, reports, assets):
    build_tools = sdk / "build-tools/36.0.0"
    signature = run([build_tools / "apksigner", "verify", "--verbose", "--print-certs", apk],
                    capture_output=True).stdout
    badging = run([build_tools / "aapt", "dump", "badging", apk], capture_output=True).stdout
    if f"package: name='{PACKAGE}'" not in badging or LAUNCHER not in badging:
        raise RuntimeError("Unexpected shell package or launcher")
    xml = run([build_tools / "aapt", "dump", "xmltree", apk, "AndroidManifest.xml"], capture_output=True).stdout
    blocks = re.split(r"\n\s*E: ", xml)
    for component, process in (("LasActivity", ":game"), ("PlayService", ":game"), ("SharedBuildService", ":build")):
        block = next((b for b in blocks if f'com.lasrecomp.android.{component}"' in b), "")
        if not block or f'"{process}"' not in block or re.search(r"android:exported[^\n]*0xffffffff", block):
            raise RuntimeError("Engine component boundary changed: " + component)
    if "org.yuzu.yuzu_emu.activities.EmulationActivity" in xml:
        raise RuntimeError("The removed Eden emulation activity is still declared")
    if "-release" in apk.name:
        # Navigation resolves custom argument enums reflectively from binary XML.
        # A debug-only UI test cannot detect R8 renaming those classes.
        mapping_path = BUILD / "gradle/app/outputs/mapping/release/mapping.txt"
        mapping = dict(re.findall(r"^([^ #\n]+) -> ([^:\n]+):$", mapping_path.read_text(), re.M))
        for name in ("org.yuzu.yuzu_emu.model.Game",
                     "org.yuzu.yuzu_emu.features.settings.model.Settings$MenuTag",
                     "org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen"):
            if mapping.get(name) != name:
                raise RuntimeError("R8 renamed a reflective navigation argument: " + name)
    table = {}
    with zipfile.ZipFile(apk) as archive:
        packaged = {Path(n).name: n for n in archive.namelist() if n.startswith("lib/") and n.endswith(".so")}
        if set(packaged) != set(reports) or any(not n.startswith("lib/arm64-v8a/") for n in packaged.values()):
            raise RuntimeError("Unexpected native inventory; Eden core must not be packaged")
        for name, report in reports.items():
            if hashlib.sha256(archive.read(packaged[name])).hexdigest() != report["sha256"]:
                raise RuntimeError("Packaged native bytes differ: " + name)
        for name, digest in assets.items():
            asset_data = archive.read("assets/" + name)
            if hashlib.sha256(asset_data).hexdigest() != digest:
                raise RuntimeError("Packaged engine asset differs: " + name)
            if name == "shared_5.ttf":
                engine.check_button_font.check_font(asset_data, "APK assets/shared_5.ttf")
        if "assets/GPL-3.0-or-later.txt" not in archive.namelist():
            raise RuntimeError("Eden shell license is missing")
        for item in archive.infolist():
            if item.filename.startswith("lib/"):
                category = Path(item.filename).name
            elif re.fullmatch(r"classes\d*\.dex", item.filename):
                category = "DEX"
            elif item.filename == "assets/ondevice.zip":
                category = "on-device compiler assets"
            elif item.filename.startswith("assets/"):
                category = "assets/fonts/notices"
            else:
                category = "resources/manifest/metadata"
            entry = table.setdefault(category, {"raw_bytes": 0, "compressed_bytes": 0})
            entry["raw_bytes"] += item.file_size
            entry["compressed_bytes"] += item.compress_size
    return {"apk": str(apk.relative_to(ROOT)), "apk_sha256": sha(apk), "apk_bytes": apk.stat().st_size,
            "size_components": table, "signature": signature.strip(), "host_build_and_lint_passed": True,
            "device_installed": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    lock = json.loads(SHELL_LOCK.read_text())
    parser.add_argument("--shell", type=Path, default=(ROOT / lock["path"]))
    parser.add_argument("--allow-unpinned-shell", action="store_true",
                        help="Build a shell checkout whose HEAD differs from shell.lock.json (development only)")
    parser.add_argument("--sdk", type=Path, default=Path.home() / "android-sdk")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--allow-downloads", action="store_true")
    parser.add_argument("--with-ondevice", action="store_true", help="Optional legacy compiler payload; omitted by default")
    parser.add_argument("--variant", choices=("debug", "release", "both"), default="both")
    parser.add_argument("--jobs", type=int, choices=(1, 2, 3, 4), default=2)
    args = parser.parse_args()
    shell, sdk = args.shell.resolve(strict=True), args.sdk.resolve(strict=True)
    BUILD.mkdir(parents=True, exist_ok=True)
    base = run(["git", "-C", shell, "rev-parse", EDEN_BASE], capture_output=True).stdout.strip()
    run(["git", "-C", shell, "merge-base", "--is-ancestor", base, "HEAD"])
    head = run(["git", "-C", shell, "rev-parse", "HEAD"], capture_output=True).stdout.strip()
    pinned = lock["commit"]
    if re.fullmatch(r"[0-9a-f]{40}", pinned) and head != pinned and not args.allow_unpinned_shell:
        raise RuntimeError(f"Shell HEAD {head[:12]} differs from shell.lock.json {pinned[:12]}")
    native, digest, libraries, unstripped_reports = engine.verify_native(sdk)
    toolkit, kit_libraries, kit_reports = engine.verify_toolkit(sdk, args.with_ondevice)
    libraries.update(kit_libraries); unstripped_reports.update(kit_reports)
    packaged_libraries, reports = strip_libraries(sdk, libraries, unstripped_reports)
    stage = engine.stage(sdk, packaged_libraries, reports, digest, toolkit)
    service = engine.WORK / "app/src/main/java/com/lasrecomp/android/SharedBuildService.java"
    old = "new Intent(this,SharedLauncherActivity.class)"
    text = service.read_text()
    if text.count(old) != 1:
        raise RuntimeError("Build notification launch target changed")
    service.write_text(text.replace(old, "getPackageManager().getLaunchIntentForPackage(getPackageName())"))
    inputs = shell_inputs(shell)
    java_inputs = {str(p.relative_to(engine.WORK)): sha(p)
                   for p in (engine.WORK / "app/src/main").rglob("*") if p.is_file()}
    receipt = {"schema": 2, "application_id": PACKAGE, "eden_base": base, "eden_head": head,
               "app_module": "src/android/app", "shell_sources": inputs, "engine_stage_sources": java_inputs,
               "builder_sha256": sha(Path(__file__)), "engine_native_build_sha256": digest,
               "engine_runtime_sha256": reports["libmain.so"]["sha256"],
               "engine_runtime_unstripped_sha256": native["library_sha256"], "engine_libraries": reports,
               "engine_stage": stage, "ondevice_compiler_included": args.with_ondevice,
               "game_data_included": False, "device_installed": False,
               "cpu_default": "continuous-nce", "gpu_backend": "Raitchi NVN/Vulkan"}
    engine.atomic_json(BUILD / "stage.json", receipt)
    if not args.build:
        print("Staged actual Eden app with stripped Raitchi native libraries; no device actions.")
        return
    (BUILD / "apk.json").unlink(missing_ok=True)
    java = Path(os.environ.get("JAVA_HOME", "/usr/lib/jvm/java-17-openjdk"))
    env = dict(os.environ, JAVA_HOME=str(java), ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    env["PATH"] = str(java / "bin") + ":" + env.get("PATH", "")
    receipt["signing"] = test_signing(env)
    variants = ("debug", "release") if args.variant == "both" else (args.variant,)
    command = [shell / "src/android/gradlew", "-p", shell / "src/android/raitchi-build"]
    for variant in variants:
        command += [":app:assemble" + variant.title(), ":app:lint" + variant.title()]
    command.append(":app:testDebugUnitTest")
    command += ["--no-daemon", "--max-workers=" + str(args.jobs), "-PraitchiWorkspace=" + str(ROOT)]
    if not args.allow_downloads:
        command.append("--offline")
    with (BUILD / "build.log").open("w") as log:
        run(command, cwd=shell / "src/android", env=env, stdout=log, stderr=subprocess.STDOUT)
    if shell_inputs(shell) != inputs or any(sha(engine.WORK / p) != h for p, h in java_inputs.items()):
        raise RuntimeError("Shell/engine Java sources changed during build; rerun after edits finish")
    receipt["variants"] = {}
    for variant in variants:
        source = BUILD / f"gradle/app/outputs/apk/{variant}/app-{variant}.apk"
        apk = BUILD / f"raitchi-eden-app-{variant}.apk"
        shutil.copy2(source, apk)
        receipt["variants"][variant] = inspect_apk(apk, sdk, reports, stage["asset_sha256"])
        if variant == "release":
            mapping = BUILD / "gradle/app/outputs/mapping/release/mapping.txt"
            archived = BUILD / "symbols/r8" / receipt["variants"][variant]["apk_sha256"] / "mapping.txt"
            archived.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(mapping, archived)
            receipt["variants"][variant]["r8_mapping"] = {
                "path": str(archived.relative_to(ROOT)), "sha256": sha(archived)}
    receipt["built_utc"] = datetime.now(timezone.utc).isoformat()
    engine.atomic_json(BUILD / "apk.json", receipt)
    print(json.dumps({"variants": {k: {key: v[key] for key in ("apk", "apk_bytes", "apk_sha256")}
                                  for k, v in receipt["variants"].items()},
                      "packaged_runtime_sha256": receipt["engine_runtime_sha256"],
                      "unstripped_runtime_sha256": receipt["engine_runtime_unstripped_sha256"],
                      "compiler_included": args.with_ondevice, "device_installed": False}, indent=2))


if __name__ == "__main__":
    main()
