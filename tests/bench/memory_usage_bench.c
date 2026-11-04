#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: %s (%d), expected %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)

#define CHUNK_SIZE 65536
#define ITERATIONS 100

static void get_memory_usage() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    long max_rss = ru.ru_maxrss;  // in KB on Linux, bytes on macOS
#ifdef __APPLE__
    printf("%ld MB", max_rss / (1024 * 1024));
#else
    printf("%ld MB", max_rss / 1024);
#endif
}

static void bench_memory_for_many_files() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-memory-many-files-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char data[256];
    for (int i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 3);

    // Measure initial memory
    struct rusage ru_before;
    getrusage(RUSAGE_SELF, &ru_before);
    long mem_before = ru_before.ru_maxrss;

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        char path[64];
        snprintf(path, sizeof(path), "/file%d", iter);

        OK(fslite_write(volume, path, data, sizeof(data)));

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, path, 0, read_buf, sizeof(read_buf), &count));
        CHECK(count == sizeof(data));
        CHECK(memcmp(data, read_buf, sizeof(data)) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    struct rusage ru_after;
    getrusage(RUSAGE_SELF, &ru_after);
    long mem_after = ru_after.ru_maxrss;
    long mem_increase = mem_after - mem_before;

    printf("Memory for many files benchmark (iter=%d, %d bytes each): %.3f seconds, "
           "%.0f ops/sec, memory increase: %ld KB\n",
           ITERATIONS, sizeof(data), elapsed, (double)ITERATIONS / elapsed, mem_increase);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_memory_for_large_files() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-memory-large-files-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char *data = malloc(CHUNK_SIZE);
    CHECK(data != NULL);
    for (int i = 0; i < CHUNK_SIZE; ++i) data[i] = (unsigned char)(i * 2);

    // Measure initial memory
    struct rusage ru_before;
    getrusage(RUSAGE_SELF, &ru_before);
    long mem_before = ru_before.ru_maxrss;

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/large_file.dat", data, CHUNK_SIZE));

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, "/large_file.dat", 0, read_buf, sizeof(read_buf), &count));
        CHECK(count == CHUNK_SIZE);
        CHECK(memcmp(data, read_buf, CHUNK_SIZE) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    struct rusage ru_after;
    getrusage(RUSAGE_SELF, &ru_after);
    long mem_after = ru_after.ru_maxrss;
    long mem_increase = mem_after - mem_before;

    printf("Memory for large files benchmark (iter=%d, %d bytes each): %.3f seconds, "
           "%.1f MB/sec, memory increase: %ld KB\n",
           ITERATIONS, CHUNK_SIZE, elapsed, (double)ITERATIONS * CHUNK_SIZE / (elapsed * 1024 * 1024), mem_increase);

    fslite_close(volume);
    free(data);
    CHECK(unlink(filename) == 0);
}

static void bench_memory_for_many_operations() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-memory-many-ops-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char data[128];
    for (int i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 5);

    // Measure initial memory
    struct rusage ru_before;
    getrusage(RUSAGE_SELF, &ru_before);
    long mem_before = ru_before.ru_maxrss;

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        if (iter % 3 == 0) {
            OK(fslite_write(volume, "/file1", data, sizeof(data)));
            unsigned char read_buf[128];
            size_t count = 0;
            OK(fslite_read(volume, "/file1", 0, read_buf, sizeof(read_buf), &count));
            CHECK(count == sizeof(data));
        } else if (iter % 3 == 1) {
            OK(fslite_write(volume, "/file2", data, sizeof(data)));
            unsigned char read_buf[128];
            size_t count = 0;
            OK(fslite_read(volume, "/file2", 0, read_buf, sizeof(read_buf), &count));
            CHECK(count == sizeof(data));
        } else {
            unsigned char read_buf[128];
            size_t count = 0;
            OK(fslite_read(volume, "/file1", 0, read_buf, sizeof(read_buf), &count));
            CHECK(count == sizeof(data));
        }
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    struct rusage ru_after;
    getrusage(RUSAGE_SELF, &ru_after);
    long mem_after = ru_after.ru_maxrss;
    long mem_increase = mem_after - mem_before;

    printf("Memory for many operations benchmark (iter=%d): %.3f seconds, "
           "%.0f ops/sec, memory increase: %ld KB\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed, mem_increase);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

int main(void) {
    printf("=== FSLite Memory Usage Benchmarks ===\n");
    bench_memory_for_many_files();
    bench_memory_for_large_files();
    bench_memory_for_many_operations();
    printf("=== All memory benchmarks completed ===\n");
    return 0;
}