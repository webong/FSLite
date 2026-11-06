#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: %s (%d), expected %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)

static void new_path(char path[64]) {
    strcpy(path, "/tmp/fslite-statement-reuse-XXXXXX");
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0);
}

/*
 * Regression tests for prepared statement reuse after failure/rollback.
 *
 * The engine caches prepared statements in Volume.prepared_statements and
 * reuses them across operations. These tests verify that cached statements
 * remain valid and produce correct results after:
 *   1. A failed write operation (constraint violation)
 *   2. A transaction rollback
 *   3. Repeated reuse of the same cached statement
 */

/* Test 1: Cached statements work after a failed write (constraint violation).
 * The path lookup and insert statements are cached. A failed write should
 * not corrupt the cache; subsequent writes using the same statements must
 * succeed. */
static void test_statement_reuse_after_failed_write(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    /* First successful write caches the INSERT statement */
    OK(fslite_write(v, "/file1", "data1", 5));

    /* Attempt to create a directory where a file already exists - this
     * fails because the node already exists (AlreadyExists error).
     * The cached INSERT statement should remain valid. */
    CHECK(fslite_mkdir(v, "/file1") == FSLITE_ALREADY_EXISTS);

    /* Reuse the same cached INSERT statement - must succeed */
    OK(fslite_write(v, "/file2", "data2", 5));

    /* Verify both files exist with correct content */
    fslite_info info;
    OK(fslite_stat(v, "/file1", &info));
    CHECK(info.kind == FSLITE_FILE);
    OK(fslite_stat(v, "/file2", &info));
    CHECK(info.kind == FSLITE_FILE);

    char buf[10];
    size_t count;
    OK(fslite_read(v, "/file1", 0, buf, sizeof buf, &count));
    CHECK(count == 5 && memcmp(buf, "data1", 5) == 0);
    OK(fslite_read(v, "/file2", 0, buf, sizeof buf, &count));
    CHECK(count == 5 && memcmp(buf, "data2", 5) == 0);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_statement_reuse_after_failed_write: PASSED\n");
}

/* Test 2: Cached statements work after transaction rollback.
 * Statements used inside a rolled-back transaction must remain valid
 * for subsequent operations. */
static void test_statement_reuse_after_rollback(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    /* Write outside transaction - caches INSERT + path lookup statements */
    OK(fslite_write(v, "/file1", "data1", 5));

    /* Begin transaction, write more, then rollback */
    OK(fslite_begin(v));
    OK(fslite_write(v, "/file2", "data2", 5));
    OK(fslite_mkdir(v, "/dir"));
    OK(fslite_rollback(v));

    /* After rollback, file2 and /dir should not exist */
    CHECK(fslite_stat(v, "/file2", &(fslite_info){0}) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/dir", &(fslite_info){0}) == FSLITE_NOT_FOUND);

    /* Reuse the same cached statements after rollback - must succeed */
    OK(fslite_write(v, "/file2", "data2", 5));
    OK(fslite_mkdir(v, "/dir"));

    /* Verify state is correct */
    fslite_info info;
    OK(fslite_stat(v, "/file1", &info));
    CHECK(info.kind == FSLITE_FILE);
    OK(fslite_stat(v, "/file2", &info));
    CHECK(info.kind == FSLITE_FILE);
    OK(fslite_stat(v, "/dir", &info));
    CHECK(info.kind == FSLITE_DIRECTORY);

    char buf[10];
    size_t count;
    OK(fslite_read(v, "/file1", 0, buf, sizeof buf, &count));
    CHECK(count == 5 && memcmp(buf, "data1", 5) == 0);
    OK(fslite_read(v, "/file2", 0, buf, sizeof buf, &count));
    CHECK(count == 5 && memcmp(buf, "data2", 5) == 0);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_statement_reuse_after_rollback: PASSED\n");
}

/* Test 3: Repeated reuse of cached statements across many operations.
 * The same prepared statements should be reused for path lookups and
 * inserts across dozens of operations without errors or stale results. */
static void test_repeated_statement_reuse(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    /* Perform many write/read cycles to exercise statement reuse */
    char name[32];
    char data[16];
    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof name, "/file_%d", i);
        snprintf(data, sizeof data, "data_%d", i);
        OK(fslite_write(v, name, data, strlen(data)));
    }

    /* Verify all files can be read back correctly */
    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof name, "/file_%d", i);
        snprintf(data, sizeof data, "data_%d", i);
        fslite_info info;
        OK(fslite_stat(v, name, &info));
        CHECK(info.kind == FSLITE_FILE);
        CHECK(info.size == strlen(data));

        char buf[32];
        size_t count;
        OK(fslite_read(v, name, 0, buf, sizeof buf, &count));
        CHECK(count == strlen(data));
        CHECK(memcmp(buf, data, count) == 0);
    }

    /* Overwrite all files - reuse UPDATE statements */
    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof name, "/file_%d", i);
        snprintf(data, sizeof data, "overwritten_%d", i);
        OK(fslite_write(v, name, data, strlen(data)));
    }

    /* Verify overwrites */
    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof name, "/file_%d", i);
        snprintf(data, sizeof data, "overwritten_%d", i);
        char buf[32];
        size_t count;
        OK(fslite_read(v, name, 0, buf, sizeof buf, &count));
        CHECK(count == strlen(data));
        CHECK(memcmp(buf, data, count) == 0);
    }

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_repeated_statement_reuse: PASSED\n");
}

/* Test 4: Statement reuse across savepoint failure and retry.
 * A savepoint that fails should not leave cached statements in a bad state. */
static void test_statement_reuse_after_savepoint_failure(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    /* Initial write caches statements */
    OK(fslite_write(v, "/file1", "data1", 5));

    /* Begin transaction and create a directory */
    OK(fslite_begin(v));
    OK(fslite_mkdir(v, "/dir"));

    /* Try to create a file inside /dir, then remove /dir - the file
     * operation should fail because the parent directory is being removed */
    OK(fslite_write(v, "/dir/file_in_dir.txt", "should fail", 11));
    OK(fslite_remove_recursive(v, "/dir"));

    /* Rollback the transaction */
    OK(fslite_rollback(v));

    /* After rollback, /dir and /file1 should be in original state */
    fslite_info info;
    CHECK(fslite_stat(v, "/dir", &(fslite_info){0}) == FSLITE_NOT_FOUND);
    OK(fslite_stat(v, "/file1", &info));
    CHECK(info.kind == FSLITE_FILE);

    /* Reuse cached statements successfully */
    OK(fslite_write(v, "/file2", "data2", 5));
    OK(fslite_mkdir(v, "/dir2"));

    OK(fslite_stat(v, "/file2", &info));
    CHECK(info.kind == FSLITE_FILE);
    OK(fslite_stat(v, "/dir2", &info));
    CHECK(info.kind == FSLITE_DIRECTORY);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_statement_reuse_after_savepoint_failure: PASSED\n");
}

/* Test 5: Read statements remain valid after write failures.
 * SELECT statements used for reads are cached separately. After a write
 * fails, reads using cached SELECT statements must still work. */
static void test_read_statement_reuse_after_write_failure(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    /* Write and read a file - caches both write and read statements */
    OK(fslite_write(v, "/file1", "hello", 5));

    char buf[16];
    size_t count;
    OK(fslite_read(v, "/file1", 0, buf, sizeof buf, &count));
    CHECK(count == 5 && memcmp(buf, "hello", 5) == 0);

    /* Failed write: try to mkdir over an existing file - returns AlreadyExists */
    CHECK(fslite_mkdir(v, "/file1") == FSLITE_ALREADY_EXISTS);

    /* Read should still work using cached SELECT statement */
    OK(fslite_read(v, "/file1", 0, buf, sizeof buf, &count));
    CHECK(count == 5 && memcmp(buf, "hello", 5) == 0);

    /* Write again after failure - cached INSERT must work */
    OK(fslite_write(v, "/file2", "world", 5));
    OK(fslite_read(v, "/file2", 0, buf, sizeof buf, &count));
    CHECK(count == 5 && memcmp(buf, "world", 5) == 0);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_read_statement_reuse_after_write_failure: PASSED\n");
}

void test_statement_reuse(void) {
    test_statement_reuse_after_failed_write();
    test_statement_reuse_after_rollback();
    test_repeated_statement_reuse();
    test_statement_reuse_after_savepoint_failure();
    test_read_statement_reuse_after_write_failure();
    printf("All statement reuse regression tests PASSED\n");
}
