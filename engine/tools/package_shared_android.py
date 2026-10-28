#!/usr/bin/env python3
"""Package the verified common runtime offline, without games or device actions."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid
import zipfile
import gpu_cache_fingerprint
import check_button_font
import check_elf16k

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ports/shared-android/apk"
WORK = ROOT / "work/shared-apk"
BUILD = ROOT / "build/shared-android"
INPUTS = BUILD / "apk-inputs"
PACKAGE = "com.recomp.shared.test"
MARKER = ".shared-apk-owned.json"
SHADER_PROFILE_CATALOG = ROOT / "ports/shared-android/assets/shader-profiles"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def checked_file(relative, digest):
    path = (ROOT / relative).resolve()
    if not path.is_relative_to(ROOT) or not path.is_file() or sha(path) != digest:
        raise RuntimeError(f"Missing or changed recorded input: {relative}")
    return path


def shader_profile_catalog_assets(directory=SHADER_PROFILE_CATALOG):
    """Explicit allowlist of codec-validated metadata, never arbitrary assets.

    A local profile exported with shader_profile_tool can be listed in this
    manifest. Nothing is inferred from game files, keys or shader journals.
    """
    manifest = directory / "manifest.json"
    if not manifest.exists():
        return {}
    if directory.is_symlink() or manifest.is_symlink() or manifest.stat().st_size > 1024 * 1024:
        raise RuntimeError("Unsafe shader profile catalog manifest")
    record = json.loads(manifest.read_text())
    if (set(record) != {"schema", "metadata_only", "profiles"} or record["schema"] != 1
            or record["metadata_only"] is not True or not isinstance(record["profiles"], list)
            or len(record["profiles"]) > 4096):
        raise RuntimeError("Invalid shader profile catalog manifest")
    resources = {"shader-profiles/manifest.json": manifest}
    for entry in record["profiles"]:
        if not isinstance(entry, dict) or set(entry) != {"game_version_identity", "file", "sha256", "records"}:
            raise RuntimeError("Invalid shader profile catalog entry")
        identity = entry["game_version_identity"]
        if (not isinstance(identity, str) or re.fullmatch(r"[0-9a-f]{64}", identity) is None
                or entry["file"] != identity + ".bin"
                or not isinstance(entry["sha256"], str)
                or re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) is None
                or type(entry["records"]) is not int or not 0 <= entry["records"] <= 4096):
            raise RuntimeError("Invalid shader profile catalog scope/path")
        path = directory / entry["file"]
        name = "shader-profiles/" + entry["file"]
        if (name in resources or path.is_symlink() or not path.is_file()
                or not 120 <= path.stat().st_size <= 120 + 4096 * 513
                or sha(path) != entry["sha256"]):
            raise RuntimeError("Missing, changed or unsafe shader metadata profile")
        inspected = subprocess.run([sys.executable, str(ROOT / "tools/shader_profile_tool.py"),
                                    "inspect", "--profile", str(path)],
                                   check=True, capture_output=True, text=True, timeout=120)
        metadata = json.loads(inspected.stdout)
        if (metadata.get("game_version_identity") != identity or metadata.get("records") != entry["records"]
                or metadata.get("shader_payload_included") is not False):
            raise RuntimeError("Shader profile differs from catalog scope/metadata declaration")
        resources[name] = path
    # Fail closed: adding a binary to this directory does not silently ship it.
    allowed = {p.name for p in resources.values()} | {"README.md"}
    if any(p.name not in allowed or p.is_symlink() for p in directory.iterdir()):
        raise RuntimeError("Unlisted shader profile catalog content")
    return resources


def verify_native(sdk):
    """Bind packaging to the completed native build, current sources and dependencies."""
    subprocess.run([sys.executable, str(ROOT / "tools/verify_sources.py")], check=True)
    manifest_path = BUILD / "native-build.json"
    record = json.loads(manifest_path.read_text())
    if (record.get("schema") != 1 or record.get("target") != "aarch64-linux-android30"
            or record.get("game_code_included") is not False or not record.get("runtime_overlay_ready")):
        raise RuntimeError("No complete game-free common Android runtime build")
    checked_file("sources.lock.json", record["source_lock_sha256"])
    checked_file("ports/shared-android/CMakeLists.txt", record["cmake_sha256"])
    checked_file("ports/shared-android/exports.map", record["exports_map_sha256"])
    checked_file("tools/build_shared_android.py", record["builder_sha256"])
    for key in ("overlay_and_native_sha256", "dependencies"):
        for relative, digest in record[key].items():
            checked_file(relative, digest)
    for name, digest in record["generated_include_sha256"].items():
        checked_file("build/shared-android/include/" + name, digest)
    checked_file("tools/gpu_cache_fingerprint.py", record["gpu_cache_fingerprint_tool_sha256"])
    identity_report = checked_file("build/shared-android/gpu-cache-fingerprint.json",
                                  record["gpu_cache_fingerprint_report_sha256"])
    if json.loads(identity_report.read_text()) != record["gpu_cache_fingerprint"]:
        raise RuntimeError("GPU cache fingerprint receipt/report mismatch")
    gpu_cache_fingerprint.verify(record["gpu_cache_fingerprint"])
    library = checked_file("build/shared-android/native/libmain.so", record["library_sha256"])
    if library.stat().st_size != record["library_bytes"]:
        raise RuntimeError("Common runtime size differs from its successful build")
    backends = ("libcapstone.so", "libunicorn.so") if record.get("native_blocks") else ("libcapstone.so",)
    expected = {"libSDL3.so", "libandroid.so", "liblog.so", "libm.so", "libdl.so", "libc.so", *backends}
    if set(record["needed"]) != expected:
        raise RuntimeError("Common runtime dependency inventory needs packaging review")
    sdl = [ROOT / name for name in record["dependencies"] if name.endswith("/libSDL3.so")]
    if len(sdl) != 1:
        raise RuntimeError("Native manifest must identify one cached SDL3 library")
    # These hooks are built by the same cached LAS Android dependency build as SDL.
    # Record their own bytes and inspect their ELF dependencies; do not rebuild them.
    libraries = {"libmain.so": library, "libSDL3.so": sdl[0]}
    preparation = checked_file("build/shared-android/native/libraitchiprepare.so", record["preparation_library_sha256"])
    if preparation.stat().st_size != record["preparation_library_bytes"]:
        raise RuntimeError("Standalone package preparation library size changed")
    libraries["libraitchiprepare.so"] = preparation
    for name in backends:
        candidates=[ROOT/path for path in record["dependencies"] if Path(path).name==name]
        if len(candidates)!=1:raise RuntimeError("Native backend dependency is ambiguous: " + name)
        libraries[name]=candidates[0]
    for name in ("libmain_hook.so", "libhook_impl.so"):
        libraries[name] = sdl[0].parent / name
    readelf = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    reports = {}
    # SDL also links its standard Android audio/GLES backends, even when the
    # game runtime selects Vulkan. All are Android platform libraries.
    platform_libraries = expected | {"libOpenSLES.so", "libGLESv1_CM.so", "libGLESv2.so"}
    for name, path in libraries.items():
        if not path.is_file():
            raise RuntimeError(f"Missing cached JNI dependency: {name}")
        text = subprocess.run([str(readelf), "--file-header", "--dynamic", str(path)],
                              check=True, capture_output=True, text=True).stdout
        needed = re.findall(r"NEEDED.*\[([^\]]+)\]", text)
        if "AArch64" not in text or any(x not in platform_libraries | set(libraries) for x in needed):
            raise RuntimeError("Unexpected JNI architecture/dependencies: " + name)
        # 16 KiB page devices (Android 15+): every packaged library must be loadable
        # on a 16 KiB page device, not only the ones we link ourselves.
        alignment = check_elf16k.audit_library(str(path))
        if not alignment["aligned16k"]:
            raise RuntimeError("Library is not 16 KiB page compatible: " + name +
                               " (" + "; ".join(alignment["problems"]) + ")")
        reports[name] = {"source": str(path.relative_to(ROOT)), "sha256": sha(path),
                         "bytes": path.stat().st_size, "needed": needed,
                         "load_alignments": alignment["alignments"], "aligned16k": True}
    return record, sha(manifest_path), libraries, reports


def verify_toolkit(sdk, enabled):
    if not enabled:
        return None, {}, {}
    path = ROOT / "build/shared-ondevice/toolkit.json"
    record = json.loads(path.read_text())
    expected = {"liblaspython.so", "libpython3.14.so", "libcapstone.so", "libunicorn.so", "libzig.so"}
    if set(record["native_libraries"]) != expected or "ondevice.zip" not in record["assets"]:
        raise RuntimeError("Incomplete Android compiler toolkit inventory")
    readelf = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    libraries, reports = {}, {}
    for name, report in record["native_libraries"].items():
        source = checked_file(report["source"], report["sha256"])
        elf = subprocess.run([str(readelf), "--file-header", "--dynamic", str(source)], check=True, capture_output=True, text=True).stdout
        needed = re.findall(r"NEEDED.*\[([^\]]+)\]", elf)
        if "AArch64" not in elf or any(n not in expected | {"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so"} for n in needed):
            raise RuntimeError("Unexpected Android compiler ELF/dependencies: " + name)
        libraries[name] = source
        reports[name] = {"source": str(source.relative_to(ROOT)), "sha256": sha(source), "bytes": source.stat().st_size, "needed": needed}
    for name, report in record["assets"].items():
        if name.startswith("/") or ".." in Path(name).parts:
            raise RuntimeError("Invalid toolkit asset path")
        checked_file(report["source"], report["sha256"])
    record["packager_manifest_sha256"] = sha(path)
    return record, libraries, reports


def verify_validation_layer(sdk, provided):
    """Snapshot an explicitly requested diagnostic layer; accept only platform dependencies."""
    if provided is None:
        return {}, {}
    source = provided.expanduser().resolve(strict=True)
    if not source.is_file():
        raise RuntimeError("Validation layer input must be a regular ELF file")
    name = "libVkLayer_khronos_validation.so"
    digest = sha(source)
    destination = BUILD / "diagnostic-inputs" / digest / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.is_file() or sha(destination) != digest:
        temporary = destination.with_name(name + "." + uuid.uuid4().hex + ".tmp")
        try:
            shutil.copyfile(source, temporary)
            if sha(temporary) != digest:
                raise RuntimeError("Validation layer changed while snapshotting")
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    readelf = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    text = subprocess.run([str(readelf), "--file-header", "--dynamic", "--dyn-syms", "--wide", str(destination)],
                          check=True, capture_output=True, text=True).stdout
    needed = re.findall(r"NEEDED.*\[([^\]]+)\]", text)
    platform = {"libandroid.so", "liblog.so", "libm.so", "libdl.so", "libc.so", "libz.so", "libvulkan.so"}
    if (not re.search(r"Class:\s+ELF64\b", text) or "little endian" not in text
            or not re.search(r"Type:\s+DYN\b", text) or not re.search(r"Machine:\s+AArch64\b", text)
            or any(item not in platform for item in needed)):
        raise RuntimeError("Validation layer must be an ARM64 Android DSO with platform-only dependencies")
    exported = set(re.findall(r"\bGLOBAL\s+DEFAULT\s+(?!UND\b)\S+\s+(\S+)", text))
    required = {"vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "vkNegotiateLoaderLayerInterfaceVersion"}
    if not required <= exported:
        raise RuntimeError("Diagnostic ELF lacks the Khronos layer loader entry points")
    report = {"source": str(destination.relative_to(ROOT)), "sha256": digest,
              "bytes": destination.stat().st_size, "needed": needed,
              "diagnostic_validation_layer": True}
    return {name: destination}, {name: report}


def replace_method(source, signature, body):
    """Replace one pinned method body. Count braces while ignoring strings/comments."""
    if source.count(signature) != 1:
        raise RuntimeError(f"Pinned activity method changed: {signature}")
    start = source.index("{", source.index(signature))
    depth, index, state = 1, start + 1, "code"
    while index < len(source) and depth:
        a = source[index]
        b = source[index:index + 2]
        if state in ("string", "char"):
            if a == "\\": index += 2; continue
            if a == ('"' if state == "string" else "'"): state = "code"
        elif state == "line":
            if a == "\n": state = "code"
        elif state == "comment":
            if b == "*/": state = "code"; index += 2; continue
        elif b == "//": state = "line"; index += 2; continue
        elif b == "/*": state = "comment"; index += 2; continue
        elif a == '"': state = "string"
        elif a == "'": state = "char"
        elif a == "{": depth += 1
        elif a == "}": depth -= 1
        index += 1
    if depth:
        raise RuntimeError("Unterminated pinned activity method")
    return source[:start + 1] + "\n" + body + "\n    " + source[index - 1:]


def stage(sdk, libraries, native_reports, manifest_digest, toolkit=None):
    pinned = next(x for x in json.loads((ROOT / "sources.lock.json").read_text())["sources"] if x["id"] == "las-android")
    source = ROOT / pinned["path"]
    if WORK.exists() and (WORK.is_symlink() or not (WORK / MARKER).is_file()):
        raise RuntimeError("Refusing to replace an unowned APK staging directory")
    temporary = WORK.parent / (".shared-apk-stage-" + uuid.uuid4().hex)
    temporary.mkdir(parents=True)
    try:
        for rel in ("gradlew", "gradlew.bat"):
            shutil.copy2(source / "android" / rel, temporary / rel)
        shutil.copytree(source / "android/gradle", temporary / "gradle")
        shutil.copytree(source / "android/app/src/main/java", temporary / "app/src/main/java")
        shutil.copytree(source / "android/app/src/main/res", temporary / "app/src/main/res")
        # Keep stable Java/JNI names, but disable old setup compilation and automatic
        # LAS translation operations. Runtime/main.c enters configured shared sessions.
        activity = temporary / "app/src/main/java/com/lasrecomp/android/LasActivity.java"
        text = activity.read_text()
        text = replace_method(text, "public static synchronized String buildTools()", "        return null; // SharedLauncherActivity owns setup; the legacy LAS-only builder is disabled.")
        text = replace_method(text, "public static void buildService(boolean on, String title)", "        // SharedBuildService owns compilation outside the SDL game process.")
        text = replace_method(text, "public static void buildProgress(int percent, String text)", "        // SharedBuildService reads the portable producer progress file.")
        before = "        super.onCreate(state);"
        if text.count(before) != 1:
            raise RuntimeError("Pinned SDL activity startup changed")
        text = text.replace(before, """        try {
            String directory = getFilesDir().getAbsolutePath();
            android.system.Os.setenv("LARECOMP_DATA_ROOT", directory, true);
            android.system.Os.setenv("LARECOMP_DATA_HOME", directory, true);
        } catch (android.system.ErrnoException error) {
            throw new IllegalStateException("Cannot initialize isolated shared session", error);
        }
""" + before + "\n        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);", 1)
        for line in ("        installBundledTranslation();\n", "        new Thread(() -> applyTranslationPatch(this), \"translation patch\").start();\n"):
            if text.count(line) != 1:
                raise RuntimeError("Pinned automatic translation setup changed")
            text = text.replace(line, "", 1)
        activity.write_text(text)
        text = replace_method(text, "protected void onDestroy()", "        PlayService.stop(this);\n        super.onDestroy();\n        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());")
        activity.write_text(text)
        (activity.parent / "BuildService.java").unlink()
        # Repository overlays own packaging and the fully disabled RootBoost class.
        for path in PORT.rglob("*"):
            if path.is_file():
                target = temporary / path.relative_to(PORT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
        for path in (temporary / "app/src/main/res").glob("values*/strings.xml"):
            path.write_text(path.read_text().replace("LAS Recomp", "Raitchi"))
        (temporary / "local.properties").write_text("sdk.dir=" + str(sdk) + "\n")
        root_boost = (activity.parent / "RootBoost.java").read_text()
        if ("return false;" not in root_boost or any(word in root_boost for word in ("ProcessBuilder", "Runtime.getRuntime", "SCRIPT", '"su"'))):
            raise RuntimeError("RootBoost implementation must stay fully disabled")
        marker = {"schema": 1, "base_source": pinned, "native_manifest_sha256": manifest_digest,
                  "root_boost_disabled": True, "ondevice_compilation": toolkit is not None,
                  "keep_screen_on_while_visible": True,
                  "java_sha256": {str(p.relative_to(temporary)): sha(p) for p in sorted((temporary / "app/src/main/java").rglob("*.java"))},
                  "sdl_java_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sorted(
                      (ROOT / "work/las-baseline/build/android/SDL3/android-project/app/src/main/java").rglob("*.java"))},
                  "packaging_overlay_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sorted(PORT.rglob("*")) if p.is_file()}}
        atomic_json(temporary / MARKER, marker)
        previous = WORK.parent / (".shared-apk-previous-" + uuid.uuid4().hex)
        if WORK.exists(): WORK.rename(previous)
        temporary.rename(WORK)
        if previous.exists(): shutil.rmtree(previous)
    finally:
        if temporary.exists(): shutil.rmtree(temporary)
    # This owned input directory contains an exact allowlist, never a baseline tree.
    if INPUTS.exists():
        if INPUTS.is_symlink() or not (INPUTS / MARKER).is_file():
            raise RuntimeError("Refusing to replace unowned APK inputs")
        shutil.rmtree(INPUTS)
    native = INPUTS / "jniLibs/arm64-v8a"
    native.mkdir(parents=True)
    for name, path in libraries.items():
        shutil.copyfile(path, native / name)
        if sha(native / name) != native_reports[name]["sha256"]:
            raise RuntimeError("JNI dependency changed during staging")
    assets = INPUTS / "assets"
    assets.mkdir()
    resources = {"Baloo2-SemiBold.ttf": source / "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf",
        "DejaVuSans.ttf": source / "third_party/fonts/DejaVu/DejaVuSans.ttf",
        "shared_5.ttf": ROOT / "ports/shared-android/assets/fonts/shared_5.ttf",
        "OFL-Fredoka.txt": ROOT / "ports/shared-android/assets/fonts/OFL-Fredoka.txt",
        "LICENSE-ButtonGlyphs.txt": ROOT / "ports/shared-android/assets/fonts/LICENSE-ButtonGlyphs.txt",
        "README-ButtonGlyphs.txt": ROOT / "ports/shared-android/assets/fonts/README.txt",
        "icon.png": ROOT / "ports/shared-android/branding/raitchi-logo.png",
        "LICENSE-LASRecomp.txt": source / "LICENSE",
        "LICENSE-LetsGo.txt": ROOT / "native/runtime/LICENSE-LetsGo",
        "LICENSE-Echoes.txt": ROOT / "native/runtime/LICENSE-Echoes",
        "LICENSE-EoWRecomp.txt": ROOT / "native/runtime/LICENSE-EoWRecomp",
        "LICENSE-DejaVu.txt": source / "third_party/fonts/DejaVu/LICENSE",
        "LICENSE-Baloo2.txt": source / "third_party/fonts/Baloo2/OFL.txt"}
    resources.update(shader_profile_catalog_assets())
    if toolkit:
        resources.update({name: ROOT / report["source"] for name, report in toolkit["assets"].items()})
        # Keep scheduling outside the frozen producer/toolkit identity so that
        # changing worker policy does not discard verified generated objects.
        resources["shared_parallel_build.py"] = ROOT / "tools/shared_parallel_build.py"
        marker["toolkit_manifest_sha256"] = toolkit["packager_manifest_sha256"]
    for name, path in resources.items():
        if name == "shared_5.ttf":
            check_button_font.check_font(path.read_bytes(), str(path))
        (assets / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, assets / name)
    if toolkit:
        tool_meta = assets / "ondevice-manifest.json"
        atomic_json(tool_meta, {"schema": 1, "archive_sha256": toolkit["assets"]["ondevice.zip"]["sha256"]})
        resources["ondevice-manifest.json"] = tool_meta
        parallel_meta = assets / "shared-parallel-build.json"
        atomic_json(parallel_meta, {"schema": 1, "sha256": sha(resources["shared_parallel_build.py"])})
        resources["shared-parallel-build.json"] = parallel_meta
    marker["asset_sha256"] = {name: sha(path) for name, path in resources.items()}
    marker["native_libraries"] = native_reports
    atomic_json(INPUTS / MARKER, marker)
    return marker


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Build/sign/inspect the game-free APK offline")
    parser.add_argument("--with-ondevice", action="store_true", help="Include the verified Android generator/compiler toolkit")
    parser.add_argument("--validation-layer", type=Path,
                        help="Include this verified ARM64 Khronos validation layer for diagnostics only")
    parser.add_argument("--jobs", type=int, choices=(1, 2, 3, 4), default=2)
    parser.add_argument("--sdk", type=Path, default=Path.home() / "android-sdk")
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    record, digest, libraries, native_reports = verify_native(sdk)
    toolkit, tools_libraries, tools_reports = verify_toolkit(sdk, args.with_ondevice)
    libraries.update(tools_libraries); native_reports.update(tools_reports)
    layer_libraries, layer_reports = verify_validation_layer(sdk, args.validation_layer)
    libraries.update(layer_libraries); native_reports.update(layer_reports)
    prepared = stage(sdk, libraries, native_reports, digest, toolkit)
    atomic_json(BUILD / "apk-stage.json", prepared)
    if not args.build:
        print("Prepared isolated game-free APK source/JNI inputs; no build or device actions.")
        return
    manifest = BUILD / "apk.json"
    manifest.unlink(missing_ok=True)
    env = dict(os.environ)
    java = Path(os.environ.get("JAVA_HOME", "/usr/lib/jvm/java-17-openjdk"))
    env.update(JAVA_HOME=str(java), ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    env["PATH"] = str(java / "bin") + ":" + env.get("PATH", "")
    command = [str(WORK / "gradlew"), ":app:assembleDebug", "--offline", "--no-daemon",
               "--max-workers=" + str(args.jobs), "-PsharedWorkspace=" + str(ROOT)]
    with (BUILD / "apk-build.log").open("a") as log:
        log.write("\n" + json.dumps(command) + "\n")
        log.flush()
        subprocess.run(command, cwd=WORK, env=env, check=True, stdout=log, stderr=subprocess.STDOUT)
    apk = BUILD / "apk/app/outputs/apk/debug/app-debug.apk"
    tools = sdk / "build-tools/36.0.0"
    signature = subprocess.run([str(tools / "apksigner"), "verify", "--verbose", str(apk)],
                               check=True, capture_output=True, text=True).stdout
    badging = subprocess.run([str(tools / "aapt"), "dump", "badging", str(apk)],
                             check=True, capture_output=True, text=True).stdout
    if not re.search(r"package: name='" + re.escape(PACKAGE) + r"'", badging):
        raise RuntimeError("APK application ID is not the separate common-runtime test package")
    manifest_xml = subprocess.run([str(tools / "aapt"), "dump", "xmltree", str(apk), "AndroidManifest.xml"],
                                  check=True, capture_output=True, text=True).stdout
    if (not re.search(r"android:extractNativeLibs[^\n]*0xffffffff", manifest_xml)
            or "com.lasrecomp.android.LasActivity" not in manifest_xml
            or "com.lasrecomp.android.BuildService" in manifest_xml):
        raise RuntimeError("Packaged manifest lost native extraction/JNI activity/build-service policy")
    # Inspect what Java compilation actually produced, not a saved preference.
    compiled_boost = BUILD / "apk/app/intermediates/javac/debug/compileDebugJavaWithJavac/classes/com/lasrecomp/android/RootBoost.class"
    boost_code = subprocess.run([str(java / "bin/javap"), "-c", "-p", str(compiled_boost)],
                                check=True, capture_output=True, text=True).stdout
    if not re.search(r"static boolean enabled[^;]*;\s+Code:\s+0: iconst_0\s+1: ireturn", boost_code):
        raise RuntimeError("Compiled RootBoost.enabled must return false immediately")
    for method in ("start", "stop"):
        if not re.search(r"static synchronized void " + method + r"[^;]*;\s+Code:\s+0: return", boost_code):
            raise RuntimeError("Compiled RootBoost start/stop must be empty")
    zipped = {}
    with zipfile.ZipFile(apk) as archive:
        natives = [n for n in archive.namelist() if n.startswith("lib/")]
        expected = {"lib/arm64-v8a/" + name for name in libraries}
        if set(natives) != expected:
            raise RuntimeError("APK native inventory differs from the exact game-free allowlist")
        for name, report in native_reports.items():
            entry = "lib/arm64-v8a/" + name
            data = archive.read(entry)
            zipped[name] = hashlib.sha256(data).hexdigest()
            if zipped[name] != report["sha256"]:
                raise RuntimeError("Gradle changed verified native bytes: " + name)
        asset_names = {n.removeprefix("assets/") for n in archive.namelist() if n.startswith("assets/")}
        if asset_names != set(prepared["asset_sha256"]):
            raise RuntimeError("Unexpected APK asset outside the verified game-free inventory")
        for name, expected_sha in prepared["asset_sha256"].items():
            asset_data = archive.read("assets/" + name)
            if hashlib.sha256(asset_data).hexdigest() != expected_sha:
                raise RuntimeError("APK resource differs from staged source")
            if name == "shared_5.ttf":
                check_button_font.check_font(asset_data, "APK assets/shared_5.ttf")
    # Concurrent native edits/rebuilds cannot receive an apparently current APK manifest.
    latest, latest_digest, _, latest_reports = verify_native(sdk)
    latest_toolkit, _, latest_tools_reports = verify_toolkit(sdk, args.with_ondevice)
    latest_reports.update(latest_tools_reports)
    _, latest_layer_reports = verify_validation_layer(sdk, args.validation_layer)
    latest_reports.update(latest_layer_reports)
    if toolkit != latest_toolkit:
        raise RuntimeError("Compiler toolkit changed during packaging")
    if latest_digest != digest or latest_reports != native_reports:
        raise RuntimeError("Native build/dependencies changed during packaging; rerun")
    for relative, expected_sha in prepared["java_sha256"].items():
        if sha(WORK / relative) != expected_sha:
            raise RuntimeError("Staged Java changed during packaging")
    for key in ("sdl_java_sha256", "packaging_overlay_sha256"):
        for relative, expected_sha in prepared[key].items(): checked_file(relative, expected_sha)
    result = {**prepared, "application_id": PACKAGE, "activity": "com.lasrecomp.android.SharedLauncherActivity", "game_activity": "com.lasrecomp.android.LasActivity",
              "built_utc": datetime.now(timezone.utc).isoformat(), "apk": str(apk.relative_to(ROOT)),
              "apk_sha256": sha(apk), "apk_bytes": apk.stat().st_size,
              "native_manifest_sha256": digest, "runtime_sha256": latest["library_sha256"],
              "packaged_native_sha256": zipped, "signature_verification": signature,
              "game_code_included": False, "installed": False, "device_executed": False,
              "compiled_root_boost_verification": boost_code,
              "gradle_command": command, "packager_sha256": sha(Path(__file__).resolve()),
              "manifest_native_extraction": True}
    atomic_json(manifest, result)
    print("Built/signature-verified separate com.recomp.shared.test APK; all runtime and tool JNI libraries retain exact bytes.")
    print("Runtime hash matches the common native build. No games, install or device execution.")


if __name__ == "__main__":
    main()
