#!/usr/bin/env bash
# Tests for tools/setup.sh --check. Uses only synthetic fake tools.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SETUP="$HERE/../tools/setup.sh"
BASH_BIN="$(command -v bash)"
fail=0
ok()  { echo "ok   - $1"; }
bad() { echo "FAIL - $1"; fail=1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
empty="$tmp/empty"; mkdir -p "$empty"

# 1. Nothing on PATH: must exit non-zero and report MISSING for each core tool.
out="$(env -i PATH="$empty" HOME="$tmp/home" "$BASH_BIN" "$SETUP" --check 2>&1)"; rc=$?
[ "$rc" -ne 0 ] && ok "empty PATH exits non-zero" || bad "empty PATH exits non-zero (rc=$rc)"
for t in java cmake ninja python3 git sdkmanager ndk; do
  echo "$out" | grep -q "^MISSING $t" && ok "reports MISSING $t" || bad "reports MISSING $t"
done

# 2. --check never installs or writes anything.
[ ! -e "$tmp/home" ] && ok "--check created nothing in HOME" || bad "--check created files in HOME"

# 3. Fake tools present -> reported OK (not MISSING) for those tools.
fake="$tmp/fake"; mkdir -p "$fake"
mk() { printf '#!/bin/sh\n%s\n' "$2" > "$fake/$1"; chmod +x "$fake/$1"; }
mk java    'echo "openjdk version \"17.0.9\"" >&2'
mk cmake   'echo "cmake version 3.31.6"'
mk ninja   'echo 1.11.1'
mk python3 'echo "Python 3.12.1"'
mk git     'echo "git version 2.43.0"'
out="$(env -i PATH="$fake" HOME="$tmp/home" "$BASH_BIN" "$SETUP" --check 2>&1)"; rc=$?
for t in java cmake ninja python3 git; do
  echo "$out" | grep -q "^OK $t" && ok "reports OK $t" || bad "reports OK $t"
done
echo "$out" | grep -q "^MISSING ndk" && ok "still MISSING ndk" || bad "still MISSING ndk"
[ "$rc" -ne 0 ] && ok "partial toolchain still non-zero" || bad "partial toolchain still non-zero"

# 4. Wrong JDK version is flagged, not accepted silently.
mk java 'echo "openjdk version \"11.0.2\"" >&2'
out="$(env -i PATH="$fake" HOME="$tmp/home" "$BASH_BIN" "$SETUP" --check 2>&1)"
echo "$out" | grep -q "^WRONG-VERSION java" && ok "flags JDK 11" || bad "flags JDK 11"

# 5. Unknown flag is rejected.
env -i PATH="$empty" "$BASH_BIN" "$SETUP" --bogus >/dev/null 2>&1
[ $? -eq 2 ] && ok "unknown flag exits 2" || bad "unknown flag exits 2"

exit $fail
