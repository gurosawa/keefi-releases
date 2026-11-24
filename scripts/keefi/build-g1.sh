#!/bin/sh
set -eu

# Compatibility entrypoint for the isolated native target. G2 uses build-g2.sh.
# No installers, API calls, login, or deployment during build/tests.
: "${KEEFI_QT_PREFIX:?Set the Qt 6.8.3 kit prefix}"
: "${KEEFI_SODIUM_PREFIX:?Set the libsodium 1.0.22 prefix}"
KEEFI_CMAKE=${KEEFI_CMAKE:-cmake}
KEEFI_NINJA=${KEEFI_NINJA:-ninja}
KEEFI_REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
KEEFI_BUILD=${KEEFI_BUILD:-"$KEEFI_REPO/fincept-qt/build-keefi-g1"}

"$KEEFI_CMAKE" -S "$KEEFI_REPO/fincept-qt" -B "$KEEFI_BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$KEEFI_NINJA" -DCMAKE_PREFIX_PATH="$KEEFI_QT_PREFIX" \
  -DKEEFI_SODIUM_ROOT="$KEEFI_SODIUM_PREFIX" -DKEEFI_G1_MODE=ON \
  -DKEEFI_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
"$KEEFI_CMAKE" --build "$KEEFI_BUILD" --parallel 4
"$KEEFI_CMAKE" --build "$KEEFI_BUILD" --target test
