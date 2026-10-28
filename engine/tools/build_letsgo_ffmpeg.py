#!/usr/bin/env python3
"""Build the pinned cached FFmpeg with native Opus for Android; offline, no ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/letsgo-ffmpeg"
ARCHIVE = ROOT / "work/las-baseline/build/android/src/ffmpeg-9.0.2.tar.xz"
ARCHIVE_SHA = "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
LIBRARIES = ("avformat", "avcodec", "swscale", "swresample", "avutil")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Configure, build, install and validate cached sources")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 4:
        parser.error("--jobs must be between 1 and 4")
    if not ARCHIVE.is_file() or sha(ARCHIVE) != ARCHIVE_SHA:
        raise RuntimeError("Missing or mismatched pinned cached FFmpeg archive; downloads are disabled")
    ndk = args.ndk.resolve()
    toolchain = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    compiler = toolchain / "aarch64-linux-android30-clang"
    if not compiler.is_file():
        raise RuntimeError("NDK r27c Linux toolchain is missing")
    ndk_properties = (ndk / "source.properties").read_text()
    if "Pkg.Revision = 27.2.12479018" not in ndk_properties:
        raise RuntimeError("Expected pinned NDK 27.2.12479018")
    source, objects, prefix = (BUILD / name for name in ("source", "objects", "prefix"))
    command = [str(source / "configure"), f"--prefix={prefix}",
               "--enable-cross-compile", "--target-os=android", "--arch=aarch64",
               f"--cc={compiler}", f"--cxx={toolchain}/aarch64-linux-android30-clang++",
               *[f"--{name}={toolchain}/llvm-{name}" for name in ("nm", "ar", "ranlib", "strip")],
               "--enable-static", "--disable-shared", "--enable-pic", "--extra-cflags=-O2",
               "--disable-autodetect", "--disable-programs", "--disable-doc", "--disable-network",
               "--disable-avdevice", "--disable-avfilter", "--enable-swresample", "--disable-everything",
               "--enable-protocol=file", "--enable-demuxer=matroska,mov",
               "--enable-decoder=vp9,vp8,vorbis,opus,h264,aac",
               "--enable-parser=vp9,vp8,vorbis,opus,h264,aac"]
    compiler_version = subprocess.run([str(compiler), "--version"], check=True,
                                      capture_output=True, text=True).stdout.strip()
    identity = {"archive_sha256": ARCHIVE_SHA, "configure_command": command,
                "compiler": compiler_version, "ndk_properties": ndk_properties,
                "target": "aarch64-linux-android30"}
    if not args.build:
        print("Verified cached FFmpeg archive and NDK. Use --build for isolated offline cross-build.")
        return
    BUILD.mkdir(parents=True, exist_ok=True)
    manifest = BUILD / "build.json"
    # Never accept an earlier success marker while an attempted rebuild is incomplete.
    manifest.unlink(missing_ok=True)
    if not source.exists():
        with tarfile.open(ARCHIVE) as archive:
            # Archive hash is pinned; still reject links/traversal through Python's data filter.
            extracted = BUILD / "unpack"
            extracted.mkdir(exist_ok=True)
            archive.extractall(extracted, filter="data")
        (extracted / "ffmpeg-9.0.2").rename(source)
        extracted.rmdir()
    objects.mkdir(exist_ok=True)
    request = BUILD / "configure.json"
    configured = objects / "ffbuild/config.mak"
    if configured.exists() and (not request.exists() or json.loads(request.read_text()) != identity):
        raise RuntimeError("Existing isolated configuration differs; use a fresh build/letsgo-ffmpeg directory")
    with (BUILD / "build.log").open("a") as log:
        def run(argv):
            log.write("\n$ " + shlex.join([str(x) for x in argv]) + "\n")
            log.flush()
            subprocess.run([str(x) for x in argv], cwd=objects, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        if not configured.exists():
            run(command)
            write_json(request, identity)
        components = (objects / "config_components.h").read_text()
        config = (objects / "config.h").read_text()
        required = ("OPUS", "VP9", "VP8", "VORBIS", "H264", "AAC")
        for codec in required:
            if f"#define CONFIG_{codec}_DECODER 1" not in components:
                raise RuntimeError(f"Requested decoder {codec} was silently disabled; see build.log")
        for flag in ("CONFIG_SWRESAMPLE 1", "CONFIG_GPL 0", "CONFIG_NONFREE 0", "CONFIG_LIBOPUS 0"):
            if f"#define {flag}" not in config:
                raise RuntimeError(f"Unexpected FFmpeg configuration: {flag}")
        run(["make", f"-j{args.jobs}"])
        run(["make", "install"])
    nm = toolchain / "llvm-nm"
    defined = subprocess.run([str(nm), "--defined-only", str(prefix / "lib/libavcodec.a")],
                             capture_output=True, text=True, check=True).stdout
    undefined = subprocess.run([str(nm), "--undefined-only", str(prefix / "lib/libavcodec.a")],
                               capture_output=True, text=True, check=True).stdout
    codec_list = (objects / "libavcodec/codec_list.c").read_text()
    if "ff_opus_decoder" not in codec_list or "ff_libopus_decoder" in codec_list:
        raise RuntimeError("Native Opus decoder is absent from the generated registration list")
    if not any(line.split()[-2:] == ["D", "ff_opus_decoder"] for line in defined.splitlines()):
        raise RuntimeError("Native Opus decoder has no definition in libavcodec.a")
    if not any(line.split()[-2:] == ["U", "ff_opus_decoder"] for line in undefined.splitlines()):
        raise RuntimeError("Opus decoder registration object has no archive reference")
    libraries = {f"lib{name}.a": sha(prefix / f"lib/lib{name}.a") for name in LIBRARIES}
    result = dict(identity, archive=str(ARCHIVE), prefix=str(prefix), jobs=args.jobs,
                  libraries=libraries, configuration_sha256=sha(objects / "config.h"),
                  components_sha256=sha(objects / "config_components.h"),
                  codec_list_sha256=sha(objects / "libavcodec/codec_list.c"),
                  enabled_components={"decoders": [x.lower() for x in required], "swresample": True,
                                      "external_libopus": False, "gpl": False, "nonfree": False},
                  validation={"native_opus_definition": True, "native_opus_registration_reference": True,
                              "target_decoder_executed": False, "device_executed": False})
    write_json(manifest, result)
    print("Built and statically validated native Opus + swresample; provenance: build/letsgo-ffmpeg/build.json")


if __name__ == "__main__":
    main()
