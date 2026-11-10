#ifndef FSLITE_H
#define FSLITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental ABI revision 7 adds extended metadata, configurable limits, and
 * volume diagnostics. Existing signatures, layouts, status codes and kind
 * values are unchanged and use fixed widths. */
typedef struct fslite_volume fslite_volume;
typedef struct fslite_writer fslite_writer;
typedef struct fslite_backup fslite_backup;
typedef int32_t fslite_status;
#define FSLITE_OK               0
#define FSLITE_INVALID_ARGUMENT 1
#define FSLITE_NOT_FOUND        2
#define FSLITE_ALREADY_EXISTS   3
#define FSLITE_NOT_DIRECTORY    4
#define FSLITE_IS_DIRECTORY     5
#define FSLITE_BUSY             6
#define FSLITE_OUT_OF_MEMORY    7
#define FSLITE_IO_ERROR         8
#define FSLITE_INVALID_FORMAT   9
#define FSLITE_INVALID_STATE    10
#define FSLITE_TOO_LARGE        11
#define FSLITE_CANCELLED        12
#define FSLITE_INTERNAL_ERROR   13
#define FSLITE_NOT_EMPTY        14
#define FSLITE_UNSUPPORTED      15
#define FSLITE_DURABILITY_UNCERTAIN 16

#define FSLITE_OPEN_CREATE UINT32_C(1)
#define FSLITE_OPEN_SPARSE UINT32_C(2) /* Skip storing all-zero chunks */
#define FSLITE_FILE        UINT32_C(1)
#define FSLITE_DIRECTORY   UINT32_C(2)

typedef struct fslite_info {
    uint64_t id;
    uint64_t size;
    uint32_t kind;
    uint32_t reserved; /* Always zero. */
} fslite_info;

/* Extended metadata for one path. chunks and allocated count physically stored
 * chunk rows, so sparse holes reduce both without changing size. depth is the
 * number of path components (root is 0). reserved is always zero. */
typedef struct fslite_file_info {
    uint64_t id;
    uint64_t size;
    uint64_t chunks;
    uint64_t allocated;
    uint32_t kind;
    uint32_t depth;
    uint32_t reserved[2]; /* Always zero. */
} fslite_file_info;

/* Per-handle enforcement limits. A zero field selects the default:
 * max_path_bytes 4096, max_name_bytes 255, max_file_size INT64_MAX,
 * max_volume_bytes unlimited. Limits apply to operations performed by this
 * handle; separate handles configure independently. A nonzero max_path_bytes
 * must be 2..4096 and max_name_bytes 1..255; larger values are rejected.
 * Limits can only change while quiescent (no active writer or backup, not
 * inside a transaction). */
typedef struct fslite_limits {
    uint64_t max_path_bytes;
    uint64_t max_name_bytes;
    uint64_t max_file_size;
    uint64_t max_volume_bytes;
    uint32_t reserved[4]; /* Always zero. */
} fslite_limits;

/* Whole-volume diagnostics. directory_count includes the root. chunk_count and
 * chunk_bytes describe physically stored rows, excluding sparse holes.
 * database_bytes is page_size * page_count (including free pages). */
typedef struct fslite_volume_stats {
    uint64_t file_count;
    uint64_t directory_count;
    uint64_t chunk_count;
    uint64_t chunk_bytes;
    uint64_t logical_bytes;
    uint64_t page_size;
    uint64_t page_count;
    uint64_t freelist_pages;
    uint64_t database_bytes;
    uint32_t in_transaction;
    uint32_t sparse;
    uint32_t reserved[2]; /* Always zero. */
} fslite_volume_stats;

uint32_t fslite_abi_version(void);
/* Returns static storage, never free it. Unknown codes return "unknown status". */
const char *fslite_status_string(fslite_status status);

/* filename is a host path (UTF-8), not a URI. CREATE initializes only an empty
 * database; existing foreign databases are rejected. No flag means open-existing.
 * FSLITE_OPEN_SPARSE lets writers skip all-zero chunks; missing
 * chunks read as zeros. The flag is a handle-level policy and is not recorded in
 * the volume, so every handle that must read holes as zeros must set it.
 * All strings must be NUL-terminated. out_volume is cleared on failure.
 * One handle may be used by one thread at a time; callers serialize access.
 * Separate handles use SQLite locking. No background threads are created.
 */
fslite_status fslite_open(const char *filename, uint32_t flags, fslite_volume **out_volume);
/* Aborts an unfinished writer or backup, rolls back an active transaction, and
 * frees the volume. Outstanding writer/backup handles remain valid only for
 * closing; further operations on them return INVALID_STATE. NULL is allowed.
 * Every non-NULL handle must be closed exactly once and never reused afterward. */
void fslite_close(fslite_volume *volume);

/* Volume paths are absolute UTF-8 paths. '/' is the root. Maximum 4096 bytes
 * total and 255 bytes per component. Empty components, trailing '/', '.', and
 * '..' are rejected. Names are case-sensitive; no Unicode normalization occurs.
 * Parent directories must exist. Permissions and symbolic links are not modeled.
 */
fslite_status fslite_mkdir(fslite_volume *volume, const char *path);
/* Atomically move a file or directory within this volume, preserving the source
 * ID, descendants, and content. Destination means the exact new path, not a
 * container directory. Its parent must exist. An existing file may replace a
 * file; a directory may replace an EMPTY directory. File-to-directory returns
 * IS_DIRECTORY; directory-to-file returns NOT_DIRECTORY; a nonempty destination
 * directory returns NOT_EMPTY. The replaced destination's ID and chunks are
 * removed. An existing identical source/destination path succeeds unchanged.
 * Root mutations, moves into a directory's own descendants, and moves making
 * descendant paths longer than 4096 UTF-8 bytes return INVALID_ARGUMENT.
 * Missing sources return NOT_FOUND, including identical missing paths.
 */
fslite_status fslite_rename(fslite_volume *volume, const char *source, const char *destination);
/* Atomically delete a file and its chunks, or an empty directory. Nonrecursive:
 * a nonempty directory returns NOT_EMPTY. Root returns INVALID_ARGUMENT, and a
 * missing path returns NOT_FOUND. Deleted IDs may be reused by future creations.
 * SQLite reuses freed pages; deletion need not shrink the host database file.
 * Both rename and remove use savepoints, participate in explicit transactions,
 * and return INVALID_STATE while a streaming writer reserves this volume handle.
 */
fslite_status fslite_remove(fslite_volume *volume, const char *path);
/* Recursively delete a directory tree: removes all descendants first
 * (depth-first post-order), then the directory itself, then the file's chunks.
 * A file path removes just that file. Missing paths return NOT_FOUND; root
 * returns INVALID_ARGUMENT. All operations use savepoints and participate
 * in explicit transactions.
 */
fslite_status fslite_remove_recursive(fslite_volume *volume, const char *path);
/* Migrate a volume from format version 1 to version 2. If already v2, this is
 * a no-op. Opens the volume without format validation so foreign-format volumes
 * can be migrated. Returns InvalidFormat if the schema is unrecognized and
 * cannot be migrated. */
fslite_status fslite_migrate(const char *path);
/* Atomically create or replace a whole file, preserving its ID on replacement.
 * data may be NULL only when size is zero. Uses the same chunked writer as the
 * streaming API. File size is limited to INT64_MAX; storage limits also apply.
 * Input buffers are borrowed only for the duration of the call. */
fslite_status fslite_write(fslite_volume *volume, const char *path, const void *data, size_t size);

/* Atomically patch an EXISTING regular file at a byte offset, preserving its ID
 * and all bytes outside [offset,offset+size). Missing files return NOT_FOUND;
 * directories return IS_DIRECTORY. A nonempty write past EOF extends the file
 * and fills any gap with zeros. A zero-length write validates the path and offset
 * but does not extend the file. NULL data is allowed only for size=0.
 * size must be <= PTRDIFF_MAX; offset and offset+size must be <= INT64_MAX.
 * Oversized requests return TOO_LARGE before modifying content. The input buffer
 * is borrowed only for this call. There is no partial-success byte count.
 */
fslite_status fslite_write_at(fslite_volume *volume, const char *path, uint64_t offset,
                             const void *data, size_t size);
/* Set an EXISTING regular file's size, preserving its ID and retained prefix.
 * Shrinking discards the tail; growing adds zeros (discarded bytes never return).
 * size must be <= INT64_MAX. Same-size truncation validates the path and is a no-op.
 * Both operations use one savepoint and participate in an explicit transaction.
 * Errors roll back the whole operation, preserving prior transaction work unless
 * SQLite aborts that transaction. Active writers/backups return INVALID_STATE.
 * Dense handles materialize zero-filled chunks and may consume time/disk
 * proportional to the gap; FSLITE_OPEN_SPARSE handles skip all-zero chunks and
 * leave holes. There are no implicit creates.
 */
fslite_status fslite_truncate(fslite_volume *volume, const char *path, uint64_t size);

/* Atomic, sequential replacement of one file. open reserves the volume handle
 * and starts a savepoint (inside an explicit transaction if one exists).
 * Until finish or close, other operations on that handle, including transaction
 * control and opening another writer, return INVALID_STATE. in_transaction and
 * volume close remain allowed. Independent handles never see partial content,
 * but may return BUSY: large streams may spill SQLite's cache and block readers.
 * out_writer is required and cleared on failure. No implicit parent creation.
 */
fslite_status fslite_writer_open(fslite_volume *volume, const char *path, fslite_writer **out_writer);
/* Appends all input on success, borrowing it only for this call. Call boundaries
 * need not match the 64 KiB storage chunks. NULL data is valid for zero bytes.
 * Pointer/length validation errors leave the stream unchanged; storage errors
 * or cumulative size overflow abort the ENTIRE replacement and detach the writer.
 * A single call is limited to PTRDIFF_MAX bytes. A detached writer returns
 * INVALID_STATE and must still be closed. No bytes-accepted/partial retry contract.
 */
fslite_status fslite_writer_write(fslite_writer *writer, const void *data, size_t size);
/* Flushes the final chunk and publishes the replacement. Within an explicit
 * transaction it remains uncommitted until fslite_commit. Any finish error aborts
 * the replacement (including BUSY); retry requires a new writer. Success also
 * detaches the writer. Always close the handle afterward, even on error.
 */
fslite_status fslite_writer_finish(fslite_writer *writer);
/* Aborts an unfinished replacement and frees the writer. Prior successful work
 * in an outer transaction is preserved unless SQLite has aborted that transaction.
 * NULL is allowed. Every non-NULL writer must be closed exactly once.
 */
void fslite_writer_close(fslite_writer *writer);
/* Read at most capacity bytes starting at offset. EOF succeeds with zero bytes.
 * buffer may be NULL only when capacity is zero. out_read is required and is
 * cleared on failure. capacity must be <= PTRDIFF_MAX. Caller owns the buffer;
 * there is no library buffer to free. Discard buffer contents on any error. */
fslite_status fslite_read(fslite_volume *volume, const char *path, uint64_t offset,
                         void *buffer, size_t capacity, size_t *out_read);
fslite_status fslite_stat(fslite_volume *volume, const char *path, fslite_info *out_info);
/* Extended metadata for one path. out_info is cleared on failure. Reads one
 * consistent snapshot, including uncommitted work in the caller's transaction.
 * Missing paths return NOT_FOUND; directories report zero chunks/allocated.
 * Active writers/backups return INVALID_STATE. */
fslite_status fslite_inspect(fslite_volume *volume, const char *path, fslite_file_info *out_info);
/* Apply per-handle limits. A NULL pointer restores every default. Zero fields
 * also keep their default. Returns INVALID_STATE while a writer or backup is
 * active or inside an explicit transaction, and INVALID_ARGUMENT for values
 * outside the ranges documented on fslite_limits. Existing data is never
 * deleted. A size limit below current usage only makes growth fail with
 * TOO_LARGE; lowering path/name limits can make existing longer paths
 * inaccessible through this handle without modifying them. */
fslite_status fslite_set_limits(fslite_volume *volume, const fslite_limits *limits);
/* Whole-volume diagnostics; out_stats is cleared on failure. Values describe
 * this handle's current view, including its pending transaction work.
 * Active writers/backups return INVALID_STATE. */
fslite_status fslite_get_volume_stats(fslite_volume *volume, fslite_volume_stats *out_stats);

/* Immediate children in bytewise name order. name and info are borrowed and
 * valid only during the callback. Return nonzero to stop (FSLITE_CANCELLED).
 * A callback must not reenter or close the same volume. */
typedef int32_t (*fslite_list_callback)(void *context, const char *name, size_t name_size,
                                      const fslite_info *info);
fslite_status fslite_list(fslite_volume *volume, const char *path,
                         fslite_list_callback callback, void *context);

/* Read-only whole-volume integrity check: SQLite integrity/foreign keys, format
 * markers, node types, root/parent/name/path validity, reachability, and chunk
 * ownership, ordering and lengths. Completeness is required only for dense
 * handles; sparse handles accept holes. Returns INVALID_FORMAT
 * for detected corruption or an unusable format schema; never repairs data.
 * All scans share one snapshot. Inside a caller transaction this includes its
 * uncommitted work and leaves it active, on success or ordinary check failure.
 * Outside a transaction the check releases its read transaction before returning.
 * Severe SQLite errors may abort a caller transaction; use in_transaction.
 * Active writers/backups return INVALID_STATE. NULL returns INVALID_ARGUMENT.
 * This synchronous scan can be expensive and can block commits on other handles.
 * It does not authenticate content, prove schema provenance, or simulate recovery
 * from power loss. Same-length content corruption may not be detectable.
 */
fslite_status fslite_check(fslite_volume *volume);

/* One transaction per handle, no nesting. begin obtains the writer reservation.
 * All subsequent operations on this handle participate until commit/rollback.
 * Each operation has its own savepoint. Ordinary operation errors do not undo
 * earlier successful operations. Severe SQLite errors can abort a transaction;
 * use in_transaction to check. A failed commit may leave it active (e.g. BUSY).
 */
fslite_status fslite_begin(fslite_volume *volume);
fslite_status fslite_commit(fslite_volume *volume);
fslite_status fslite_rollback(fslite_volume *volume);
uint32_t fslite_in_transaction(fslite_volume *volume);

typedef struct fslite_backup_progress {
    uint64_t remaining_pages;
    uint64_t total_pages;
    uint32_t done;
    uint32_t reserved; /* Always zero. */
} fslite_backup_progress;

/* Export a consistent committed snapshot to a NEW host filesystem path.
 * Equivalent to open, repeated 256-page steps, finish, close. Source stays open.
 * Rejects a source handle with an active transaction, writer, or backup. Never
 * commits or rolls back caller work. BUSY aborts this convenience operation;
 * incremental callers can retry a BUSY step instead. Continuous external writes
 * can repeatedly restart copying; use incremental backup for a work/time budget.
 * Destination and its SQLite sidecar names must be unused. Existing files,
 * directories, symlinks (even dangling), and source aliases return ALREADY_EXISTS.
 * POSIX implementation needs a local filesystem with hard links and directory
 * fsync; Windows returns UNSUPPORTED. Parent directory must exist and remain
 * stable/trusted during backup. No overwrite or cross-volume transaction API.
 */
fslite_status fslite_export(fslite_volume *source, const char *destination);

/* Reserves the source handle until finish/close. Ordinary operations on that
 * handle return INVALID_STATE; in_transaction and volume close remain allowed.
 * Other source connections can read/write between steps. Such writes may restart
 * the copy: the result is a consistent committed state, NOT necessarily the state
 * at open time. A completed copy (progress.done=1) is frozen until publication.
 * Uses private adjacent staging storage; destination stays absent until finish.
 * Destination is a UTF-8 host path, not a URI or ':memory:'. out_backup is cleared
 * on failure. An in-memory SOURCE is supported.
 */
fslite_status fslite_backup_open(fslite_volume *source, const char *destination, fslite_backup **out_backup);
/* Copy up to pages pages (1..INT32_MAX). Progress is valid only on OK; outputs
 * are cleared on error. Counters need not be monotonic when other writers commit.
 * BUSY keeps the backup active for retry. Fatal storage errors abort and detach
 * it. Invalid arguments leave it active. No background work occurs between calls.
 */
fslite_status fslite_backup_step(fslite_backup *backup, uint32_t pages, fslite_backup_progress *out_progress);
/* Requires done=1; otherwise INVALID_STATE leaves the backup active. Finalizes
 * SQLite, syncs the completed file, and atomically publishes without overwriting.
 * OK confirms file and parent-directory fsync. DURABILITY_UNCERTAIN means a
 * COMPLETE destination WAS published, but directory fsync failed; do not treat
 * this as an absent destination. Other failures publish nothing. After an actual
 * publication attempt the backup detaches on success or failure. Always close it.
 */
fslite_status fslite_backup_finish(fslite_backup *backup);
/* Abort and remove staging resources, or free a finished/detached handle.
 * Closing the source volume also aborts/detaches its backup; the backup handle
 * must still be closed once. NULL is allowed. Cleanup is best effort on OS errors.
 * A process crash may leave a private .fslite-backup-* directory for host cleanup;
 * a crash before publication never exposes a partial destination file.
 */
void fslite_backup_close(fslite_backup *backup);

#ifdef __cplusplus
}
#endif
#endif
