#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef FSLITE_TEST_SQLITE
#include "sqlite3.h"
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define STATUS(call, want) do { fslite_status got = (call); if (got != (want)) { \
    fprintf(stderr, "%s:%d: %s: got %d, want %d\n", __FILE__, __LINE__, #call, got, (want)); exit(1); } } while (0)
#define OK(call) STATUS(call, FSLITE_OK)
static void new_path(char path[64]) {
    strcpy(path, "/tmp/fslite-integrity-XXXXXX");
    int fd = mkstemp(path); CHECK(fd >= 0 && close(fd) == 0);
}
static void valid_boundaries(void) {
    fslite_volume *v = NULL; OK(fslite_open(":memory:", FSLITE_OPEN_CREATE, &v));
    char path[4097]; size_t size = 0;
    // Sixteen 255-byte names give an exact 4096-byte absolute path.
    for (unsigned i = 0; i < 16; ++i) {
        path[size++] = '/'; memset(path + size, 'a', 255); size += 255; path[size] = 0;
        OK(fslite_mkdir(v, path));
    }
    OK(fslite_write(v, "/caf\xc3\xa9", "unicode", 7));
    OK(fslite_check(v)); fslite_close(v);
}
static void lifecycle(void) {
    char path[64], destination[64]; new_path(path); new_path(destination);
    CHECK(unlink(destination) == 0);
    fslite_volume *v = NULL, *reader = NULL;
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));
    OK(fslite_open(path, 0, &reader));
    STATUS(fslite_check(NULL), FSLITE_INVALID_ARGUMENT);
    OK(fslite_check(v)); CHECK(!fslite_in_transaction(v));
    OK(fslite_begin(v)); OK(fslite_mkdir(v, "/pending"));
    OK(fslite_write(v, "/pending/file", "abc", 3));
    OK(fslite_check(v)); CHECK(fslite_in_transaction(v));
    fslite_info info;
    STATUS(fslite_stat(reader, "/pending", &info), FSLITE_NOT_FOUND);
    OK(fslite_check(reader)); CHECK(!fslite_in_transaction(reader));
    OK(fslite_rollback(v)); STATUS(fslite_stat(v, "/pending", &info), FSLITE_NOT_FOUND);
    OK(fslite_begin(v)); OK(fslite_write(v, "/committed", "xyz", 3));
    OK(fslite_check(v)); OK(fslite_commit(v));
    OK(fslite_stat(reader, "/committed", &info));
    fslite_writer *writer = NULL;
    OK(fslite_writer_open(v, "/committed", &writer));
    OK(fslite_writer_write(writer, "replacement", 11));
    STATUS(fslite_check(v), FSLITE_INVALID_STATE);
    fslite_writer_close(writer); OK(fslite_check(v));
    fslite_backup *backup = NULL;
    OK(fslite_backup_open(v, destination, &backup));
    STATUS(fslite_check(v), FSLITE_INVALID_STATE);
    fslite_backup_close(backup); OK(fslite_check(v));
    OK(fslite_export(v, destination));
    fslite_volume *copy = NULL; OK(fslite_open(destination, 0, &copy));
    OK(fslite_check(copy)); fslite_close(copy);
    fslite_close(reader); fslite_close(v);
    CHECK(unlink(path) == 0 && unlink(destination) == 0);
}
#ifdef FSLITE_TEST_SQLITE
static void sql(sqlite3 *db, const char *query) {
    char *message = NULL;
    int result = sqlite3_exec(db, query, NULL, NULL, &message);
    if (result != SQLITE_OK) { fprintf(stderr, "%s: %s\n", query, message); exit(1); }
    sqlite3_free(message);
}
static uint64_t fingerprint(const char *path) {
    FILE *file = fopen(path, "rb"); CHECK(file != NULL);
    uint64_t hash = UINT64_C(14695981039346656037); int byte;
    while ((byte = fgetc(file)) != EOF) hash = (hash ^ (unsigned char)byte) * UINT64_C(1099511628211);
    CHECK(!ferror(file) && fclose(file) == 0); return hash;
}
static void corruptions(void) {
    const char *cases[] = {
        "PRAGMA application_id=0", "PRAGMA user_version=99",
        "DELETE FROM nodes WHERE id=1", "UPDATE nodes SET name='root' WHERE id=1",
        "UPDATE nodes SET kind=1 WHERE id=1", "UPDATE nodes SET parent=2 WHERE id=1",
        "UPDATE nodes SET parent=NULL WHERE id=2", "UPDATE nodes SET id=-2 WHERE id=2",
        "UPDATE nodes SET parent=999 WHERE id=2", "UPDATE nodes SET parent=4 WHERE id=2",
        "UPDATE nodes SET parent=2 WHERE id=2",
        "UPDATE nodes SET parent=3 WHERE id=2; UPDATE nodes SET parent=2 WHERE id=3",
        "UPDATE nodes SET kind=7 WHERE id=2", "UPDATE nodes SET size=1 WHERE id=2",
        "UPDATE nodes SET size=-1 WHERE id=4", "UPDATE nodes SET size=1.5 WHERE id=4",
        "UPDATE nodes SET name='' WHERE id=2", "UPDATE nodes SET name='.' WHERE id=2",
        "UPDATE nodes SET name='..' WHERE id=2", "UPDATE nodes SET name='a/b' WHERE id=2",
        "UPDATE nodes SET name=CAST(x'610062' AS TEXT) WHERE id=2",
        "UPDATE nodes SET name=CAST(x'ff' AS TEXT) WHERE id=2",
        "UPDATE nodes SET name=zeroblob(2) WHERE id=2",
        "UPDATE nodes SET name=printf('%0256d',0) WHERE id=2",
        "DELETE FROM chunks WHERE node_id=4 AND chunk_index=0",
        "UPDATE chunks SET data=x'00' WHERE node_id=4 AND chunk_index=0",
        "UPDATE chunks SET data=zeroblob(65536) WHERE node_id=4 AND chunk_index=1",
        "UPDATE chunks SET data='text' WHERE node_id=4 AND chunk_index=1",
        "UPDATE chunks SET chunk_index=-1 WHERE node_id=4 AND chunk_index=0",
        "UPDATE chunks SET chunk_index=9223372036854775807 WHERE node_id=4 AND chunk_index=1",
        "INSERT INTO chunks VALUES(4,2,x'00')", "INSERT INTO chunks VALUES(999,0,x'00')",
        "INSERT INTO chunks VALUES(2,0,x'00')", "INSERT INTO chunks VALUES(5,0,x'00')",
        "UPDATE nodes SET size=131073 WHERE id=4",
        ("WITH RECURSIVE seq(i) AS (VALUES(10) UNION ALL SELECT i+1 FROM seq WHERE i<26) "
            "INSERT INTO nodes SELECT i,CASE WHEN i=10 THEN 1 ELSE i-1 END,printf('%0255d',i),2,0 FROM seq"),
        "DROP TABLE chunks", "ALTER TABLE chunks RENAME COLUMN data TO broken",
        "DROP TABLE nodes"
    };
    unsigned char *data = calloc(65537, 1); CHECK(data != NULL);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        char path[64]; new_path(path); fslite_volume *v = NULL;
        OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));
        OK(fslite_mkdir(v, "/a")); OK(fslite_mkdir(v, "/b"));
        OK(fslite_write(v, "/file", data, 65537)); OK(fslite_write(v, "/empty", NULL, 0));
        OK(fslite_check(v));
        sqlite3 *db = NULL; CHECK(sqlite3_open(path, &db) == SQLITE_OK);
        sql(db, "PRAGMA foreign_keys=OFF; PRAGMA ignore_check_constraints=ON;");
        sql(db, cases[i]);
        uint64_t before = fingerprint(path);
        fslite_status result = fslite_check(v);
        if (result != FSLITE_INVALID_FORMAT) {
            fprintf(stderr, "corruption %zu (%s): got %d\n", i, cases[i], result); exit(1);
        }
        CHECK(!fslite_in_transaction(v)); CHECK(before == fingerprint(path));
        // A failed scan must release its snapshot/locks as well as avoid writes.
        sql(db, "BEGIN EXCLUSIVE; ROLLBACK;");
        CHECK(sqlite3_close(db) == SQLITE_OK); fslite_close(v); CHECK(unlink(path) == 0);
    }
    free(data);
}
static void failure_preserves_transaction(void) {
    char path[64]; new_path(path); fslite_volume *v = NULL;
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));
    OK(fslite_write(v, "/bad", "abc", 3));
    sqlite3 *db = NULL; CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    sql(db, "DELETE FROM chunks");
    OK(fslite_begin(v)); OK(fslite_write(v, "/pending", "keep", 4));
    STATUS(fslite_check(v), FSLITE_INVALID_FORMAT); CHECK(fslite_in_transaction(v));
    char bytes[4]; size_t count = 0;
    OK(fslite_read(v, "/pending", 0, bytes, sizeof bytes, &count));
    CHECK(count == 4 && memcmp(bytes, "keep", 4) == 0);
    OK(fslite_rollback(v));
    fslite_info info; STATUS(fslite_stat(v, "/pending", &info), FSLITE_NOT_FOUND);
    OK(fslite_begin(v)); OK(fslite_write(v, "/pending", "keep", 4));
    STATUS(fslite_check(v), FSLITE_INVALID_FORMAT); CHECK(fslite_in_transaction(v));
    OK(fslite_commit(v)); fslite_close(v);
    OK(fslite_open(path, 0, &v)); OK(fslite_stat(v, "/pending", &info)); CHECK(info.size == 4);
    OK(fslite_write(v, "/bad", "repaired by caller", 18)); OK(fslite_check(v));
    sql(db, "BEGIN EXCLUSIVE");
    STATUS(fslite_check(v), FSLITE_BUSY); CHECK(!fslite_in_transaction(v));
    sql(db, "ROLLBACK"); OK(fslite_check(v));
    CHECK(sqlite3_close(db) == SQLITE_OK); fslite_close(v); CHECK(unlink(path) == 0);
}
#endif
void test_integrity(void) {
    valid_boundaries(); lifecycle();
#ifdef FSLITE_TEST_SQLITE
    corruptions(); failure_preserves_transaction();
#endif
}
