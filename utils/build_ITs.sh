#!/usr/bin/env bash
# =============================================================================
# build_ITs.sh — build + run the multithreaded integration test (release profile)
#
# author  Roman Horshkov <github.com/RomanHorshkov>
# date    2026
# (c) 2026
# =============================================================================
#
# Usage:
#   ./utils/build_ITs.sh                # build + run
#   ./utils/build_ITs.sh --build-only   # just compile the binary
#   ./utils/build_ITs.sh --run-only     # run the already-built binary
#
# Flags come from the shared catalog's release profile (-O2, hardening); -g is
# appended so a failing run still produces usable backtraces.
#
# Outputs:
#   build/ITs/integration_test
#   tests/results/ITs/integration_result.txt
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
    IT_CPPFLAGS=("${CPPFLAGS_RELEASE[@]}" -D_GNU_SOURCE -Iapp)
    IT_CFLAGS=("${CFLAGS_RELEASE[@]}" -g)
    IT_LDFLAGS=("${LDFLAGS_RELEASE[@]}")

    gcc "${IT_CPPFLAGS[@]}" "${IT_CFLAGS[@]}" -c app/emlog.c -o "${BUILD_DIR}/emlog.o"
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

# The test binary writes tests/results/ITs/integration_result.txt itself.
"${BUILD_DIR}/integration_test"
