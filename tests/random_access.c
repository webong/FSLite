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
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: %s (%d), expected %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)
enum { CHUNK = 65536, CAPACITY = CHUNK * 4 + 99 };

static void new_path(char path[64]) {
    strcpy(path, "/tmp/fslite-random-XXXXXX");
    int fd = mkstemp(path); CHECK(fd >= 0 && close(fd) == 0);
}
static uint64_t file_id(fslite_volume *v) {
    fslite_info info; OK(fslite_stat(v, "/file", &info)); return info.id;
}
static void verify(fslite_volume *v, const unsigned char *expected, size_t size, uint64_t id) {
    fslite_info info; OK(fslite_stat(v, "/file", &info));
    CHECK(info.id == id && info.size == size && info.kind == FSLITE_FILE);
    unsigned char buffer[4093]; size_t count;
    for (size_t offset = 0; offset < size; offset += count) {
        OK(fslite_read(v, "/file", offset, buffer, sizeof buffer, &count));
        size_t wanted = size - offset < sizeof buffer ? size - offset : sizeof buffer;
        CHECK(count == wanted && memcmp(buffer, expected + offset, count) == 0);
    }
    OK(fslite_read(v, "/file", size, buffer, sizeof buffer, &count)); CHECK(count == 0);
}
static void model_write(fslite_volume *v, unsigned char *model, size_t *size, uint64_t id,
                        size_t offset, const unsigned char *data, size_t length) {
    CHECK(offset + length <= CAPACITY);
    OK(fslite_write_at(v, "/file", offset, data, length));
    if (length) {
        if (offset > *size) memset(model + *size, 0, offset - *size);
        memcpy(model + offset, data, length);
        if (offset + length > *size) *size = offset + length;
    }
    verify(v, model, *size, id);
}
static void model_truncate(fslite_volume *v, unsigned char *model, size_t *size, uint64_t id, size_t next) {
    CHECK(next <= CAPACITY);
    OK(fslite_truncate(v, "/file", next));
    if (next > *size) memset(model + *size, 0, next - *size);
    *size = next; verify(v, model, *size, id);
}
static uint32_t random_number(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223); return *state;
}

static void semantics(void) {
    fslite_volume *v = NULL;
    OK(fslite_open(":memory:", FSLITE_OPEN_CREATE, &v));
    unsigned char *model = malloc(CAPACITY), *data = malloc(CHUNK + 13);
    CHECK(model && data);
    for (size_t i = 0; i < CAPACITY; ++i) model[i] = (unsigned char)(i * 17 + i / 257);
    for (size_t i = 0; i < CHUNK + 13; ++i) data[i] = (unsigned char)(i * 31 + i / 251);
    size_t size = CHUNK * 3 + 7;
    OK(fslite_write(v, "/file", model, size)); uint64_t id = file_id(v);
    model_write(v, model, &size, id, 0, data, 1);
    model_write(v, model, &size, id, CHUNK - 3, data, 9);
    model_write(v, model, &size, id, CHUNK, data, CHUNK);
    model_write(v, model, &size, id, CHUNK * 2 - 1, data, CHUNK + 13);
    model_write(v, model, &size, id, CHUNK * 4 + 70, data, 17);
    model_write(v, model, &size, id, CAPACITY, NULL, 0);
    const size_t lengths[] = { CHUNK * 2 + 1, CHUNK * 3 + 17, CHUNK, CHUNK * 2,
                               1, CHUNK + 1, 0, CHUNK, 0, 0, 17 };
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; ++i)
        model_truncate(v, model, &size, id, lengths[i]);
    model_truncate(v, model, &size, id, 0);
    model_write(v, model, &size, id, 0, NULL, 0);
    model_write(v, model, &size, id, 7, data, 3);
    model_truncate(v, model, &size, id, 0);
    model_write(v, model, &size, id, CHUNK + 1, data, 3);
    model_truncate(v, model, &size, id, CHUNK * 2);
    model_write(v, model, &size, id, CHUNK * 2, data, 3);
    // Exercise a variety of overlapping writes and resizes against an independent
    // byte-array model, including growth after a previously discarded tail.
    uint32_t state = 0x51f17e;
    for (int i = 0; i < 120; ++i) {
        if (random_number(&state) % 3 == 0) {
            model_truncate(v, model, &size, id, random_number(&state) % (CAPACITY + 1));
        } else {
            size_t offset = random_number(&state) % CAPACITY;
            size_t length = random_number(&state) % (CHUNK + 14);
            if (length > CAPACITY - offset) length = CAPACITY - offset;
            model_write(v, model, &size, id, offset, data, length);
        }
    }
    OK(fslite_write_at(v, "/file", INT64_MAX, NULL, 0)); verify(v, model, size, id);
    STATUS(fslite_write_at(v, "/file", UINT64_MAX, NULL, 0), FSLITE_TOO_LARGE);
    STATUS(fslite_write_at(v, "/file", INT64_MAX, "x", 1), FSLITE_TOO_LARGE);
    STATUS(fslite_write_at(v, "/file", 0, "x", SIZE_MAX), FSLITE_TOO_LARGE);
    STATUS(fslite_truncate(v, "/file", UINT64_MAX), FSLITE_TOO_LARGE);
    STATUS(fslite_truncate(v, "/file", (uint64_t)INT64_MAX + 1), FSLITE_TOO_LARGE);
    STATUS(fslite_write_at(v, "/missing", 0, NULL, 0), FSLITE_NOT_FOUND);
    STATUS(fslite_truncate(v, "/missing", 0), FSLITE_NOT_FOUND);
    STATUS(fslite_write_at(v, "/", 0, NULL, 0), FSLITE_IS_DIRECTORY);
    STATUS(fslite_truncate(v, "/", 0), FSLITE_IS_DIRECTORY);
    STATUS(fslite_write_at(v, "/file/child", 0, "x", 1), FSLITE_NOT_DIRECTORY);
    STATUS(fslite_truncate(v, "/file/child", 0), FSLITE_NOT_DIRECTORY);
    const char *invalid[] = { "", "relative", "/file/", "/a/../file", "/\xff" };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        STATUS(fslite_write_at(v, invalid[i], 0, NULL, 0), FSLITE_INVALID_ARGUMENT);
        STATUS(fslite_truncate(v, invalid[i], 0), FSLITE_INVALID_ARGUMENT);
    }
    STATUS(fslite_write_at(NULL, "/file", 0, NULL, 0), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_write_at(v, NULL, 0, NULL, 0), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_write_at(v, "/file", 0, NULL, 1), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_truncate(NULL, "/file", 0), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_truncate(v, NULL, 0), FSLITE_INVALID_ARGUMENT);
    verify(v, model, size, id); CHECK(!fslite_in_transaction(v));
    free(model); free(data); fslite_close(v);
}

static void transactions(void) {
    char filename[64], snapshot[80]; new_path(filename);
    CHECK(snprintf(snapshot, sizeof snapshot, "%s-export", filename) < (int)sizeof snapshot);
    fslite_volume *v = NULL, *other = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    unsigned char original[CHUNK * 2 + 17], expected[CHUNK * 3];
    for (size_t i = 0; i < sizeof original; ++i) original[i] = (unsigned char)(i * 13 + i / 257);
    memcpy(expected, original, sizeof original);
    OK(fslite_write(v, "/file", original, sizeof original)); uint64_t id = file_id(v);
    OK(fslite_open(filename, 0, &other));
    OK(fslite_begin(v)); OK(fslite_write(v, "/earlier", "keep", 4));
    OK(fslite_write_at(v, "/file", CHUNK - 2, "changed", 7));
    OK(fslite_truncate(v, "/file", CHUNK + 3));
    STATUS(fslite_truncate(v, "/missing", 1), FSLITE_NOT_FOUND);
    CHECK(fslite_in_transaction(v)); verify(other, original, sizeof original, id);
    OK(fslite_rollback(v)); verify(v, original, sizeof original, id);
    fslite_info info; STATUS(fslite_stat(v, "/earlier", &info), FSLITE_NOT_FOUND);
    OK(fslite_begin(v));
    OK(fslite_write_at(v, "/file", CHUNK - 2, "changed", 7));
    memcpy(expected + CHUNK - 2, "changed", 7);
    OK(fslite_truncate(v, "/file", CHUNK + 3));
    OK(fslite_truncate(v, "/file", sizeof expected));
    memset(expected + CHUNK + 3, 0, sizeof expected - CHUNK - 3);
    OK(fslite_commit(v)); verify(other, expected, sizeof expected, id);
    fslite_writer *writer = NULL;
    OK(fslite_writer_open(v, "/pending", &writer));
    STATUS(fslite_write_at(v, "/file", 0, "x", 1), FSLITE_INVALID_STATE);
    STATUS(fslite_truncate(v, "/file", 0), FSLITE_INVALID_STATE);
    fslite_writer_close(writer);
    fslite_backup *backup = NULL;
    OK(fslite_backup_open(v, snapshot, &backup));
    STATUS(fslite_write_at(v, "/file", 0, "x", 1), FSLITE_INVALID_STATE);
    STATUS(fslite_truncate(v, "/file", 0), FSLITE_INVALID_STATE);
    fslite_backup_close(backup);
    OK(fslite_export(v, snapshot));
    fslite_volume *copy = NULL; OK(fslite_open(snapshot, 0, &copy));
    verify(copy, expected, sizeof expected, id); fslite_close(copy); CHECK(unlink(snapshot) == 0);
    OK(fslite_begin(v));
    OK(fslite_truncate(v, "/file", 1)); OK(fslite_write_at(v, "/file", CHUNK * 2, "x", 1));
    fslite_close(v); fslite_close(other);
    OK(fslite_open(filename, 0, &v)); verify(v, expected, sizeof expected, id); fslite_close(v);
    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        OK(fslite_open(filename, 0, &v)); OK(fslite_begin(v));
        OK(fslite_write_at(v, "/file", 8 * 1024 * 1024, "end", 3));
        OK(fslite_truncate(v, "/file", 0));
        _exit(0);
    }
    int status; CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    OK(fslite_open(filename, 0, &v)); verify(v, expected, sizeof expected, id); fslite_close(v);
    CHECK(unlink(filename) == 0);
}

#ifdef FSLITE_TEST_SQLITE
static void sql(const char *filename, const char *statement) {
    sqlite3 *db = NULL; CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, statement, NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}
static int64_t scalar(const char *filename, const char *query) {
    sqlite3 *db = NULL; sqlite3_stmt *statement = NULL;
    CHECK(sqlite3_open(filename, &db) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, query, -1, &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(statement, 0);
    CHECK(sqlite3_finalize(statement) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
    return value;
}
static void failures(void) {
    char filename[64]; new_path(filename);
    fslite_volume *v = NULL;
    unsigned char original[CHUNK * 2 + 17];
    for (size_t i = 0; i < sizeof original; ++i) original[i] = (unsigned char)(i * 19 + i / 251);
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    OK(fslite_write(v, "/file", original, sizeof original)); uint64_t id = file_id(v);
    const char *triggers[] = {
        "CREATE TRIGGER fail BEFORE UPDATE ON chunks WHEN NEW.chunk_index=1 BEGIN SELECT RAISE(FAIL,'patch'); END",
        "CREATE TRIGGER fail BEFORE INSERT ON chunks WHEN NEW.chunk_index=4 BEGIN SELECT RAISE(FAIL,'gap'); END",
        "CREATE TRIGGER fail BEFORE UPDATE ON chunks WHEN NEW.chunk_index=1 BEGIN SELECT RAISE(FAIL,'shrink'); END",
        "CREATE TRIGGER fail BEFORE UPDATE OF size ON nodes WHEN NEW.name='file' BEGIN SELECT RAISE(FAIL,'size'); END",
        "CREATE TRIGGER fail AFTER DELETE ON chunks WHEN OLD.chunk_index=1 BEGIN SELECT RAISE(FAIL,'delete'); END"
    };
    for (size_t which = 0; which < sizeof triggers / sizeof *triggers; ++which) {
        sql(filename, triggers[which]);
        for (int nested = 0; nested < 2; ++nested) {
            if (nested) { OK(fslite_begin(v)); OK(fslite_write(v, "/earlier", "keep", 4)); }
            switch (which) {
                case 0: STATUS(fslite_write_at(v, "/file", CHUNK - 3, "0123456789", 10), FSLITE_INTERNAL_ERROR); break;
                case 1: STATUS(fslite_write_at(v, "/file", CHUNK * 4 + 7, "x", 1), FSLITE_INTERNAL_ERROR); break;
                case 2: STATUS(fslite_truncate(v, "/file", CHUNK + 3), FSLITE_INTERNAL_ERROR); break;
                case 3:
                    STATUS(fslite_truncate(v, "/file", CHUNK * 3 + 11), FSLITE_INTERNAL_ERROR);
                    STATUS(fslite_truncate(v, "/file", 0), FSLITE_INTERNAL_ERROR);
                    STATUS(fslite_write_at(v, "/file", sizeof original, "x", 1), FSLITE_INTERNAL_ERROR);
                    break;
                case 4: STATUS(fslite_truncate(v, "/file", 0), FSLITE_INTERNAL_ERROR); break;
            }
            CHECK(fslite_in_transaction(v) == (uint32_t)nested);
            verify(v, original, sizeof original, id);
            if (nested) {
                char bytes[4]; size_t count;
                OK(fslite_read(v, "/earlier", 0, bytes, sizeof bytes, &count));
                CHECK(count == 4 && memcmp(bytes, "keep", 4) == 0);
                OK(fslite_commit(v));
            }
        }
        sql(filename, "DROP TRIGGER fail");
        CHECK(scalar(filename, "SELECT count(*) FROM chunks WHERE node_id=(SELECT id FROM nodes WHERE name='file')") == 3);
    }
    sqlite3 *locker = NULL; CHECK(sqlite3_open(filename, &locker) == SQLITE_OK);
    CHECK(sqlite3_exec(locker, "BEGIN; SELECT count(*) FROM nodes", NULL, NULL, NULL) == SQLITE_OK);
    STATUS(fslite_write_at(v, "/file", CHUNK-3, "changed", 7), FSLITE_BUSY);
    CHECK(!fslite_in_transaction(v)); verify(v, original, sizeof original, id);
    STATUS(fslite_truncate(v, "/file", 0), FSLITE_BUSY);
    CHECK(!fslite_in_transaction(v)); verify(v, original, sizeof original, id);
    CHECK(sqlite3_exec(locker, "ROLLBACK; BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    STATUS(fslite_write_at(v, "/file", 0, "x", 1), FSLITE_BUSY);
    STATUS(fslite_truncate(v, "/file", CHUNK), FSLITE_BUSY);
    CHECK(!fslite_in_transaction(v));
    CHECK(sqlite3_exec(locker, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(locker) == SQLITE_OK);
    // Missing retained chunks are corruption. A preceding chunk update rolls back.
    sql(filename, "DELETE FROM chunks WHERE node_id=(SELECT id FROM nodes WHERE name='file') AND chunk_index=1");
    STATUS(fslite_write_at(v, "/file", CHUNK-3, "0123456789", 10), FSLITE_INVALID_FORMAT);
    STATUS(fslite_truncate(v, "/file", CHUNK+3), FSLITE_INVALID_FORMAT);
    unsigned char buffer[CHUNK]; size_t count;
    OK(fslite_read(v, "/file", 0, buffer, sizeof buffer, &count));
    CHECK(count == CHUNK && memcmp(buffer, original, count) == 0);
    OK(fslite_read(v, "/file", CHUNK * 2, buffer, sizeof buffer, &count));
    CHECK(count == 17 && memcmp(buffer, original + CHUNK * 2, count) == 0);
    fslite_close(v); CHECK(unlink(filename) == 0);
}

static void chunk_updates(void) {
    char filename[64]; new_path(filename);
    fslite_volume *v = NULL;
    unsigned char initial[CHUNK * 3 + 17]; memset(initial, 'a', sizeof initial);
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &v));
    OK(fslite_write(v, "/file", initial, sizeof initial));
    sql(filename, "CREATE TABLE audit(kind TEXT,idx INTEGER);"
        "CREATE TRIGGER audit_insert AFTER INSERT ON chunks BEGIN INSERT INTO audit VALUES('I',NEW.chunk_index); END;"
        "CREATE TRIGGER audit_update AFTER UPDATE ON chunks BEGIN INSERT INTO audit VALUES('U',NEW.chunk_index); END;"
        "CREATE TRIGGER audit_delete AFTER DELETE ON chunks BEGIN INSERT INTO audit VALUES('D',OLD.chunk_index); END;");
    OK(fslite_write_at(v, "/file", CHUNK+3, "x", 1));
    CHECK(scalar(filename, "SELECT count(*) FROM audit") == 1);
    CHECK(scalar(filename, "SELECT count(*) FROM audit WHERE kind='U' AND idx=1") == 1);
    sql(filename, "DELETE FROM audit");
    OK(fslite_write_at(v, "/file", CHUNK-1, "xy", 2));
    CHECK(scalar(filename, "SELECT count(*) FROM audit") == 2);
    CHECK(scalar(filename, "SELECT count(*) FROM audit WHERE kind='U' AND idx IN(0,1)") == 2);
    sql(filename, "DELETE FROM audit");
    OK(fslite_truncate(v, "/file", CHUNK+3));
    CHECK(scalar(filename, "SELECT count(*) FROM audit WHERE kind='D'") == 2);
    CHECK(scalar(filename, "SELECT count(*) FROM audit WHERE kind='U' AND idx=1") == 1);
    CHECK(scalar(filename, "SELECT length(data) FROM chunks WHERE chunk_index=1") == 3);
    sql(filename, "DELETE FROM audit");
    OK(fslite_truncate(v, "/file", CHUNK*3+17));
    CHECK(scalar(filename, "SELECT count(*) FROM audit") == 3);
    CHECK(scalar(filename, "SELECT count(*) FROM audit WHERE kind='U' AND idx=1") == 1);
    CHECK(scalar(filename, "SELECT count(*) FROM audit WHERE kind='I' AND idx IN(2,3)") == 2);
    CHECK(scalar(filename, "SELECT sum(length(data)) FROM chunks") == CHUNK*3+17);
    sql(filename, "DELETE FROM audit");
    OK(fslite_write_at(v, "/file", INT64_MAX, NULL, 0));
    OK(fslite_truncate(v, "/file", CHUNK*3+17));
    CHECK(scalar(filename, "SELECT count(*) FROM audit") == 0);
    OK(fslite_truncate(v, "/file", 0));
    CHECK(scalar(filename, "SELECT count(*) FROM chunks") == 0);
    CHECK(scalar(filename, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    CHECK(scalar(filename, "PRAGMA user_version") == 2);
    fslite_close(v); CHECK(unlink(filename) == 0);
}
#endif

void test_random_access(void) {
    semantics();
    transactions();
#ifdef FSLITE_TEST_SQLITE
    failures();
    chunk_updates();
#endif
}
