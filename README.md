# FSLite

An embedded file engine written in Zig, with a C ABI and a SQLite-backed volume
format. Host applications link the engine directly. It needs no server, event
loop, garbage collector, or background service.

## Current implementation

- Static and shared libraries with an opaque volume handle.
- File and directory hierarchy with case-sensitive UTF-8 names.
- Atomic whole-file creation/replacement, ranged reads into caller-owned buffers.
- 64 KiB content chunks and sequential streaming writes with bounded buffering.
- Random-access writes and truncation with zero-filled extension and atomic updates.
- File metadata (`id`, `size`, `kind`) and ordered directory listing.
- Atomic rename with replacement, and removal of files or empty directories.
- Consistent live export and incremental backups with cancellation and atomic publication.
- Read-only whole-volume integrity checking, including hierarchy and chunk validation.
- Explicit transactions spanning multiple file operations.
- Per-operation savepoints, including inside an explicit transaction.
- Persistent volumes, format/version checks, and rejection of foreign databases.
- Bundled official SQLite 3.54.1; the host does not need to install SQLite.

This is an **experimental first implementation**, not a production filesystem.
ABI revision 6 adds integrity checking, preserving existing
function signatures, structure layouts, and status values. The on-disk format
remains version 2. **Volume format 2 replaces the previous single-BLOB layout.**
Version-1 volumes are rejected without modification, including with CREATE; no
migration is implemented yet. Keep the previous engine to read/export old volumes.
Compatibility is not yet promised across releases.

The engine buffers at most one 64 KiB content chunk per active writer or random-access mutation, in addition
to SQLite's page cache and bookkeeping. Total file size is no longer limited to
one SQLite BLOB. The format caps it at INT64_MAX bytes; practical database and
disk limits are lower. Large-file throughput has not been characterized.

## Build

Pin **Zig 0.15.2**, matching `.zig-version`. SQLite source is vendored, so builds
do not fetch dependencies. On a machine with that Zig release installed:

```sh
zig build -Doptimize=ReleaseSafe
zig build test -Doptimize=ReleaseSafe
```

Outputs:

```text
zig-out/include/fslite.h
zig-out/lib/libfslite.a
zig-out/lib/libfslite.dylib   # macOS; platform equivalent elsewhere
```

This checkout also has a checksum-verified, ignored project-local macOS ARM64
compiler. Use it without changing the host installation:

```sh
export PATH="$PWD/.tools/zig-aarch64-macos-0.15.2:$PATH"
export ZIG_GLOBAL_CACHE_DIR="$PWD/.tools/zig-cache"
zig build -Doptimize=ReleaseSafe
zig build test -Doptimize=ReleaseSafe
```

The C integration tests exercise both static and shared libraries. The test
harness currently requires POSIX (`mkstemp`, `fork`, `waitpid`). It covers binary
and empty files, partial reads, persistence, transaction isolation, writer
contention, rollback on close/process exit, path validation, format rejection,
and invalid ABI arguments. Streaming coverage includes uneven input fragments,
cross-chunk reads, empty and shortened replacements, nested transactions, writer
lifetime, interrupted cache-spilling writes, injected statement errors, and busy
commits. Namespace tests cover replacement, directory moves, cycle/path-length
rejection, chunk cleanup, partial-operation failures, reader/writer contention,
and transaction rollback after close/process exit. Backup tests cover interleaved
commits, uncommitted-data exclusion, cancellation, collisions, process exit, busy
retry, OS write failure, and a directory-sync failure after publication. They do
not simulate hardware power loss. Random-access tests compare mixed writes and
resizes with an independent byte-array model, audit which chunks change, and
verify rollback on chunk/metadata failures, contention, and process exit.

## Continuous integration

[GitHub Actions](https://github.com/webong/fslite/actions/workflows/ci.yml) builds
and tests every push and pull request, with optional manual runs. The matrix
covers Ubuntu 24.04 and macOS 15 in Debug and ReleaseSafe modes, using the Zig
version in `.zig-version`. Each job checks formatting, builds and tests both
libraries, and runs native C examples against the static and shared artifacts.
It also verifies exported volumes and C++ compatibility of the public header.

Run the native-consumer checks locally after building:

```sh
bash ci/smoke.sh
```

## Benchmarks

Run the four benchmark suites against both static and shared libraries:

```sh
zig build bench -Doptimize=ReleaseSafe
```

For quick correctness checks use `zig build bench-smoke -Doptimize=ReleaseSafe`.
CI runs smoke workloads in both build modes on macOS and Linux, without timing
thresholds. `zig build test` runs only integration tests. Benchmarks use private
temporary volumes, honor `TMPDIR`, and clean them up on normal completion or a
reported failure. Full runs can take minutes and use several hundred MiB of disk.

The suites cover small-file creation/replacement, streaming with bounded buffers,
seeded random-access patches, and process peak RSS as streamed file size grows.
They retain SQLite's normal durable transaction settings, include commits in the
write timing, and verify persisted data outside the timed region. See
[docs/benchmarks.md](docs/benchmarks.md) for workloads, commands, and measurement
limitations.

## Use from C

```c
#include <fslite.h>

fslite_volume *volume = NULL;
fslite_status status = fslite_open("project.fslite", FSLITE_OPEN_CREATE, &volume);
/* Check every returned status. See examples/basic.c for a complete program. */
if (status == FSLITE_OK) {
    status = fslite_write(volume, "/hello.txt", "Hello", 5);
    if (status == FSLITE_OK) {
        char bytes[5];
        size_t count = 0;
        status = fslite_read(volume, "/hello.txt", 0, bytes, sizeof bytes, &count);
    }
    fslite_close(volume);
}
```

Compile the example on macOS using the installed static artifact:

```sh
cc -std=c11 -Izig-out/include examples/basic.c zig-out/lib/libfslite.a -o zig-out/basic
./zig-out/basic /tmp/example.fslite
```

On Linux, link the required platform libraries (typically `-lpthread -ldl -lm`).
Cross-target builds can use Zig's `-Dtarget`; target builds and runtime behavior
must be validated separately. Browser persistence is not implemented.
Native macOS builds default to a macOS 14.0 deployment target.

## Stream a file

The streaming lifecycle is `fslite_writer_open`, repeated `fslite_writer_write`,
`fslite_writer_finish`, then `fslite_writer_close`. Always close the writer,
including after failure. Close without finish aborts the entire replacement.
Input fragments can have any size up to PTRDIFF_MAX per call and do not need to
align with storage chunks. Successful finish with no input creates an empty file.

[examples/stream.c](examples/stream.c) imports stdin using a 16 KiB caller buffer:

```sh
cc -std=c11 -Izig-out/include examples/stream.c zig-out/lib/libfslite.a -o zig-out/stream
./zig-out/stream /tmp/assets.fslite /video.bin < /path/to/video.bin
```

A writer reserves the volume handle until finish or abort. Other operations on
that handle return INVALID_STATE; closing the volume aborts its writer but the
writer handle still needs closing. Independent handles see committed content or
receive BUSY, never a partial replacement. Large writes may cause SQLite cache
spills that block readers; the engine does not disable spilling to retain an
entire large transaction in memory.

Inside an explicit transaction, finish releases only the stream's savepoint;
the caller still commits or rolls back the outer transaction. Failed storage
operations and failed finish (including BUSY) abort the entire stream; open a new
writer to retry. Argument validation errors leave a valid stream unchanged.
Earlier outer-transaction work survives stream abort unless SQLite itself has
aborted that transaction because of a severe storage error.

## Random-access writes and truncation

`fslite_write_at(volume, path, offset, data, size)` patches an existing regular
file. It preserves its ID and bytes outside the written range. A nonempty write
beyond EOF zero-fills the gap; a zero-length write does not extend the file.
`fslite_truncate(volume, path, size)` shrinks or grows an existing regular file.
Growth adds zeros, including when regrowing a previously discarded tail. Neither
operation creates missing files; use `fslite_write` or a writer first.

Only overlapping chunks and any necessary extension chunks are rewritten.
Shrinking removes later chunks and shortens the final partial chunk. One 64 KiB
heap buffer is reused, and file size is updated after chunk changes. Each call
uses one savepoint, including inside an explicit transaction: a failed call rolls
back all its changes while preserving prior work unless SQLite aborts the whole
transaction. A reserved writer or backup blocks both operations on that handle.

Offsets and resulting file sizes must fit INT64_MAX; individual input buffers
must fit PTRDIFF_MAX. Format 2 remains dense: growing by a large amount writes
zero-filled chunks proportional to the extension. This implementation does not
provide sparse holes, per-operation quotas, or partial-success byte counts.
See [examples/random_access.c](examples/random_access.c) for a C transaction
that edits, shrinks, and regrows a file.

## Rename and remove

`fslite_rename(volume, source, destination)` moves a node to an exact destination
path within the same volume. It preserves the source ID, content, and descendant
IDs. It replaces an existing file with a file or an empty directory with a
directory. It never implicitly moves a file *into* an existing directory. A
same-path rename succeeds only if the non-root source exists.

`fslite_remove(volume, path)` removes a file and its chunks, or an empty directory.
Removal is nonrecursive: nonempty directories return `FSLITE_NOT_EMPTY`.
Root mutations, directory cycles, and directory moves that would make descendant
paths exceed 4096 UTF-8 bytes return `FSLITE_INVALID_ARGUMENT`.

Both operations use savepoints. A failed rename restores any replaced destination
and its chunks; a failed removal restores the node and chunks. Inside an explicit
transaction, earlier successful work is preserved unless SQLite aborts the whole
transaction. A successful operation remains pending until the caller commits.
An active streaming writer blocks both operations on its volume handle.

Deleted node IDs may be reused later. Removed data frees SQLite pages for reuse;
these operations do not promise to shrink the host database file.

## Live export and backup

`fslite_export(source, destination)` creates a standalone format-2 snapshot while
the source remains open. Destination is a **new host filesystem path**. Existing
files, directories, links, and SQLite sidecar names are never overwritten.

```sh
cc -std=c11 -Izig-out/include examples/backup.c zig-out/lib/libfslite.a -o zig-out/backup
./zig-out/backup project.fslite backup-001.fslite
```

For caller-controlled pacing/cancellation use `fslite_backup_open`, repeated
`fslite_backup_step`, `fslite_backup_finish` when progress reports `done=1`, and
`fslite_backup_close`. Always close the handle. Close before finish cancels, even
if copying already completed. A BUSY step can be retried; fatal storage errors
abort. The one-call export aborts on BUSY instead of retrying indefinitely.

The source handle must have no active transaction, writer, or backup. Opening a
backup reserves that handle; ordinary calls return INVALID_STATE until it detaches.
Other connections can write between steps. SQLite may restart copying when they
commit, so the result is a consistent committed snapshot at completion, not a
snapshot fixed at backup-open time. Progress counters may go backward and sustained
writes can delay completion. Once `done=1`, the staged snapshot is frozen.
These semantics follow [SQLite's online backup API](https://sqlite.org/c3ref/backup_finish.html).

The implementation copies into an adjacent private `.fslite-backup-*` directory,
closes and syncs the completed database, then publishes it with a non-overwriting
hard link and syncs the destination's parent directory. It does not hold the
complete volume in memory. Cancellation and ordinary failures clean staging;
a process crash can leave private staging for the host to remove after confirming
the owning backup is no longer active. FSLite never automatically deletes another
operation's staging directory.

`FSLITE_DURABILITY_UNCERTAIN` means a **complete destination exists**, but the
parent-directory sync failed. Do not treat it as an absent file or blindly retry
the same path. Other publication failures do not create a destination. Source
transactions are never committed or rolled back by backup.

Publication currently targets POSIX local filesystems supporting hard links and
directory fsync; CI covers macOS and Linux, while Windows returns UNSUPPORTED.
Keep the destination parent stable and trusted, and leave
the destination and its sidecar names unused until finish returns. Backup files
are created with owner-only permissions. No overwrite, restore-in-place, format
migration, or distributed/cross-volume transaction is provided.

## Integrity checking

`fslite_check(volume)` scans a consistent view of the whole volume. It checks
SQLite integrity and foreign keys, format markers, node types, root and parent
relationships, UTF-8 names, path lengths, reachability, and chunk ownership,
completeness, and exact lengths. It returns `FSLITE_INVALID_FORMAT` for detected
corruption or an unusable format schema; other failures retain their usual status.
It reports pass/fail, not individual diagnostic records, and never repairs data.

The check uses one read snapshot. Inside an explicit transaction it sees your
pending changes and leaves the transaction active, even when it detects
corruption. Earlier work is preserved unless SQLite itself aborts the transaction
on a severe storage error. Outside a transaction its read snapshot is released
before returning. An active writer or backup on the same handle returns
`FSLITE_INVALID_STATE`.

This is a synchronous full scan, with SQLite-managed working memory, and may
block another connection's commit. It does not verify checksums of file content,
authenticate schema provenance, or prove power-loss durability. Changes to bytes
that leave the structure and lengths valid may be undetectable.

Tests include deliberately corrupted volumes and three reproducible random seeds
covering 432 mixed operations per linkage. An independent byte-array and namespace
model checks writes, patches, resizes, streams, file/directory renames, removal,
commit, rollback, close-without-commit, and visibility from a second connection.
Failures print the seed, batch, and step. Static tests additionally inject chunk
statement failures to verify preservation of earlier transaction work.

## Host contract

The authoritative ABI is [include/fslite.h](include/fslite.h).

- The host owns input/output buffers. List callback arguments are borrowed only
  for the duration of the callback. No Zig slices or allocator objects cross the ABI.
- Close each handle once. Null close is permitted. Invalid non-null pointers,
  unterminated strings, insufficient buffers, and use-after-close violate the ABI.
- Serialize all use of an individual handle. Callbacks must not reenter it.
  Independent handles coordinate through SQLite locking, with a 1-second busy
  timeout. FSLite never silently retries an entire caller transaction.
- Paths are absolute within the volume. Parents must exist. No `.` or `..`,
  repeated separators, trailing slash except root, or implicit normalization.
- `FSLITE_OPEN_CREATE` creates a missing database or initializes an empty one;
  it does not overwrite existing files or adopt another application's database.
  `:memory:` with this flag creates an ephemeral volume. Filenames are not URIs.
- `begin` reserves the SQLite writer. Operations participate until commit or
  rollback. Normal operation errors preserve earlier transaction work; severe
  storage errors can cause SQLite to roll back. Check `fslite_in_transaction`.
- SQLite's default rollback journal and FULL synchronous setting are retained.
  A live volume can have a journal sidecar. Use the export/backup API for live
  snapshots; raw filesystem copies require a closed, quiescent volume.
- The shared library hides bundled SQLite symbols. The static archive includes
  SQLite symbols: hosts already embedding SQLite need a deliberate linking
  strategy before integration.

## Next milestones

1. Broader fault-injection and concurrent workloads; optional recursive removal.
2. Benchmark baselines across hardware, explicit format migration, and optional sparse storage.
3. Richer metadata, configurable limits, diagnostics, and backup-platform coverage.
4. C ABI compatibility tests across platforms and release packaging.
5. Optional bindings, CLI, filesystem mounts, and browser storage integration.

## Layout

```text
include/fslite.h      Public C contract
src/c_api.zig         ABI validation and status mapping
src/engine.zig        File operations and SQLite ownership
src/backup_file.c     Private staging and atomic host-file publication
src/schema.sql       Native volume schema
tests/abi.c          C consumer integration tests
tests/namespace.c    Rename/removal and transaction tests
tests/backup.c       Snapshot, cancellation, publication and failure tests
tests/random_access.c Offset writes, truncation, byte-array model and failure tests
tests/integrity.c    Corrupt-volume and check transaction-lifecycle tests
tests/transactions.c Reproducible randomized transactional model tests
examples/basic.c     Minimal host application
examples/stream.c    Bounded-buffer stdin import
examples/backup.c    Live export to a new volume file
examples/random_access.c Transactional patching and resizing
vendor/sqlite/       Unmodified SQLite amalgamation and provenance
```

See [docs/format.md](docs/format.md) for the experimental format. Language
bindings are consumers of this engine; they must not reimplement its semantics.
