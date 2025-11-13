#!/usr/bin/env bash
set -euo pipefail

# Generate HTML coverage for the unit tests.
# Optional env vars:
#   BUILD_DIR  - build directory (defaults to <repo>/build/coverage)
#   BUILD_TYPE - CMake build type (default Debug)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/coverage}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
RESULT_DIR="${REPO_ROOT}/tests/results"

printf '[coverage] configuring build with instrumentation...\n'
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DEMLOG_BUILD_TESTS=ON \
      -DEMLOG_ENABLE_COVERAGE=ON

printf '[coverage] cleaning previous .gcda data...\n'
find "${BUILD_DIR}" -name "*.gcda" -delete 2>/dev/null || true

printf '[coverage] building unit tests...\n'
cmake --build "${BUILD_DIR}" --target emlog_unit_tests

printf '[coverage] running unit tests with instrumentation...\n'
ctest --test-dir "${BUILD_DIR}" -R emlog_unit -V

mkdir -p "${RESULT_DIR}"

if command -v gcovr >/dev/null 2>&1; then
    printf '[coverage] generating HTML via gcovr...\n'
    gcovr -r "${REPO_ROOT}" \
          --object-directory "${BUILD_DIR}" \
          --exclude 'tests/' \
          --html --html-details \
          -o "${RESULT_DIR}/UT_coverage.html"
    gcovr -r "${REPO_ROOT}" \
          --object-directory "${BUILD_DIR}" \
          --exclude 'tests/' \
          --xml \
          -o "${RESULT_DIR}/UT_coverage.xml"
    printf '[coverage] report ready: %s/UT_coverage.html\n' "${RESULT_DIR}"
elif command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then
    printf '[coverage] generating HTML via lcov/genhtml...\n'
    INFO_FILE="${RESULT_DIR}/UT_coverage.info"
    lcov --capture --directory "${BUILD_DIR}" --output-file "${INFO_FILE}"
    lcov --remove "${INFO_FILE}" '/usr/*' 'tests/*' --output-file "${INFO_FILE}"
    genhtml "${INFO_FILE}" --output-directory "${RESULT_DIR}/UT_coverage_html"
    printf '[coverage] report ready: %s/UT_coverage_html/index.html\n' "${RESULT_DIR}"
else
    echo "[coverage] gcovr or lcov/genhtml not found. Please install one of them."
    exit 1
fi
