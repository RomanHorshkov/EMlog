#!/usr/bin/env bash
# Build and run the private UTs, public UTs, and integration suite under ASan, UBSan, and LSan.
set -euo pipefail

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/sanitizers"
cd -- "${ROOT_DIR}"

# shellcheck source=/dev/null
source "${SCRIPT_DIR}/gcc_build_profiles.sh"

mkdir -p "${BUILD_DIR}"

SANITIZE_CPPFLAGS=("${CPPFLAGS_SANITIZE[@]}" -Isrc)
SANITIZE_CFLAGS=("${CFLAGS_SANITIZE[@]}")
SANITIZE_LDFLAGS=("${LDFLAGS_SANITIZE[@]}")

run_sanitized() {
    env ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" LSAN_OPTIONS="exitcode=1" "$@"
}

# Private UTs are white-box (test_private.c #includes emlog.c directly) — no separate
# library object to build.
gcc "${SANITIZE_CPPFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" -c tests/UTs/privateAPI/test_private.c \
    -o "${BUILD_DIR}/test_private.o"
gcc "${BUILD_DIR}/test_private.o" -o "${BUILD_DIR}/ut_private_sanitized" "${SANITIZE_LDFLAGS[@]}" -lcmocka -pthread

# Public UTs are black-box: build a sanitized emlog.o, link against it.
gcc "${SANITIZE_CPPFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" -c src/emlog.c -o "${BUILD_DIR}/emlog_pub.o"
PUB_OBJECTS=("${BUILD_DIR}/emlog_pub.o")
for src in tests/UTs/publicAPI/*.c; do
    out="${BUILD_DIR}/pub_$(basename "${src%.c}").o"
    gcc "${SANITIZE_CPPFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" -Itests/UTs/publicAPI -c "${src}" -o "${out}"
    PUB_OBJECTS+=("${out}")
done
gcc "${PUB_OBJECTS[@]}" -o "${BUILD_DIR}/ut_public_sanitized" "${SANITIZE_LDFLAGS[@]}" -lcmocka -pthread

# Integration test: real multi-threaded concurrency against a sanitized emlog.o.
gcc "${SANITIZE_CPPFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" -D_GNU_SOURCE -c src/emlog.c -o "${BUILD_DIR}/emlog_it.o"
gcc "${SANITIZE_CPPFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" -D_GNU_SOURCE -c tests/ITs/integration_test.c \
    -o "${BUILD_DIR}/integration_test.o"
gcc "${BUILD_DIR}/emlog_it.o" "${BUILD_DIR}/integration_test.o" \
    -o "${BUILD_DIR}/it_sanitized" "${SANITIZE_LDFLAGS[@]}" -pthread

run_sanitized "${BUILD_DIR}/ut_private_sanitized"
run_sanitized "${BUILD_DIR}/ut_public_sanitized"
run_sanitized "${BUILD_DIR}/it_sanitized"
