#!/usr/bin/env python3
"""Prepare an isolated, stock-clock LAS Android reference; build only on request."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "work/las-baseline"


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def replace_once(path, old, new):
    content = path.read_text()
    if new in content:
        return
    if content.count(old) != 1:
        raise RuntimeError(f"Unexpected pinned source content: {path.name}")
    path.write_text(content.replace(old, new, 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps", action="store_true", help="Fetch and build pinned native dependencies")
    parser.add_argument("--apk", action="store_true", help="Build the separate .test launcher APK (no game code)")
    parser.add_argument("--game", action="store_true", help="Build the private game library from imported LAS inputs")
    parser.add_argument("--package-game", action="store_true", help="Package an existing local game library in a private .test APK")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--sdk", type=Path, default=Path.home() / "android-sdk")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    run(["python3", ROOT / "tools/verify_sources.py"])
    if not WORK.exists():
        WORK.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--shared", ROOT / "sources/las-android", WORK])
    original = run(["git", "-C", ROOT / "sources/las-android", "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    actual = run(["git", "-C", WORK, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if original != actual:
        raise RuntimeError("Baseline working tree no longer uses the pinned source")
    replace_once(
        WORK / "android/app/src/main/java/com/lasrecomp/android/RootBoost.java",
        'return p.getBoolean("root_boost", true) && hasSu();',
        'return false; // Shared-platform baseline: retain the device owner\'s clock and thermal policy.',
    )
    replace_once(
        WORK / "tools/android/build_deps.sh",
        'JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)',
        'JOBS=${RECOMP_BUILD_JOBS:-4} # Bounded baseline build; avoid oversubscribing the host.',
    )
    replace_once(WORK / "tools/android/build_game.sh", "JOBS=$(nproc)", "JOBS=${RECOMP_BUILD_JOBS:-4}")
    # 16 KiB page devices (Android 15+): the SDL3 shared library is linked by the pinned
    # upstream CMake project, so it does not inherit the runtime's own link options.
    # Without this, libSDL3.so is the only packaged library left with a 4 KiB
    # p_align and the loader refuses it on a 16 KiB page device.
    replace_once(
        WORK / "android/app/CMakeLists.txt",
        "add_subdirectory(${ROOT}/build/android/SDL3 ${CMAKE_BINARY_DIR}/SDL3)",
        "add_subdirectory(${ROOT}/build/android/SDL3 ${CMAKE_BINARY_DIR}/SDL3)\n"
        "target_link_options(SDL3-shared PRIVATE \"-Wl,-z,max-page-size=16384\")"
        "  # (16 KB page devices, as libmain.so)",
    )
    sdk = args.sdk.resolve()
    (WORK / "android/local.properties").write_text(f"sdk.dir={sdk}\ncmake.dir={sdk}/cmake/3.31.6\n")
    evidence = ROOT / "evidence"
    evidence.mkdir(exist_ok=True)
    patch = run(["git", "-C", WORK, "diff", "--", "android/app/src/main/java/com/lasrecomp/android/RootBoost.java", "android/app/CMakeLists.txt", "tools/android/build_deps.sh", "tools/android/build_game.sh"], capture_output=True, text=True).stdout
    (evidence / "las-baseline.patch").write_text(patch)
    env = dict(os.environ)
    env.update(ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk),
               ANDROID_NDK_HOME=str(sdk / "ndk/27.2.12479018"),
               JAVA_HOME=os.environ.get("JAVA_HOME", "/usr/lib/jvm/java-17-openjdk"),
               RECOMP_BUILD_JOBS=str(args.jobs), CMAKE_BUILD_PARALLEL_LEVEL=str(args.jobs))
    env["PATH"] = f"{sdk}/cmake/3.31.6/bin:{env['JAVA_HOME']}/bin:{env.get('PATH', '')}"
    if args.deps:
        run(["bash", "tools/android/build_deps.sh"], cwd=WORK, env=env)
    if args.apk:
        run(["./gradlew", ":app:assembleDebug", "-PlasTestId", "--no-daemon", f"--max-workers={args.jobs}"], cwd=WORK / "android", env=env)
    if args.game:
        inputs = ROOT / "private/las-baseline/extracted"
        if not (inputs / "exefs/main").is_file():
            raise RuntimeError("Import LAS inputs first with tools/import_baseline.py --title las")
        run(["bash", "tools/android/build_game.sh", inputs], cwd=WORK, env=env)
    if args.package_game:
        library = WORK / "build/android/gamelib/arm64-v8a/liblasgame.so"
        if not library.is_file():
            raise RuntimeError("Build the private game library first with --game")
        run(["./gradlew", ":app:assembleDebug", "-PlasTestId", f"-PlasGameLib={library.parent.parent}", "--no-daemon", f"--max-workers={args.jobs}"], cwd=WORK / "android", env=env)
    print(f"Prepared isolated baseline at {WORK}; RootBoost disabled, application ID uses .test when built.")


if __name__ == "__main__":
    main()
