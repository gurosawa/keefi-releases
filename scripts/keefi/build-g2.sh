#!/bin/sh
set -eu
# Native local KEEFI target. Tests use fake keys and fixed responses only.
KEEFI_REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export KEEFI_BUILD=${KEEFI_BUILD:-"$KEEFI_REPO/fincept-qt/build-keefi-g2"}
exec /bin/sh "$KEEFI_REPO/scripts/keefi/build-g1.sh"
