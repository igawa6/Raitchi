#!/usr/bin/env python3
"""Stage/build one game-free common Android runtime using pinned sources and offline caches."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import uuid
import gpu_cache_fingerprint
import branding_substitutions

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "work/shared-android"
BUILD = ROOT / "build/shared-android"
PORT = ROOT / "ports/shared-android"
LAS = ROOT / "work/las-baseline"
MARKER = ".shared-android-stage.json"
REQUIRED_OVERLAYS = ("cpu.h", "rt.h", "main.c", "gamelib.c", "dispatch.c", "loader.c", "fnprof.c", "hle/hooks.c")
REQUIRED_NATIVE = ("runtime/aot_cpu.h", "runtime/shared_aot.c", "runtime/nso_image.c", "runtime/title_manifest.c",
                   "adapters/title_adapter.c", "adapters/las/profile.c", "adapters/letsgo/profile.c")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def write_changed(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_bytes() != content:
        path.write_bytes(content)


def stage():
    subprocess.run([sys.executable, str(ROOT / "tools/verify_sources.py")], check=True)
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    pinned = next(x for x in lock["sources"] if x["id"] == "las-android")
    source = ROOT / pinned["path"]
    overlays = sorted(p for p in (PORT / "runtime").rglob("*") if p.is_file())
    native = [p for p in (ROOT / "native").glob("module_*.*") if p.suffix in (".c", ".h")]
    for directory in (ROOT / "native/runtime", ROOT / "native/adapters", ROOT / "native/native-game-backend", ROOT / "native/continuous-nce"):
        native += [p for p in directory.rglob("*") if p.is_file() and
                   (p.suffix in (".h", ".c", ".cpp") or p.name.startswith("LICENSE"))]
    native = sorted(set(native))
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in [*overlays, *native]}
    branding_rules = sha(branding_substitutions.RULES)
    WORK.parent.mkdir(parents=True, exist_ok=True)
    if WORK.exists() and (WORK.is_symlink() or not (WORK / MARKER).is_file()):
        raise RuntimeError("Refusing to replace an unowned shared staging directory")
    temporary = WORK.parent / (".shared-android-stage-" + uuid.uuid4().hex)
    temporary.mkdir()
    try:
        # Exact committed source, excluding ignored/untracked files and Git metadata; no repository build script runs.
        process = subprocess.Popen(["git", "-C", str(source), "archive", "--format=tar", pinned["commit"]],
                                   stdout=subprocess.PIPE)
        try:
            with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
                archive.extractall(temporary, filter="data")
        finally:
            process.stdout.close()
        if process.wait() != 0:
            raise RuntimeError("Could not stage the pinned LAS archive")
        for path in overlays:
            target = temporary / "runtime" / path.relative_to(PORT / "runtime")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        # User-visible product text in base-only files; literal-only, exact counts.
        branding = branding_substitutions.apply(temporary / "runtime", PORT / "runtime")
        for path in native:
            target = temporary / path.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        if any(sha(ROOT / relative) != digest for relative, digest in hashes.items()) or \
                sha(branding_substitutions.RULES) != branding_rules:
            raise RuntimeError("Overlay/common source changed while staging; rerun after edits finish")
        missing = ["runtime/" + name for name in REQUIRED_OVERLAYS if not (PORT / "runtime" / name).is_file()]
        missing += ["native/" + name for name in REQUIRED_NATIVE if not (ROOT / "native" / name).is_file()]
        manifest = {"schema": 1, "base_source": pinned, "source_lock_sha256": sha(ROOT / "sources.lock.json"),
                    "echoes_source_lock_sha256": sha(ROOT / "echoes-source.lock.json"),
                    "overlay_and_native_sha256": hashes,
                    "runtime_overlays": [str(p.relative_to(PORT / "runtime")) for p in overlays],
                    "branding_substitutions": branding, "branding_rules_sha256": branding_rules,
                    "missing_required_port_files": missing, "runtime_overlay_ready": not missing,
                    "game_code_included": False, "device_executed": False}
        atomic_json(temporary / MARKER, manifest)
        previous = WORK.parent / (".shared-android-previous-" + uuid.uuid4().hex)
        if WORK.exists():
            WORK.rename(previous)
        temporary.rename(WORK)
        if previous.exists():
            shutil.rmtree(previous)
        return manifest
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def dependencies(sdk, native_blocks=False):
    deps = LAS / "build/android/deps"
    ffmpeg = ROOT / "build/letsgo-ffmpeg/prefix"
    ffmpeg_manifest = ROOT / "build/letsgo-ffmpeg/build.json"
    ffmpeg_record = json.loads(ffmpeg_manifest.read_text())
    if not ffmpeg_record.get("validation", {}).get("native_opus_registration_reference"):
        raise RuntimeError("Cached isolated FFmpeg lacks validated native Opus")
    archives = [ffmpeg / "lib" / name for name in ("libavformat.a", "libavcodec.a", "libswscale.a", "libswresample.a", "libavutil.a")]
    for path in archives:
        if sha(path) != ffmpeg_record["libraries"].get(path.name):
            raise RuntimeError("Isolated FFmpeg archive differs from its build manifest")
    sdl_candidates = list((LAS / "android/app/build/intermediates/cxx/RelWithDebInfo").glob("*/obj/arm64-v8a/libSDL3.so"))
    if len(sdl_candidates) != 1:
        raise RuntimeError("Expected one cached ARM64 SDL3 library")
    sdl = sdl_candidates[0]
    tag = sdl.parent.parent.parent.name
    cached = LAS / "build/android/cxx/RelWithDebInfo" / tag / "arm64-v8a/_deps"
    include = cached / "adrenotools-src/include"
    adreno = cached / "adrenotools-build/libadrenotools.a"
    namespace = cached / "adrenotools-build/lib/linkernsbypass/liblinkernsbypass.a"
    shaderc = deps / "lib/libshaderc_combined.a"
    for path in [sdl, adreno, namespace, shaderc, include / "adrenotools/driver.h", deps / "include/volk/volk.c",
                 deps / "include/vulkan/vulkan.h", deps / "include/vma/vk_mem_alloc.h",
                 LAS / "build/android/SDL3/include/SDL3/SDL.h"]:
        if not path.is_file():
            raise RuntimeError(f"Required local cached dependency is missing: {path}")
    vm = ROOT / "build/shared-ondevice"
    capstone_include = vm / "work/build/ondevice/src/capstone-5.0.9/src/include"
    unicorn_include = vm / "work/build/ondevice/src/unicorn-2.1.4/src/include"
    capstone = vm / "stage/jniLibs/arm64-v8a/libcapstone.so"
    unicorn = vm / "stage/jniLibs/arm64-v8a/libunicorn.so"
    backend = [capstone, capstone_include / "capstone/capstone.h"]
    if native_blocks:  # Unicorn (GPL-2.0) only for the opt-in native-blocks diagnostic backend
        backend += [unicorn, unicorn_include / "unicorn/unicorn.h"]
    for path in backend:
        if not path.is_file():raise RuntimeError("Missing cached native backend dependency: " + str(path))
    backend_libraries = [capstone, unicorn] if native_blocks else [capstone]
    dependency_files = [*archives, sdl, adreno, namespace, shaderc, ffmpeg_manifest, *backend_libraries]
    header_roots = [deps / "include", ffmpeg / "include", LAS / "build/android/SDL3/include", include, capstone_include]
    if native_blocks:
        header_roots.append(unicorn_include)
    for headers in header_roots:
        dependency_files += [path for path in headers.rglob("*") if path.is_file() and path.suffix in (".h", ".hpp", ".c")]
    return {"deps": deps, "ffmpeg": ffmpeg, "sdl": sdl, "adreno_include": include,
            "capstone_include": capstone_include, "unicorn_include": unicorn_include, "capstone": capstone, "unicorn": unicorn,
            "adreno": adreno, "namespace": namespace,
            "hashes": {str(p.relative_to(ROOT)): sha(p) for p in sorted(set(dependency_files))}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Configure/link libmain.so after runtime overlays are ready")
    parser.add_argument("--jobs", type=int, choices=(1, 2, 3, 4), default=4)
    parser.add_argument("--sdk", type=Path, default=Path.home() / "android-sdk")
    parser.add_argument("--with-native-blocks", action="store_true",
                        help="Also build the Unicorn native-blocks diagnostic backend (links GPL-2.0 Unicorn)")
    args = parser.parse_args()
    prepared = stage()
    BUILD.mkdir(parents=True, exist_ok=True)
    include = BUILD / "include"
    gpu_cache_fingerprint.bootstrap(include / "gpu_cache_build.h")
    contract = hashlib.sha256(b"shared-aot-v1\0" + (WORK / "native/runtime/aot_cpu.h").read_bytes()
                              + (WORK / "native/module_abi.h").read_bytes()).hexdigest()
    native_contract = hashlib.sha256(b"shared-native-v1\0" + (WORK / "native/runtime/aot_cpu.h").read_bytes()
                                     + (WORK / "native/module_abi.h").read_bytes()
                                     + (WORK / "native/runtime/native_manifest.h").read_bytes()).hexdigest()
    write_changed(include / "shared_contract.h", ("#pragma once\n#define SHARED_AOT_CONTRACT_SHA256_HEX \"" + contract
        + "\"\n#define SHARED_NATIVE_CONTRACT_SHA256_HEX \"" + native_contract + "\"\n").encode())
    # Same ASCII string conversion as the pinned build, without shell tools or external generation scripts.
    smaa = (WORK / "third_party/smaa/SMAA.hlsl").read_bytes().replace(b"\r", b"")
    text = "".join(chr(value) if value < 128 else "?" for value in smaa)
    def embed(content):
        return "".join('"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"\n' for line in content.splitlines()).encode()
    write_changed(include / "smaa_hlsl.inc", embed(text))
    variants = (WORK / "runtime/nvn/shader_variants.txt").read_text().replace("\r", "")
    write_changed(include / "shader_variants.inc", embed(variants) + b'""\n')
    overlay_index = include / "runtime_overlays.cmake"
    write_changed(overlay_index, ("set(RECOMP_RUNTIME_OVERLAYS\n" + "".join(
        '  "' + name.replace("\\", "/") + '"\n' for name in prepared["runtime_overlays"]) + ")\n").encode())
    prepared.update(shared_contract_sha256=contract, native_contract_sha256=native_contract,
                    cmake_sha256=sha(PORT / "CMakeLists.txt"), exports_map_sha256=sha(PORT / "exports.map"),
                    builder_sha256=sha(Path(__file__)),
                    generated_include_sha256={path.name: sha(path) for path in include.iterdir() if path.is_file()})
    atomic_json(BUILD / "stage.json", prepared)
    if not args.build:
        print("Prepared owned common Android source; no native build or device actions.")
        if prepared["missing_required_port_files"]:
            print("Runtime files not yet ready: " + ", ".join(prepared["missing_required_port_files"]))
        return
    if not prepared["runtime_overlay_ready"]:
        raise RuntimeError("Runtime overlays are incomplete; see build/shared-android/stage.json")
    sdk = args.sdk.resolve()
    ndk = sdk / "ndk/27.2.12479018"
    if "Pkg.Revision = 27.2.12479018" not in (ndk / "source.properties").read_text():
        raise RuntimeError("Expected cached NDK r27c")
    dep = dependencies(sdk, args.with_native_blocks)
    cmake = sdk / "cmake/3.31.6/bin/cmake"
    native = BUILD / "native"
    command = [str(cmake), "-S", str(PORT), "-B", str(native), "-G", "Ninja",
               f"-DCMAKE_MAKE_PROGRAM={sdk}/cmake/3.31.6/bin/ninja",
               f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake",
               "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-30", "-DANDROID_STL=c++_static",
               "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
               f"-DRECOMP_SOURCE={WORK}", f"-DRECOMP_ANDROID_DEPS={dep['deps']}",
               f"-DRECOMP_FFMPEG_PREFIX={dep['ffmpeg']}", f"-DRECOMP_SDL_LIBRARY={dep['sdl']}",
               f"-DRECOMP_SDL_SOURCE={LAS}/build/android/SDL3", f"-DRECOMP_GENERATED_INCLUDE={include}",
               f"-DRECOMP_OVERLAY_INDEX={overlay_index}", f"-DRECOMP_ADRENOTOOLS_INCLUDE={dep['adreno_include']}",
               f"-DRECOMP_ADRENOTOOLS_LIBRARY={dep['adreno']}", f"-DRECOMP_ADRENOTOOLS_NS_LIBRARY={dep['namespace']}",
               f"-DRECOMP_CAPSTONE_INCLUDE={dep['capstone_include']}",f"-DRECOMP_UNICORN_INCLUDE={dep['unicorn_include']}",
               f"-DRECOMP_CAPSTONE_LIBRARY={dep['capstone']}",f"-DRECOMP_UNICORN_LIBRARY={dep['unicorn']}",
               f"-DRAITCHI_NATIVE_BLOCKS={'ON' if args.with_native_blocks else 'OFF'}"]
    manifest = BUILD / "native-build.json"
    manifest.unlink(missing_ok=True)
    with (BUILD / "build.log").open("a") as log:
        log.write("\n" + json.dumps(command) + "\n"); log.flush()
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        toolchain = ndk / "toolchains/llvm/prebuilt/linux-x86_64"
        resource = Path(subprocess.run([str(toolchain / "bin/clang"), "--print-resource-dir"],
            check=True, capture_output=True, text=True).stdout.strip())
        # Real compiler preprocessor dependencies, compiled flags and static
        # implementations: source-only CPU/UI changes do not affect replay ABI.
        identity = gpu_cache_fingerprint.build(native / "compile_commands.json", WORK, ROOT, include,
            dep["deps"] / "lib/libshaderc_combined.a", [
                PORT / "CMakeLists.txt", PORT / "exports.map", Path(__file__),
                ndk / "source.properties", dep["sdl"], dep["adreno"], dep["namespace"],
                toolchain / "sysroot/usr/lib/aarch64-linux-android/libc++_static.a",
                toolchain / "sysroot/usr/lib/aarch64-linux-android/libc++abi.a",
                resource / "lib/linux/libclang_rt.builtins-aarch64-android.a"])
        identity_report = BUILD / "gpu-cache-fingerprint.json"
        gpu_cache_fingerprint.write(identity, include / "gpu_cache_build.h", identity_report)
        prepared.update(gpu_cache_fingerprint=identity,
            gpu_cache_fingerprint_tool_sha256=sha(ROOT / "tools/gpu_cache_fingerprint.py"),
            gpu_cache_fingerprint_report_sha256=sha(identity_report),
            generated_include_sha256={path.name: sha(path) for path in include.iterdir() if path.is_file()})
        atomic_json(BUILD / "stage.json", prepared)
        argv = [str(cmake), "--build", str(native), "--target", "main", "raitchiprepare", "-j", str(args.jobs)]
        log.write("\n" + json.dumps(argv) + "\n"); log.flush()
        subprocess.run(argv, check=True, stdout=log, stderr=subprocess.STDOUT)
    gpu_cache_fingerprint.verify(identity)
    library = native / "libmain.so"
    if sha(PORT / "CMakeLists.txt") != prepared["cmake_sha256"] or sha(PORT / "exports.map") != prepared["exports_map_sha256"]:
        raise RuntimeError("Build configuration/export map changed during build")
    for relative, digest in prepared["overlay_and_native_sha256"].items():
        path = (WORK / "runtime" / Path(relative).relative_to("ports/shared-android/runtime")
                if relative.startswith("ports/shared-android/runtime/") else WORK / relative)
        if sha(path) != digest:
            raise RuntimeError("Staged source changed during build; no success manifest published")
    if sha(branding_substitutions.RULES) != prepared["branding_rules_sha256"]:
        raise RuntimeError("Branding substitutions changed during build; no success manifest published")
    if any(sha(ROOT / relative) != digest for relative, digest in dep["hashes"].items()):
        raise RuntimeError("Cached dependency changed during build; no success manifest published")
    if any(sha(include / name) != digest for name, digest in prepared["generated_include_sha256"].items()):
        raise RuntimeError("Generated include changed during build; no success manifest published")
    readelf = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    report = subprocess.run([str(readelf), "--file-header", "--program-headers", "--dynamic", "--dyn-syms", "--wide", str(library)],
                            check=True, capture_output=True, text=True).stdout
    (BUILD / "library-elf.txt").write_text(report)
    loads = [line.split() for line in report.splitlines() if line.lstrip().startswith("LOAD ")]
    exports = {parts[7] for line in report.splitlines() if len(parts := line.split()) >= 8
               and re.fullmatch(r"\d+:", parts[0]) and parts[4] in ("GLOBAL", "WEAK") and parts[5] == "DEFAULT" and parts[6] != "UND"}
    required = {"SDL_main", "module_abi_validate", "module_loader_open", "shared_aot_open", "rt_dispatch", "rt_fnhook", "rt_hook", "hle_memcpy"}
    if ("AArch64" not in report or not re.search(r"SONAME.*\[libmain.so\]", report) or not loads
            or any(int(fields[-1], 16) < 16384 for fields in loads) or not required <= exports):
        raise RuntimeError("Common runtime ELF architecture/SONAME/alignment/exports check failed")
    preparation_library = native / "libraitchiprepare.so"
    preparation_report = subprocess.run([str(readelf), "--file-header", "--program-headers", "--dynamic", "--dyn-syms", "--wide", str(preparation_library)],
                                        check=True, capture_output=True, text=True).stdout
    (BUILD / "preparation-library-elf.txt").write_text(preparation_report)
    preparation_loads = [line.split() for line in preparation_report.splitlines() if line.lstrip().startswith("LOAD ")]
    preparation_needed = re.findall(r"NEEDED.*\[([^\]]+)\]", preparation_report)
    if ("AArch64" not in preparation_report or not re.search(r"SONAME.*\[libraitchiprepare.so\]", preparation_report)
            or not preparation_loads or any(int(fields[-1], 16) < 16384 for fields in preparation_loads)
            or "Java_com_lasrecomp_android_NativePackage_prepare" not in preparation_report
            or any(name not in {"libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so"} for name in preparation_needed)):
        raise RuntimeError("Standalone package preparation ELF/dependency/JNI check failed")
    compiler = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
    result = {**prepared, "library_sha256": sha(library), "library_bytes": library.stat().st_size,
              "preparation_library_sha256": sha(preparation_library), "preparation_library_bytes": preparation_library.stat().st_size,
              "preparation_needed": preparation_needed, "native_blocks": args.with_native_blocks,
              "target": "aarch64-linux-android30", "configure_command": command,
              "dependencies": dep["hashes"], "ndk_properties_sha256": sha(ndk / "source.properties"),
              "compiler_sha256": sha(compiler.resolve()),
              "compiler_version": subprocess.run([str(compiler), "--version"], check=True,
                                                   capture_output=True, text=True).stdout,
              "needed": re.findall(r"NEEDED.*\[([^\]]+)\]", report), "dynamic_exports": sorted(exports),
              "built_utc": datetime.now(timezone.utc).isoformat(), "game_code_included": False, "device_executed": False}
    atomic_json(manifest, result)
    print("Linked and inspected one common Android libmain.so; no APK or device actions.")


if __name__ == "__main__":
    main()
