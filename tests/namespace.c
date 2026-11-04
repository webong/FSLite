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

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: got %s (%d), wanted %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)

static void new_path(char path[64]) {
    strcpy(path, "/tmp/fslite-namespace-XXXXXX");
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0);
}
static fslite_info info(fslite_volume *v, const char *path) {
    fslite_info result;
    OK(fslite_stat(v, path, &result));
    return result;
}
static void absent(fslite_volume *v, const char *path) {
    fslite_info result;
    STATUS(fslite_stat(v, path, &result), FSLITE_NOT_FOUND);
}
static void content(fslite_volume *v, const char *path, const void *expected, size_t size) {
    unsigned char *bytes = malloc(size + 1);
    CHECK(bytes != NULL);
    size_t count = 0;
    OK(fslite_read(v, path, 0, bytes, size + 1, &count));
    CHECK(count == size && memcmp(bytes, expected, size) == 0);
    free(bytes);
}

static void semantics(void) {
    fslite_volume *v = NULL;
    OK(fslite_open(":memory:", FSLITE_OPEN_CREATE, &v));
    STATUS(fslite_rename(NULL, "/x", "/y"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_rename(v, NULL, "/y"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_rename(v, "/x", NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_remove(NULL, "/x"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_remove(v, NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_remove(v, "/"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_rename(v, "/", "/root"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_rename(v, "/", "/"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_remove(v, "/missing"), FSLITE_NOT_FOUND);
    STATUS(fslite_rename(v, "/missing", "/missing"), FSLITE_NOT_FOUND);
    OK(fslite_mkdir(v, "/from"));
    OK(fslite_mkdir(v, "/to"));
    unsigned char bytes[65536 * 2 + 31];
    for (size_t i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)(i * 11 + i / 253);
    OK(fslite_write(v, "/from/file", bytes, sizeof bytes));
    uint64_t file_id = info(v, "/from/file").id;
    STATUS(fslite_rename(v, "/from/file", "/"), FSLITE_INVALID_ARGUMENT);
    const char *bad[] = { "", "relative", "/to/", "/to//x", "/to/../x", "/\xff" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        STATUS(fslite_rename(v, "/from/file", bad[i]), FSLITE_INVALID_ARGUMENT);
        STATUS(fslite_rename(v, bad[i], "/to/file"), FSLITE_INVALID_ARGUMENT);
        STATUS(fslite_remove(v, bad[i]), FSLITE_INVALID_ARGUMENT);
    }
    STATUS(fslite_rename(v, "/from/file", "/missing/file"), FSLITE_NOT_FOUND);
    STATUS(fslite_rename(v, "/from/file", "/from/file/child"), FSLITE_NOT_DIRECTORY);
    OK(fslite_rename(v, "/from/file", "/from/file"));
    OK(fslite_rename(v, "/from/file", "/to/renamed"));
    absent(v, "/from/file");
    CHECK(info(v, "/to/renamed").id == file_id);
    content(v, "/to/renamed", bytes, sizeof bytes);
    OK(fslite_write(v, "/to/target", "old", 3));
    CHECK(info(v, "/to/target").id != file_id);
    OK(fslite_rename(v, "/to/renamed", "/to/target"));
    absent(v, "/to/renamed");
    CHECK(info(v, "/to/target").id == file_id);
    content(v, "/to/target", bytes, sizeof bytes);
    STATUS(fslite_rename(v, "/to/target", "/from"), FSLITE_IS_DIRECTORY);
    STATUS(fslite_rename(v, "/from", "/to/target"), FSLITE_NOT_DIRECTORY);
    STATUS(fslite_remove(v, "/to"), FSLITE_NOT_EMPTY);
    CHECK(strcmp(fslite_status_string(FSLITE_NOT_EMPTY), "directory not empty") == 0);

    OK(fslite_mkdir(v, "/from/sub"));
    OK(fslite_write(v, "/from/sub/資料", bytes, sizeof bytes));
    const uint64_t directory_id = info(v, "/from").id;
    const uint64_t child_id = info(v, "/from/sub/資料").id;
    OK(fslite_rename(v, "/from", "/from"));
    STATUS(fslite_rename(v, "/from", "/from/sub"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_rename(v, "/from", "/from/sub/moved"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_rename(v, "/from", "/to"), FSLITE_NOT_EMPTY);
    // A shared lexical prefix is not a descendant path.
    OK(fslite_rename(v, "/from", "/from-longer"));
    OK(fslite_mkdir(v, "/empty"));
    OK(fslite_rename(v, "/from-longer", "/empty"));
    absent(v, "/from-longer");
    CHECK(info(v, "/empty").id == directory_id);
    CHECK(info(v, "/empty/sub/資料").id == child_id);
    content(v, "/empty/sub/資料", bytes, sizeof bytes);
    STATUS(fslite_rename(v, "/empty/sub", "/empty"), FSLITE_NOT_EMPTY);
    OK(fslite_remove(v, "/empty/sub/資料"));
    absent(v, "/empty/sub/資料");
    OK(fslite_remove(v, "/empty/sub"));
    OK(fslite_remove(v, "/empty"));
    OK(fslite_remove(v, "/to/target"));
    OK(fslite_remove(v, "/to"));
    CHECK(!fslite_in_transaction(v));

    // UTF-8 byte lengths, rather than character counts, limit moved subtrees.
    OK(fslite_mkdir(v, "/a"));
    char path[4097] = "/a";
    char component[256];
    for (int i = 0; i < 85; ++i) memcpy(component + i * 3, "界", 3);
    component[255] = 0;
    for (int i = 0; i < 15; ++i) {
        strcat(path, "/"); strcat(path, component);
        OK(fslite_mkdir(v, path));
    }
    strcat(path, "/");
    size_t prefix = strlen(path);
    memset(path + prefix, 'z', 253); path[prefix + 253] = 0;
    CHECK(strlen(path) == 4096);
    OK(fslite_write(v, path, "edge", 4));
    OK(fslite_mkdir(v, "/bb"));
    STATUS(fslite_rename(v, "/a", "/bb"), FSLITE_INVALID_ARGUMENT);
    CHECK(info(v, "/bb").kind == FSLITE_DIRECTORY);
    content(v, path, "edge", 4);
    OK(fslite_rename(v, "/a", "/b"));
    path[1] = 'b'; content(v, path, "edge", 4);
    fslite_close(v);
}

static void transactions(void) {
    char filename[64]; new_path(filename);
    fslite_volume *v = NULL, *observer = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    OK(fslite_write(v, "/source", "source", 6));
    OK(fslite_write(v, "/target", "target", 6));
    OK(fslite_write(v, "/remove", "remove", 6));
    const uint64_t source_id = info(v, "/source").id;
    const uint64_t target_id = info(v, "/target").id;
    OK(fslite_open(filename, 0, &observer));
    OK(fslite_begin(v));
    OK(fslite_write(v, "/earlier", "keep", 4));
    OK(fslite_rename(v, "/source", "/target"));
    OK(fslite_remove(v, "/remove"));
    STATUS(fslite_remove(v, "/missing"), FSLITE_NOT_FOUND);
    CHECK(fslite_in_transaction(v));
    absent(v, "/source"); absent(v, "/remove");
    CHECK(info(v, "/target").id == source_id);
    content(observer, "/source", "source", 6);
    content(observer, "/target", "target", 6);
    content(observer, "/remove", "remove", 6);
    OK(fslite_rollback(v));
    CHECK(info(v, "/target").id == target_id);
    content(v, "/source", "source", 6); content(v, "/remove", "remove", 6);
    absent(v, "/earlier");
    OK(fslite_begin(v));
    OK(fslite_write(v, "/earlier", "keep", 4));
    OK(fslite_rename(v, "/source", "/target"));
    OK(fslite_remove(v, "/remove"));
    OK(fslite_commit(v));
    content(observer, "/earlier", "keep", 4);
    content(observer, "/target", "source", 6);
    absent(observer, "/source"); absent(observer, "/remove");
    fslite_writer *writer = NULL;
    OK(fslite_writer_open(v, "/stream", &writer));
    OK(fslite_writer_write(writer, "stream", 6));
    STATUS(fslite_rename(v, "/target", "/target"), FSLITE_INVALID_STATE);
    STATUS(fslite_remove(v, "/target"), FSLITE_INVALID_STATE);
    OK(fslite_writer_finish(writer)); fslite_writer_close(writer);
    OK(fslite_rename(v, "/stream", "/streamed"));
    content(v, "/streamed", "stream", 6);
    OK(fslite_begin(v));
    OK(fslite_rename(v, "/target", "/uncommitted"));
    OK(fslite_remove(v, "/earlier"));
    fslite_close(v); fslite_close(observer);
    OK(fslite_open(filename, 0, &v));
    absent(v, "/uncommitted");
    content(v, "/target", "source", 6); content(v, "/earlier", "keep", 4);
    fslite_close(v);

    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        OK(fslite_open(filename, 0, &v));
        OK(fslite_begin(v));
        OK(fslite_rename(v, "/target", "/streamed"));
        OK(fslite_remove(v, "/earlier"));
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    OK(fslite_open(filename, 0, &v));
    content(v, "/target", "source", 6); content(v, "/streamed", "stream", 6);
    content(v, "/earlier", "keep", 4);
    fslite_close(v);
    CHECK(unlink(filename) == 0);
}

#ifdef FSLITE_TEST_SQLITE
static void exec_sql(const char *filename, const char *sql) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}
static int64_t scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    int64_t result = sqlite3_column_int64(stmt, 0);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
    return result;
}
static void failures_and_chunk_cleanup(void) {
    char filename[64]; new_path(filename);
    fslite_volume *v = NULL;
    unsigned char source[65536 * 2 + 7], target[65536 + 19];
    memset(source, 's', sizeof source); memset(target, 't', sizeof target);
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    OK(fslite_write(v, "/source", source, sizeof source));
    OK(fslite_write(v, "/target", target, sizeof target));
    OK(fslite_write(v, "/remove", source, sizeof source));
    uint64_t source_id = info(v, "/source").id;
    uint64_t target_id = info(v, "/target").id;
    uint64_t remove_id = info(v, "/remove").id;
    fslite_close(v);
    exec_sql(filename, "CREATE TRIGGER fail_move BEFORE UPDATE OF parent,name ON nodes "
        "BEGIN SELECT RAISE(FAIL,'injected move failure'); END");
    OK(fslite_open(filename, 0, &v));
    for (int nested = 0; nested < 2; ++nested) {
        if (nested) {
            OK(fslite_begin(v));
            OK(fslite_write(v, "/earlier", "keep", 4));
        }
        // Destination node and chunks were deleted before the UPDATE fails.
        STATUS(fslite_rename(v, "/source", "/target"), FSLITE_INTERNAL_ERROR);
        CHECK(fslite_in_transaction(v) == (uint32_t)nested);
        CHECK(info(v, "/source").id == source_id && info(v, "/target").id == target_id);
        content(v, "/source", source, sizeof source); content(v, "/target", target, sizeof target);
        if (nested) OK(fslite_commit(v));
    }
    content(v, "/earlier", "keep", 4);
    fslite_close(v);
    exec_sql(filename, "DROP TRIGGER fail_move; CREATE TRIGGER fail_delete AFTER DELETE ON chunks "
        "WHEN OLD.chunk_index=1 BEGIN SELECT RAISE(FAIL,'injected delete failure'); END");
    OK(fslite_open(filename, 0, &v));
    for (int nested = 0; nested < 2; ++nested) {
        if (nested) {
            OK(fslite_begin(v));
            OK(fslite_mkdir(v, "/prior-directory"));
        }
        STATUS(fslite_remove(v, "/remove"), FSLITE_INTERNAL_ERROR);
        CHECK(fslite_in_transaction(v) == (uint32_t)nested);
        CHECK(info(v, "/remove").id == remove_id);
        content(v, "/remove", source, sizeof source);
        if (nested) OK(fslite_commit(v));
    }
    CHECK(info(v, "/prior-directory").kind == FSLITE_DIRECTORY);
    fslite_close(v);
    exec_sql(filename, "DROP TRIGGER fail_delete");

    // A reader holds a shared lock: implicit commits must roll back completely.
    sqlite3 *reader = NULL;
    CHECK(sqlite3_open(filename, &reader) == SQLITE_OK);
    CHECK(sqlite3_exec(reader, "BEGIN; SELECT count(*) FROM nodes", NULL, NULL, NULL) == SQLITE_OK);
    OK(fslite_open(filename, 0, &v));
    STATUS(fslite_rename(v, "/source", "/target"), FSLITE_BUSY);
    CHECK(!fslite_in_transaction(v));
    content(v, "/source", source, sizeof source); content(v, "/target", target, sizeof target);
    STATUS(fslite_remove(v, "/remove"), FSLITE_BUSY);
    CHECK(!fslite_in_transaction(v));
    content(v, "/remove", source, sizeof source);
    CHECK(sqlite3_exec(reader, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(reader) == SQLITE_OK);
    // A competing writer must not leave either operation's implicit transaction open.
    CHECK(sqlite3_open(filename, &reader) == SQLITE_OK);
    CHECK(sqlite3_exec(reader, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    STATUS(fslite_rename(v, "/source", "/target"), FSLITE_BUSY);
    STATUS(fslite_remove(v, "/remove"), FSLITE_BUSY);
    CHECK(!fslite_in_transaction(v));
    CHECK(sqlite3_exec(reader, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(reader) == SQLITE_OK);
    OK(fslite_rename(v, "/source", "/target"));
    OK(fslite_remove(v, "/remove"));
    content(v, "/target", source, sizeof source);
    CHECK(info(v, "/target").id == source_id);
    fslite_close(v);
    CHECK(sqlite3_open(filename, &reader) == SQLITE_OK);
    char query[128];
    snprintf(query, sizeof query, "SELECT count(*) FROM chunks WHERE node_id=%llu OR node_id=%llu",
        (unsigned long long)target_id, (unsigned long long)remove_id);
    CHECK(scalar(reader, query) == 0);
    snprintf(query, sizeof query, "SELECT count(*) FROM chunks WHERE node_id=%llu", (unsigned long long)source_id);
    CHECK(scalar(reader, query) == 3);
    CHECK(scalar(reader, "SELECT count(*) FROM chunks c LEFT JOIN nodes n ON n.id=c.node_id WHERE n.id IS NULL") == 0);
    CHECK(scalar(reader, "PRAGMA user_version") == 2);
    CHECK(scalar(reader, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    CHECK(sqlite3_close(reader) == SQLITE_OK);
    CHECK(unlink(filename) == 0);
}
#endif

void test_namespace(void) {
    semantics();
    transactions();
#ifdef FSLITE_TEST_SQLITE
    failures_and_chunk_cleanup();
#endif
}
