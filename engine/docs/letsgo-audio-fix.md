# Let's Go Android native Opus dependency

The first manual Eevee route stopped after controller selection because `runtime/services/hwopus.c:opus_open()` could not find `AV_CODEC_ID_OPUS`. Private evidence: `evidence/letsgo-launch/20261024T235603.897028Z/native.log`. This identifies a missing decoder, not an audio-device or guest-code failure.

The pinned LAS Android dependency recipe requests `--enable-decoder=opus` but also passes `--disable-swresample`. FFmpeg 9.0.2 `configure` declares `opus_decoder_deps="swresample"`; the existing cache consequently has `CONFIG_OPUS_DECODER 0`. The native decoder includes `libswresample/swresample.h`. External `libopus` is a separate optional implementation and is unnecessary for this fix.

`tools/build_letsgo_ffmpeg.py` uses the already cached `ffmpeg-9.0.2.tar.xz`, SHA-256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`, as pinned by `sources/las-android/tools/android/build_deps.sh`. It extracts/builds exclusively under ignored `build/letsgo-ffmpeg/`; it neither downloads nor replaces the working LAS cache.

```sh
python3 tools/build_letsgo_ffmpeg.py --build --jobs 4 \
  --ndk "$HOME/android-sdk/ndk/27.2.12479018"
```

Without `--build`, only the cached archive and pinned toolchain are checked. Builds are resumable with unchanged configuration; logs append to `build/letsgo-ffmpeg/build.log`, and `build.json` is published atomically only after validation. Jobs are bounded to four. A changed toolchain/configuration requires a fresh isolated output directory.

Configuration retains the LAS recipe's Android ARM64/API30, static PIC/O2, disabled autodetection/programs/docs/network, disabled avdevice/avfilter, file protocol, Matroska/MOV demuxers, VP9/VP8/Vorbis/Opus/H264/AAC decoders and parsers. The deliberate correction is `--enable-swresample` in place of `--disable-swresample`. GPL, nonfree and external libopus must remain disabled; no new external dependency is enabled.

Integration uses `build/letsgo-ffmpeg/prefix/include` and, in dependency order, `libavformat.a`, `libavcodec.a`, `libswscale.a`, `libswresample.a`, `libavutil.a` from its `lib` directory. Preserve the Android native target's existing system libraries. Shaderc, Vulkan, SDL and the LAS baseline remain on their existing cache. Rebuild/repackage the native runtime after switching all FFmpeg headers and archives together; generated game-code identity is unchanged by this audio-only dependency fix. The runtime/APK dependency hashes must change.

The manifest records the archive hash, complete configure argv, NDK properties/compiler identity, target, configuration and generated codec-list hashes, five installed archive hashes, enabled decoder list and static validation results. Verification requires `CONFIG_OPUS_DECODER=1`, `CONFIG_SWRESAMPLE=1`, the native `ff_opus_decoder` definition and registration reference in `libavcodec.a`, and its entry in generated `codec_list.c`. These checks establish decoder inclusion; they do not execute target decoding or prove audible output. Runtime validation must subsequently show `[hwopus] decoder opened` after the controller page, continued guest execution, and owner-confirmed sound.

Verified 2026-10-25: the isolated NDK r27c/API30 cross-build completed with four jobs. All six requested decoders and swresample are enabled; GPL/nonfree/libopus are disabled. Native Opus definition, registration reference and codec-list checks passed, and all five installed archives are hashed in `build/letsgo-ffmpeg/build.json`. No target decoder or device execution was performed; native runtime integration and the audio route remain the coordinator's responsibility.

Coordinator follow-up: the complete runtime relinked and the replacement APK
passed signature verification. ELF inspection confirms `ff_opus_decoder` is in
the actual linked runtime; the existing private title library still matches code
identity `4a34b9127ec182c1`. After the owner's explicit update/reopen instruction,
the APK installed into the same isolated test app and reached readable controller
selection at30fps, with all four guest tables loaded and no startup fatal. This
does not exercise Opus until the owner continues. Decoder-open logs, audible
output and the reported blinking remain pending. Manual-session protection is
active again; no game input or automatic stop was issued.

During the later owner-authorized [blinking diagnostics](letsgo-presentation-fix.md),
native Opus decoders opened and the VP9/Vorbis title movie advanced without the
earlier fatal. This verifies decoder availability on the device. Audible quality
still needs owner confirmation. A finite controller-confirmation script was used
for that diagnostic and removed before restoring normal play.
