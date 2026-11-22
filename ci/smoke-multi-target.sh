#!/usr/bin/env bash
# Cross-target build and link smoke test for FSLite.
#
# Builds the static and shared libraries for multiple Zig targets, compiles the
# C ABI examples against each target's static library with `zig cc`, and runs
# the examples (plus an independent SQLite integrity check) only for the host
# target. Non-native targets verify cross compilation and static linking; they
# cannot be executed without an emulator or a matching machine.
#
# Usage: ci/smoke-multi-target.sh [target ...]
#   If no targets are given, the host target plus common cross targets are used.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
if [[ -d "${REPO_ROOT}/.tools/zig-aarch64-macos-0.15.2" ]]; then
    export PATH="${REPO_ROOT}/.tools/zig-aarch64-macos-0.15.2:${PATH}"
    export ZIG_GLOBAL_CACHE_DIR="${REPO_ROOT}/.tools/zig-cache"
fi

host_target() {
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64) echo aarch64-macos ;;
        Darwin-x86_64) echo x86_64-macos ;;
        Linux-x86_64) echo x86_64-linux-gnu ;;
        Linux-aarch64 | Linux-arm64) echo aarch64-linux-gnu ;;
        MINGW* | MSYS* | CYGWIN*) echo x86_64-windows-gnu ;;
        *) echo "" ;;
    esac
}

host="$(host_target)"
if [[ -z "${host}" ]]; then
    echo "ERROR: unsupported host platform: $(uname -s)-$(uname -m)"
    exit 1
fi

if [[ $# -eq 0 ]]; then
    targets=("${host}")
    for candidate in aarch64-macos x86_64-macos x86_64-linux-gnu aarch64-linux-gnu x86_64-windows-gnu; do
        [[ "${candidate}" == "${host}" ]] || targets+=("${candidate}")
    done
else
    targets=("$@")
fi

zig version >/dev/null 2>&1 || {
    echo "ERROR: 'zig' not found on PATH. Install the pinned Zig release or add .tools zig to PATH."
    exit 1
}

smoke_dir=$(mktemp -d "${TMPDIR:-/tmp}/fslite-smoke.XXXXXX")
trap 'rm -rf "${smoke_dir}"' EXIT

echo "=== Cross-target build and link smoke test ==="
echo "Repo: ${REPO_ROOT}"
echo "Host target: ${host}"
echo "Workdir: ${smoke_dir}"
echo "Targets: ${targets[*]}"
echo ""

failed=0

for target in "${targets[@]}"; do
    echo "--- Target: ${target} ---"
    prefix="${smoke_dir}/install-${target}"

    if ! zig build install -Dtarget="${target}" -Doptimize=ReleaseSafe --prefix "${prefix}" \
        >"${smoke_dir}/build-${target}.log" 2>&1; then
        echo "  BUILD FAILED:"
        tail -20 "${smoke_dir}/build-${target}.log" >&2
        failed=1
        continue
    fi

    static="${prefix}/lib/libfslite.a"
    [[ -f "${static}" ]] || static="${prefix}/lib/fslite-static.lib"
    if [[ ! -f "${static}" ]]; then
        echo "  MISSING STATIC LIBRARY under ${prefix}/lib"
        ls -l "${prefix}/lib" >&2 || true
        failed=1
        continue
    fi

    link_ok=1
    exe_suffix=""
    [[ "${target}" == *windows* ]] && exe_suffix=".exe"
    for example in basic stream backup random_access; do
        if ! zig cc -target "${target}" -std=c11 -Wall -Wextra -Werror \
            -I"${prefix}/include" \
            "${REPO_ROOT}/examples/${example}.c" "${static}" \
            -o "${smoke_dir}/${example}-${target}${exe_suffix}" \
            >"${smoke_dir}/link-${example}-${target}.log" 2>&1; then
            echo "  LINK FAILED: ${example}"
            cat "${smoke_dir}/link-${example}-${target}.log" >&2
            link_ok=0
        fi
    done
    if [[ ${link_ok} -eq 0 ]]; then
        failed=1
        continue
    fi

    if [[ "${target}" != "${host}" ]]; then
        echo "  build/link OK (native execution skipped)"
        continue
    fi

    run_ok=1
    "${smoke_dir}/basic-${target}${exe_suffix}" "${smoke_dir}/source-${target}.fslite" >/dev/null 2>&1 || {
        echo "  RUN FAILED: basic"
        run_ok=0
    }
    "${smoke_dir}/random_access-${target}${exe_suffix}" "${smoke_dir}/edited-${target}.fslite" >/dev/null 2>&1 || {
        echo "  RUN FAILED: random_access"
        run_ok=0
    }
    printf 'Streaming from CI multi-target\n' |
        "${smoke_dir}/stream-${target}${exe_suffix}" "${smoke_dir}/source-${target}.fslite" /stream.txt >/dev/null 2>&1 || {
        echo "  RUN FAILED: stream"
        run_ok=0
    }
    "${smoke_dir}/backup-${target}${exe_suffix}" "${smoke_dir}/source-${target}.fslite" \
        "${smoke_dir}/backup-${target}.fslite" >/dev/null 2>&1 || {
        echo "  RUN FAILED: backup"
        run_ok=0
    }

    if [[ ${run_ok} -eq 0 ]]; then
        failed=1
        continue
    fi

    if ! python3 - "${smoke_dir}" "${target}" <<'PY'
import pathlib, sqlite3, sys
directory, target = pathlib.Path(sys.argv[1]), sys.argv[2]
for name in ("source", "backup", "edited"):
    db_path = directory / f"{name}-{target}.fslite"
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
            assert row is not None, f"{filename} not found in {name}-{target}"
            node_id, size = row
            chunks = db.execute(
                "SELECT data FROM chunks WHERE node_id=? ORDER BY chunk_index", (node_id,)
            )
            assert size == len(content)
            assert b"".join(r[0] for r in chunks) == content
PY
    then
        echo "  INTEGRITY CHECK FAILED"
        failed=1
        continue
    fi
    echo "  build/link/run/integrity OK"
done

echo ""
if [[ ${failed} -eq 0 ]]; then
    echo "All cross-target checks passed."
    exit 0
else
    echo "One or more cross-target checks FAILED."
    exit 1
fi
