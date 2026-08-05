#!/usr/bin/env bash
# =============================================================================
# build_UTs_release.sh — both UT suites compiled and run under the RELEASE profile
#
# author  Roman Horshkov <github.com/RomanHorshkov>
# date    2026
# (c) 2026
# =============================================================================
#
# Usage:
#   ./utils/build_UTs_release.sh                # build both suites, run both
#   ./utils/build_UTs_release.sh --build-only   # just compile the binaries
#   ./utils/build_UTs_release.sh --run-only     # run already-built binaries
#
# Same two suites as build_UTs.sh, but everything is compiled with the release
# profile from the shared catalog (-O2, NDEBUG, hardening) so shipping-profile
# behavior is what gets exercised. cmocka assertions are library calls, not
# <assert.h>, so NDEBUG does not neuter the tests. The public suite links the
# release static library the deb actually ships.
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

PRIV_DIR="${ROOT_DIR}/build/UTs_release/private"
PUB_DIR="${ROOT_DIR}/build/UTs_release/public"
mkdir -p "${PRIV_DIR}" "${PUB_DIR}"

if [[ "${MODE}" != "run" ]]; then
    # Build release libraries first (flat build/libemlog.a is a symlink into
    # build/release/ maintained by build_libs.sh).
    "${ROOT_DIR}/utils/build_libs.sh" release

    REL_CPPFLAGS=("${CPPFLAGS_RELEASE[@]}" -D_GNU_SOURCE -Iapp)
    REL_CFLAGS=("${CFLAGS_RELEASE[@]}")
    REL_LDFLAGS=("${LDFLAGS_RELEASE[@]}")

    # Private UTs are white-box (test_private.c #includes emlog.c directly), so
    # they compile and link standalone — no separate library object needed.
    gcc "${REL_CPPFLAGS[@]}" "${REL_CFLAGS[@]}" -c tests/UTs/privateAPI/test_private.c \
        -o "${PRIV_DIR}/test_private.o"
    gcc "${REL_LDFLAGS[@]}" "${PRIV_DIR}/test_private.o" -o "${PRIV_DIR}/ut_private_release" \
        -lcmocka -pthread

    # Public UTs link against the release static library (black-box, public API only).
    for src in tests/UTs/publicAPI/*.c; do
        gcc "${REL_CPPFLAGS[@]}" "${REL_CFLAGS[@]}" -Itests/UTs/publicAPI \
            -c "${src}" -o "${PUB_DIR}/$(basename "${src%.c}").o"
    done
    gcc "${REL_LDFLAGS[@]}" "${PUB_DIR}"/*.o -o "${PUB_DIR}/ut_public_release" \
        build/libemlog.a -lcmocka -pthread
fi

if [[ "${MODE}" == "build" ]]; then
    printf '[UTs-release] build-only: binaries ready:\n  %s\n  %s\n' \
        "${PRIV_DIR}/ut_private_release" "${PUB_DIR}/ut_public_release"
    exit 0
fi

for bin in "${PRIV_DIR}/ut_private_release" "${PUB_DIR}/ut_public_release"; do
    if [[ ! -x "${bin}" ]]; then
        printf 'missing test binary: %s (build first: %s --build-only)\n' "${bin}" "$0" >&2
        exit 1
    fi
done

# Run both.
"${PRIV_DIR}/ut_private_release"
"${PUB_DIR}/ut_public_release"
