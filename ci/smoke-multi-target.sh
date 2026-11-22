#!/usr/bin/env bash
# Cross-target smoke test for FSLite.
#
# Builds static and shared libraries for multiple Zig targets, compiles the C
# ABI examples against each, runs them, and verifies volume integrity.
#
# Usage: ci/smoke-multi-target.sh [target ...]
#   If no targets are given, the host target plus common cross targets are used.
#
# Requires Zig (pinned version via .zig-version) and a C compiler
# matching each target's linker (cc for host; mingw/gcc for cross).
#
# Cross targets require the appropriate system linker; on macOS the default
# ld64 can target x86_64-macos, and on Linux clang/gcc can link x86_64.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
export PATH="${REPO_ROOT}/.tools/zig-aarch64-macos-0.15.2:${PATH}"
export ZIG_GLOBAL_CACHE_DIR="${REPO_ROOT}/.tools/zig-cache"

# Default targets if none provided: host + common cross targets.
if [[ $# -eq 0 ]]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64)
            targets=(aarch64-macos x86_64-macos aarch64-linux-gnu x86_64-linux-gnu)
            ;;
        Darwin-x86_64)
            targets=(x86_64-macos aarch64-macos x86_64-linux-gnu aarch64-linux-gnu)
            ;;
        Linux-x86_64)
            targets=(x86_64-linux-gnu aarch64-linux-gnu x86_64-macos aarch64-macos)
            ;;
        *)
            targets=($(zig targets 2>/dev/null | awk '{print $1}' | head -6))
            ;;
    esac
else
    targets=("$@")
fi

# Pin Zig version.
zig version >/dev/null 2>&1 || {
    echo "ERROR: 'zig' not found on PATH. Install Zig 0.15.2 or add .tools zig to PATH."
    exit 1
}

smoke_dir=$(mktemp -d "${TMPDIR:-/tmp}/fslite-smoke.XXXXXX")
trap 'rm -rf "$smoke_dir"' EXIT

echo "=== Cross-target smoke test ==="
echo "Repo: ${REPO_ROOT}"
echo "Workdir: ${smoke_dir}"
echo "Targets: ${targets[*]}"
echo ""

failed=0

for target in "${targets[@]}"; do
    echo "--- Target: ${target} ---"

    # Build install for this target.
    if ! zig build install -Dtarget="${target}" -Doptimize=ReleaseSafe >"${smoke_dir}/build-${target}.log" 2>&1; then
        echo "BUILD FAILED for ${target}:"
        tail -20 "${smoke_dir}/build-${target}.log" >&2
        failed=1
        continue
    fi

    compiler=${CC:-cc}

    for linkage in static shared; do
        if [[ "$linkage" == static ]]; then
            libraries=("${REPO_ROOT}/zig-out/lib/libfslite.a")
        else
            libraries=(-L"${REPO_ROOT}/zig-out/lib" -lfslite "-Wl,-rpath,${REPO_ROOT}/zig-out/lib")
        fi
        if [[ "$(uname -s)" == Linux ]]; then
            libraries+=(-lpthread -ldl -lm)
        fi

        link_ok=1
        for example in basic stream backup random_access; do
            if ! "$compiler" -std=c11 -Wall -Wextra -Werror -I"${REPO_ROOT}/zig-out/include" \
                "${REPO_ROOT}/examples/${example}.c" "${libraries[@]}" \
                -o "${smoke_dir}/${example}-${target}-${linkage}" 2>"${smoke_dir}/compile-${example}-${target}-${linkage}.log"; then
                echo "  COMPILE FAILED: ${example} (${linkage})"
                cat "${smoke_dir}/compile-${example}-${target}-${linkage}.log" >&2
                link_ok=0
            fi
        done

        if [[ $link_ok -eq 0 ]]; then
            failed=1
            continue
        fi

        # Run the examples.
        "${smoke_dir}/basic-${target}-${linkage}" \
            "${smoke_dir}/source-${target}-${linkage}.fslite" >/dev/null 2>&1 || {
            echo "  RUN FAILED: basic (${linkage})"
            failed=1
        }
        "${smoke_dir}/random_access-${target}-${linkage}" \
            "${smoke_dir}/edited-${target}-${linkage}.fslite" >/dev/null 2>&1 || {
            echo "  RUN FAILED: random_access (${linkage})"
            failed=1
        }
        printf 'Streaming from CI multi-target\n' | \
            "${smoke_dir}/stream-${target}-${linkage}" \
            "${smoke_dir}/source-${target}-${linkage}.fslite" /stream.txt >/dev/null 2>&1 || {
            echo "  RUN FAILED: stream (${linkage})"
            failed=1
        }
        "${smoke_dir}/backup-${target}-${linkage}" \
            "${smoke_dir}/source-${target}-${linkage}.fslite" \
            "${smoke_dir}/backup-${target}-${linkage}.fslite" >/dev/null 2>&1 || {
            echo "  RUN FAILED: backup (${linkage})"
            failed=1
        }

        # Verify volume integrity with SQLite via Python.
        python3 - "${smoke_dir}" "${target}" "${linkage}" <<'PY' || {
echo "  INTEGRITY CHECK FAILED (${linkage})"
failed=1
}
import pathlib, sqlite3, sys
directory, target, linkage = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
for name in ("source", "backup", "edited"):
    db_path = directory / f"{name}-{target}-{linkage}.fslite"
    if not db_path.exists():
        print(f"  MISSING VOLUME: {db_path}", file=sys.stderr)
        sys.exit(1)
    with sqlite3.connect(db_path) as db:
        assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
        assert db.execute("PRAGMA user_version").fetchone()[0] == 2
        assert db.execute("PRAGMA foreign_key_check").fetchall() == []
        expected = {"edited.bin": b"Hello FSL\0\0\0"} if name == "edited" else {
            "hello.txt": b"Hello from FSLite!",
            "stream.txt": b"Streaming from CI multi-target\n",
        }
        for filename, content in expected.items():
            row = db.execute(
                "SELECT id,size FROM nodes WHERE parent=1 AND name=?", (filename,)
            ).fetchone()
            assert row is not None, f"{filename} not found in {name}-{target}-{linkage}"
            node_id, size = row
            chunks = db.execute(
                "SELECT data FROM chunks WHERE node_id=? ORDER BY chunk_index", (node_id,)
            )
            assert size == len(content)
            assert b"".join(r[0] for r in chunks) == content
PY
        echo "  OK: ${linkage}"
    done

    # Clean up artifacts before the next target so install paths don't collide.
    zig build clean >/dev/null 2>&1 || true

done

echo ""
if [[ $failed -eq 0 ]]; then
    echo "All cross-target smoke tests passed."
    exit 0
else
    echo "One or more cross-target smoke tests FAILED."
    exit 1
fi
