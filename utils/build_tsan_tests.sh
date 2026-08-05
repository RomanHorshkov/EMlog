#!/usr/bin/env bash
# Build and run the multi-threaded integration test under ThreadSanitizer.
set -euo pipefail

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/tsan"
cd -- "${ROOT_DIR}"

# shellcheck source=/dev/null
source "${SCRIPT_DIR}/gcc_build_profiles.sh"
mkdir -p "${BUILD_DIR}"

TSAN_CPPFLAGS=("${CPPFLAGS_TSAN[@]}" -D_GNU_SOURCE -Iapp)
TSAN_CFLAGS=("${CFLAGS_TSAN[@]}" -fno-pie)
TSAN_LDFLAGS=("${LDFLAGS_TSAN[@]}" -no-pie)

gcc "${TSAN_CPPFLAGS[@]}" "${TSAN_CFLAGS[@]}" -c app/emlog.c -o "${BUILD_DIR}/emlog_it.o"
gcc "${TSAN_CPPFLAGS[@]}" "${TSAN_CFLAGS[@]}" -c tests/ITs/integration_test.c -o "${BUILD_DIR}/integration_test.o"
gcc "${BUILD_DIR}/emlog_it.o" "${BUILD_DIR}/integration_test.o" \
    -o "${BUILD_DIR}/it_tsan" "${TSAN_LDFLAGS[@]}" -pthread

run_tsan() {
    timeout --signal=TERM 300 env TSAN_OPTIONS="halt_on_error=1:history_size=7" "$@"
}

run_tsan "${BUILD_DIR}/it_tsan"
printf 'ThreadSanitizer integration gate passed\n'
