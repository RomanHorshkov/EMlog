#!/usr/bin/env bash
# =============================================================================
# build_UTs.sh — build + run BOTH unit-test suites in one run, with coverage
#
# author  Roman Horshkov <github.com/RomanHorshkov>
# date    2026
# (c) 2026
# =============================================================================
#
# Usage:
#   ./utils/build_UTs.sh                # build both suites, run both, coverage
#   ./utils/build_UTs.sh --build-only   # just compile the two suite binaries
#   ./utils/build_UTs.sh --run-only     # run already-built binaries + coverage
#
# The two suites stay separately compiled binaries (they must — the private
# suite is white-box and #includes app/emlog.c directly, the public suite is
# black-box and links a separately compiled emlog object), but ONE script
# builds them, runs them, and produces the coverage reports:
#
#   build/UTs/private/ut_private        white-box suite
#   build/UTs/public/ut_public          black-box suite
#
#   tests/results/private_UTs/coverage-summary.json   per-suite summaries
#   tests/results/public_UTs/coverage-summary.json    (run_pipeline.sh reads these)
#   tests/results/UTs_all/UTs_all_coverage.{html,xml} combined report
#   tests/results/UTs_all/coverage-summary.json
#
# Flags come from the shared profile catalog (debug profile + the composable
# coverage layer), NOT from ad-hoc literals. -O0 is appended after the
# profile's -Og so gcov line/branch data maps 1:1 onto the source.
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

PRIV_DIR="${ROOT_DIR}/build/UTs/private"
PUB_DIR="${ROOT_DIR}/build/UTs/public"
PRIV_RESULT_DIR="${ROOT_DIR}/tests/results/private_UTs"
PUB_RESULT_DIR="${ROOT_DIR}/tests/results/public_UTs"
ALL_RESULT_DIR="${ROOT_DIR}/tests/results/UTs_all"
mkdir -p "${PRIV_DIR}" "${PUB_DIR}" "${PRIV_RESULT_DIR}" "${PUB_RESULT_DIR}" "${ALL_RESULT_DIR}"

if [[ "${MODE}" != "run" ]]; then
    # Clean ALL previous coverage data — stale .gcno from an old compile makes
    # gcov/gcovr fail with stamp mismatches.
    rm -f "${PRIV_DIR}"/*.gcda "${PRIV_DIR}"/*.gcno "${PRIV_DIR}"/*.gcov \
          "${PUB_DIR}"/*.gcda  "${PUB_DIR}"/*.gcno  "${PUB_DIR}"/*.gcov 2>/dev/null || true

    # Debug profile + coverage layer; -O0 (after the profile's -Og) for exact
    # line/branch attribution in gcov data.
    UT_CPPFLAGS=("${CPPFLAGS_DEBUG[@]}" -D_GNU_SOURCE -Iapp)
    UT_CFLAGS=("${CFLAGS_DEBUG[@]}" -O0 "${CFLAGS_INSTRUMENT_COVERAGE[@]}")
    UT_LDFLAGS=("${LDFLAGS_DEBUG[@]}" "${LDFLAGS_INSTRUMENT_COVERAGE[@]}")

    # --- private suite: white-box, test_private.c #includes emlog.c directly -
    printf '[UTs] building private (white-box) suite...\n'
    gcc "${UT_CPPFLAGS[@]}" "${UT_CFLAGS[@]}" -c tests/UTs/privateAPI/test_private.c \
        -o "${PRIV_DIR}/test_private.o"
    gcc "${UT_LDFLAGS[@]}" "${PRIV_DIR}/test_private.o" -o "${PRIV_DIR}/ut_private" -lcmocka -pthread

    # --- public suite: black-box, links a separately compiled emlog object ---
    printf '[UTs] building public (black-box) suite...\n'
    gcc "${UT_CPPFLAGS[@]}" "${UT_CFLAGS[@]}" -c app/emlog.c -o "${PUB_DIR}/emlog.o"
    for src in tests/UTs/publicAPI/*.c; do
        gcc "${UT_CPPFLAGS[@]}" "${UT_CFLAGS[@]}" -Itests/UTs/publicAPI \
            -c "${src}" -o "${PUB_DIR}/$(basename "${src%.c}").o"
    done
    gcc "${UT_LDFLAGS[@]}" "${PUB_DIR}"/*.o -o "${PUB_DIR}/ut_public" -lcmocka -pthread
fi

if [[ "${MODE}" == "build" ]]; then
    printf '[UTs] build-only: binaries ready:\n  %s\n  %s\n' \
        "${PRIV_DIR}/ut_private" "${PUB_DIR}/ut_public"
    exit 0
fi

# --- run both under the same run ---------------------------------------------
for bin in "${PRIV_DIR}/ut_private" "${PUB_DIR}/ut_public"; do
    if [[ ! -x "${bin}" ]]; then
        printf 'missing test binary: %s (build first: %s --build-only)\n' "${bin}" "$0" >&2
        exit 1
    fi
done

# Fresh run counters only (.gcda accumulate across runs; .gcno stay — they
# belong to the existing binaries).
rm -f "${PRIV_DIR}"/*.gcda "${PUB_DIR}"/*.gcda 2>/dev/null || true

printf '[UTs] running private suite...\n'
"${PRIV_DIR}/ut_private"
printf '[UTs] running public suite...\n'
"${PUB_DIR}/ut_public"

# --- coverage ----------------------------------------------------------------
if ! command -v gcovr >/dev/null 2>&1; then
    echo "gcovr not found."
    exit 1
fi

# Each suite's build dir is passed as the SEARCH PATH so its report counts
# only its own gcov data (--object-directory would sweep the whole tree and
# silently merge both suites into every "per-suite" number).
printf '[coverage] per-suite reports...\n'
# --html-details / --json-summary each take their OWN optional OUTPUT arg
# directly after the flag; -o is only the shared fallback for formats that
# don't. Repeating -o for two formats in one call makes the second -o win
# for BOTH (the HTML report gets written into the .json path) — pass each
# format's path inline instead.
gcovr -r "${ROOT_DIR}" \
    --exclude 'tests/' \
    --html --html-details "${PRIV_RESULT_DIR}/UTs_private_coverage.html" \
    --json-summary "${PRIV_RESULT_DIR}/coverage-summary.json" \
    "${PRIV_DIR}"
gcovr -r "${ROOT_DIR}" \
    --exclude 'tests/' \
    --html --html-details "${PUB_RESULT_DIR}/UTs_public_coverage.html" \
    --json-summary "${PUB_RESULT_DIR}/coverage-summary.json" \
    "${PUB_DIR}"

# Combined report: both build dirs given as SEARCH PATHS in one invocation.
# (Never pass --object-directory twice — the second silently overrides the
# first and the "combined" report becomes single-suite.)
printf '[coverage] combined report...\n'
gcovr -r "${ROOT_DIR}" \
    --exclude 'tests/' \
    --html --html-details -o "${ALL_RESULT_DIR}/UTs_all_coverage.html" \
    "${PRIV_DIR}" "${PUB_DIR}"
gcovr -r "${ROOT_DIR}" \
    --exclude 'tests/' \
    --xml -o "${ALL_RESULT_DIR}/UTs_all_coverage.xml" \
    "${PRIV_DIR}" "${PUB_DIR}"
gcovr -r "${ROOT_DIR}" \
    --exclude 'tests/' \
    --json-summary -o "${ALL_RESULT_DIR}/coverage-summary.json" \
    "${PRIV_DIR}" "${PUB_DIR}"

printf '[coverage] report ready: %s\n' "${ALL_RESULT_DIR}/UTs_all_coverage.html"
