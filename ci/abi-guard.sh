#!/usr/bin/env bash
# ABI guard for FSLite.
#
# Verifies that the exported C symbols in built static/shared libraries match
# the `export fn` declarations in src/c_api.zig. This catches accidental ABI
# drift (missing exports, renamed functions, or added symbols) across platforms.
#
# Usage:
#   ci/abi-guard.sh                          check against built artifacts
#   ci/abi-guard.sh --update                 write expected-symbol list
#
# Environment:
#   ABI_SYMBOLS_FILE   path to expected symbols list (default: ci/abi-symbols.txt)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SYMBOLS_FILE="${ABI_SYMBOLS_FILE:-${REPO_ROOT}/ci/abi-symbols.txt}"

# Extract exported function names from c_api.zig.
extract_expected_symbols() {
    grep -n '^export fn ' "${REPO_ROOT}/src/c_api.zig" | \
        sed -E 's/^[0-9]+: *export fn ([a-zA-Z_][a-zA-Z0-9_]*).*/\1/' | \
        sort -u
}

# Extract defined (T/t) symbols from a static archive or shared library.
extract_actual_symbols() {
    local file="$1"
    if [[ ! -f "${file}" ]]; then
        return 0
    fi
    case "${file}" in
        *.a)
            # Static archive: list all defined text symbols across members.
            # macOS nm: column 2 is 'T'/'t', symbol in column 3 with _ prefix.
            # Linux nm: column 2 is 'T'/'t', symbol in column 3.
            nm --defined-only "${file}" 2>/dev/null | \
                awk '$2 == "T" || $2 == "t" {print $3}' | \
                sed -E 's/^_//' | sort -u
            ;;
        *.so|*.dylib|*.dll)
            # Try nm -g (macOS) first, fall back to nm -D (Linux).
            if nm -g --defined-only "${file}" >/dev/null 2>&1; then
                nm -g --defined-only "${file}" 2>/dev/null | \
                    awk '{print $NF}' | sed -E 's/^_//' | sort -u
            else
                nm -D --defined-only "${file}" 2>/dev/null | \
                    awk '{print $NF}' | sed -E 's/^_//' | sort -u || true
            fi
            ;;
    esac
}

CURRENT_KEY="${REPO_ROOT}/ci/abi-symbols-current.txt"

update_mode=0
if [[ "${1:-}" == "--update" ]]; then
    update_mode=1
fi

expected=$(extract_expected_symbols)
expected_count=$(echo "${expected}" | grep -c '.' || true)

if [[ $update_mode -eq 1 ]]; then
    echo "${expected}" > "${SYMBOLS_FILE}"
    echo "Wrote ${expected_count} expected symbols to ${SYMBOLS_FILE}"
    exit 0
fi

if [[ ! -f "${SYMBOLS_FILE}" ]]; then
    echo "ERROR: ${SYMBOLS_FILE} not found. Run 'ci/abi-guard.sh --update' first."
    exit 1
fi

expected_sorted=$(grep -v '^#' "${SYMBOLS_FILE}" | grep -c '.' >/dev/null && grep -v '^#' "${SYMBOLS_FILE}" | grep '.' | sort -u || true)
expected_count=$(echo "${expected_sorted}" | grep -c '.' || true)

# Collect actual fslite_* symbols from all installed artifacts.
actual=""
for artifact in "${REPO_ROOT}/zig-out/lib/libfslite.a" \
                "${REPO_ROOT}/zig-out/lib/libfslite.so" \
                "${REPO_ROOT}/zig-out/lib/libfslite.dylib" \
                "${REPO_ROOT}/zig-out/lib/libfslite.dll"; do
    if [[ -f "${artifact}" ]]; then
        art_syms=$(extract_actual_symbols "${artifact}" | grep '^fslite_' | grep -v '^fslite_backup_file_' || true)
        if [[ -n "${art_syms}" ]]; then
            actual+="${art_syms}"$'\n'
        fi
    fi
done

actual_sorted=$(echo "${actual}" | sort -u)

# Compare.
missing=""
extra=""
while IFS= read -r sym; do
    [[ -z "${sym}" || "${sym}" == \#* ]] && continue
    if ! echo "${actual_sorted}" | grep -qx "${sym}"; then
        missing+="${sym}"$'\n'
    fi
done < <(echo "${expected_sorted}")

while IFS= read -r sym; do
    [[ -z "${sym}" || "${sym}" == \#* ]] && continue
    if ! echo "${expected_sorted}" | grep -qx "${sym}"; then
        extra+="${sym}"$'\n'
    fi
done < <(echo "${actual_sorted}")

failed=0
if [[ -n "${missing}" ]]; then
    echo "MISSING exported symbols (in header/API but not in built artifacts):"
    echo "${missing}" | sed 's/^/  /'
    failed=1
fi
if [[ -n "${extra}" ]]; then
    echo "EXTRA exported symbols (in built artifacts but not in header/API):"
    echo "${extra}" | sed 's/^/  /'
    failed=1
fi

if [[ $failed -eq 0 ]]; then
    echo "ABI guard OK: ${expected_count} exported symbols match across built artifacts."
    exit 0
else
    echo "ABI guard FAILED."
    exit 1
fi
