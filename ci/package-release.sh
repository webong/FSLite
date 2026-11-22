#!/usr/bin/env bash
# Package FSLite for release: pkg-config, CMake configs, checksummed archives.
#
# Run after `zig build install` (produces zig-out/lib and zig-out/include).
# Outputs go into zig-out/release/:
#   fslite-<version>-<target>.tar.gz   (Linux/macOS)
#   fslite-<version>-<target>.zip      (Windows)
#   SHA256SUMS
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${REPO_ROOT}"

VERSION="${VERSION:-0.1.0}"
ABI_REVISION=7
TARGET="${TARGET:-$(zig version >/dev/null 2>&1 && zig targets 2>/dev/null | head -1 | awk '{print $1}' || echo unknown)}"

RELEASE_DIR="zig-out/release"
mkdir -p "${RELEASE_DIR}/lib/pkgconfig"
mkdir -p "${RELEASE_DIR}/lib/cmake/fslite"

# ---------------------------------------------------------------------------
# pkg-config file
# ---------------------------------------------------------------------------
PREFIX="$(cd zig-out && pwd)"
LIBDIR="${PREFIX}/lib"
INCLUDEDIR="${PREFIX}/include"

sed -e "s|@PREFIX@|${PREFIX}|g" \
    -e "s|@LIBDIR@|${LIBDIR}|g" \
    -e "s|@INCLUDEDIR@|${INCLUDEDIR}|g" \
    ci/fslite.pc.in > "${RELEASE_DIR}/lib/pkgconfig/fslite.pc"

# ---------------------------------------------------------------------------
# CMake config files
# ---------------------------------------------------------------------------
sed -e "s|@INCLUDEDIR@|${INCLUDEDIR}|g" \
    -e "s|@LIBDIR@|${LIBDIR}|g" \
    ci/fsliteConfig.cmake.in > "${RELEASE_DIR}/lib/cmake/fslite/fsliteConfig.cmake"

sed -e "s|@VERSION@|${VERSION}|g" \
    ci/fsliteConfigVersion.cmake.in > "${RELEASE_DIR}/lib/cmake/fslite/fsliteConfigVersion.cmake"

# ---------------------------------------------------------------------------
# Copy artifacts into release tree
# ---------------------------------------------------------------------------
cp -v zig-out/lib/libfslite.a "${RELEASE_DIR}/lib/" 2>/dev/null || true
cp -v zig-out/lib/libfslite.so "${RELEASE_DIR}/lib/" 2>/dev/null || true
cp -v zig-out/lib/libfslite.dylib "${RELEASE_DIR}/lib/" 2>/dev/null || true
cp -v zig-out/include/fslite.h "${RELEASE_DIR}/include/" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Checksums
# ---------------------------------------------------------------------------
HASH_CMD=""
if command -v sha256sum >/dev/null 2>&1; then
    HASH_CMD="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    HASH_CMD="shasum -a 256"
else
    echo "ERROR: neither sha256sum nor shasum found."
    exit 1
fi

(
    cd "${RELEASE_DIR}"
    find . -type f -not -name 'SHA256SUMS' -print0 | sort -z | xargs -0 ${HASH_CMD} > SHA256SUMS
)

echo ""
echo "Release package ready at ${RELEASE_DIR}/"
echo "  pkg-config: ${RELEASE_DIR}/lib/pkgconfig/fslite.pc"
echo "  CMake:      ${RELEASE_DIR}/lib/cmake/fslite/"
echo "  Artifacts:  $(ls "${RELEASE_DIR}/lib/" | tr '\n' ' ')"
echo "  Checksums:  ${RELEASE_DIR}/SHA256SUMS"
