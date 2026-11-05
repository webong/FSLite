#ifndef FSLITE_BENCH_H
#define FSLITE_BENCH_H
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "fslite.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define OK(call) do { fslite_status result_ = (call); if (result_ != FSLITE_OK) { \
    fprintf(stderr, "%s:%d: %s: %s (%d)\n", __FILE__, __LINE__, #call, \
        fslite_status_string(result_), result_); exit(1); } } while (0)
#ifndef FSLITE_BENCH_LINKAGE
#define FSLITE_BENCH_LINKAGE "unspecified"
#endif
#ifndef FSLITE_BENCH_OPTIMIZE
#define FSLITE_BENCH_OPTIMIZE "unspecified"
#endif
#define MIB UINT64_C(1048576)
#define BUFFER_SIZE 65536
static char bench_directory[1024], bench_filename[1100];
static fslite_volume *bench_volume;

static inline double now_seconds(void) {
    struct timespec ts; CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static inline double peak_rss_mib(void) {
    struct rusage usage; CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
#ifdef __APPLE__
    return (double)usage.ru_maxrss / (double)MIB;
#else
    return (double)usage.ru_maxrss * 1024.0 / (double)MIB;
#endif
}
static inline void cleanup(void) {
    if (bench_volume) { fslite_close(bench_volume); bench_volume = NULL; }
    if (bench_filename[0]) {
        const char *suffixes[] = {"", "-journal", "-wal", "-shm"};
        for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; ++i) {
            char path[1200]; snprintf(path, sizeof path, "%s%s", bench_filename, suffixes[i]);
            if (unlink(path) != 0 && errno != ENOENT) perror(path);
        }
        bench_filename[0] = 0;
    }
    if (bench_directory[0]) {
        if (rmdir(bench_directory) != 0) perror(bench_directory);
        bench_directory[0] = 0;
    }
}
static inline int options(int argc, char **argv) {
    if (argc == 1) return 0;
    if (argc == 2 && strcmp(argv[1], "--smoke") == 0) return 1;
    fprintf(stderr, "Usage: %s [--smoke]\n", argv[0]); exit(2);
}
static inline void setup(int smoke) {
    CHECK(atexit(cleanup) == 0);
    const char *temp = getenv("TMPDIR"); if (!temp || !*temp) temp = "/tmp";
    int count = snprintf(bench_directory, sizeof bench_directory, "%s/fslite-bench-XXXXXX", temp);
    CHECK(count > 0 && (size_t)count < sizeof bench_directory);
    CHECK(mkdtemp(bench_directory) != NULL);
    count = snprintf(bench_filename, sizeof bench_filename, "%s/volume.fslite", bench_directory);
    CHECK(count > 0 && (size_t)count < sizeof bench_filename);
    OK(fslite_open(bench_filename, FSLITE_OPEN_CREATE, &bench_volume));
    struct utsname host; CHECK(uname(&host) == 0);
    printf("# os=%s arch=%s linkage=%s optimize=%s abi=%u mode=%s storage=%s journal=DELETE synchronous=FULL\n",
        host.sysname, host.machine, FSLITE_BENCH_LINKAGE, FSLITE_BENCH_OPTIMIZE,
        fslite_abi_version(), smoke ? "smoke" : "full", temp);
}
static inline void report(const char *name, uint64_t operations, uint64_t bytes, double elapsed) {
    CHECK(elapsed > 0);
    printf("case=%s operations=%" PRIu64 " bytes=%" PRIu64 " seconds=%.6f ops_per_second=%.2f mib_per_second=%.2f\n",
        name, operations, bytes, elapsed, (double)operations / elapsed, (double)bytes / (double)MIB / elapsed);
}
static inline void reopen(void) {
    fslite_close(bench_volume); bench_volume = NULL;
    OK(fslite_open(bench_filename, 0, &bench_volume));
}
static inline void verify_bytes(const char *path, uint64_t offset, const unsigned char *expected, size_t size) {
    unsigned char actual[BUFFER_SIZE];
    for (size_t copied = 0; copied < size;) {
        size_t amount = size - copied; if (amount > sizeof actual) amount = sizeof actual;
        size_t count = 0; OK(fslite_read(bench_volume, path, offset + copied, actual, amount, &count));
        CHECK(count == amount && memcmp(actual, expected + copied, amount) == 0); copied += count;
    }
}
static inline void verify_size(const char *path, uint64_t size) {
    fslite_info info; OK(fslite_stat(bench_volume, path, &info));
    CHECK(info.kind == FSLITE_FILE && info.size == size);
    unsigned char byte; size_t count;
    OK(fslite_read(bench_volume, path, size, &byte, 1, &count)); CHECK(count == 0);
}
static inline void pattern(unsigned char *buffer, size_t size, unsigned salt) {
    for (size_t i = 0; i < size; ++i) buffer[i] = (unsigned char)(i * 31u + salt);
}
static inline void stream_file(const char *path, uint64_t bytes, const unsigned char *buffer, size_t size) {
    fslite_writer *writer = NULL; OK(fslite_writer_open(bench_volume, path, &writer));
    for (uint64_t offset = 0; offset < bytes;) {
        size_t amount = bytes - offset < size ? (size_t)(bytes - offset) : size;
        OK(fslite_writer_write(writer, buffer, amount)); offset += amount;
    }
    OK(fslite_writer_finish(writer)); fslite_writer_close(writer);
}
static inline void verify_stream(const char *path, uint64_t bytes, const unsigned char *buffer, size_t size) {
    verify_size(path, bytes);
    for (uint64_t offset = 0; offset < bytes;) {
        size_t amount = bytes - offset < size ? (size_t)(bytes - offset) : size;
        verify_bytes(path, offset, buffer, amount); offset += amount;
    }
    OK(fslite_check(bench_volume));
}
#endif
