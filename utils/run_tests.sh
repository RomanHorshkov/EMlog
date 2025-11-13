#!/usr/bin/env bash
set -euo pipefail

# Run all registered CTest suites (unit + integration by default).
# Optional env vars:
#   BUILD_DIR - build directory (defaults to <repo>/build)
# Extra args are forwarded to ctest (e.g., -R emlog_unit -V)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DEMLOG_BUILD_TESTS=ON >/dev/null

ctest --test-dir "${BUILD_DIR}" "$@"
