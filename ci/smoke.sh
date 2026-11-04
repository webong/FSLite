#!/usr/bin/env bash
set -euo pipefail

# Run from the repository root, after `zig build install`.
smoke_dir=$(mktemp -d "${TMPDIR:-/tmp}/fslite-ci.XXXXXX")
trap 'rm -rf "$smoke_dir"' EXIT
compiler=${CC:-cc}

for linkage in static shared; do
  if [[ "$linkage" == static ]]; then
    libraries=(zig-out/lib/libfslite.a)
  else
    libraries=(-Lzig-out/lib -lfslite "-Wl,-rpath,$PWD/zig-out/lib")
  fi
  if [[ "$(uname -s)" == Linux ]]; then
    libraries+=(-lpthread -ldl -lm)
  fi

  for example in basic stream backup random_access; do
    "$compiler" -std=c11 -Wall -Wextra -Werror -Izig-out/include \
      "examples/$example.c" "${libraries[@]}" -o "$smoke_dir/$example-$linkage"
  done

  "$smoke_dir/basic-$linkage" "$smoke_dir/source-$linkage.fslite"
  "$smoke_dir/random_access-$linkage" "$smoke_dir/edited-$linkage.fslite"
  printf 'Streaming from CI\n' | "$smoke_dir/stream-$linkage" \
    "$smoke_dir/source-$linkage.fslite" /stream.txt
  "$smoke_dir/backup-$linkage" "$smoke_dir/source-$linkage.fslite" \
    "$smoke_dir/backup-$linkage.fslite"

  python3 - "$smoke_dir" "$linkage" <<'PY'
import pathlib
import sqlite3
import sys

directory, linkage = pathlib.Path(sys.argv[1]), sys.argv[2]
for name in ("source", "backup", "edited"):
    with sqlite3.connect(directory / f"{name}-{linkage}.fslite") as db:
        assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
        assert db.execute("PRAGMA user_version").fetchone()[0] == 2
        assert db.execute("PRAGMA foreign_key_check").fetchall() == []
        expected = {"edited.bin": b"Hello FSL\0\0\0"} if name == "edited" else {
            "hello.txt": b"Hello from FSLite!",
            "stream.txt": b"Streaming from CI\n",
        }
        for filename, content in expected.items():
            node_id, size = db.execute(
                "SELECT id,size FROM nodes WHERE parent=1 AND name=?", (filename,)
            ).fetchone()
            chunks = db.execute(
                "SELECT data FROM chunks WHERE node_id=? ORDER BY chunk_index", (node_id,)
            )
            assert size == len(content)
            assert b"".join(row[0] for row in chunks) == content
print(f"Native {linkage} consumers and volume integrity passed")
PY
done

"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror -Izig-out/include -x c++ -fsyntax-only - <<'CPP'
#include "fslite.h"
static_assert(sizeof(fslite_info) == 24, "file info ABI layout");
static_assert(sizeof(fslite_backup_progress) == 24, "backup progress ABI layout");
int main() {
    fslite_write_at(nullptr, "/file", 0, nullptr, 0);
    fslite_truncate(nullptr, "/file", 0);
    fslite_backup_close(nullptr);
}
CPP
