#!/usr/bin/env python3
"""Prepare/build an offline Let's Go Android native reference; never use ADB."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "work/letsgo-android"
PORT = ROOT / "ports/letsgo-android"
LAS = ROOT / "work/las-baseline"


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def patch(name):
    path = PORT / name
    if not path.is_file():
        raise RuntimeError(f"Port component not ready: {name}")
    reverse = subprocess.run(["git", "apply", "--reverse", "--check", str(path)],
                             cwd=WORK, capture_output=True)
    if reverse.returncode == 0:
        return
    run(["git", "apply", "--check", path], cwd=WORK)
    run(["git", "apply", path], cwd=WORK)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Configure/link libmain.so using cached dependencies")
    parser.add_argument("--apk", action="store_true", help="Build native code and package an asset-free isolated APK, offline")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--sdk", type=Path, default=Path.home() / "android-sdk")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error("--jobs must be between 1 and 8")
    run([sys.executable, ROOT / "tools/verify_sources.py"])
    if not WORK.exists():
        WORK.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--shared", ROOT / "sources/letsgo", WORK])
    revision = run(["git", "-C", ROOT / "sources/letsgo", "rev-parse", "HEAD"],
                   capture_output=True, text=True).stdout.strip()
    actual = run(["git", "-C", WORK, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if actual != revision:
        raise RuntimeError("Owned Android checkout differs from the pinned Let's Go revision")
    for name in ("host.patch", "renderer.patch"):
        patch(name)
    if (PORT / "integration.patch").exists():
        patch("integration.patch")
    build = ROOT / "build/letsgo-android"
    include = build / "include"
    include.mkdir(parents=True, exist_ok=True)
    source = (WORK / "third_party/smaa/SMAA.hlsl").read_bytes().replace(b"\r", b"")
    ascii_text = "".join(chr(x) if x < 128 else "?" for x in source)
    embedded = "".join('"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"\n'
                       for line in ascii_text.splitlines())
    target = include / "smaa_hlsl.inc"
    if not target.exists() or target.read_text() != embedded:
        target.write_text(embedded)

    dependencies = LAS / "build/android/deps"
    sdl_candidates = list((LAS / "android/app/build/intermediates/cxx/RelWithDebInfo").glob(
        "*/obj/arm64-v8a/libSDL3.so"))
    if len(sdl_candidates) != 1:
        raise RuntimeError("Expected one verified cached ARM64 SDL3 library")
    sdl = sdl_candidates[0]
    cache_tag = sdl.parent.parent.parent.name
    cached_native = LAS / "build/android/cxx/RelWithDebInfo" / cache_tag / "arm64-v8a"
    adreno_include = cached_native / "_deps/adrenotools-src/include"
    adreno_library = cached_native / "_deps/adrenotools-build/libadrenotools.a"
    adreno_ns = cached_native / "_deps/adrenotools-build/lib/linkernsbypass/liblinkernsbypass.a"
    hooks = [sdl.parent / name for name in ("libmain_hook.so", "libhook_impl.so")]
    if not adreno_library.is_file() or not adreno_ns.is_file() or not (adreno_include / "adrenotools/driver.h").is_file() or not all(p.is_file() for p in hooks):
        raise RuntimeError("Pinned cached AdrenoTools library/headers/hooks are missing")
    # This reference ID is independent of game files. A full title library must
    # later use these same patched headers/options and a verified input manifest.
    identity_files = [*sorted((WORK / "tools").glob("*.py")),
                      *sorted((WORK / "runtime").glob("*.h")),
                      WORK / "runtime/hle/hle_symbols.txt", WORK / "tools/gamelib_entry.c"]
    identity = {"source": revision, "simd": "asm", "shift": 0, "abi": 3,
                "target": "aarch64-linux-android30", "cpu_size": 1024,
                "files": {str(p.relative_to(WORK)): sha(p) for p in identity_files}}
    game_id = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:16]
    if not (args.build or args.apk):
        print(f"Prepared Android host-port source; code identity {game_id}. No device actions.")
        return
    sdk = args.sdk.resolve()
    ndk = sdk / "ndk/27.2.12479018"
    cmake = sdk / "cmake/3.31.6/bin/cmake"
    # Eevee's hwopus service needs the native Opus decoder, which depends on
    # swresample. Keep these archives separate from the frozen LAS reference.
    run([sys.executable, ROOT / "tools/build_letsgo_ffmpeg.py", "--build",
         "--jobs", min(args.jobs, 4), "--ndk", ndk])
    ffmpeg = ROOT / "build/letsgo-ffmpeg/prefix"
    ffmpeg_manifest = ROOT / "build/letsgo-ffmpeg/build.json"
    native = build / "native"
    manifest = build / "native-build.json"
    manifest.unlink(missing_ok=True)
    run([cmake, "-S", PORT, "-B", native, "-G", "Ninja",
         f"-DCMAKE_MAKE_PROGRAM={sdk}/cmake/3.31.6/bin/ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake",
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-30",
         "-DANDROID_STL=c++_static", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DRECOMP_SOURCE={WORK}",
         f"-DRECOMP_ANDROID_DEPS={dependencies}", f"-DRECOMP_SDL_SOURCE={LAS}/build/android/SDL3",
         f"-DRECOMP_FFMPEG_PREFIX={ffmpeg}",
         f"-DRECOMP_SDL_LIBRARY={sdl}", f"-DRECOMP_GENERATED_INCLUDE={include}",
         f"-DRECOMP_ADRENOTOOLS_INCLUDE={adreno_include}",
         f"-DRECOMP_ADRENOTOOLS_LIBRARY={adreno_library}",
         f"-DRECOMP_ADRENOTOOLS_NS_LIBRARY={adreno_ns}",
         f"-DRECOMP_GAME_ID={game_id}"])
    run([cmake, "--build", native, "--target", "main", "-j", args.jobs])
    library = native / "libmain.so"
    archives = [ffmpeg / "lib" / f"lib{x}.a"
                for x in ("avformat", "avcodec", "swscale", "swresample", "avutil")]
    archives.append(dependencies / "lib/libshaderc_combined.a")
    result = {"source_commit": revision, "game_code_identity": game_id,
              "library_sha256": sha(library), "library_bytes": library.stat().st_size,
              "patches": {p.name: sha(p) for p in sorted(PORT.glob("*.patch"))},
              "dependencies": {p.name: sha(p) for p in [sdl, *archives, adreno_library, adreno_ns, *hooks]},
              "ffmpeg_build_manifest_sha256": sha(ffmpeg_manifest),
              "target": "aarch64-linux-android30", "game_code_included": False,
              "device_executed": False, "guest_shift": 0}
    manifest.write_text(json.dumps(result, indent=2) + "\n")
    print("Linked asset-free Let's Go Android runtime. No APK install or guest execution.")
    if args.apk:
        apk_inputs = build / "apk-inputs"
        jni = apk_inputs / "jniLibs/arm64-v8a"
        assets = apk_inputs / "assets"
        jni.mkdir(parents=True, exist_ok=True)
        assets.mkdir(parents=True, exist_ok=True)
        for dependency in (library, sdl, *hooks):
            shutil.copy2(dependency, jni / dependency.name)
        asset_files = {
            "Baloo2-SemiBold.ttf": WORK / "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf",
            "Baloo2-OFL.txt": WORK / "third_party/fonts/Baloo2/OFL.txt",
            "DejaVuSans.ttf": ROOT / "sources/las-android/third_party/fonts/DejaVu/DejaVuSans.ttf",
            "DejaVu-LICENSE.txt": ROOT / "sources/las-android/third_party/fonts/DejaVu/LICENSE",
            "icon.png": WORK / "docs/icon/icon_1024.png",
            "LetsGo-LICENSE.txt": WORK / "LICENSE",
            "LASRecomp-LICENSE.txt": WORK / "LICENSE-LASRecomp",
        }
        for name, path in asset_files.items():
            shutil.copy2(path, assets / name)
        environment = dict(os.environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk),
                           JAVA_HOME=os.environ.get("JAVA_HOME", "/usr/lib/jvm/java-17-openjdk"))
        environment["PATH"] = f"{environment['JAVA_HOME']}/bin:{environment.get('PATH', '')}"
        run([LAS / "android/gradlew", "-p", PORT / "apk", ":app:assembleDebug", "--offline",
             "--no-daemon", f"--max-workers={args.jobs}"], env=environment)
        apk = build / "apk/outputs/apk/debug/app-debug.apk"
        apksigner = sdk / "build-tools/36.0.0/apksigner"
        run([apksigner, "verify", apk], env=environment)
        with zipfile.ZipFile(apk) as archive:
            names = archive.namelist()
            packaged_libraries = sorted(x for x in names if x.startswith("lib/") and x.endswith(".so"))
            expected_libraries = sorted("lib/arm64-v8a/" + p.name for p in (library, sdl, *hooks))
            if packaged_libraries != expected_libraries:
                raise RuntimeError(f"Unexpected APK native library inventory: {packaged_libraries}")
            library_hashes = {p: hashlib.sha256(archive.read(p)).hexdigest() for p in packaged_libraries}
        result["apk"] = {"sha256": sha(apk), "bytes": apk.stat().st_size,
                         "package": "com.recomp.letsgo.test", "signature_verified": True,
                         "native_libraries": library_hashes, "game_code_included": False,
                         "installed": False}
        manifest.write_text(json.dumps(result, indent=2) + "\n")
        print("Built and verified separate com.recomp.letsgo.test APK locally; not installed.")


if __name__ == "__main__":
    main()
