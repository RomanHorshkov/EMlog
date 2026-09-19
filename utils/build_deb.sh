#!/usr/bin/env bash
# =============================================================================
# build_deb.sh — package the release-profile libemlog artifacts into debs
#
# author  Roman Horshkov <github.com/RomanHorshkov>
# date    2026
# (c) 2026
# =============================================================================
#
# Produces the standard Debian library split:
#
#   libemlog_<ver>_<arch>.deb      runtime: libemlog.so.<ver> + soname symlink
#   libemlog-dev_<ver>_<arch>.deb  development: emlog.h, libemlog.a,
#                                  libemlog.so linker symlink; depends on the
#                                  exact-version runtime package
#
# plus a SHA256SUMS manifest covering both, in build/debs/.
# =============================================================================
set -euo pipefail

ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
PKG_RUNTIME="libemlog"
PKG_DEV="libemlog-dev"
STRIP="${STRIP:-strip}"

die() { printf '%s: %s\n' "${BASH_SOURCE[0]}" "$1" >&2; exit 1; }

cd "$ROOT_DIR"

# Read + validate version (packaged versions must be strict semver).
VER="$(tr -d '[:space:]' < VERSION)"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION '${VER}' does not match ^[0-9]+\\.[0-9]+\\.[0-9]+\$"

# Build the release library artifacts (also refreshes the flat build/ symlinks
# and runs the hardening gate on the freshly linked .so).
./utils/build_libs.sh release

ARCH="$(dpkg --print-architecture)"

# Split version safely (keep IFS local)
IFS='.' read -r MAJOR MINOR PATCH <<< "$VER"

OUT_DIR="${OUT_DIR:-${ROOT_DIR}/build/debs}"
# Start clean: stale debs (including ones from before a package rename) must
# never linger into the SHA256SUMS manifest or a report.
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

# --- runtime package: libemlog ----------------------------------------------
STAGE_RT="${ROOT_DIR}/build/pkgroot/${PKG_RUNTIME}"
rm -rf "$STAGE_RT"
mkdir -p "$STAGE_RT/DEBIAN" "$STAGE_RT/usr/local/lib"

install -m 0755 "build/release/libemlog.so.$VER" "$STAGE_RT/usr/local/lib/libemlog.so.$VER"
"$STRIP" --strip-unneeded "$STAGE_RT/usr/local/lib/libemlog.so.$VER"
ln -sf "libemlog.so.$VER" "$STAGE_RT/usr/local/lib/libemlog.so.$MAJOR"

# Gate the staged, stripped shared library: the exact deb payload must carry
# the hardening the release profile promises. A hard failure aborts the build.
"${ROOT_DIR}/utils/check_hardening.sh" "$STAGE_RT/usr/local/lib/libemlog.so.$VER"

cat > "$STAGE_RT/DEBIAN/control" <<EOF
Package: $PKG_RUNTIME
Version: $VER
Section: libs
Priority: optional
Architecture: $ARCH
Maintainer: Roman Horshkov <https://github.com/RomanHorshkov>
Description: Tiny thread-safe C logger with printf-style API and canonical errno mapping
EOF

# ldconfig hooks so the runtime linker sees the library immediately
cat > "$STAGE_RT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
ldconfig
exit 0
EOF
chmod 0755 "$STAGE_RT/DEBIAN/postinst"

cat > "$STAGE_RT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
ldconfig
exit 0
EOF
chmod 0755 "$STAGE_RT/DEBIAN/postrm"

# Ship the DEP-5 copyright file (first-party terms + every third-party notice)
# at /usr/share/doc/<pkg>/copyright (Debian Policy 12.5). A missing file is a
# build error: a binary must never leave without its notices.
COPYRIGHT_SRC="${ROOT_DIR}/debian/copyright"
[[ -f "${COPYRIGHT_SRC}" ]] || die "missing ${COPYRIGHT_SRC} — third-party notices must ship in the deb"
install -d -m 0755 "${STAGE_RT}/usr/share" "${STAGE_RT}/usr/share/doc" "${STAGE_RT}/usr/share/doc/${PKG_RUNTIME}"
install -m 0644 "${COPYRIGHT_SRC}" "${STAGE_RT}/usr/share/doc/${PKG_RUNTIME}/copyright"
DEB_RT="${PKG_RUNTIME}_${VER}_${ARCH}.deb"
fakeroot dpkg-deb --build "$STAGE_RT" "$OUT_DIR/$DEB_RT"

# --- development package: libemlog-dev --------------------------------------
STAGE_DEV="${ROOT_DIR}/build/pkgroot/${PKG_DEV}"
rm -rf "$STAGE_DEV"
mkdir -p "$STAGE_DEV/DEBIAN" "$STAGE_DEV/usr/local/lib" "$STAGE_DEV/usr/local/include"

install -m 0644 app/emlog.h "$STAGE_DEV/usr/local/include/emlog.h"
install -m 0644 build/release/libemlog.a "$STAGE_DEV/usr/local/lib/libemlog.a"
ln -sf "libemlog.so.$VER" "$STAGE_DEV/usr/local/lib/libemlog.so"

cat > "$STAGE_DEV/DEBIAN/control" <<EOF
Package: $PKG_DEV
Version: $VER
Section: libdevel
Priority: optional
Architecture: $ARCH
Depends: $PKG_RUNTIME (= $VER)
Maintainer: Roman Horshkov <https://github.com/RomanHorshkov>
Description: Development files for libemlog (header, static library, linker symlink)
EOF

# Ship the DEP-5 copyright file (first-party terms + every third-party notice)
# at /usr/share/doc/<pkg>/copyright (Debian Policy 12.5). A missing file is a
# build error: a binary must never leave without its notices.
COPYRIGHT_SRC="${ROOT_DIR}/debian/copyright"
[[ -f "${COPYRIGHT_SRC}" ]] || die "missing ${COPYRIGHT_SRC} — third-party notices must ship in the deb"
install -d -m 0755 "${STAGE_DEV}/usr/share" "${STAGE_DEV}/usr/share/doc" "${STAGE_DEV}/usr/share/doc/${PKG_DEV}"
install -m 0644 "${COPYRIGHT_SRC}" "${STAGE_DEV}/usr/share/doc/${PKG_DEV}/copyright"
DEB_DEV="${PKG_DEV}_${VER}_${ARCH}.deb"
fakeroot dpkg-deb --build "$STAGE_DEV" "$OUT_DIR/$DEB_DEV"

# --- manifest ----------------------------------------------------------------
(
    cd "$OUT_DIR"
    sha256sum -- *.deb > SHA256SUMS
)

printf '\nBuilt:\n  %s\n  %s\n' "$OUT_DIR/$DEB_RT" "$OUT_DIR/$DEB_DEV"
printf 'checksums: %s/SHA256SUMS\n' "$OUT_DIR"
printf 'install with: sudo apt install %s/%s %s/%s\n' "$OUT_DIR" "$DEB_RT" "$OUT_DIR" "$DEB_DEV"
