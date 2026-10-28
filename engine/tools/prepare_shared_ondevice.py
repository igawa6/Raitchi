#!/usr/bin/env python3
"""Prepare real Android Python/Zig tools and shared-AOT headers, without games.

Default inventories local inputs. --fetch explicitly obtains pinned archives;
--build cross-builds with cached NDK and at most two jobs. Never uses ADB.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "sources/las-android"
OUT = ROOT / "build/shared-ondevice"
LAS_PIN = "cca791a3e7218e1dac95e7d4a11b8f97dba3ea51"
LG_PIN = "96894fd8e9ea20c1b38cdea53105ec2a335df620"
INPUTS = [
    ("python-dl/python-3.14.8-aarch64-linux-android.tar.gz", "https://www.python.org/ftp/python/3.14.8/python-3.14.8-aarch64-linux-android.tar.gz", "11324313957da3736f44e90257304d288864321a4dd376ed0cc35a54eacf9a33"),
    ("capstone-dl/capstone-5.0.9.tar.gz", "https://files.pythonhosted.org/packages/39/01/6516910f546fbb996068207b9dd0229b14bc8dae223114d5e0e27d3cad11/capstone-5.0.9.tar.gz", "0429af292ddc604d3c9344696807e281d5e728db029f00e6a4ea9e3bff1aac9e"),
    ("unicorn-dl/unicorn-2.1.4.tar.gz", "https://files.pythonhosted.org/packages/b2/1b/b4248aa8422e86de690cf8e85cf8feae4c33405a097d1ebe71570bdaa6f5/unicorn-2.1.4.tar.gz", "00567a70e323f749b419cd86bee4f9115beab7ebba32194581c090cbb7c59cff"),
    ("lz4-dl/lz4-4.4.5.tar.gz", "https://files.pythonhosted.org/packages/57/51/f1b86d93029f418033dddf9b9f79c8d2641e7454080478ee2aab5123173e/lz4-4.4.5.tar.gz", "5f0b9e53c1e82e88c10d7c180069363980136b9d7a8306c4dca4f760d60c39f0"),
    ("zig-dl/zig-aarch64-linux-0.17.0.tar.xz", "https://ziglang.org/download/0.17.0/zig-aarch64-linux-0.17.0.tar.xz", "9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8"),
    ("zig-dl/zig-x86_64-linux-0.17.0.tar.xz", "https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz", "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"),
    ("licenses-dl/unicorn-COPYING.txt", "https://raw.githubusercontent.com/unicorn-engine/unicorn/2.1.4/COPYING", "6f04ae8364d0079a192b14635f4b1da294ce18724c034c39a6a41d1b09df6100"),
    ("licenses-dl/llvm-LICENSE.txt", "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-22.1.8/llvm/LICENSE.TXT", "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee"),
]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def record(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def run(argv, log, env=None):
    print(log.stem, flush=True)
    with log.open("w") as stream:
        result = subprocess.run([str(x) for x in argv], stdout=stream, stderr=subprocess.STDOUT,
                                env=env, cwd=ROOT)
    if result.returncode:
        raise RuntimeError(f"{log.stem} failed; see {log}")


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def acquire(work, fetch, cache_dirs):
    records = []
    missing = []
    for relative, url, digest in INPUTS:
        target = work / "build/ondevice" / relative
        if not target.is_file():
            for cache in cache_dirs:
                candidates = (cache / relative, cache / Path(relative).name)
                candidate = next((p for p in candidates if p.is_file() and sha(p) == digest), None)
                if candidate:
                    copy(candidate, target)
                    break
        if not target.is_file() and fetch:
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_suffix(target.suffix + ".part")
            print("Fetching pinned " + target.name, flush=True)
            with urllib.request.urlopen(url, timeout=90) as response, temporary.open("wb") as stream:
                shutil.copyfileobj(response, stream)
            if sha(temporary) != digest:
                raise RuntimeError("Downloaded input SHA256 mismatch: " + target.name)
            temporary.replace(target)
        present = target.is_file()
        if present and sha(target) != digest:
            raise RuntimeError("Cached input SHA256 mismatch: " + target.name)
        if not present:
            missing.append(target.name)
        records.append({"file": relative, "url": url, "sha256": digest,
                        "available": present, "bytes": target.stat().st_size if present else None})
    return records, missing


def source_snapshot(work):
    scripts = ["tools/android/ondevice/build_python.sh", "tools/android/ondevice/build_zig.sh",
               "tools/android/ondevice/pylauncher.c", "tools/fetch_zig.sh"]
    hashes = {}
    for relative in scripts:
        src = UPSTREAM / relative
        hashes[relative] = sha(src)
        copy(src, work / relative)
    script = work / scripts[0]
    text = script.read_text()
    if text.count("JOBS=$(nproc)") != 1 or text.count("-m compileall -q -j0") != 1:
        raise RuntimeError("Upstream Python recipe changed; review job-limit adaptation")
    text = text.replace("JOBS=$(nproc)", "JOBS=${SHARED_ONDEVICE_JOBS:-2}")
    text = text.replace("-m compileall -q -j0", "-m compileall -q -j1")
    script.write_text(text)
    return hashes


def compiler_kit(stage, work, ndk, logdir):
    tc = ndk / "toolchains/llvm/prebuilt/linux-x86_64"
    zig = work / "build/ondevice/zig-host/zig"
    inc = work / "build/ondevice/zig-host/lib/include"
    kit = stage / "cckit"
    include = kit / "include"
    for relative in ("runtime/aot_cpu.h", "module_abi.h"):
        copy(ROOT / "native" / relative, include / relative)
    for title, source, shift, caps in (("las", UPSTREAM, "0x6000000000", 1),
                                       ("eevee", ROOT / "sources/letsgo", "0", 2)):
        profile = include / "profiles" / title
        profile.mkdir(parents=True, exist_ok=True)
        (profile / "cpu.h").write_text(f"#define AOT_GUEST_SHIFT {shift}ull\n#define AOT_PROFILE_CAPABILITIES {caps}u\n#include \"runtime/aot_cpu.h\"\n")
        for name in ("simd.h", "simd_fp.h"):
            copy(source / "runtime" / name, profile / name)
        probe = logdir / f"headers-{title}.c"
        probe.write_text('#include "cpu.h"\n#include "simd.h"\n#include "module_abi.h"\n')
        dep = logdir / f"headers-{title}.d"
        command = [zig, "clang", "-target", "aarch64-linux-android30", "-nostdinc",
                   "-isystem", inc, "-isystem", tc / "sysroot/usr/include",
                   "-isystem", tc / "sysroot/usr/include/aarch64-linux-android",
                   "-I", include, "-I", profile, "-O1", "-fPIC", "-ffixed-x18",
                   "-MD", "-MF", dep, "-c", probe, "-o", logdir / f"headers-{title}.o"]
        run(command, logdir / f"headers-{title}.log")
        # Follow actual dependency paths, including compiler-version-specific headers.
        dependencies = dep.read_text().replace("\\\n", " ").split(":", 1)[1].split()
        for item in dependencies:
            header = Path(item).resolve()
            for base, destination in ((inc.resolve(), kit / "clang/include"),
                                      ((tc / "sysroot").resolve(), kit / "sysroot")):
                if header.is_relative_to(base):
                    copy(header, destination / header.relative_to(base))
                    break
    lib = tc / "sysroot/usr/lib/aarch64-linux-android/30"
    for name in ("crtbegin_so.o", "crtend_so.o", "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so"):
        copy(lib / name, kit / "sysroot/usr/lib/aarch64-linux-android/30" / name)
    builtins = next((tc / "lib/clang").glob("*/lib/linux/libclang_rt.builtins-aarch64-android.a"))
    copy(builtins, kit / "clang/lib" / builtins.name)
    (kit / "link.rsp").write_text("-EL\n--fix-cortex-a53-843419\n-z now\n-z relro\n--hash-style=gnu\n--eh-frame-hdr\n-m aarch64linux\n-shared\n--no-undefined\n-z max-page-size=16384\n-Bsymbolic\n-soname liblasgame.so\n")
    # A public fixture exercises actual memory/exclusive helpers and AArch64 SIMD headers.
    (kit / "smoke.c").write_text('#include "cpu.h"\n#include "simd.h"\n__attribute__((visibility("default"))) uint64_t shared_tool_smoke(Cpu *c,uint64_t p) { uint64_t x=ldx64(c,p); return stx64(c,p,x+1); }\n')


def generators(stage):
    hashes = {}
    for title, source in (("las", UPSTREAM), ("eevee", ROOT / "sources/letsgo")):
        destination = stage / "generators" / title
        files = list((source / "tools").glob("*.py"))
        for pattern in ("tools/hot_functions.txt", "tools/rando/patch_variants.json",
                        "runtime/game_addresses/*.h", "runtime/hle/hle_symbols.txt"):
            files.extend(source.glob(pattern))
        for file in sorted(set(files)):
            relative = file.relative_to(source)
            copy(file, destination / relative)
            hashes[f"generators/{title}/{relative}"] = sha(file)
    return hashes


def licenses(stage, work, ndk):
    base = work / "build/ondevice"
    files = {
        "python-LICENSE.txt": base / "stage/LICENSE.python.txt",
        "capstone-LICENSE.txt": base / "stage/LICENSE.capstone.txt",
        "python-lz4-LICENSE.txt": base / "stage/LICENSE.python-lz4.txt",
        "zig-LICENSE.txt": base / "stage/LICENSE.zig.txt",
        "unicorn-COPYING.txt": base / "licenses-dl/unicorn-COPYING.txt",
        "llvm-LICENSE.txt": base / "licenses-dl/llvm-LICENSE.txt",
        "musl-COPYRIGHT.txt": base / "zig/lib/libc/musl/COPYRIGHT",
        "android-ndk-NOTICE.txt": ndk / "NOTICE",
        "android-ndk-toolchain-NOTICE.txt": ndk / "NOTICE.toolchain",
        "LAS-MIT.txt": UPSTREAM / "LICENSE",
        "LetsGo-MIT.txt": ROOT / "sources/letsgo/LICENSE",
    }
    for name, source in files.items():
        copy(source, stage / "licenses" / name)
    record(stage / "licenses/components.json", {
        "las_source_commit": LAS_PIN, "eevee_source_commit": LG_PIN,
        "upstream_sources": [{"file": Path(name).name, "url": url, "sha256": digest}
                             for name, url, digest in INPUTS]})
    (stage / "licenses/README.txt").write_text(
        "Python3.14.8 PSF2; Capstone5.0.9 BSD3; Unicorn2.1.4 GPL2 (Python bindings BSD); "
        "python-lz44.4.5 BSD3/LZ4 BSD2; Zig0.17 MIT/LLVM Apache2 with exception/musl MIT; "
        "Android NDKr27c BSD/Apache2. LAS and Let's Go generators MIT.\n"
        "Exact source URLs and archive SHA256 are recorded in licenses/components.json. Unicorn complete source "
        "archive is retained in the preparation cache. No game-derived code is included.\n")


def producer_metadata(stage, binaries):
    producer = ROOT / "tools/shared_ondevice_build.py"
    if not producer.is_file():
        raise RuntimeError("Shared on-device producer source is not ready")
    copy(producer, stage / "shared_ondevice_build.py")
    runtime = ROOT / "build/shared-android/native/libmain.so"
    native_manifest = json.loads((ROOT / "build/shared-android/native-build.json").read_text())
    runtime_sha = sha(runtime)
    if native_manifest.get("library_sha256") != runtime_sha:
        raise RuntimeError("Common native runtime does not match completed provenance")
    contract = hashlib.sha256(b"shared-aot-v1\0" + (ROOT / "native/runtime/aot_cpu.h").read_bytes()
                              + (ROOT / "native/module_abi.h").read_bytes()).hexdigest()
    if native_manifest.get("shared_contract_sha256") != contract:
        raise RuntimeError("Common runtime/helper contract differs")
    for name in ("runtime/aot_cpu.h", "module_abi.h"):
        if sha(stage / "cckit/include" / name) != sha(ROOT / "native" / name):
            raise RuntimeError("Staged shared CPU/ABI header differs; rebuild the toolkit")
    titles = {}
    allowed = {"name", "module_id", "canonical_base", "sha256", "build_id", "mapped_size", "text_offset", "text_size"}
    for title, pin in (("las", LAS_PIN), ("eevee", LG_PIN)):
        prepare = json.loads((ROOT / f"build/shared-gamelib/{title}/prepare.json").read_text())
        modules = prepare["semantic"]["modules"]
        if isinstance(modules, dict):
            modules = [dict(value, name=name) for name, value in modules.items()]
        titles[title] = {"source_commit": pin,
                         "modules": [{key: value for key, value in item.items() if key in allowed} for item in modules]}
    files = {p.relative_to(stage).as_posix(): sha(p) for p in sorted(stage.rglob("*"))
             if p.is_file() and "jniLibs" not in p.relative_to(stage).parts and p.name != "shared-build-kit.json"}
    data = {"schema": 1, "contract_sha256": contract, "runtime_sha256": runtime_sha,
            "files": files, "native_files": {name: value["sha256"] for name, value in binaries.items()},
            "compiler_kind": "zig", "python_home": "pyhome",
            "python_paths": ["pyhome/lib/python3.14/site-packages"], "titles": titles}
    data["native_files"]["libmain.so"] = runtime_sha
    record(stage / "shared-build-kit.json", data)


def verify_compiler_kit(stage, work, logs):
    kit = stage / "cckit"
    zig = work / "build/ondevice/zig-host/zig"
    lib = kit / "sysroot/usr/lib/aarch64-linux-android/30"
    result = {}
    for title in ("las", "eevee"):
        obj = logs / f"kit-smoke-{title}.o"
        library = logs / f"kit-smoke-{title}.so"
        compile = [zig, "clang", "-target", "aarch64-linux-android30", "-O1", "-fPIC", "-ffixed-x18",
                   "-fvisibility=hidden", "-nostdinc", "-isystem", kit / "clang/include",
                   "-isystem", kit / "sysroot/usr/include", "-isystem", kit / "sysroot/usr/include/aarch64-linux-android",
                   "-I", kit / "include", "-I", kit / "include/profiles" / title,
                   "-c", kit / "smoke.c", "-o", obj]
        link = [zig, "ld.lld", "@" + str(kit / "link.rsp"), "-o", library, lib / "crtbegin_so.o", obj,
                "-L" + str(ROOT / "build/shared-android/native"), "-lmain", "-L" + str(lib),
                kit / "clang/lib/libclang_rt.builtins-aarch64-android.a", "-ldl", "-lm", "-lc", lib / "crtend_so.o"]
        run(compile, logs / f"kit-smoke-{title}-compile.log")
        run(link, logs / f"kit-smoke-{title}-link.log")
        result[title] = {"compile": [str(x) for x in compile], "link": [str(x) for x in link],
                         "library_sha256": sha(library), "scope": "host Zig cross-compile/link of public memory/exclusive fixture"}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="download missing pinned inputs serially")
    parser.add_argument("--build", action="store_true", help="build Android tools from verified cached inputs")
    parser.add_argument("--package-only", action="store_true", help="verify existing stage and refresh frozen producer/metadata/archive")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    parser.add_argument("--cache-dir", type=Path, action="append", default=[])
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    for path, pin in ((UPSTREAM, LAS_PIN), (ROOT / "sources/letsgo", LG_PIN)):
        actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
        if actual != pin:
            raise SystemExit("Pinned reference checkout changed")
    # Verify these exact hashes are present in the reviewed pinned recipes.
    pin_text = "\n".join((UPSTREAM / file).read_text() for file in (
        "tools/android/ondevice/build_python.sh", "tools/fetch_zig.sh", "tools/android/ondevice/make_assets.sh"))
    if any(digest not in pin_text for _, _, digest in INPUTS):
        raise SystemExit("Dependency pins differ from reviewed upstream recipes")
    work = OUT / "work"
    work.mkdir(exist_ok=True)
    inputs, missing = acquire(work, args.fetch, args.cache_dir)
    record(OUT / "inventory.json", {"inputs": inputs, "missing": missing,
           "cached_bytes": sum(x["bytes"] or 0 for x in inputs), "device_executed": False})
    print(f"Verified {len(inputs)-len(missing)}/{len(inputs)} inputs; cached {sum(x['bytes'] or 0 for x in inputs):,} bytes")
    if missing:
        print("Missing: " + ", ".join(missing))
        if args.build or args.package_only:
            raise SystemExit("Use --fetch or supply --cache-dir before --build")
        return
    if not (args.build or args.package_only):
        return
    logs = OUT / "logs"
    logs.mkdir(exist_ok=True)
    if args.package_only:
        previous = json.loads((OUT / "build.json").read_text())
        source_hashes = previous["source_recipe_sha256"]
        if any(sha(UPSTREAM / name) != value for name, value in source_hashes.items()):
            raise RuntimeError("Upstream recipe changed; rebuild the toolkit")
        if previous["ndk_properties_sha256"] != sha(args.ndk / "source.properties"):
            raise RuntimeError("NDK changed; rebuild the toolkit")
        for name, value in previous["files"].items():
            if name in ("shared_ondevice_build.py", "shared-build-kit.json"):
                continue
            if sha(OUT / "stage" / name) != value["sha256"]:
                raise RuntimeError("Existing toolkit stage changed: " + name)
    else:
        source_hashes = source_snapshot(work)
    env = os.environ.copy()
    env.update(ANDROID_HOME=str(args.ndk.parent.parent), ANDROID_NDK_HOME=str(args.ndk),
               SHARED_ONDEVICE_JOBS=str(args.jobs), CMAKE_BUILD_PARALLEL_LEVEL=str(args.jobs))
    # All recipe downloads are already verified; block accidental network fallback.
    guard = OUT / "offline-bin"
    guard.mkdir(exist_ok=True)
    (guard / "curl").write_text('#!/bin/sh\necho "Unexpected recipe network access" >&2\nexit 1\n')
    (guard / "curl").chmod(0o755)
    env["PATH"] = str(guard) + os.pathsep + env["PATH"]
    if not args.package_only:
        for name in ("build_python.sh", "build_zig.sh"):
            run(["bash", work / "tools/android/ondevice" / name], logs / (name + ".log"), env)
    stage = OUT / "stage"
    stage.mkdir(exist_ok=True)
    original_stage = work / "build/ondevice/stage"
    if not args.package_only:
        for name in ("pyhome", "jniLibs"):
            shutil.copytree(original_stage / name, stage / name, dirs_exist_ok=True)
        compiler_kit(stage, work, args.ndk, logs)
        generator_hashes = generators(stage)
        (stage / "selftest.py").write_text((UPSTREAM / "tools/android/ondevice/selftest.py").read_text())
    else:
        generator_hashes = previous["generator_sha256"]
    licenses(stage, work, args.ndk)
    readelf = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    binaries = {}
    for binary in sorted((stage / "jniLibs/arm64-v8a").glob("*.so")):
        result = subprocess.check_output([str(readelf), "-h", "-l", "-d", str(binary)], text=True)
        if "AArch64" not in result:
            raise RuntimeError("Non-Android-ARM64 toolkit binary: " + binary.name)
        (logs / (binary.name + ".elf.txt")).write_text(result)
        binaries[binary.name] = {"sha256": sha(binary), "bytes": binary.stat().st_size}
    smoke = verify_compiler_kit(stage, work, logs)
    producer_metadata(stage, binaries)
    asset = OUT / "ondevice.zip"
    with zipfile.ZipFile(asset.with_suffix(".zip.tmp"), "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(stage.rglob("*")):
            if not path.is_file() or "jniLibs" in path.relative_to(stage).parts:
                continue
            name = path.relative_to(stage).as_posix()
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, path.read_bytes(), compresslevel=9)
    asset.with_suffix(".zip.tmp").replace(asset)
    files = {str(p.relative_to(stage)): {"sha256": sha(p), "bytes": p.stat().st_size}
             for p in sorted(stage.rglob("*")) if p.is_file()}
    record(OUT / "build.json", {"format": 1, "target": "aarch64-linux-android30", "cpu_identity": 3,
        "builder_sha256": sha(Path(__file__)),
        "source_pins": {"las": LAS_PIN, "eevee": LG_PIN}, "inputs": inputs,
        "source_recipe_sha256": source_hashes,
        "adapted_recipe_sha256": {name: sha(work / name) for name in source_hashes},
        "recipe_adaptations": ["Linux CPU jobs capped at --jobs <=2", "host compileall jobs1", "offline curl guard"],
        "generator_sha256": generator_hashes, "files": files, "binaries": binaries,
        "asset": {"sha256": sha(asset), "bytes": asset.stat().st_size},
        "ndk_properties_sha256": sha(args.ndk / "source.properties"),
        "host_compiler_kit_smoke": smoke,
        "scope": "real Android executables/extensions; host ELF and header probes only",
        "device_executed": False, "generated_game_code_included": False})
    artifact = lambda path: {"source": path.relative_to(ROOT).as_posix(), "sha256": sha(path), "bytes": path.stat().st_size}
    record(OUT / "toolkit.json", {"schema": 1, "root": "files/ondevice", "compiler": "zig0.17.0/clang22.1.8",
        "python_version": "3.14.8", "licenses": "ondevice.zip:licenses/", "device_executed": False,
        "native_libraries": {name: artifact(stage / "jniLibs/arm64-v8a" / name) for name in binaries},
        "assets": {"ondevice.zip": artifact(asset)}})
    print(f"Prepared real Android toolkit: asset {asset.stat().st_size:,} bytes; native executables/libraries {sum(x['bytes'] for x in binaries.values()):,} bytes")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print("error: " + str(exc), file=sys.stderr)
        sys.exit(1)
