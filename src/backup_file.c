#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include "backup_file.h"
#include "fslite.h"

#if defined(_WIN32)
/* Windows host-file publication.
 *
 * The staging directory is created next to the destination, and the completed
 * database is published with MoveFileExW(MOVEFILE_WRITE_THROUGH). That call
 * fails when the destination exists, so publication never replaces an existing
 * file, and it returns only after the move is flushed to disk. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <wchar.h>
#include <windows.h>

struct fslite_backup_file {
    HANDLE file;
    char *directory_path;
    char *database_path;
    wchar_t *destination;
    wchar_t *directory;
    wchar_t *database;
    int directory_created;
};

static int32_t win_error(DWORD code) {
    return code == ERROR_NOT_ENOUGH_MEMORY || code == ERROR_OUTOFMEMORY
               ? FSLITE_OUT_OF_MEMORY
               : FSLITE_IO_ERROR;
}

static wchar_t *utf8_to_wide(const char *text, int32_t *status) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) {
        *status = FSLITE_INVALID_ARGUMENT;
        return NULL;
    }
    wchar_t *wide = malloc((size_t)length * sizeof *wide);
    if (!wide) {
        *status = FSLITE_OUT_OF_MEMORY;
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        free(wide);
        *status = FSLITE_INVALID_ARGUMENT;
        return NULL;
    }
    *status = FSLITE_OK;
    return wide;
}

static char *wide_to_utf8(const wchar_t *text, int32_t *status) {
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        *status = FSLITE_INVALID_ARGUMENT;
        return NULL;
    }
    char *narrow = malloc((size_t)length);
    if (!narrow) {
        *status = FSLITE_OUT_OF_MEMORY;
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, length, NULL, NULL) <= 0) {
        free(narrow);
        *status = FSLITE_INVALID_ARGUMENT;
        return NULL;
    }
    *status = FSLITE_OK;
    return narrow;
}

static wchar_t *absolute_path(const wchar_t *path, int32_t *status) {
    DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed == 0) {
        *status = win_error(GetLastError());
        return NULL;
    }
    wchar_t *result = malloc((size_t)needed * sizeof *result);
    if (!result) {
        *status = FSLITE_OUT_OF_MEMORY;
        return NULL;
    }
    DWORD written = GetFullPathNameW(path, needed, result, NULL);
    if (written == 0 || written >= needed) {
        free(result);
        *status = FSLITE_IO_ERROR;
        return NULL;
    }
    *status = FSLITE_OK;
    return result;
}

/* Returns a newly allocated parent directory for an absolute path and points
 * base at the last component inside that path. */
static wchar_t *split_parent(const wchar_t *absolute, const wchar_t **base) {
    const wchar_t *back = wcsrchr(absolute, L'\\');
    const wchar_t *forward = wcsrchr(absolute, L'/');
    const wchar_t *slash = back;
    if (forward && (!slash || forward > slash)) slash = forward;
    if (!slash) return NULL;
    *base = slash + 1;
    if (!**base) return NULL;
    size_t length = (size_t)(slash - absolute);
    if (length == 0) {
        static const wchar_t root[] = L"\\";
        wchar_t *parent = malloc(sizeof root);
        if (parent) memcpy(parent, root, sizeof root);
        return parent;
    }
    size_t extra = length == 2 && absolute[1] == L':' ? 1 : 0;
    wchar_t *parent = malloc((length + extra + 1) * sizeof *parent);
    if (!parent) return NULL;
    memcpy(parent, absolute, length * sizeof *parent);
    if (extra) parent[length++] = L'\\';
    parent[length] = 0;
    return parent;
}

static wchar_t *join_path(const wchar_t *directory, const wchar_t *name) {
    size_t directory_length = wcslen(directory);
    size_t name_length = wcslen(name);
    int separator = directory_length > 0 && directory[directory_length - 1] != L'\\' &&
                    directory[directory_length - 1] != L'/';
    wchar_t *result = malloc((directory_length + (size_t)separator + name_length + 1) * sizeof *result);
    if (!result) return NULL;
    wmemcpy(result, directory, directory_length);
    if (separator) result[directory_length++] = L'\\';
    wmemcpy(result + directory_length, name, name_length + 1);
    return result;
}

static wchar_t *append_path(const wchar_t *path, const wchar_t *suffix) {
    size_t path_length = wcslen(path);
    size_t suffix_length = wcslen(suffix);
    wchar_t *result = malloc((path_length + suffix_length + 1) * sizeof *result);
    if (!result) return NULL;
    wmemcpy(result, path, path_length);
    wmemcpy(result + path_length, suffix, suffix_length + 1);
    return result;
}

static int32_t unused_destination(fslite_backup_file *file) {
    static const wchar_t *suffixes[] = { L"", L"-journal", L"-wal", L"-shm" };
    for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; ++i) {
        wchar_t *candidate = append_path(file->destination, suffixes[i]);
        if (!candidate) return FSLITE_OUT_OF_MEMORY;
        DWORD attributes = GetFileAttributesW(candidate);
        free(candidate);
        if (attributes != INVALID_FILE_ATTRIBUTES) return FSLITE_ALREADY_EXISTS;
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return FSLITE_IO_ERROR;
    }
    return FSLITE_OK;
}

static volatile LONG staging_counter;

int32_t fslite_backup_file_create(const char *destination, fslite_backup_file **output) {
    *output = NULL;
    if (!destination || !*destination) return FSLITE_INVALID_ARGUMENT;
    const char *original_base = destination;
    for (const char *cursor = destination; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') original_base = cursor + 1;
    if (!*original_base || !strcmp(original_base, ".") || !strcmp(original_base, ".."))
        return FSLITE_INVALID_ARGUMENT;

    int32_t status = FSLITE_OK;
    wchar_t *wide_destination = utf8_to_wide(destination, &status);
    if (!wide_destination) return status;
    wchar_t *absolute = absolute_path(wide_destination, &status);
    free(wide_destination);
    if (!absolute) return status;

    const wchar_t *base = NULL;
    wchar_t *parent = split_parent(absolute, &base);
    if (!parent) {
        free(absolute);
        return status == FSLITE_OK ? FSLITE_INVALID_ARGUMENT : status;
    }

    fslite_backup_file *file = calloc(1, sizeof *file);
    if (!file) {
        free(parent);
        free(absolute);
        return FSLITE_OUT_OF_MEMORY;
    }
    file->file = INVALID_HANDLE_VALUE;
    file->destination = absolute;

    status = unused_destination(file);
    if (status != FSLITE_OK) goto fail;

    for (int attempt = 0; attempt < 64; ++attempt) {
        unsigned long long seed = GetTickCount64() ^
                                  ((unsigned long long)GetCurrentProcessId() << 32) ^
                                  ((unsigned long long)InterlockedIncrement(&staging_counter) * 0x9e3779b97f4a7c15ULL);
        wchar_t name[32];
        if (swprintf(name, sizeof name / sizeof *name, L".fslite-backup-%016llx", seed) < 0) {
            status = FSLITE_INTERNAL_ERROR;
            goto fail;
        }
        file->directory = join_path(parent, name);
        if (!file->directory) {
            status = FSLITE_OUT_OF_MEMORY;
            goto fail;
        }
        if (CreateDirectoryW(file->directory, NULL)) {
            file->directory_created = 1;
            break;
        }
        DWORD error = GetLastError();
        free(file->directory);
        file->directory = NULL;
        if (error != ERROR_ALREADY_EXISTS) {
            status = win_error(error);
            goto fail;
        }
    }
    if (!file->directory_created) {
        status = FSLITE_IO_ERROR;
        goto fail;
    }

    file->database = join_path(file->directory, L"volume.sqlite");
    if (!file->database) {
        status = FSLITE_OUT_OF_MEMORY;
        goto fail;
    }
    file->directory_path = wide_to_utf8(file->directory, &status);
    if (!file->directory_path) goto fail;
    file->database_path = wide_to_utf8(file->database, &status);
    if (!file->database_path) goto fail;

    file->file = CreateFileW(file->database, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file->file == INVALID_HANDLE_VALUE) {
        status = win_error(GetLastError());
        goto fail;
    }

    free(parent);
    *output = file;
    return FSLITE_OK;
fail:
    free(parent);
    fslite_backup_file_close(file);
    return status;
}

const char *fslite_backup_file_path(fslite_backup_file *file) { return file->database_path; }

int32_t fslite_backup_file_publish(fslite_backup_file *file) {
    int32_t result = unused_destination(file);
    if (result != FSLITE_OK) return result;
    if (file->file == INVALID_HANDLE_VALUE) return FSLITE_IO_ERROR;

    BY_HANDLE_FILE_INFORMATION opened;
    HANDLE named = CreateFileW(file->database, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (named == INVALID_HANDLE_VALUE) return FSLITE_IO_ERROR;
    BY_HANDLE_FILE_INFORMATION info;
    BOOL named_ok = GetFileInformationByHandle(named, &info);
    CloseHandle(named);
    if (!GetFileInformationByHandle(file->file, &opened) || !named_ok) return FSLITE_IO_ERROR;
    if ((opened.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        opened.dwVolumeSerialNumber != info.dwVolumeSerialNumber ||
        opened.nFileIndexHigh != info.nFileIndexHigh ||
        opened.nFileIndexLow != info.nFileIndexLow)
        return FSLITE_IO_ERROR;

    if (!FlushFileBuffers(file->file)) return FSLITE_IO_ERROR;
    CloseHandle(file->file);
    file->file = INVALID_HANDLE_VALUE;

    /* Without MOVEFILE_REPLACE_EXISTING this publishes only into an unused
     * name, including one created after the check above. */
    if (!MoveFileExW(file->database, file->destination, MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
                   ? FSLITE_ALREADY_EXISTS
                   : FSLITE_IO_ERROR;
    }
    return FSLITE_OK;
}

void fslite_backup_file_close(fslite_backup_file *file) {
    if (!file) return;
    if (file->file != INVALID_HANDLE_VALUE) CloseHandle(file->file);
    if (file->directory) {
        static const wchar_t *names[] = {
            L"volume.sqlite-journal", L"volume.sqlite-wal", L"volume.sqlite-shm", L"volume.sqlite",
        };
        for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
            wchar_t *path = join_path(file->directory, names[i]);
            if (path) {
                DeleteFileW(path);
                free(path);
            }
        }
        if (file->directory_created) RemoveDirectoryW(file->directory);
    }
    free(file->destination);
    free(file->directory);
    free(file->database);
    free(file->directory_path);
    free(file->database_path);
    free(file);
}
#else
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fslite_backup_file {
    int parent_fd, directory_fd, file_fd;
    char *target, *directory_path, *database_path;
    int directory_created;
};

static int32_t unused_destination(fslite_backup_file *file) {
    const char *suffixes[] = { "", "-journal", "-wal", "-shm" };
    char *name = malloc(strlen(file->target) + 9);
    if (!name) return FSLITE_OUT_OF_MEMORY;
    int32_t result = FSLITE_OK;
    for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; ++i) {
        strcpy(name, file->target); strcat(name, suffixes[i]);
        struct stat info;
        if (fstatat(file->parent_fd, name, &info, AT_SYMLINK_NOFOLLOW) == 0) {
            result = FSLITE_ALREADY_EXISTS; break;
        }
        if (errno != ENOENT) { result = FSLITE_IO_ERROR; break; }
    }
    free(name);
    return result;
}

int32_t fslite_backup_file_create(const char *destination, fslite_backup_file **output) {
    *output = NULL;
    const char *slash = strrchr(destination, '/');
    const char *base = slash ? slash + 1 : destination;
    if (!*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) return FSLITE_INVALID_ARGUMENT;
    char *directory = slash ? strndup(destination, slash == destination ? 1 : (size_t)(slash - destination)) : strdup(".");
    if (!directory) return FSLITE_OUT_OF_MEMORY;
    char *absolute = realpath(directory, NULL);
    free(directory);
    if (!absolute) return errno == ENOMEM ? FSLITE_OUT_OF_MEMORY : FSLITE_IO_ERROR;
    fslite_backup_file *file = calloc(1, sizeof *file);
    if (!file) { free(absolute); return FSLITE_OUT_OF_MEMORY; }
    file->parent_fd = file->directory_fd = file->file_fd = -1;
    int32_t result = FSLITE_OUT_OF_MEMORY;
    file->target = strdup(base);
    file->directory_path = malloc(strlen(absolute) + sizeof "/.fslite-backup-XXXXXX");
    if (!file->target || !file->directory_path) goto fail;
    sprintf(file->directory_path, "%s/.fslite-backup-XXXXXX", absolute);
    file->parent_fd = open(absolute, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (file->parent_fd < 0) { result = FSLITE_IO_ERROR; goto fail; }
    result = unused_destination(file);
    if (result != FSLITE_OK) goto fail;
    if (!mkdtemp(file->directory_path)) { result = FSLITE_IO_ERROR; goto fail; }
    file->directory_created = 1;
    file->directory_fd = open(file->directory_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (file->directory_fd < 0) { result = FSLITE_IO_ERROR; goto fail; }
    file->database_path = malloc(strlen(file->directory_path) + sizeof "/volume.sqlite");
    if (!file->database_path) { result = FSLITE_OUT_OF_MEMORY; goto fail; }
    sprintf(file->database_path, "%s/volume.sqlite", file->directory_path);
    file->file_fd = openat(file->directory_fd, "volume.sqlite", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file->file_fd < 0) { result = FSLITE_IO_ERROR; goto fail; }
    free(absolute);
    *output = file;
    return FSLITE_OK;
fail:
    free(absolute);
    fslite_backup_file_close(file);
    return result;
}

const char *fslite_backup_file_path(fslite_backup_file *file) { return file->database_path; }

int32_t fslite_backup_file_publish(fslite_backup_file *file) {
    int32_t result = unused_destination(file);
    if (result != FSLITE_OK) return result;
    struct stat opened, named;
    if (fstat(file->file_fd, &opened) != 0 ||
        fstatat(file->directory_fd, "volume.sqlite", &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named.st_mode) || opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
        return FSLITE_IO_ERROR;
    if (fsync(file->file_fd) != 0) return FSLITE_IO_ERROR;
    // A same-filesystem hard link publishes without replacing an existing entry,
    // including one that appeared after unused_destination checked the path.
    if (linkat(file->directory_fd, "volume.sqlite", file->parent_fd, file->target, 0) != 0)
        return errno == EEXIST ? FSLITE_ALREADY_EXISTS : FSLITE_IO_ERROR;
    // Publication has happened. Never delete the completed destination on a
    // directory-sync error: report that durability could not be confirmed.
    if (fsync(file->parent_fd) != 0) return FSLITE_DURABILITY_UNCERTAIN;
    return FSLITE_OK;
}

void fslite_backup_file_close(fslite_backup_file *file) {
    if (!file) return;
    if (file->file_fd >= 0) close(file->file_fd);
    if (file->directory_fd >= 0) {
        unlinkat(file->directory_fd, "volume.sqlite-journal", 0);
        unlinkat(file->directory_fd, "volume.sqlite-wal", 0);
        unlinkat(file->directory_fd, "volume.sqlite-shm", 0);
        unlinkat(file->directory_fd, "volume.sqlite", 0);
        close(file->directory_fd);
    }
    if (file->directory_created) rmdir(file->directory_path);
    if (file->parent_fd >= 0) close(file->parent_fd);
    free(file->target); free(file->directory_path); free(file->database_path); free(file);
}
#endif
