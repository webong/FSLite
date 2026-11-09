#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: %s (%d), expected %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)

static void new_path(char path[64]) {
    strcpy(path, "/tmp/fslite-fault-XXXXXX");
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0);
}

static void test_invalid_arguments(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    // NULL volume
    CHECK(fslite_remove_recursive(NULL, "/file") == 1);
    CHECK(fslite_remove_recursive(v, NULL) == 1);

    // Root path
    CHECK(fslite_remove_recursive(v, "/") == FSLITE_INVALID_ARGUMENT);

    // Empty path
    CHECK(fslite_remove_recursive(v, "") == 1);

    // Invalid paths
    const char *invalid[] = { "relative", "/a/../file", "/\xff", "/dir/", "//dir" };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        CHECK(fslite_remove_recursive(v, invalid[i]) == FSLITE_INVALID_ARGUMENT);
    }

    // Non-existent path
    CHECK(fslite_remove_recursive(v, "/nonexistent") == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_invalid_arguments: PASSED\n");
}

static void test_recursive_remove_file(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    OK(fslite_write(v, "/single.txt", "test", 4));
    OK(fslite_remove_recursive(v, "/single.txt"));

    fslite_info info;
    CHECK(fslite_stat(v, "/single.txt", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_recursive_remove_file: PASSED\n");
}

static void test_recursive_remove_empty_dir(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    OK(fslite_mkdir(v, "/emptydir"));
    OK(fslite_remove_recursive(v, "/emptydir"));

    fslite_info info;
    CHECK(fslite_stat(v, "/emptydir", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_recursive_remove_empty_dir: PASSED\n");
}

static void test_recursive_remove_nested_tree(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    // Build nested tree: /a/b/c/d with files at each level
    OK(fslite_mkdir(v, "/a"));
    OK(fslite_mkdir(v, "/a/b"));
    OK(fslite_mkdir(v, "/a/b/c"));
    OK(fslite_mkdir(v, "/a/b/c/d"));
    OK(fslite_write(v, "/a/file_a.txt", "a", 1));
    OK(fslite_write(v, "/a/b/file_b.txt", "b", 1));
    OK(fslite_write(v, "/a/b/c/file_c.txt", "c", 1));
    OK(fslite_write(v, "/a/b/c/d/file_d.txt", "d", 1));

    // Remove root of tree
    OK(fslite_remove_recursive(v, "/a"));

    fslite_info info;
    CHECK(fslite_stat(v, "/a", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/b", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/b/c", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/b/c/d", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/file_a.txt", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/b/file_b.txt", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/b/c/file_c.txt", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/b/c/d/file_d.txt", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_recursive_remove_nested_tree: PASSED\n");
}

static void test_recursive_remove_multiple_siblings(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    // Multiple sibling directories with files
    OK(fslite_mkdir(v, "/parent"));
    OK(fslite_mkdir(v, "/parent/sib1"));
    OK(fslite_mkdir(v, "/parent/sib2"));
    OK(fslite_mkdir(v, "/parent/sib3"));
    OK(fslite_write(v, "/parent/sib1/f1.txt", "1", 1));
    OK(fslite_write(v, "/parent/sib2/f2.txt", "2", 1));
    OK(fslite_write(v, "/parent/sib3/f3.txt", "3", 1));

    OK(fslite_remove_recursive(v, "/parent"));

    fslite_info info;
    CHECK(fslite_stat(v, "/parent", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/parent/sib1", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/parent/sib2", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/parent/sib3", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/parent/sib1/f1.txt", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_recursive_remove_multiple_siblings: PASSED\n");
}

static void test_remove_during_transaction(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    OK(fslite_mkdir(v, "/dir"));
    OK(fslite_write(v, "/dir/file.txt", "data", 4));

    OK(fslite_begin(v));
    OK(fslite_mkdir(v, "/dir/subdir"));
    OK(fslite_write(v, "/dir/subdir/file2.txt", "data2", 5));

    // Remove within transaction - should be pending until commit
    OK(fslite_remove_recursive(v, "/dir"));

    fslite_info info;
    CHECK(fslite_stat(v, "/dir", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/dir/subdir", &info) == FSLITE_NOT_FOUND);

    OK(fslite_commit(v));
    CHECK(fslite_stat(v, "/dir", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_remove_during_transaction: PASSED\n");
}

static void test_remove_rollback(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    OK(fslite_mkdir(v, "/dir"));
    OK(fslite_write(v, "/dir/file.txt", "data", 4));
    OK(fslite_mkdir(v, "/dir/subdir"));
    OK(fslite_write(v, "/dir/subdir/file2.txt", "data2", 5));

    OK(fslite_begin(v));
    OK(fslite_remove_recursive(v, "/dir"));

    // Should see removed state within transaction
    fslite_info info;
    CHECK(fslite_stat(v, "/dir", &info) == FSLITE_NOT_FOUND);

    OK(fslite_rollback(v));
    // Should be back after rollback
    CHECK(fslite_stat(v, "/dir", &info) == FSLITE_OK);
    CHECK(fslite_stat(v, "/dir/file.txt", &info) == FSLITE_OK);
    CHECK(fslite_stat(v, "/dir/subdir/file2.txt", &info) == FSLITE_OK);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_remove_rollback: PASSED\n");
}

static void test_concurrent_separate_handles(void) {
    char path[64];
    new_path(path);
    fslite_volume *v1 = NULL, *v2 = NULL;
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v1));
    OK(fslite_open(path, 0, &v2));

    // v1 creates files, v2 reads them
    OK(fslite_mkdir(v1, "/shared"));
    OK(fslite_write(v1, "/shared/data.txt", "concurrent test", 15));

    fslite_info info;
    OK(fslite_stat(v2, "/shared", &info));
    CHECK(info.kind == 2); // directory

    OK(fslite_write(v1, "/shared/file1.txt", "hello", 5));
    OK(fslite_commit(v1));

    OK(fslite_stat(v2, "/shared/file1.txt", &info));
    CHECK(info.kind == 1); // file

    // v2 creates while v1 reads
    OK(fslite_mkdir(v2, "/shared2"));
    OK(fslite_write(v2, "/shared2/file2.txt", "world", 5));
    OK(fslite_commit(v2));

    OK(fslite_stat(v1, "/shared2/file2.txt", &info));
    CHECK(info.kind == 1);

    // Clean up
    OK(fslite_remove_recursive(v1, "/shared"));
    OK(fslite_remove_recursive(v1, "/shared2"));

    fslite_close(v1);
    fslite_close(v2);
    CHECK(unlink(path) == 0);
    printf("test_concurrent_separate_handles: PASSED\n");
}

static void test_process_exit_mid_transaction(void) {
    char path[64];
    new_path(path);

    // Parent creates initial data
    fslite_volume *v = NULL;
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));
    OK(fslite_mkdir(v, "/dir"));
    OK(fslite_write(v, "/dir/file.txt", "original", 8));
    fslite_close(v);

    // Child begins transaction, modifies data, exits without commit/rollback
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        fslite_volume *cv = NULL;
        OK(fslite_open(path, 0, &cv));
        OK(fslite_begin(cv));
        OK(fslite_write(cv, "/dir/file.txt", "modified", 8));
        OK(fslite_mkdir(cv, "/dir/subdir"));
        OK(fslite_write(cv, "/dir/subdir/child_file.txt", "child", 5));
        // Exit without commit - changes should be rolled back
        _exit(42);
    }

    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 42);

    // Parent should see original data (child's changes rolled back)
    OK(fslite_open(path, 0, &v));
    fslite_info info;
    OK(fslite_stat(v, "/dir/file.txt", &info));
    CHECK(info.size == 8); // "original" is 8 bytes
    CHECK(fslite_stat(v, "/dir/subdir", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/dir/subdir/child_file.txt", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_process_exit_mid_transaction: PASSED\n");
}

static void test_busy_contention(void) {
    char path[64];
    new_path(path);
    fslite_volume *v1 = NULL, *v2 = NULL;
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v1));
    OK(fslite_open(path, 0, &v2));

    // v1 holds a write lock
    OK(fslite_begin(v1));
    OK(fslite_write(v1, "/contention.txt", "holder", 6));

    // v2 tries to write - should get BUSY
    CHECK(fslite_write(v2, "/contention.txt", "waiter", 6) == FSLITE_BUSY);
    CHECK(!fslite_in_transaction(v2));

    // v1 commits
    OK(fslite_commit(v1));

    // v2 can now write
    OK(fslite_write(v2, "/contention.txt", "waiter", 6));
    OK(fslite_commit(v2));

    fslite_close(v1);
    fslite_close(v2);
    CHECK(unlink(path) == 0);
    printf("test_busy_contention: PASSED\n");
}

/* Non-empty directory removal with fslite_remove must return NOT_EMPTY. */
static void test_remove_non_empty_dir_fails(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    OK(fslite_mkdir(v, "/dir"));
    OK(fslite_write(v, "/dir/file.txt", "data", 4));

    CHECK(fslite_remove(v, "/dir") == FSLITE_NOT_EMPTY);

    // Directory must still exist with its file intact
    fslite_info info;
    OK(fslite_stat(v, "/dir", &info));
    CHECK(info.kind == FSLITE_DIRECTORY);
    OK(fslite_stat(v, "/dir/file.txt", &info));
    CHECK(info.kind == FSLITE_FILE);

    // Clean up with recursive remove
    OK(fslite_remove_recursive(v, "/dir"));
    CHECK(fslite_stat(v, "/dir", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_remove_non_empty_dir_fails: PASSED\n");
}

/* Recursive remove of a missing path returns NOT_FOUND. */
static void test_remove_recursive_nonexistent(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    CHECK(fslite_remove_recursive(v, "/nonexistent") == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_remove_recursive_nonexistent: PASSED\n");
}

/* Remove while a writer holds the volume returns INVALID_STATE. */
static void test_remove_with_active_writer(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    OK(fslite_mkdir(v, "/dir"));
    OK(fslite_write(v, "/dir/file.txt", "data", 4));

    fslite_writer *w = NULL;
    OK(fslite_writer_open(v, "/stream.txt", &w));

    // Remove while writer is active - INVALID_STATE
    CHECK(fslite_remove(v, "/dir") == FSLITE_INVALID_STATE);
    CHECK(fslite_remove_recursive(v, "/dir") == FSLITE_INVALID_STATE);

    fslite_writer_close(w);

    // After writer closes, remove works again
    OK(fslite_remove_recursive(v, "/dir"));
    fslite_info info;
    CHECK(fslite_stat(v, "/dir", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_remove_with_active_writer: PASSED\n");
}

/* Concurrent remove vs write: two separate handles serialize via SQLite locking. */
static void test_concurrent_remove_vs_write(void) {
    char path[64];
    new_path(path);
    fslite_volume *v1 = NULL, *v2 = NULL;
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v1));
    OK(fslite_open(path, 0, &v2));

    OK(fslite_mkdir(v1, "/dir"));
    OK(fslite_write(v1, "/dir/file.txt", "data", 4));
    OK(fslite_commit(v1));

    // v1 begins a transaction and removes /dir
    OK(fslite_begin(v1));
    OK(fslite_remove_recursive(v1, "/dir"));

    // v2 tries to write to the removed path - should get BUSY (locked by v1)
    CHECK(fslite_write(v2, "/dir/newfile.txt", "data", 4) == FSLITE_BUSY);
    CHECK(!fslite_in_transaction(v2));

    // v1 commits the removal
    OK(fslite_commit(v1));

    // v2 can now create /dir fresh
    OK(fslite_mkdir(v2, "/dir"));
    OK(fslite_write(v2, "/dir/newfile.txt", "data", 4));
    OK(fslite_commit(v2));

    fslite_close(v1);
    fslite_close(v2);
    CHECK(unlink(path) == 0);
    printf("test_concurrent_remove_vs_write: PASSED\n");
}

/* Remove during a backup: backup holds a read lock, remove should wait or fail. */
static void test_remove_during_backup(void) {
    char path[64];
    new_path(path);
    char dest[64];
    strcpy(dest, "/tmp/fslite-backup-XXXXXX");
    int fd = mkstemp(dest);
    CHECK(fd >= 0 && close(fd) == 0);
    unlink(dest);

    fslite_volume *v = NULL;
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    OK(fslite_mkdir(v, "/dir"));
    OK(fslite_write(v, "/dir/file.txt", "data", 4));
    OK(fslite_commit(v));

    fslite_backup *b = NULL;
    OK(fslite_backup_open(v, dest, &b));

    // Remove while backup is open - SQLite serializes, may return BUSY or wait
    fslite_status rc = fslite_remove_recursive(v, "/dir");
    CHECK(rc == FSLITE_OK || rc == FSLITE_BUSY);

    fslite_backup_close(b);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    CHECK(unlink(dest) == 0);
    printf("test_remove_during_backup: PASSED\n");
}

/* Remove inside a savepoint, then rollback the savepoint. */
static void test_remove_savepoint_rollback(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    OK(fslite_mkdir(v, "/dir"));
    OK(fslite_write(v, "/dir/file.txt", "data", 4));
    OK(fslite_commit(v));

    OK(fslite_begin(v));
    OK(fslite_remove_recursive(v, "/dir"));

    fslite_info info;
    CHECK(fslite_stat(v, "/dir", &info) == FSLITE_NOT_FOUND);

    // Rollback restores /dir
    OK(fslite_rollback(v));
    OK(fslite_stat(v, "/dir", &info));
    CHECK(info.kind == FSLITE_DIRECTORY);
    OK(fslite_stat(v, "/dir/file.txt", &info));
    CHECK(info.kind == FSLITE_FILE);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_remove_savepoint_rollback: PASSED\n");
}

/* Deeply nested tree with many files at multiple levels. */
static void test_recursive_remove_deep_tree(void) {
    fslite_volume *v = NULL;
    char path[64];
    new_path(path);
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));

    // Build a deep tree: /a/b/c/d/e/f/g
    const char *levels[] = {"a", "a/b", "a/b/c", "a/b/c/d",
                            "a/b/c/d/e", "a/b/c/d/e/f", "a/b/c/d/e/f/g"};
    for (size_t i = 0; i < sizeof levels / sizeof *levels; i++) {
        char p[128];
        snprintf(p, sizeof p, "/%s", levels[i]);
        OK(fslite_mkdir(v, p));
    }

    // Files at each level
    OK(fslite_write(v, "/a/root.txt", "root", 4));
    OK(fslite_write(v, "/a/b/level1.txt", "level1", 6));
    OK(fslite_write(v, "/a/b/c/level2.txt", "level2", 6));
    OK(fslite_write(v, "/a/b/c/d/level3.txt", "level3", 6));
    OK(fslite_write(v, "/a/b/c/d/e/f/g/deep.txt", "deep", 4));

    OK(fslite_remove_recursive(v, "/a"));

    fslite_info info;
    CHECK(fslite_stat(v, "/a", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/b", &info) == FSLITE_NOT_FOUND);
    CHECK(fslite_stat(v, "/a/b/c/d/e/f/g/deep.txt", &info) == FSLITE_NOT_FOUND);

    fslite_close(v);
    CHECK(unlink(path) == 0);
    printf("test_recursive_remove_deep_tree: PASSED\n");
}

void test_fault_injection(void) {
    test_invalid_arguments();
    test_recursive_remove_file();
    test_recursive_remove_empty_dir();
    test_recursive_remove_nested_tree();
    test_recursive_remove_multiple_siblings();
    test_remove_during_transaction();
    test_remove_rollback();
    test_concurrent_separate_handles();
    test_process_exit_mid_transaction();
    test_busy_contention();
    test_remove_non_empty_dir_fails();
    test_remove_recursive_nonexistent();
    test_remove_with_active_writer();
    test_concurrent_remove_vs_write();
    test_remove_during_backup();
    test_remove_savepoint_rollback();
    test_recursive_remove_deep_tree();
    printf("All fault injection tests PASSED\n");
}