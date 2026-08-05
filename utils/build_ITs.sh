#!/usr/bin/env bash
# =============================================================================
# build_ITs.sh — build + run the integration test with its OWN gcovr coverage
#
# author  Roman Horshkov <github.com/RomanHorshkov>
# date    2026
# (c) 2026
# =============================================================================
#
# Usage:
#   ./utils/build_ITs.sh                # build + run + coverage
#   ./utils/build_ITs.sh --build-only   # just compile the coverage binary
#   ./utils/build_ITs.sh --run-only     # run + regenerate coverage from an
#                                        # already-built binary
#
# Mirrors build_UTs.sh: debug profile + the composable coverage layer, -O0
# appended after the profile's -Og for exact gcov line/branch attribution.
# This is a SEPARATE coverage report from the UT suites — private, public,
# and integration coverage all stay independent files; nothing here feeds
# into tests/results/UTs_all/.
#
# For the real -O2 + hardening correctness gate, see build_ITs_release.sh.
#
# Outputs:
#   build/ITs/integration_test
#   tests/results/ITs/integration_result.txt   (written by the test binary)
#   tests/results/ITs/coverage-summary.json
#   tests/results/ITs/ITs_coverage.{html,xml}
# =============================================================================
set -euo pipefail

MODE="all"
case "${1:-}" in
    "") ;;
    --build-only) MODE="build" ;;
    --run-only)   MODE="run" ;;
    *)
        printf 'usage: %s [--build-only|--run-only]\n' "$0" >&2
        exit 2
        ;;
esac

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

# shellcheck source=/dev/null
source "${SCRIPT_DIR}/gcc_build_profiles.sh"

BUILD_DIR="${ROOT_DIR}/build/ITs"
RESULT_DIR="${ROOT_DIR}/tests/results/ITs"
mkdir -p "${BUILD_DIR}" "${RESULT_DIR}"

if [[ "${MODE}" != "run" ]]; then
    # Clean previous coverage data — stale .gcno makes gcov/gcovr fail with
    # stamp mismatches.
    rm -f "${BUILD_DIR}"/*.gcda "${BUILD_DIR}"/*.gcno "${BUILD_DIR}"/*.gcov 2>/dev/null || true

    IT_CPPFLAGS=("${CPPFLAGS_DEBUG[@]}" -D_GNU_SOURCE -Isrc)
    IT_CFLAGS=("${CFLAGS_DEBUG[@]}" -O0 "${CFLAGS_INSTRUMENT_COVERAGE[@]}")
    IT_LDFLAGS=("${LDFLAGS_DEBUG[@]}" "${LDFLAGS_INSTRUMENT_COVERAGE[@]}")

    gcc "${IT_CPPFLAGS[@]}" "${IT_CFLAGS[@]}" -c src/emlog.c -o "${BUILD_DIR}/emlog.o"
    gcc "${IT_CPPFLAGS[@]}" "${IT_CFLAGS[@]}" -c tests/ITs/integration_test.c \
        -o "${BUILD_DIR}/integration_test.o"
    gcc "${IT_LDFLAGS[@]}" "${BUILD_DIR}/emlog.o" "${BUILD_DIR}/integration_test.o" \
        -o "${BUILD_DIR}/integration_test" -pthread
fi

if [[ "${MODE}" == "build" ]]; then
    printf '[ITs] build-only: binary ready: %s\n' "${BUILD_DIR}/integration_test"
    exit 0
fi

if [[ ! -x "${BUILD_DIR}/integration_test" ]]; then
    printf 'missing test binary: %s (build first: %s --build-only)\n' \
        "${BUILD_DIR}/integration_test" "$0" >&2
    exit 1
fi

rm -f "${BUILD_DIR}"/*.gcda 2>/dev/null || true

# The test binary writes tests/results/ITs/integration_result.txt itself.
"${BUILD_DIR}/integration_test"

if ! command -v gcovr >/dev/null 2>&1; then
    echo "gcovr not found."
    exit 1
fi

printf '[coverage] integration-test report...\n'
gcovr -r "${ROOT_DIR}" \
    --exclude 'tests/' \
    --json-summary -o "${RESULT_DIR}/coverage-summary.json" \
    "${BUILD_DIR}"
gcovr -r "${ROOT_DIR}" \
    --exclude 'tests/' \
    --html --html-details -o "${RESULT_DIR}/ITs_coverage.html" \
    "${BUILD_DIR}"
gcovr -r "${ROOT_DIR}" \
    --exclude 'tests/' \
    --xml -o "${RESULT_DIR}/ITs_coverage.xml" \
    "${BUILD_DIR}"

printf '[coverage] report ready: %s\n' "${RESULT_DIR}/ITs_coverage.html"
