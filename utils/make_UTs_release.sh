#!/usr/bin/env bash
set -euo pipefail

# SET THE SCRIPT TO GO INTO DESIRED FOLDER AND COME BACK FROM WHERE LAUNCHED.
START_DIR="$(pwd -P)"
cleanup() { cd -- "$START_DIR"; }
trap cleanup EXIT

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "$ROOT_DIR"

PRIV_DIR="${ROOT_DIR}/build/UTs_release/private"
PUB_DIR="${ROOT_DIR}/build/UTs_release/public"
mkdir -p "$PRIV_DIR" "$PUB_DIR"

# Build release libraries first (flat build/libemlog.a is a symlink into
# build/release/ maintained by build_libs.sh).
"${ROOT_DIR}/utils/build_libs.sh" release

# Private UTs are white-box (test_private.c #includes emlog.c directly), so they compile
# and link standalone — no separate library object needed.
gcc -std=c11 -O2 -D_GNU_SOURCE -Iapp -c tests/UTs/privateAPI/test_private.c \
    -o "${PRIV_DIR}/test_private.o"
gcc "${PRIV_DIR}/test_private.o" -o "${PRIV_DIR}/ut_private_release" -lcmocka -pthread

# Public UTs link against the release static library (black-box, public API only).
UT_CFLAGS=(-std=c11 -O2 -D_GNU_SOURCE -Iapp -Itests/UTs/publicAPI)
for src in tests/UTs/publicAPI/*.c; do
  out="${PUB_DIR}/$(basename "${src%.c}").o"
  gcc "${UT_CFLAGS[@]}" -c "$src" -o "$out"
done
gcc "${PUB_DIR}"/*.o -o "${PUB_DIR}/ut_public_release" build/libemlog.a -lcmocka -pthread

# Run both.
"${PRIV_DIR}/ut_private_release"
"${PUB_DIR}/ut_public_release"
