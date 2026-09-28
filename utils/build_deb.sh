#!/usr/bin/env bash
# build_deb.sh — build/debs/: libemlog_<ver>_<arch>.deb (runtime: libemlog.so.<ver> and its soname link) and libemlog-dev (app/emlog.h,
# libemlog.a, the linker link), both replacing the former single package "emlog", through kyr-build's lib/deb.sh: X-Kyr-Tier
# from deps.txt, SHA256SUMS, one .buildinfo per deb, and the same bytes from the same commit anywhere.
set -euo pipefail
ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
export ROOT_DIR
LIB="emlog"
PKG_RUNTIME="libemlog"
PKG_DEV="libemlog-dev"
PKG_OLD="emlog"
DESCRIPTION="Tiny thread-safe C logger with printf-style API and canonical errno mapping"
# shellcheck source=/dev/null
source "${ROOT_DIR}/utils/gcc_build_profiles.sh"
# shellcheck source=/dev/null
source "${KYR_BUILD_DIR}/lib/deb.sh"
cd "${ROOT_DIR}"
./utils/build_libs.sh release
deb_init
REAL="lib${LIB}.so.${DEB_VERSION}"

stage="$(deb_stage "${PKG_RUNTIME}" lib)"
install -m 0755 "build/release/${REAL}" "${stage}/usr/local/lib/${REAL}"
"${STRIP:-strip}" --strip-unneeded "${stage}/usr/local/lib/${REAL}"
ln -sfn "${REAL}" "${stage}/usr/local/lib/lib${LIB}.so.${DEB_MAJOR}"
./utils/check_hardening.sh "${stage}/usr/local/lib/${REAL}"
deb_control "${stage}" "${PKG_RUNTIME}" libs "${DEB_ARCH}" "${DESCRIPTION}" "Conflicts: ${PKG_OLD}" "Replaces: ${PKG_OLD}"
deb_ldconfig_hooks "${stage}"
deb_copyright "${stage}" "${PKG_RUNTIME}"
deb_build "${stage}" "${PKG_RUNTIME}" > /dev/null

stage="$(deb_stage "${PKG_DEV}" lib include)"
install -m 0644 "app/${LIB}.h" "${stage}/usr/local/include/${LIB}.h"
install -m 0644 "build/release/lib${LIB}.a" "${stage}/usr/local/lib/lib${LIB}.a"
ln -sfn "${REAL}" "${stage}/usr/local/lib/lib${LIB}.so"
deb_control "${stage}" "${PKG_DEV}" libdevel "${DEB_ARCH}" "Development files for ${PKG_RUNTIME} (header, static library, linker symlink)" \
    "Depends: ${PKG_RUNTIME} (= ${DEB_VERSION})" "Conflicts: ${PKG_OLD}" "Replaces: ${PKG_OLD}"
deb_copyright "${stage}" "${PKG_DEV}"
deb_build "${stage}" "${PKG_DEV}" > /dev/null

deb_sha256sums
cat "${DEB_OUT_DIR}/SHA256SUMS"
