# FSLite experimental volume format 2

A volume is a SQLite database with `application_id = 0x46534c54` (`FSLT`) and
`user_version = 2`. Both must match when opening an existing volume. Format 1
stored content in `nodes.data`; it is rejected without modification. There is no
automatic migration. Retain the previous engine to export version-1 data.
ABI revision and volume format version are independent identifiers.

`nodes` stores the hierarchy:

| Column | Meaning |
| --- | --- |
| `id` | Positive integer node identity, root is 1 |
| `parent` | Parent node ID, NULL only for root |
| `name` | UTF-8 basename, compared bytewise |
| `kind` | 1 for a regular file, 2 for a directory |
| `size` | Nonnegative signed 64-bit byte length; zero for directories |

The root has an empty name. Other nodes have a unique `(parent, name)` pair.
The engine requires parent nodes to be directories. Replacing content and moving
a node preserve its ID. Deleted IDs can be reused by later creations. There are
no hard links, symlinks, permissions, timestamps, or deduplication.

`chunks` is a WITHOUT ROWID table keyed by `(node_id, chunk_index)`:

| Column | Meaning |
| --- | --- |
| `node_id` | Foreign key to the owning regular file |
| `chunk_index` | Zero-based chunk number |
| `data` | Nonempty BLOB of at most 65,536 bytes |

File chunks are contiguous from index zero. Every chunk except the last is
exactly 65,536 bytes; the last contains the remaining 1–65,536 bytes. Empty files
and directories have no chunks. The sum of chunk lengths equals `nodes.size`.
Chunk size is fixed for this format; it is an initial engineering choice, not a
benchmark result. File size is capped at INT64_MAX; SQLite database/disk limits
can impose a smaller practical maximum.

Ranged reads select only overlapping chunks and validate their sequence and
expected lengths. Missing or malformed selected chunks return INVALID_FORMAT,
not successful short reads or synthesized zeros. This is not a whole-volume
integrity audit: unrelated chunks and extra trailing chunks are not scanned.

## Streaming transactions

Writer open creates a savepoint, creates or identifies the destination node,
deletes old chunks, and reserves the volume handle. Those changes are uncommitted
and ordinary operations on that handle are blocked until the writer detaches.
One 64 KiB writer buffer accumulates input; full chunks are inserted immediately.
Finish inserts the final partial chunk, updates size, and releases the savepoint.
There are no persistent staging objects or orphan cleanup jobs.

Each stream is atomic. Aborting rolls back to the old chunks/size or removes a
new file entirely. An outer explicit transaction retains earlier work when a
stream aborts, unless SQLite has automatically rolled back that transaction.
When a stream owns the transaction, any error in finish rolls it back completely,
including a BUSY error at commit. When nested, successful finish leaves changes
pending in the caller transaction. Transactions never span multiple volumes.

Finished or aborted writer handles are detached from the volume and must still
be closed. Closing the volume aborts an active stream and detaches its writer,
allowing the host to close that writer safely afterward. Further writes/finish
on detached handles return INVALID_STATE.

## Random-access mutation

Offset writes and truncation operate on existing regular files and retain node
identity. An offset write preserves bytes outside its range. Growth zero-fills
new bytes; a zero-length write validates the path and offset without extending
the file. Truncation to the current size is also a no-op. Both enforce the signed
64-bit format size limit before modifying content.

The engine reads and validates each affected existing chunk, patches it in a
reused 64 KiB heap buffer, and upserts the new chunk data. Unaffected chunks are
not rewritten. Extension pads the old partial tail and inserts all intervening
chunks as needed; no sparse holes are introduced. Shrinking deletes chunks beyond
the new EOF and shortens a retained partial tail. Later growth cannot expose
discarded bytes because deleted/shortened data is replaced with zeros.

The node size changes after the chunk operations within the same savepoint.
Any failure restores the pre-call chunks and size. Successful calls in an outer
transaction stay pending until its commit. Missing/malformed chunks that must be
retained return INVALID_FORMAT; mutation is not a whole-file integrity audit.
The schema and format version stay unchanged at version 2.

## Namespace mutations

Rename updates the source node's `parent` and `name`; descendants and content
chunks keep their existing IDs and references. When replacing a destination, its
node is deleted first, cascading to its chunks. Both statements belong to one
savepoint, so failure restores the destination and source. Only like types may
replace each other, and a replaced directory must be empty.

Removal deletes one file or empty directory. The existing foreign key from
`chunks.node_id` cascades the file's chunk deletion; `nodes.parent` restricts
deletion of directories that still have children. The engine checks emptiness
before mutation and reports NOT_EMPTY. No recursive deletion is exposed.

The root cannot be moved, replaced, or deleted. Canonical path validation and
descendant checks prevent cycles. A move to a longer directory path checks the
subtree's relative byte lengths so all descendant paths remain within the
4096-byte API limit. Namespace mutations share the same savepoint/transaction
rules as other operations and are blocked by an active writer on that handle.
No schema change or migration is required for these operations on format 2.

## Live snapshots

Export uses SQLite's online backup API to copy the complete database, including
format markers, hierarchy, chunks, and IDs. It produces another format-2 volume;
no schema migration or re-encoding occurs. It excludes uncommitted caller data.
The source handle must be idle and is reserved while backup is active, but other
connections may commit between steps. SQLite restarts copying as needed to avoid
mixing database versions. The final copy need not represent backup-open time.
After copy completion it remains frozen even if publication is delayed.

The destination database lives in a private directory adjacent to the final
destination until finish. Incomplete backup transactions are rolled back before
closing; a completed but unpublished backup is still discarded on cancellation.
Successful finish closes SQLite, syncs the staging file, links it to an unused
destination name, and syncs the parent directory. The destination never contains
a partially copied database. Existing destination files, links, directories, and
SQLite sidecars are rejected. The source database is never rewritten by backup.

A directory-sync failure after linking returns DURABILITY_UNCERTAIN: a complete
destination is visible but crash persistence of the new directory entry was not
confirmed. A crash may leave private staging directories. Cleanup after normal
failure is best effort if host filesystem operations themselves fail. Export is
not a transaction spanning the source and destination files: successful source
commits can happen independently during copying.

## Durability and inspection

SQLite's default rollback journal and synchronous FULL are retained. A live
volume can have a `-journal` file. Large writes may spill the page cache and
acquire locks that block other readers. Memory does not intentionally grow with
the complete input; rollback journal/disk space can grow with the transaction.
The writer's 64 KiB buffer is additional to SQLite's cache and other allocations.

Raw filesystem copying requires a closed, quiescent volume; use live export for
an open source. Do not discard a journal left after a crash; SQLite needs it for
recovery. WAL configuration is not exposed by this version.

Inspecting volumes with SQLite tools is useful for diagnostics. Direct schema or
data mutation is unsupported: SQL constraints enforce only part of the filesystem
invariants. Open validates format markers and the root; read checks only requested
content. `fslite_check` performs a whole-volume scan of SQLite integrity and foreign
keys, markers, node types, names, parents, reachability/path limits, and dense
chunk ownership, indices, counts, and lengths. The scan shares one savepoint
snapshot and includes caller-transaction changes without committing them. It is
read-only and returns INVALID_FORMAT on detected corruption; it does not repair,
authenticate the schema/content, or migrate the format. ABI revision 6 adds this
function without changing volume format 2.
