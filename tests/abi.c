#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef FSLITE_TEST_SQLITE
#include "sqlite3.h"
#endif

_Static_assert(sizeof(fslite_status) == 4, "fixed-width status");
_Static_assert(sizeof(fslite_info) == 24, "ABI info layout");
_Static_assert(offsetof(fslite_info, kind) == 16, "ABI kind offset");
_Static_assert(sizeof(fslite_file_info) == 48, "ABI file info layout");
_Static_assert(offsetof(fslite_file_info, kind) == 32, "ABI file info kind offset");
_Static_assert(sizeof(fslite_limits) == 48, "ABI limits layout");
_Static_assert(offsetof(fslite_limits, max_volume_bytes) == 24, "ABI limits offset");
_Static_assert(sizeof(fslite_volume_stats) == 88, "ABI stats layout");
_Static_assert(offsetof(fslite_volume_stats, in_transaction) == 72, "ABI stats offset");

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); } } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: got %s (%d), wanted %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); } } while (0)
#define OK(call) STATUS(call, FSLITE_OK)

static void new_path(char path[64]) {
    strcpy(path, "/tmp/fslite-abi-XXXXXX");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
}

struct listing { size_t count; };
static int32_t collect(void *context, const char *name, size_t len, const fslite_info *info) {
    struct listing *state = context;
    const char *expected[] = { "a-empty", "b-binary" };
    CHECK(state->count < 2);
    CHECK(len == strlen(expected[state->count]));
    CHECK(memcmp(name, expected[state->count], len) == 0);
    CHECK(info->kind == FSLITE_FILE && info->reserved == 0);
    state->count++;
    return 0;
}
static int32_t cancel(void *context, const char *name, size_t len, const fslite_info *info) {
    (void)context; (void)name; (void)len; (void)info;
    return 1;
}

static void test_engine(void) {
    char filename[64];
    new_path(filename);
    fslite_volume *v = NULL, *other = NULL;
    CHECK(fslite_abi_version() == 7);
    STATUS(fslite_open(filename, 0, &v), FSLITE_INVALID_FORMAT);
    CHECK(v == NULL);
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    fslite_info info;
    OK(fslite_stat(v, "/", &info));
    CHECK(info.id == 1 && info.kind == FSLITE_DIRECTORY);
    OK(fslite_mkdir(v, "/docs"));
    STATUS(fslite_mkdir(v, "/docs"), FSLITE_ALREADY_EXISTS);
    STATUS(fslite_mkdir(v, "/"), FSLITE_ALREADY_EXISTS);
    STATUS(fslite_write(v, "/missing/file", "x", 1), FSLITE_NOT_FOUND);
    STATUS(fslite_write(v, "/docs", "x", 1), FSLITE_IS_DIRECTORY);
    OK(fslite_write(v, "/docs/a-empty", NULL, 0));
    const unsigned char payload[] = { 'A', 0, 'B', 255, 'C' };
    OK(fslite_write(v, "/docs/b-binary", payload, sizeof payload));
    OK(fslite_stat(v, "/docs/b-binary", &info));
    uint64_t id = info.id;
    CHECK(info.size == sizeof payload && info.kind == FSLITE_FILE);
    unsigned char output[16] = {0};
    size_t count = 99;
    OK(fslite_read(v, "/docs/b-binary", 1, output, 3, &count));
    CHECK(count == 3 && memcmp(output, payload + 1, 3) == 0);
    OK(fslite_read(v, "/docs/b-binary", UINT64_MAX, output, sizeof output, &count));
    CHECK(count == 0);
    OK(fslite_read(v, "/docs/a-empty", 0, NULL, 0, &count));
    CHECK(count == 0);
    STATUS(fslite_read(v, "/docs", 0, output, sizeof output, &count), FSLITE_IS_DIRECTORY);
    CHECK(count == 0);
    STATUS(fslite_mkdir(v, "/docs/b-binary/child"), FSLITE_NOT_DIRECTORY);
    struct listing listing = {0};
    OK(fslite_list(v, "/docs", collect, &listing));
    CHECK(listing.count == 2);
    STATUS(fslite_list(v, "/docs", cancel, NULL), FSLITE_CANCELLED);
    CHECK(!fslite_in_transaction(v));
    STATUS(fslite_list(v, "/docs/b-binary", cancel, NULL), FSLITE_NOT_DIRECTORY);

    const char *bad_paths[] = { "", "relative", "/docs/", "/docs//x", "/./x", "/docs/../x", "/\xff" };
    for (size_t i = 0; i < sizeof bad_paths / sizeof *bad_paths; ++i)
        STATUS(fslite_stat(v, bad_paths[i], &info), FSLITE_INVALID_ARGUMENT);
    char long_component[258];
    long_component[0] = '/'; memset(long_component + 1, 'x', 256); long_component[257] = 0;
    STATUS(fslite_mkdir(v, long_component), FSLITE_INVALID_ARGUMENT);
    OK(fslite_mkdir(v, "/資料"));
    OK(fslite_write(v, "/資料/hello.txt", "world", 5));

    STATUS(fslite_commit(v), FSLITE_INVALID_STATE);
    STATUS(fslite_rollback(v), FSLITE_INVALID_STATE);
    OK(fslite_open(filename, 0, &other));
    OK(fslite_begin(v));
    CHECK(fslite_in_transaction(v));
    STATUS(fslite_begin(v), FSLITE_INVALID_STATE);
    OK(fslite_write(v, "/docs/b-binary", "replacement", 11));
    OK(fslite_mkdir(v, "/rolled-back"));
    STATUS(fslite_write(v, "/missing/file", "x", 1), FSLITE_NOT_FOUND);
    CHECK(fslite_in_transaction(v));
    OK(fslite_read(other, "/docs/b-binary", 0, output, sizeof output, &count));
    CHECK(count == sizeof payload && memcmp(output, payload, count) == 0);
    STATUS(fslite_begin(other), FSLITE_BUSY);
    CHECK(!fslite_in_transaction(other));
    OK(fslite_rollback(v));
    STATUS(fslite_stat(v, "/rolled-back", &info), FSLITE_NOT_FOUND);
    OK(fslite_read(v, "/docs/b-binary", 0, output, sizeof output, &count));
    CHECK(count == sizeof payload && memcmp(output, payload, count) == 0);
    OK(fslite_begin(v));
    OK(fslite_write(v, "/docs/b-binary", "new", 3));
    // A failed call rolls back only itself, not this earlier successful write.
    STATUS(fslite_mkdir(v, "/docs"), FSLITE_ALREADY_EXISTS);
    OK(fslite_commit(v));
    OK(fslite_stat(other, "/docs/b-binary", &info));
    CHECK(info.id == id && info.size == 3);
    OK(fslite_write(v, "/docs/b-binary", NULL, 0));
    OK(fslite_stat(v, "/docs/b-binary", &info));
    CHECK(info.id == id && info.size == 0);
    OK(fslite_begin(v));
    OK(fslite_write(v, "/close-rollback", "x", 1));
    fslite_close(v);
    fslite_close(other);

    OK(fslite_open(filename, 0, &v));
    STATUS(fslite_stat(v, "/close-rollback", &info), FSLITE_NOT_FOUND);
    OK(fslite_read(v, "/資料/hello.txt", 0, output, sizeof output, &count));
    CHECK(count == 5 && memcmp(output, "world", 5) == 0);
    fslite_close(v);

    // A process exiting without cleanup must not persist uncommitted changes.
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        OK(fslite_open(filename, 0, &v));
        OK(fslite_begin(v));
        OK(fslite_write(v, "/interrupted", "x", 1));
        _exit(0);
    }
    int child_status = 0;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    OK(fslite_open(filename, 0, &v));
    STATUS(fslite_stat(v, "/interrupted", &info), FSLITE_NOT_FOUND);
    OK(fslite_stat(v, "/資料/hello.txt", &info));
    fslite_close(v);
    CHECK(unlink(filename) == 0);
    STATUS(fslite_open(filename, 0, &v), FSLITE_IO_ERROR);
    CHECK(v == NULL && access(filename, F_OK) != 0);
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    fslite_close(v);
    CHECK(unlink(filename) == 0);
}

static void test_invalid_arguments(void) {
    fslite_volume *v = (fslite_volume *)(uintptr_t)1;
    STATUS(fslite_open(NULL, 0, &v), FSLITE_INVALID_ARGUMENT);
    CHECK(v == NULL);
    STATUS(fslite_open("unused", 4, &v), FSLITE_INVALID_ARGUMENT);
    CHECK(v == NULL);
    STATUS(fslite_open("unused", 0, NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_begin(NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_write(NULL, "/x", NULL, 1), FSLITE_INVALID_ARGUMENT);
    size_t count = 99;
    STATUS(fslite_read(NULL, "/x", 0, NULL, 0, &count), FSLITE_INVALID_ARGUMENT);
    CHECK(count == 0);
    fslite_close(NULL);
    CHECK(strcmp(fslite_status_string(-1), "unknown status") == 0);
    OK(fslite_open(":memory:", FSLITE_OPEN_CREATE, &v));
    STATUS(fslite_write(v, "/x", NULL, 1), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_write(v, "/x", "x", SIZE_MAX), FSLITE_TOO_LARGE);
    STATUS(fslite_read(v, "/x", 0, NULL, 1, &count), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_stat(v, "/", NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_list(v, "/", NULL, NULL), FSLITE_INVALID_ARGUMENT);
    fslite_close(v);
}

enum { CHUNK = 65536, PAYLOAD_SIZE = CHUNK * 3 + 17 };

static void verify_bytes(fslite_volume *v, const char *path, const unsigned char *expected, size_t length) {
    const size_t allocation = length + 16 > CHUNK + 7 ? length + 16 : CHUNK + 7;
    unsigned char *output = malloc(allocation);
    CHECK(output != NULL);
    const uint64_t offsets[] = { 0, 1, CHUNK-3, CHUNK-1, CHUNK, CHUNK+1, CHUNK*2-1,
                                 length, length+1, UINT64_MAX };
    const size_t capacities[] = { 0, 1, 9, CHUNK+7, length+16 };
    for (size_t i = 0; i < sizeof offsets / sizeof *offsets; ++i) {
        for (size_t j = 0; j < sizeof capacities / sizeof *capacities; ++j) {
            size_t count = SIZE_MAX;
            size_t wanted = offsets[i] >= length ? 0 : length - (size_t)offsets[i];
            if (wanted > capacities[j]) wanted = capacities[j];
            OK(fslite_read(v, path, offsets[i], output, capacities[j], &count));
            CHECK(count == wanted);
            if (wanted) CHECK(memcmp(output, expected + offsets[i], wanted) == 0);
        }
    }
    free(output);
}

static void test_streaming(void) {
    char filename[64]; new_path(filename);
    fslite_volume *v = NULL, *other = NULL;
    fslite_writer *writer = NULL, *blocked = NULL;
    fslite_info info;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    OK(fslite_open(filename, 0, &other));
    unsigned char *payload = malloc(PAYLOAD_SIZE);
    CHECK(payload != NULL);
    for (size_t i = 0; i < PAYLOAD_SIZE; ++i) payload[i] = (unsigned char)((i * 17 + i / 251) % 256);
    OK(fslite_write(v, "/file", "old", 3));
    OK(fslite_stat(v, "/file", &info));
    const uint64_t id = info.id;
    const size_t lengths[] = { 0, 1, CHUNK-1, CHUNK, CHUNK+1, PAYLOAD_SIZE };
    const size_t fragments[] = { 1, 13, CHUNK-1, CHUNK, CHUNK+1 };
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; ++i) {
        for (size_t j = 0; j < sizeof fragments / sizeof *fragments; ++j) {
            OK(fslite_writer_open(v, "/file", &writer));
            OK(fslite_writer_write(writer, NULL, 0));
            for (size_t offset = 0; offset < lengths[i];) {
                size_t take = lengths[i] - offset;
                if (take > fragments[j]) take = fragments[j];
                OK(fslite_writer_write(writer, payload + offset, take));
                offset += take;
            }
            OK(fslite_writer_finish(writer));
            STATUS(fslite_writer_finish(writer), FSLITE_INVALID_STATE);
            STATUS(fslite_writer_write(writer, payload, 1), FSLITE_INVALID_STATE);
            fslite_writer_close(writer);
            CHECK(!fslite_in_transaction(v));
            OK(fslite_stat(v, "/file", &info));
            CHECK(info.id == id && info.size == lengths[i]);
            verify_bytes(other, "/file", payload, lengths[i]);
        }
    }
    // Whole-buffer writes share the same storage path, including truncation.
    OK(fslite_write(v, "/file", payload, PAYLOAD_SIZE));
    verify_bytes(v, "/file", payload, PAYLOAD_SIZE);
    OK(fslite_write(v, "/file", "old", 3));

    OK(fslite_writer_open(v, "/file", &writer));
    OK(fslite_writer_write(writer, payload, CHUNK+3));
    CHECK(fslite_in_transaction(v));
    STATUS(fslite_writer_open(v, "/blocked", &blocked), FSLITE_INVALID_STATE);
    CHECK(blocked == NULL);
    STATUS(fslite_stat(v, "/file", &info), FSLITE_INVALID_STATE);
    STATUS(fslite_mkdir(v, "/blocked"), FSLITE_INVALID_STATE);
    STATUS(fslite_write(v, "/blocked", "x", 1), FSLITE_INVALID_STATE);
    STATUS(fslite_list(v, "/", cancel, NULL), FSLITE_INVALID_STATE);
    STATUS(fslite_begin(v), FSLITE_INVALID_STATE);
    STATUS(fslite_commit(v), FSLITE_INVALID_STATE);
    STATUS(fslite_rollback(v), FSLITE_INVALID_STATE);
    verify_bytes(other, "/file", (const unsigned char *)"old", 3);
    STATUS(fslite_begin(other), FSLITE_BUSY);
    // ABI argument validation must not change a valid stream.
    STATUS(fslite_writer_write(writer, NULL, 1), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_writer_write(writer, "x", SIZE_MAX), FSLITE_TOO_LARGE);
    fslite_writer_close(writer);
    CHECK(!fslite_in_transaction(v));
    verify_bytes(v, "/file", (const unsigned char *)"old", 3);

    OK(fslite_begin(v));
    OK(fslite_write(v, "/earlier", "keep", 4));
    OK(fslite_writer_open(v, "/abandoned", &writer));
    OK(fslite_writer_write(writer, payload, PAYLOAD_SIZE));
    fslite_writer_close(writer);
    CHECK(fslite_in_transaction(v));
    STATUS(fslite_stat(v, "/abandoned", &info), FSLITE_NOT_FOUND);
    OK(fslite_writer_open(v, "/file", &writer));
    OK(fslite_writer_write(writer, payload, PAYLOAD_SIZE));
    OK(fslite_writer_finish(writer));
    fslite_writer_close(writer);
    verify_bytes(v, "/file", payload, PAYLOAD_SIZE);
    verify_bytes(other, "/file", (const unsigned char *)"old", 3);
    OK(fslite_rollback(v));
    STATUS(fslite_stat(v, "/earlier", &info), FSLITE_NOT_FOUND);
    verify_bytes(v, "/file", (const unsigned char *)"old", 3);
    OK(fslite_begin(v));
    OK(fslite_writer_open(v, "/file", &writer));
    OK(fslite_writer_write(writer, payload, PAYLOAD_SIZE));
    OK(fslite_writer_finish(writer));
    fslite_writer_close(writer);
    OK(fslite_commit(v));
    fslite_close(other);

    // Volume close invalidates an active writer without leaving a dangling volume.
    OK(fslite_writer_open(v, "/file", &writer));
    OK(fslite_writer_write(writer, payload, CHUNK));
    fslite_close(v);
    STATUS(fslite_writer_write(writer, "x", 1), FSLITE_INVALID_STATE);
    STATUS(fslite_writer_finish(writer), FSLITE_INVALID_STATE);
    fslite_writer_close(writer);
    OK(fslite_open(filename, 0, &v));
    verify_bytes(v, "/file", payload, PAYLOAD_SIZE);
    fslite_close(v);

    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        OK(fslite_open(filename, 0, &v));
        OK(fslite_writer_open(v, "/file", &writer));
        // Spill well beyond the SQLite page cache before exiting without cleanup.
        for (int i = 0; i < 32; ++i) OK(fslite_writer_write(writer, payload, PAYLOAD_SIZE));
        _exit(0);
    }
    int child_status = 0;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    OK(fslite_open(filename, 0, &v));
    verify_bytes(v, "/file", payload, PAYLOAD_SIZE);
    fslite_close(v);
    CHECK(unlink(filename) == 0);
    free(payload);

    writer = (fslite_writer *)(uintptr_t)1;
    STATUS(fslite_writer_open(NULL, "/x", &writer), FSLITE_INVALID_ARGUMENT);
    CHECK(writer == NULL);
    STATUS(fslite_writer_open(NULL, "/x", NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_writer_write(NULL, NULL, 0), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_writer_finish(NULL), FSLITE_INVALID_ARGUMENT);
    fslite_writer_close(NULL);
}

#ifdef FSLITE_TEST_SQLITE
static void test_stream_failures_and_format(void) {
    char filename[64]; new_path(filename);
    fslite_volume *v = NULL;
    fslite_writer *writer = NULL;
    sqlite3 *db = NULL;
    fslite_info info;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    OK(fslite_write(v, "/file", "old", 3));
    fslite_close(v);
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "CREATE TRIGGER fail_chunk BEFORE INSERT ON chunks WHEN NEW.chunk_index=1 "
        "BEGIN SELECT RAISE(ABORT,'injected chunk failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    unsigned char *payload = calloc(3, CHUNK);
    CHECK(payload != NULL);
    OK(fslite_open(filename, 0, &v));
    OK(fslite_begin(v));
    OK(fslite_write(v, "/earlier", "keep", 4));
    OK(fslite_writer_open(v, "/file", &writer));
    // One chunk is inserted before the second insert fails. The whole stream rolls back.
    STATUS(fslite_writer_write(writer, payload, CHUNK*2), FSLITE_INTERNAL_ERROR);
    STATUS(fslite_writer_finish(writer), FSLITE_INVALID_STATE);
    fslite_writer_close(writer);
    CHECK(fslite_in_transaction(v));
    verify_bytes(v, "/file", (const unsigned char *)"old", 3);
    OK(fslite_commit(v));
    OK(fslite_stat(v, "/earlier", &info));
    // Exercise a failure flushing the final partial chunk at finish.
    OK(fslite_writer_open(v, "/file", &writer));
    OK(fslite_writer_write(writer, payload, CHUNK+1));
    STATUS(fslite_writer_finish(writer), FSLITE_INTERNAL_ERROR);
    fslite_writer_close(writer);
    CHECK(!fslite_in_transaction(v));
    verify_bytes(v, "/file", (const unsigned char *)"old", 3);
    fslite_close(v);

    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "DROP TRIGGER fail_chunk; CREATE TRIGGER fail_size BEFORE UPDATE OF size ON nodes "
        "WHEN NEW.size>0 BEGIN SELECT RAISE(ABORT,'injected size failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    OK(fslite_open(filename, 0, &v));
    OK(fslite_writer_open(v, "/file", &writer));
    OK(fslite_writer_write(writer, payload, CHUNK+3));
    STATUS(fslite_writer_finish(writer), FSLITE_INTERNAL_ERROR);
    fslite_writer_close(writer);
    CHECK(!fslite_in_transaction(v));
    verify_bytes(v, "/file", (const unsigned char *)"old", 3);
    fslite_close(v);
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "DROP TRIGGER fail_size", NULL, NULL, NULL) == SQLITE_OK);
    // Hold a reader across finish: committing the stream must fail cleanly.
    CHECK(sqlite3_exec(db, "BEGIN; SELECT count(*) FROM nodes", NULL, NULL, NULL) == SQLITE_OK);
    OK(fslite_open(filename, 0, &v));
    OK(fslite_writer_open(v, "/file", &writer));
    OK(fslite_writer_write(writer, payload, CHUNK+3));
    STATUS(fslite_writer_finish(writer), FSLITE_BUSY);
    fslite_writer_close(writer);
    CHECK(!fslite_in_transaction(v));
    verify_bytes(v, "/file", (const unsigned char *)"old", 3);
    fslite_close(v);
    CHECK(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    OK(fslite_open(filename, 0, &v));
    OK(fslite_write(v, "/file", payload, CHUNK*2+7));
    OK(fslite_write(v, "/empty", NULL, 0));
    fslite_close(v);
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT chunk_index,length(data) FROM chunks "
        "WHERE node_id=(SELECT id FROM nodes WHERE name='file') ORDER BY chunk_index", -1, &stmt, NULL) == SQLITE_OK);
    for (int i = 0; i < 3; ++i) {
        CHECK(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int(stmt, 0) == i);
        CHECK(sqlite3_column_int(stmt, 1) == (i == 2 ? 7 : CHUNK));
    }
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM chunks WHERE node_id=(SELECT id FROM nodes WHERE name='empty')", -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
    // A missing chunk is corruption, not a successful short read or zero-filled gap.
    CHECK(sqlite3_exec(db, "DELETE FROM chunks WHERE chunk_index=1", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    OK(fslite_open(filename, 0, &v));
    size_t count = 99;
    STATUS(fslite_read(v, "/file", 0, payload, CHUNK*2, &count), FSLITE_INVALID_FORMAT);
    CHECK(count == 0);
    fslite_close(v);
    CHECK(unlink(filename) == 0);
    free(payload);

    // A real v1 layout is rejected without rewriting markers or original content.
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "PRAGMA application_id=0x46534c54; PRAGMA user_version=1; "
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,parent INTEGER,name TEXT,kind INTEGER,data BLOB);"
        "INSERT INTO nodes VALUES(1,NULL,'',2,NULL),(2,1,'legacy',1,x'6f6c64');", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    STATUS(fslite_open(filename, 0, &v), FSLITE_INVALID_FORMAT);
    STATUS(fslite_open(filename, FSLITE_OPEN_CREATE, &v), FSLITE_INVALID_FORMAT);
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "SELECT data FROM nodes WHERE id=2", -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_bytes(stmt, 0) == 3 && memcmp(sqlite3_column_blob(stmt, 0), "old", 3) == 0);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(unlink(filename) == 0);
}

static void test_foreign_databases(void) {
    char filename[64]; new_path(filename);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "CREATE TABLE precious(value); INSERT INTO precious VALUES('keep me');", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    fslite_volume *v = NULL;
    STATUS(fslite_open(filename, FSLITE_OPEN_CREATE, &v), FSLITE_INVALID_FORMAT);
    CHECK(v == NULL);
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT value FROM precious", -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(strcmp((const char *)sqlite3_column_text(stmt, 0), "keep me") == 0);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(unlink(filename) == 0);

    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v)); fslite_close(v);
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "PRAGMA user_version=999", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    STATUS(fslite_open(filename, 0, &v), FSLITE_INVALID_FORMAT);
    CHECK(v == NULL);
    CHECK(unlink(filename) == 0);
}
#endif

static void test_metadata_limits_stats(void) {
    char filename[64];
    new_path(filename);
    fslite_volume *v = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));

    OK(fslite_mkdir(v, "/dir"));
    unsigned char payload[70000];
    for (size_t i = 0; i < sizeof payload; ++i) payload[i] = (unsigned char)(i * 7 + 1);
    OK(fslite_write(v, "/dir/data", payload, sizeof payload));

    fslite_file_info file;
    OK(fslite_inspect(v, "/dir/data", &file));
    CHECK(file.kind == FSLITE_FILE && file.size == sizeof payload && file.depth == 2);
    CHECK(file.chunks == 2 && file.allocated == sizeof payload);
    CHECK(file.reserved[0] == 0 && file.reserved[1] == 0);
    fslite_file_info dir;
    OK(fslite_inspect(v, "/dir", &dir));
    CHECK(dir.kind == FSLITE_DIRECTORY && dir.size == 0 && dir.depth == 1);
    CHECK(dir.chunks == 0 && dir.allocated == 0);
    fslite_file_info root;
    OK(fslite_inspect(v, "/", &root));
    CHECK(root.id == 1 && root.kind == FSLITE_DIRECTORY && root.depth == 0);
    STATUS(fslite_inspect(v, "/missing", &file), FSLITE_NOT_FOUND);
    CHECK(file.id == 0 && file.size == 0 && file.chunks == 0 && file.allocated == 0);

    fslite_volume_stats stats;
    OK(fslite_get_volume_stats(v, &stats));
    CHECK(stats.file_count == 1 && stats.directory_count == 2);
    CHECK(stats.chunk_count == 2 && stats.chunk_bytes == sizeof payload);
    CHECK(stats.logical_bytes == sizeof payload);
    CHECK(stats.page_size > 0 && stats.page_count > 0);
    CHECK(stats.database_bytes == stats.page_size * stats.page_count);
    CHECK(stats.freelist_pages <= stats.page_count);
    CHECK(stats.in_transaction == 0 && stats.sparse == 0);
    CHECK(stats.reserved[0] == 0 && stats.reserved[1] == 0);

    // Diagnostics observe this handle's uncommitted work.
    OK(fslite_begin(v));
    OK(fslite_write(v, "/dir/extra", "abcd", 4));
    OK(fslite_get_volume_stats(v, &stats));
    CHECK(stats.file_count == 2 && stats.in_transaction == 1);
    OK(fslite_rollback(v));
    OK(fslite_get_volume_stats(v, &stats));
    CHECK(stats.file_count == 1 && stats.in_transaction == 0);

    fslite_limits limits;
    memset(&limits, 0, sizeof limits);
    limits.max_file_size = 1000;
    limits.max_volume_bytes = 1500;
    limits.max_path_bytes = 32;
    limits.max_name_bytes = 8;
    OK(fslite_set_limits(v, &limits));
    STATUS(fslite_get_volume_stats(v, &stats), FSLITE_OK);

    fslite_info info;
    STATUS(fslite_write(v, "/dir/too-big", payload, 1001), FSLITE_TOO_LARGE);
    STATUS(fslite_stat(v, "/dir/too-big", &info), FSLITE_NOT_FOUND);
    STATUS(fslite_truncate(v, "/dir/data", 70001), FSLITE_TOO_LARGE);
    STATUS(fslite_write_at(v, "/dir/data", 70499, "x", 1), FSLITE_TOO_LARGE);
    // Shrinking is always allowed, even below a newly configured limit.
    OK(fslite_truncate(v, "/dir/data", 500));
    OK(fslite_inspect(v, "/dir/data", &file));
    CHECK(file.size == 500 && file.chunks == 1 && file.allocated == 500);

    // Path and name limits use UTF-8 byte counts.
    STATUS(fslite_mkdir(v, "/012345678"), FSLITE_INVALID_ARGUMENT);
    char long_path[64] = "/";
    memset(long_path + 1, 'a', 40);
    long_path[41] = 0;
    STATUS(fslite_mkdir(v, long_path), FSLITE_INVALID_ARGUMENT);

    // Volume accounting: 500 + 1000 fits exactly; one more byte does not.
    OK(fslite_write(v, "/dir/second", payload, 1000));
    OK(fslite_write(v, "/dir/second", payload, 1000)); // replacement, not growth
    STATUS(fslite_write(v, "/dir/third", "x", 1), FSLITE_TOO_LARGE);
    STATUS(fslite_stat(v, "/dir/third", &info), FSLITE_NOT_FOUND);

    // A rejected replacement must roll back atomically and keep the old bytes.
    char kept[8];
    size_t kept_count = 0;
    OK(fslite_read(v, "/dir/second", 0, kept, sizeof kept, &kept_count));
    CHECK(kept_count == sizeof kept && memcmp(kept, payload, sizeof kept) == 0);
    STATUS(fslite_write(v, "/dir/second", payload, 1001), FSLITE_TOO_LARGE);
    kept_count = 0;
    OK(fslite_read(v, "/dir/second", 0, kept, sizeof kept, &kept_count));
    CHECK(kept_count == sizeof kept && memcmp(kept, payload, sizeof kept) == 0);
    OK(fslite_inspect(v, "/dir/second", &file));
    CHECK(file.size == 1000 && file.chunks == 1 && file.allocated == 1000);

    // Limits may only change while quiescent.
    OK(fslite_begin(v));
    STATUS(fslite_set_limits(v, NULL), FSLITE_INVALID_STATE);
    OK(fslite_rollback(v));
    fslite_writer *writer = NULL;
    OK(fslite_writer_open(v, "/stream", &writer));
    STATUS(fslite_set_limits(v, NULL), FSLITE_INVALID_STATE);
    fslite_writer_close(writer);

    // NULL restores every default.
    OK(fslite_set_limits(v, NULL));
    OK(fslite_write(v, "/dir/third", payload, sizeof payload));

    // A file larger than a lowered per-file limit can still be modified in place.
    OK(fslite_write(v, "/dir/large", payload, 2000));
    memset(&limits, 0, sizeof limits);
    limits.max_file_size = 1000;
    OK(fslite_set_limits(v, &limits));
    OK(fslite_write_at(v, "/dir/large", 0, "tiny", 4));
    STATUS(fslite_write_at(v, "/dir/large", 2000, "x", 1), FSLITE_TOO_LARGE);
    OK(fslite_truncate(v, "/dir/large", 800));
    OK(fslite_truncate(v, "/dir/large", 900));
    STATUS(fslite_truncate(v, "/dir/large", 1001), FSLITE_TOO_LARGE);

    // A volume limit below current usage also permits non-growing writes.
    memset(&limits, 0, sizeof limits);
    limits.max_volume_bytes = 100;
    OK(fslite_set_limits(v, &limits));
    OK(fslite_write_at(v, "/dir/large", 0, "t", 1));
    STATUS(fslite_write_at(v, "/dir/large", 900, "xx", 2), FSLITE_TOO_LARGE);
    OK(fslite_set_limits(v, NULL));

    memset(&limits, 0, sizeof limits);
    limits.max_path_bytes = 4097;
    STATUS(fslite_set_limits(v, &limits), FSLITE_INVALID_ARGUMENT);
    memset(&limits, 0, sizeof limits);
    limits.max_name_bytes = 256;
    STATUS(fslite_set_limits(v, &limits), FSLITE_INVALID_ARGUMENT);
    memset(&limits, 0, sizeof limits);
    limits.max_file_size = (uint64_t)INT64_MAX + 1;
    STATUS(fslite_set_limits(v, &limits), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_set_limits(NULL, &limits), FSLITE_INVALID_ARGUMENT);

    fslite_close(v);
    CHECK(unlink(filename) == 0);

    // Sparse volumes expose storage accounting through the same APIs.
    char sparse_name[64];
    new_path(sparse_name);
    fslite_volume *sparse = NULL;
    OK(fslite_open(sparse_name, FSLITE_OPEN_CREATE | FSLITE_OPEN_SPARSE, &sparse));
    OK(fslite_write(sparse, "/sparse", NULL, 0));
    OK(fslite_write_at(sparse, "/sparse", 0, "A", 1));
    OK(fslite_write_at(sparse, "/sparse", 65536 * 2, "B", 1));
    OK(fslite_inspect(sparse, "/sparse", &file));
    CHECK(file.size == 65536 * 2 + 1 && file.chunks == 2 && file.allocated == 65537);
    OK(fslite_get_volume_stats(sparse, &stats));
    CHECK(stats.sparse == 1 && stats.chunk_count == 2 && stats.chunk_bytes == 65537);
    fslite_close(sparse);
    CHECK(unlink(sparse_name) == 0);
}

void test_namespace(void);
void test_backup(void);
void test_random_access(void);
void test_integrity(void);
void test_transactions(void);
void test_statement_reuse(void);

int main(void) {
    test_integrity();
    test_transactions();
    test_random_access();
    test_backup();
    test_namespace();
    test_engine();
    test_invalid_arguments();
    test_streaming();
    test_statement_reuse();
    test_metadata_limits_stats();
#ifdef FSLITE_TEST_SQLITE
    test_foreign_databases();
    test_stream_failures_and_format();
#endif
    puts("FSLite C ABI tests passed");
    return 0;
}
