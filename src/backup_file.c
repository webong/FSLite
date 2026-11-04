#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include "backup_file.h"
#include "fslite.h"

#if defined(_WIN32)
/* The storage engine remains portable; atomic host-file publication needs a
 * Windows implementation before live export is enabled there. */
int32_t fslite_backup_file_create(const char *path, fslite_backup_file **out) {
    (void)path; *out = 0; return FSLITE_UNSUPPORTED;
}
const char *fslite_backup_file_path(fslite_backup_file *file) { (void)file; return 0; }
int32_t fslite_backup_file_publish(fslite_backup_file *file) { (void)file; return FSLITE_UNSUPPORTED; }
void fslite_backup_file_close(fslite_backup_file *file) { (void)file; }
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
