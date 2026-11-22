/* Windows-specific host-file publication coverage. The POSIX integration
 * tests use fork, signals, and directory fsync interposition, so this file
 * exercises the portable API surface plus Windows publication semantics. */
#ifndef _WIN32
#error "windows_smoke.c is only for Windows targets"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: %s (%d), expected %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)

static wchar_t directory[MAX_PATH];

static void path(char *out, size_t capacity, const char *name) {
    char utf8[MAX_PATH * 4];
    int length = WideCharToMultiByte(CP_UTF8, 0, directory, -1, utf8, sizeof utf8, NULL, NULL);
    CHECK(length > 0 && (size_t)length <= sizeof utf8);
    CHECK(snprintf(out, capacity, "%s\\%s", utf8, name) < (int)capacity);
}

static int exists(const char *name) {
    char text[4096];
    wchar_t wide[4096];
    path(text, sizeof text, name);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 4096) > 0);
    if (GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES) return 1;
    DWORD error = GetLastError();
    CHECK(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
    return 0;
}

static void delete_file(const char *name) {
    char text[4096];
    wchar_t wide[4096];
    path(text, sizeof text, name);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 4096) > 0);
    CHECK(DeleteFileW(wide) != 0);
}

static void sentinel(const char *name) {
    FILE *file = fopen(name, "wb");
    CHECK(file != NULL);
    CHECK(fwrite("keep", 1, 4, file) == 4);
    CHECK(fclose(file) == 0);
}

static void content(fslite_volume *volume, const char *name, const char *expected) {
    char bytes[128];
    size_t count = 0;
    OK(fslite_read(volume, name, 0, bytes, sizeof bytes, &count));
    CHECK(count == strlen(expected) && memcmp(bytes, expected, count) == 0);
}

static void no_staging(void) {
    wchar_t pattern[MAX_PATH];
    CHECK(swprintf(pattern, MAX_PATH, L"%s\\.fslite-backup-*", directory) > 0);
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);
        return;
    }
    FindClose(find);
    CHECK(0 && "staging directory was not cleaned up");
}

int main(void) {
    wchar_t temp[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, temp);
    CHECK(length > 0 && length < MAX_PATH);
    CHECK(swprintf(directory, MAX_PATH, L"%sfslite-windows-%lu",
                   temp, (unsigned long)GetCurrentProcessId()) > 0);
    RemoveDirectoryW(directory);
    CHECK(CreateDirectoryW(directory, NULL) != 0);

    char source[4096], destination[4096], snapshot[4096], sidecar_base[4096], unicode[4096];
    path(source, sizeof source, "source.fslite");
    path(destination, sizeof destination, "export.fslite");
    path(snapshot, sizeof snapshot, "snapshot.fslite");
    path(sidecar_base, sizeof sidecar_base, "sidecar");
    path(unicode, sizeof unicode, "snapshot-\xc3\xa9.fslite");

    fslite_volume *volume = NULL, *copy = NULL;
    fslite_backup *backup = NULL;
    fslite_backup_progress progress;

    OK(fslite_open(source, FSLITE_OPEN_CREATE, &volume));
    OK(fslite_write(volume, "/hello", "windows", 7));

    // One-call export publishes a complete, readable destination.
    OK(fslite_export(volume, destination));
    CHECK(exists("export.fslite"));
    OK(fslite_open(destination, 0, &copy));
    content(copy, "/hello", "windows");
    fslite_close(copy);
    copy = NULL;
    STATUS(fslite_export(volume, destination), FSLITE_ALREADY_EXISTS);
    no_staging();

    // A SQLite sidecar name is reserved as well.
    char sidecar[4096];
    CHECK(snprintf(sidecar, sizeof sidecar, "%s-journal", sidecar_base) < (int)sizeof sidecar);
    sentinel(sidecar);
    STATUS(fslite_export(volume, sidecar_base), FSLITE_ALREADY_EXISTS);
    CHECK(remove(sidecar) == 0);
    CHECK(!exists("sidecar"));

    // Stepwise backup publishes with a non-overwriting rename.
    OK(fslite_backup_open(volume, snapshot, &backup));
    CHECK(!exists("snapshot.fslite"));
    int steps = 0;
    do {
        OK(fslite_backup_step(backup, 1, &progress));
        CHECK(++steps < 100000);
    } while (!progress.done);
    OK(fslite_backup_finish(backup));
    fslite_backup_close(backup);
    backup = NULL;
    CHECK(exists("snapshot.fslite"));
    OK(fslite_open(snapshot, 0, &copy));
    content(copy, "/hello", "windows");
    fslite_close(copy);
    copy = NULL;
    delete_file("snapshot.fslite");
    no_staging();

    // Cancellation removes private staging and never creates a destination.
    OK(fslite_backup_open(volume, snapshot, &backup));
    OK(fslite_backup_step(backup, 1, &progress));
    fslite_backup_close(backup);
    backup = NULL;
    CHECK(!exists("snapshot.fslite"));
    no_staging();

    // Non-ASCII UTF-8 destination names round-trip through the wide API.
    OK(fslite_export(volume, unicode));
    CHECK(exists("snapshot-\xc3\xa9.fslite"));
    OK(fslite_open(unicode, 0, &copy));
    content(copy, "/hello", "windows");
    fslite_close(copy);
    copy = NULL;
    no_staging();

    // In-memory volumes can be exported to a standalone file.
    fslite_close(volume);
    volume = NULL;
    OK(fslite_open(":memory:", FSLITE_OPEN_CREATE, &volume));
    OK(fslite_write(volume, "/memory", "saved", 5));
    OK(fslite_export(volume, sidecar_base));
    OK(fslite_open(sidecar_base, 0, &copy));
    content(copy, "/memory", "saved");
    fslite_close(copy);
    copy = NULL;

    STATUS(fslite_export(volume, ":memory:"), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_export(volume, ""), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_export(volume, NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_export(NULL, sidecar_base), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_backup_open(volume, sidecar_base, NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_backup_open(NULL, sidecar_base, &backup), FSLITE_INVALID_ARGUMENT);
    CHECK(backup == NULL);
    STATUS(fslite_backup_step(NULL, 1, &progress), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_backup_step(NULL, 1, NULL), FSLITE_INVALID_ARGUMENT);
    STATUS(fslite_backup_finish(NULL), FSLITE_INVALID_ARGUMENT);
    fslite_backup_close(NULL);
    fslite_close(volume);
    fslite_close(NULL);

    if (exists("export.fslite")) delete_file("export.fslite");
    if (exists("sidecar")) delete_file("sidecar");
    if (exists("snapshot-\xc3\xa9.fslite")) delete_file("snapshot-\xc3\xa9.fslite");
    if (exists("source.fslite")) delete_file("source.fslite");
    CHECK(RemoveDirectoryW(directory) != 0);
    puts("Windows publication smoke test passed");
    return 0;
}
