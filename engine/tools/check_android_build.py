#!/usr/bin/env python3
"""Read-only, offline LAS Android build prerequisite inventory; never builds/downloads."""
import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root / "sources/las-android")
    parser.add_argument("--sdk", type=Path, default=Path(
        os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
        or Path.home() / "android-sdk"))
    args = parser.parse_args()
    source, sdk = args.source.resolve(), args.sdk.resolve()
    gradle = Path(os.environ.get("GRADLE_USER_HOME", Path.home() / ".gradle"))
    required = {
        "Android 36 platform": sdk / "platforms/android-36/android.jar",
        "NDK r27c Android30 clang": sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang",
        "SDK CMake 3.31.6 (explicit selection recommended)": sdk / "cmake/3.31.6/bin/cmake",
        "Gradle 8.14.3 cached distribution": gradle / "wrapper/dists/gradle-8.14.3-bin",
        "AGP 8.11.1 cached artifacts": gradle / "caches/modules-2/files-2.1/com.android.tools.build/gradle/8.11.1",
        "SDL3 unpacked sources": source / "build/android/SDL3/CMakeLists.txt",
        "shaderc static library": source / "build/android/deps/lib/libshaderc_combined.a",
        "FFmpeg static library": source / "build/android/deps/lib/libavcodec.a",
        "Vulkan headers": source / "build/android/deps/include/vulkan/vulkan.h",
        "VMA header": source / "build/android/deps/include/vma/vk_mem_alloc.h",
        "volk source": source / "build/android/deps/include/volk/volk.c",
        "Baloo2 font": source / "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf",
        "DejaVu font": source / "third_party/fonts/DejaVu/DejaVuSans.ttf",
        "launcher icon": source / "docs/icon/icon_1024.png",
    }
    checks = {name: {"present": path.exists(), "path": str(path)}
              for name, path in required.items()}
    tools = {name: shutil.which(name) for name in (
        "java", "javac", "cmake", "ninja", "python3", "make", "git", "curl",
        "shasum", "tar", "unzip", "zip", "xxd")}
    python = {}
    for name in ("capstone", "unicorn", "lz4"):
        try:
            python[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            python[name] = None
    commit = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=False)
    print(json.dumps({
        "source_commit": commit.stdout.strip() or None,
        "checks": checks, "host_tools": tools, "system_python_packages": python,
        "ondevice_optional": {
            "compiler": (source / "build/ondevice/stage/jniLibs/arm64-v8a/libzig.so").exists(),
            "assets": (source / "build/ondevice/assets/ondevice.zip").exists(),
        },
        "limits": "Presence inventory only: no integrity, execution, compilation, Gradle resolution, APK or device validation. Cache directories can be incomplete. No commands install or build anything.",
    }, indent=2))


if __name__ == "__main__":
    main()
