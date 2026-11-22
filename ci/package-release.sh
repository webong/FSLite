#!/usr/bin/env bash
# Package FSLite for release: libraries, headers, CLI, pkg-config, CMake files.
#
# Run after `zig build install` (produces zig-out/lib, zig-out/include, and
# zig-out/bin). The assembled, relocatable tree is written to zig-out/release/
# with a SHA256SUMS manifest.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${REPO_ROOT}"

VERSION="${VERSION:-0.1.0}"

RELEASE_DIR="zig-out/release"
mkdir -p "${RELEASE_DIR}/lib/pkgconfig"
mkdir -p "${RELEASE_DIR}/lib/cmake/fslite"
mkdir -p "${RELEASE_DIR}/include"
mkdir -p "${RELEASE_DIR}/bin"

# ---------------------------------------------------------------------------
# pkg-config file
# ---------------------------------------------------------------------------
sed -e "s|@VERSION@|${VERSION}|g" \
    ci/fslite.pc.in > "${RELEASE_DIR}/lib/pkgconfig/fslite.pc"

# ---------------------------------------------------------------------------
# CMake config files
# ---------------------------------------------------------------------------
cp ci/fsliteConfig.cmake.in "${RELEASE_DIR}/lib/cmake/fslite/fsliteConfig.cmake"

sed -e "s|@VERSION@|${VERSION}|g" \
    ci/fsliteConfigVersion.cmake.in > "${RELEASE_DIR}/lib/cmake/fslite/fsliteConfigVersion.cmake"

# ---------------------------------------------------------------------------
# Copy artifacts into release tree
# ---------------------------------------------------------------------------
cp -v zig-out/lib/libfslite.a "${RELEASE_DIR}/lib/" 2>/dev/null || true
cp -v zig-out/lib/libfslite.so "${RELEASE_DIR}/lib/" 2>/dev/null || true
cp -v zig-out/lib/libfslite.dylib "${RELEASE_DIR}/lib/" 2>/dev/null || true
cp -v zig-out/lib/fslite-static.lib "${RELEASE_DIR}/lib/" 2>/dev/null || true
cp -v zig-out/lib/fslite.lib "${RELEASE_DIR}/lib/" 2>/dev/null || true
cp -v zig-out/bin/fslite.dll "${RELEASE_DIR}/bin/" 2>/dev/null || true
cp -v zig-out/include/fslite.h "${RELEASE_DIR}/include/" 2>/dev/null || true
cp -v zig-out/include/fslite.hpp "${RELEASE_DIR}/include/" 2>/dev/null || true
cp -v zig-out/bin/fslite "${RELEASE_DIR}/bin/" 2>/dev/null || true
cp -v zig-out/bin/fslite.exe "${RELEASE_DIR}/bin/" 2>/dev/null || true

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
