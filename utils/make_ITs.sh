#!/usr/bin/env bash
set -euo pipefail

# Build + run the integration test (Release-ish: -O2, with symbols: -g).
#
# Outputs:
#   build/ITs/integration_test
#   tests/results/ITs/integration_result.txt

START_DIR="$(pwd -P)"
cleanup() { cd -- "$START_DIR"; }
trap cleanup EXIT

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "$ROOT_DIR"

BUILD_DIR="${ROOT_DIR}/build/ITs"
mkdir -p "$BUILD_DIR"
mkdir -p "${ROOT_DIR}/tests/results/ITs"

CFLAGS=(
  -std=c11
  -O2
  -g
  -D_GNU_SOURCE
  -Iapp
)

gcc "${CFLAGS[@]}" -c app/emlog.c -o "${BUILD_DIR}/emlog.o"
gcc "${CFLAGS[@]}" -c tests/ITs/integration_test.c -o "${BUILD_DIR}/integration_test.o"
gcc -O2 -g "${BUILD_DIR}/emlog.o" "${BUILD_DIR}/integration_test.o" -o "${BUILD_DIR}/integration_test" -pthread

"${BUILD_DIR}/integration_test"
