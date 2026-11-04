#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "fslite.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef FSLITE_TEST_SQLITE
#include "sqlite3.h"
#include <dlfcn.h>
#include <errno.h>
static int fail_directory_sync;
static int failed_directory_syncs;
static struct stat sync_directory;
/* Static test consumer interposes fsync; production code has no fault hooks. */
int fsync(int fd) {
    static int (*real_sync)(int);
    if (!real_sync) real_sync = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
    if (!real_sync) abort();
    struct stat st;
    if (fail_directory_sync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
        st.st_dev == sync_directory.st_dev && st.st_ino == sync_directory.st_ino) {
        failed_directory_syncs++; errno = EIO; return -1;
    }
    return real_sync(fd);
}
#endif

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: %s (%d), expected %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)
_Static_assert(sizeof(fslite_backup_progress) == 24, "backup progress ABI");
_Static_assert(offsetof(fslite_backup_progress, done) == 16, "backup progress layout");

static void path(char out[1024], const char *directory, const char *name) {
    CHECK(snprintf(out, 1024, "%s/%s", directory, name) < 1024);
}
static int stages(const char *directory) {
    DIR *dir = opendir(directory); CHECK(dir != NULL);
    int count = 0; struct dirent *entry;
    while ((entry = readdir(dir))) if (strncmp(entry->d_name, ".fslite-backup-", 15) == 0) count++;
    CHECK(closedir(dir) == 0); return count;
}
/* Only called on this test's private mkdtemp directory, including crash residue. */
static void cleanup(const char *directory) {
    DIR *dir = opendir(directory); CHECK(dir != NULL);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char name[1024]; path(name, directory, entry->d_name);
        struct stat stat; CHECK(lstat(name, &stat) == 0);
        if (S_ISDIR(stat.st_mode)) cleanup(name); else CHECK(unlink(name) == 0);
    }
    CHECK(closedir(dir) == 0); CHECK(rmdir(directory) == 0);
}
static void content(fslite_volume *v, const char *name, const char *expected) {
    char bytes[128]; size_t n = 0;
    OK(fslite_read(v, name, 0, bytes, sizeof bytes, &n));
    CHECK(n == strlen(expected) && memcmp(bytes, expected, n) == 0);
}
static void absent(const char *filename) { struct stat st; CHECK(lstat(filename, &st) != 0); }
static void complete(fslite_backup *backup) {
    fslite_backup_progress progress;
    for (int i = 0; i < 10000; ++i) {
        OK(fslite_backup_step(backup, 16, &progress));
        CHECK(progress.reserved == 0 && progress.remaining_pages <= progress.total_pages);
        if (progress.done) { CHECK(progress.remaining_pages == 0); return; }
    }
    CHECK(0 && "backup did not complete");
}
static void sentinel(const char *filename) {
    FILE *f = fopen(filename, "wb"); CHECK(f != NULL);
    CHECK(fwrite("keep", 1, 4, f) == 4 && fclose(f) == 0);
}
static void check_sentinel(const char *filename) {
    FILE *f = fopen(filename, "rb"); CHECK(f != NULL);
    char bytes[5]; CHECK(fread(bytes, 1, sizeof bytes, f) == 4);
    CHECK(memcmp(bytes, "keep", 4) == 0 && fclose(f) == 0);
}

void test_backup(void) {
    char directory[] = "/tmp/fslite-backup-tests-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char source[1024], destination[1024], collision[1024];
    path(source, directory, "source.fslite"); path(destination, directory, "snapshot.fslite");
    path(collision, directory, "collision");
    fslite_volume *v = NULL, *other = NULL, *copy = NULL;
    fslite_backup *backup = NULL, *blocked = NULL;
    fslite_backup_progress progress;
    fslite_info info;
    OK(fslite_open(source, FSLITE_OPEN_CREATE, &v));
    OK(fslite_write(v, "/one", "old", 3)); OK(fslite_write(v, "/two", "old", 3));
    unsigned char payload[65536 * 4 + 11];
    for (size_t i = 0; i < sizeof payload; ++i) payload[i] = (unsigned char)(i * 31 + i / 251);
    OK(fslite_write(v, "/payload", payload, sizeof payload));
    OK(fslite_stat(v, "/payload", &info)); uint64_t payload_id = info.id;
    OK(fslite_open(source, 0, &other));
    OK(fslite_backup_open(v, destination, &backup));
    absent(destination); CHECK(stages(directory) == 1);
    CHECK(!fslite_in_transaction(v));
    STATUS(fslite_begin(v), FSLITE_INVALID_STATE);
    STATUS(fslite_stat(v, "/one", &info), FSLITE_INVALID_STATE);
    STATUS(fslite_write(v, "/blocked", "x", 1), FSLITE_INVALID_STATE);
    STATUS(fslite_rename(v, "/one", "/two"), FSLITE_INVALID_STATE);
    STATUS(fslite_remove(v, "/one"), FSLITE_INVALID_STATE);
    STATUS(fslite_export(v, collision), FSLITE_INVALID_STATE);
    STATUS(fslite_backup_open(v, collision, &blocked), FSLITE_INVALID_STATE); CHECK(blocked == NULL);
    STATUS(fslite_backup_finish(backup), FSLITE_INVALID_STATE);
    STATUS(fslite_backup_step(backup, 0, &progress), FSLITE_INVALID_ARGUMENT);
    CHECK(progress.done == 0 && progress.total_pages == 0);
    STATUS(fslite_backup_step(backup, UINT32_MAX, &progress), FSLITE_INVALID_ARGUMENT);
    OK(fslite_backup_step(backup, 1, &progress)); CHECK(!progress.done);
    // One transaction changes two related records between copy steps.
    OK(fslite_begin(other));
    OK(fslite_write(other, "/one", "new", 3)); OK(fslite_write(other, "/two", "new", 3));
    OK(fslite_commit(other));
    // Uncommitted source changes are excluded; the external writer can roll back.
    OK(fslite_begin(other)); OK(fslite_write(other, "/one", "uncommitted", 11));
    complete(backup); absent(destination);
    OK(fslite_rollback(other));
    // Completion freezes the copied state even if publication is delayed.
    OK(fslite_begin(other));
    OK(fslite_write(other, "/one", "later", 5)); OK(fslite_write(other, "/two", "later", 5));
    OK(fslite_commit(other));
    STATUS(fslite_backup_step(backup, 1, &progress), FSLITE_INVALID_STATE);
    OK(fslite_backup_finish(backup));
    STATUS(fslite_backup_finish(backup), FSLITE_INVALID_STATE);
    fslite_backup_close(backup); CHECK(stages(directory) == 0);
    OK(fslite_open(destination, 0, &copy));
    content(copy, "/one", "new"); content(copy, "/two", "new");
    OK(fslite_stat(copy, "/payload", &info)); CHECK(info.id == payload_id && info.size == sizeof payload);
    unsigned char output[sizeof payload]; size_t n = 0;
    OK(fslite_read(copy, "/payload", 0, output, sizeof output, &n));
    CHECK(n == sizeof payload && memcmp(output, payload, n) == 0);
    fslite_close(copy);
    content(v, "/one", "later");
    STATUS(fslite_export(v, destination), FSLITE_ALREADY_EXISTS);
    STATUS(fslite_export(v, source), FSLITE_ALREADY_EXISTS);

    // Never overwrite files, empty files, directories, links, or SQLite sidecars.
    sentinel(collision); STATUS(fslite_export(v, collision), FSLITE_ALREADY_EXISTS);
    check_sentinel(collision); CHECK(unlink(collision) == 0);
    FILE *empty = fopen(collision, "wb"); CHECK(empty && fclose(empty) == 0);
    STATUS(fslite_export(v, collision), FSLITE_ALREADY_EXISTS); CHECK(unlink(collision) == 0);
    CHECK(mkdir(collision, 0700) == 0);
    STATUS(fslite_export(v, collision), FSLITE_ALREADY_EXISTS); CHECK(rmdir(collision) == 0);
    CHECK(symlink(source, collision) == 0);
    STATUS(fslite_export(v, collision), FSLITE_ALREADY_EXISTS); CHECK(unlink(collision) == 0);
    CHECK(symlink("missing", collision) == 0);
    STATUS(fslite_export(v, collision), FSLITE_ALREADY_EXISTS); CHECK(unlink(collision) == 0);
    CHECK(link(source, collision) == 0);
    STATUS(fslite_export(v, collision), FSLITE_ALREADY_EXISTS); CHECK(unlink(collision) == 0);
    const char *suffixes[] = { "-journal", "-wal", "-shm" };
    for (int i = 0; i < 3; ++i) {
        char sidecar[1032]; CHECK(snprintf(sidecar, sizeof sidecar, "%s%s", collision, suffixes[i]) < (int)sizeof sidecar);
        sentinel(sidecar); STATUS(fslite_export(v, collision), FSLITE_ALREADY_EXISTS);
        check_sentinel(sidecar); CHECK(unlink(sidecar) == 0); absent(collision);
    }
    CHECK(stages(directory) == 0);
    OK(fslite_backup_open(v, collision, &backup)); complete(backup);
    sentinel(collision); // A new destination appears after backup-open.
    STATUS(fslite_backup_finish(backup), FSLITE_ALREADY_EXISTS);
    fslite_backup_close(backup); check_sentinel(collision);
    CHECK(stages(directory) == 0 && unlink(collision) == 0);

    // Cancellation before copying, mid-copy, and after copy completion.
    for (int state = 0; state < 3; ++state) {
        OK(fslite_backup_open(v, collision, &backup));
        if (state == 1) OK(fslite_backup_step(backup, 1, &progress));
        if (state == 2) complete(backup);
        fslite_backup_close(backup); absent(collision); CHECK(stages(directory) == 0);
        content(v, "/one", "later");
    }
    OK(fslite_begin(v)); OK(fslite_write(v, "/pending", "keep", 4));
    STATUS(fslite_export(v, collision), FSLITE_INVALID_STATE);
    STATUS(fslite_backup_open(v, collision, &backup), FSLITE_INVALID_STATE); CHECK(backup == NULL);
    CHECK(fslite_in_transaction(v)); content(v, "/pending", "keep"); OK(fslite_commit(v));
    fslite_writer *writer = NULL;
    OK(fslite_writer_open(v, "/stream", &writer)); OK(fslite_writer_write(writer, "keep", 4));
    STATUS(fslite_export(v, collision), FSLITE_INVALID_STATE);
    OK(fslite_writer_finish(writer)); fslite_writer_close(writer);
    content(v, "/stream", "keep"); absent(collision); CHECK(stages(directory) == 0);
    OK(fslite_backup_open(v, collision, &backup)); OK(fslite_backup_step(backup, 1, &progress));
    fslite_close(v);
    STATUS(fslite_backup_step(backup, 1, &progress), FSLITE_INVALID_STATE);
    STATUS(fslite_backup_finish(backup), FSLITE_INVALID_STATE);
    fslite_backup_close(backup); absent(collision); CHECK(stages(directory) == 0);
    fslite_close(other);
    OK(fslite_open(source, 0, &v));
#ifdef FSLITE_TEST_SQLITE
    sqlite3 *locker = NULL;
    CHECK(sqlite3_open(source, &locker) == SQLITE_OK);
    CHECK(sqlite3_exec(locker, "BEGIN EXCLUSIVE", NULL, NULL, NULL) == SQLITE_OK);
    OK(fslite_backup_open(v, collision, &backup));
    STATUS(fslite_backup_step(backup, 1, &progress), FSLITE_BUSY);
    CHECK(progress.done == 0 && progress.total_pages == 0);
    CHECK(sqlite3_exec(locker, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(locker) == SQLITE_OK);
    complete(backup); OK(fslite_backup_finish(backup)); fslite_backup_close(backup);
    CHECK(unlink(collision) == 0);
    // Publication is complete when its directory fsync fails. Report this
    // distinctly and retain a usable destination instead of pretending absence.
    CHECK(stat(directory, &sync_directory) == 0);
    fail_directory_sync = 1;
    STATUS(fslite_export(v, collision), FSLITE_DURABILITY_UNCERTAIN);
    fail_directory_sync = 0;
    CHECK(failed_directory_syncs == 1 && stages(directory) == 0);
    OK(fslite_open(collision, 0, &copy)); content(copy, "/one", "later"); fslite_close(copy);
    CHECK(unlink(collision) == 0);
#endif
    fslite_close(v);
    // An actual OS write failure must clean staging and leave the source intact.
    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        struct rlimit limit; CHECK(getrlimit(RLIMIT_FSIZE, &limit) == 0);
        limit.rlim_cur = 4096; CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        CHECK(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
        OK(fslite_open(source, 0, &v));
        STATUS(fslite_export(v, collision), FSLITE_IO_ERROR);
        CHECK(stages(directory) == 0); content(v, "/one", "later");
        fslite_close(v); _exit(0);
    }
    int child_status;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0); absent(collision);
    // A crash may leave only private staging, never a partial public destination.
    child = fork(); CHECK(child >= 0);
    if (child == 0) {
        OK(fslite_open(source, 0, &v));
        OK(fslite_backup_open(v, collision, &backup));
        OK(fslite_backup_step(backup, 1, &progress));
        _exit(0);
    }
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    absent(collision); CHECK(stages(directory) == 1);
    OK(fslite_open(source, 0, &v)); content(v, "/one", "later"); fslite_close(v);

    // In-memory sources can be exported into standalone volumes.
    OK(fslite_open(":memory:", FSLITE_OPEN_CREATE, &v)); OK(fslite_write(v, "/memory", "saved", 5));
    OK(fslite_export(v, collision));
    OK(fslite_open(collision, 0, &copy)); content(copy, "/memory", "saved"); fslite_close(copy);
    STATUS(fslite_export(v, ":memory:"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_export(v, ""), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_export(v, NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_export(NULL, collision), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_backup_open(v, collision, NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_backup_open(NULL, collision, &backup), FSLITE_INVALID_ARGUMENT); CHECK(backup == NULL);
    STATUS(fslite_backup_step(NULL, 1, &progress), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_backup_step(NULL, 1, NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_backup_finish(NULL), FSLITE_INVALID_ARGUMENT);
    fslite_backup_close(NULL); fslite_close(v);
    cleanup(directory);
}
