#!/usr/bin/env bash
set -euo pipefail

# SET THE SCRIPT TO GO INTO DESIRED FOLDER AND COME BACK FROM WHERE LAUNCHED.
# Save where launched the script from
START_DIR="$(pwd -P)"
# Always come back, even if something fails
cleanup() { cd -- "$START_DIR"; }
trap cleanup EXIT

# Set the project root (works regardless of where the script is launched from)
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "$ROOT_DIR"

# Build the UTs private object file
BUILD_DIR="${ROOT_DIR}/build/UTs"

mkdir -p "$BUILD_DIR"

# Clean previous coverage data (otherwise gcov/gcovr can fail with stamp mismatches).
rm -f "${BUILD_DIR}"/*.gcda "${BUILD_DIR}"/*.gcno "${BUILD_DIR}"/*.gcov 2>/dev/null || true

# build without optimization and debug options and coverage option
gcc -std=c11 -O0 -g --coverage -I. -Iapp -c tests/UTs/privateAPI/test_private.c -o "${BUILD_DIR}/test_private.o"
gcc --coverage "${BUILD_DIR}/test_private.o" -o "${BUILD_DIR}/ut_private" -lcmocka -pthread

# generate coverage
# check gcovr presence
if ! command -v gcovr >/dev/null 2>&1; then
    echo "gcovr not found."
    exit 1
fi

RESULT_DIR="$ROOT_DIR/tests/results/private_UTs"

mkdir -p "$RESULT_DIR"

# Run tests to generate fresh .gcda files.
"${BUILD_DIR}/ut_private"

gcovr -r "${ROOT_DIR}" \
    --object-directory "${BUILD_DIR}" \
    --exclude 'tests/' \
    --html --html-details \
    -o "${RESULT_DIR}/UTs_private_coverage.html"
gcovr -r "${ROOT_DIR}" \
    --object-directory "${BUILD_DIR}" \
    --exclude 'tests/' \
    --xml \
    -o "${RESULT_DIR}/UTs_private_coverage.xml"
gcovr -r "${ROOT_DIR}" \
    --object-directory "${BUILD_DIR}" \
    --exclude 'tests/' \
    --json-summary \
    -o "${RESULT_DIR}/coverage-summary.json"
