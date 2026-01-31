#!/usr/bin/env bash
set -euo pipefail

# Build + run both private and public unit tests, then generate a combined
# coverage report (HTML/XML/JSON summary) using gcovr.
#
# Prereqs:
#   - gcovr installed
#   - libcmocka installed
#
# Outputs:
#   tests/results/UTs_all/UTs_all_coverage.html
#   tests/results/UTs_all/UTs_all_coverage.xml
#   tests/results/UTs_all/coverage-summary.json

START_DIR="$(pwd -P)"
cleanup() { cd -- "$START_DIR"; }
trap cleanup EXIT

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "$ROOT_DIR"

if ! command -v gcovr >/dev/null 2>&1; then
  echo "gcovr not found."
  exit 1
fi

printf '[UTs] running private UTs...\n'
"${ROOT_DIR}/utils/make_UTs_priv.sh"

printf '[UTs] running public UTs...\n'
"${ROOT_DIR}/utils/make_UTs_pub.sh"

RESULT_DIR="${ROOT_DIR}/tests/results/UTs_all"
mkdir -p "${RESULT_DIR}"

printf '[coverage] generating combined reports via gcovr...\n'
gcovr -r "${ROOT_DIR}" \
  --object-directory "${ROOT_DIR}/build/UTs" \
  --object-directory "${ROOT_DIR}/build/UTs_public" \
  --exclude 'tests/' \
  --html --html-details \
  -o "${RESULT_DIR}/UTs_all_coverage.html"

gcovr -r "${ROOT_DIR}" \
  --object-directory "${ROOT_DIR}/build/UTs" \
  --object-directory "${ROOT_DIR}/build/UTs_public" \
  --exclude 'tests/' \
  --xml \
  -o "${RESULT_DIR}/UTs_all_coverage.xml"

gcovr -r "${ROOT_DIR}" \
  --object-directory "${ROOT_DIR}/build/UTs" \
  --object-directory "${ROOT_DIR}/build/UTs_public" \
  --exclude 'tests/' \
  --json-summary \
  -o "${RESULT_DIR}/coverage-summary.json"

printf '[coverage] report ready: %s\n' "${RESULT_DIR}/UTs_all_coverage.html"

