#!/bin/sh
set -eu

# Build in staging only. SDK installation, /Applications replacement and
# personal profiles are deliberately outside this command.
[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = x86_64 ] || {
  printf '%s\n' 'This entrypoint is verified only for Intel macOS.' >&2
  exit 2
}
: "${KEEFI_QT_PREFIX:?Set the Qt 6.8.3 macOS kit prefix}"
: "${KEEFI_SODIUM_PREFIX:?Set the libsodium 1.0.22 prefix}"
KEEFI_CMAKE=${KEEFI_CMAKE:-cmake}
KEEFI_NINJA=${KEEFI_NINJA:-ninja}
KEEFI_PYTHON=${KEEFI_PYTHON:-python3}
KEEFI_REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
KEEFI_BUILD="$KEEFI_REPO/fincept-qt/build-keefi-g5-readonly"
KEEFI_APP="$KEEFI_BUILD/.app-staging.noindex/KEEFI Dev.app"
command -v "$KEEFI_CMAKE" >/dev/null
command -v "$KEEFI_NINJA" >/dev/null
command -v "$KEEFI_PYTHON" >/dev/null
test -x "$KEEFI_QT_PREFIX/bin/macdeployqt"
# RCC otherwise embeds checkout mtimes. The commit timestamp is stable when a
# release tag is rebuilt; an explicit release epoch can override that default.
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$KEEFI_REPO" show -s --format=%ct HEAD)}
"$KEEFI_PYTHON" -c 'import sys; v=sys.argv[1]; sys.exit(0 if v.isascii() and v.isdigit() and 0<int(v)<=253402300799 else "Invalid SOURCE_DATE_EPOCH")' "$SOURCE_DATE_EPOCH"
QT_RCC_SOURCE_DATE_OVERRIDE=$SOURCE_DATE_EPOCH
export SOURCE_DATE_EPOCH QT_RCC_SOURCE_DATE_OVERRIDE
KEEFI_SOURCE_DIGEST=$("$KEEFI_PYTHON" "$KEEFI_REPO/scripts/keefi/verification_receipt.py" --fingerprint)

"$KEEFI_CMAKE" -S "$KEEFI_REPO/fincept-qt" -B "$KEEFI_BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$KEEFI_NINJA" -DCMAKE_PREFIX_PATH="$KEEFI_QT_PREFIX" \
  -DKEEFI_SODIUM_ROOT="$KEEFI_SODIUM_PREFIX" -DKEEFI_G1_MODE=ON \
  -DKEEFI_READ_ONLY_BETA=ON -DKEEFI_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
"$KEEFI_CMAKE" --build "$KEEFI_BUILD" --target KEEFI_Dev tst_keefi_read_only --parallel 4
QT_QPA_PLATFORM=offscreen "$KEEFI_BUILD/tests/keefi/tst_keefi_read_only" \
  -o -,txt -o "$KEEFI_BUILD/readonly-results.xml",xml
"$KEEFI_QT_PREFIX/bin/macdeployqt" "$KEEFI_APP" -no-strip -always-overwrite -codesign=-
"$KEEFI_PYTHON" "$KEEFI_REPO/scripts/keefi/verify-readonly-macos.py" \
  "$KEEFI_APP" "$KEEFI_BUILD/readonly-verification.json" "$KEEFI_SOURCE_DIGEST" "$KEEFI_BUILD/readonly-results.xml"
printf 'Verified staging bundle: %s\n' "$KEEFI_APP"
printf '%s\n' 'No installed app or personal profile was changed.'
