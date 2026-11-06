#!/usr/bin/env bash
# AC-NativeNX toolchain check / setup.
#
#   tools/setup.sh --check     report only; installs and writes nothing
#   tools/setup.sh             check, then install the Android SDK pieces that can be
#                              installed safely (needs sdkmanager). System tools (JDK,
#                              CMake, ...) are never installed silently: you get the
#                              exact command for your OS instead.
#   --accept-android-licenses  required before sdkmanager may install anything
#
# One line per tool:  OK <tool> <detail> | MISSING <tool> <hint> | WRONG-VERSION <tool> <detail>
# Exit 0 only if every required tool is OK. Versions follow engine/BUILDING.md.
set -u

JDK_MAJOR=17
CMAKE_VERSION=3.31.6
NDK_VERSION=27.2.12479018
PLATFORM=android-36
BUILD_TOOLS=36.0.0

MODE=install; ACCEPT=0
for a in "$@"; do
  case "$a" in
    --check) MODE=check ;;
    --accept-android-licenses) ACCEPT=1 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $a (use --check, --accept-android-licenses)" >&2; exit 2 ;;
  esac
done

# Bash builtins only: this script must work (and report) even with an almost empty PATH.
case "$OSTYPE" in
  darwin*) OS=macos ;;
  linux*)  if [ -r /proc/version ] && read -r _ver < /proc/version && [[ "${_ver,,}" == *microsoft* ]]; then OS=wsl; else OS=linux; fi ;;
  msys*|cygwin*|win32*) OS=windows ;;
  *) OS=unknown ;;
esac

hint() { # hint <tool>
  case "$OS:$1" in
    macos:java)    echo "brew install --cask temurin@17" ;;
    macos:cmake)   echo "brew install cmake" ;;
    macos:ninja)   echo "brew install ninja" ;;
    macos:python3) echo "brew install python@3.12" ;;
    macos:git)     echo "xcode-select --install" ;;
    linux:java|wsl:java)       echo "sudo apt install openjdk-17-jdk  (or your distro's JDK 17)" ;;
    linux:cmake|wsl:cmake)     echo "sudo apt install cmake" ;;
    linux:ninja|wsl:ninja)     echo "sudo apt install ninja-build" ;;
    linux:python3|wsl:python3) echo "sudo apt install python3" ;;
    linux:git|wsl:git)         echo "sudo apt install git" ;;
    windows:*) echo "native Windows is not supported: install WSL2 (wsl --install) and run this script inside it" ;;
    *:sdkmanager) echo "install Android command-line tools from https://developer.android.com/studio#command-line-tools-only into \$ANDROID_SDK_ROOT/cmdline-tools/latest" ;;
    *:ndk)         echo "sdkmanager \"ndk;$NDK_VERSION\" (run this script without --check and with --accept-android-licenses)" ;;
    *) echo "install it with your package manager" ;;
  esac
}

FAILED=0
ok()      { echo "OK $1 $2"; }
missing() { echo "MISSING $1 -- $(hint "$1")"; FAILED=1; }
wrong()   { echo "WRONG-VERSION $1 $2 -- $(hint "$1")"; FAILED=1; }
have()    { command -v "$1" >/dev/null 2>&1; }

[ "$OS" = windows ] && { echo "MISSING wsl -- $(hint java)"; exit 1; }

# --- system tools ---
if have java; then
  jv="$(java -version 2>&1)"; v=""
  [[ "$jv" =~ version\ \"([0-9]+) ]] && v="${BASH_REMATCH[1]}"
  [ "$v" = "$JDK_MAJOR" ] && ok java "JDK $v" || wrong java "found JDK ${v:-none (a stub that is not a real JDK?)}, need $JDK_MAJOR"
else missing java; fi

if have cmake; then
  cv="$(cmake --version 2>/dev/null)"; v=""
  [[ "$cv" =~ version\ ([0-9.]+) ]] && v="${BASH_REMATCH[1]}"
  ok cmake "${v:-unknown} (SDK-pinned $CMAKE_VERSION is installed separately for Android builds)"
else missing cmake; fi

have ninja && ok ninja "$(ninja --version 2>/dev/null)" || missing ninja

if have python3; then
  v="$(python3 --version 2>&1)"; v="${v#Python }"
  maj="${v%%.*}"; rest="${v#*.}"; min="${rest%%.*}"
  if [ "${maj:-0}" -gt 3 ] 2>/dev/null || { [ "${maj:-0}" -eq 3 ] && [ "${min:-0}" -ge 11 ]; } 2>/dev/null; then
    ok python3 "$v"
  else wrong python3 "found $v, need 3.11+"; fi
else missing python3; fi

have git && ok git "$(git --version 2>/dev/null)" || missing git

# --- Android SDK / NDK ---
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/android-sdk}}"
SDKM=""
if have sdkmanager; then SDKM="$(command -v sdkmanager)"
elif [ -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then SDKM="$SDK/cmdline-tools/latest/bin/sdkmanager"; fi
[ -n "$SDKM" ] && ok sdkmanager "$SDKM" || missing sdkmanager

if [ -d "$SDK/ndk/$NDK_VERSION" ]; then ok ndk "r27c ($NDK_VERSION) in $SDK"; else missing ndk; fi

# --- install step (SDK pieces only) ---
if [ "$MODE" = install ] && [ -n "$SDKM" ]; then
  if [ "$ACCEPT" -ne 1 ]; then
    echo "NOTE: Android SDK packages are licensed by Google. Re-run with --accept-android-licenses to install them."
  else
    echo "Installing Android SDK packages into $SDK ..."
    if yes | "$SDKM" --sdk_root="$SDK" "platforms;$PLATFORM" "build-tools;$BUILD_TOOLS" \
         "cmake;$CMAKE_VERSION" "ndk;$NDK_VERSION" "platform-tools"; then
      echo "--- re-checking after install ---"
      exec "$0" --check
    else
      echo "FAILED sdkmanager install -- nothing was marked as installed; see output above"
      FAILED=1
    fi
  fi
fi

if [ "$FAILED" -eq 0 ]; then echo "READY: toolchain complete."; else echo "NOT READY: fix the MISSING / WRONG-VERSION lines above."; fi
exit "$FAILED"
