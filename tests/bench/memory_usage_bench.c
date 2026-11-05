#define _POSIX_C_SOURCE 200809L
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define ITERATIONS 30
#define STREAM_SIZE (5 * 1024 * 1024)  // 5MB

static void bench_memory_many_files() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-memory-many-files-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char data[256];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 3);

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

    printf("Memory for many files (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_memory_large_files() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-memory-large-files-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char *data = malloc(STREAM_SIZE);
    CHECK(data != NULL);
    for (size_t i = 0; i < STREAM_SIZE; ++i) data[i] = (unsigned char)(i * 2);

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/large_file.dat", data, STREAM_SIZE));

        unsigned char *read_buf = malloc(STREAM_SIZE);
        CHECK(read_buf != NULL);
        size_t count = 0;
        OK(fslite_read(volume, "/large_file.dat", 0, read_buf, STREAM_SIZE, &count));
        CHECK(count == STREAM_SIZE);
        CHECK(memcmp(data, read_buf, STREAM_SIZE) == 0);
        free(read_buf);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Memory for large files (iter=%d): %.3f seconds, %.1f MB/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS * STREAM_SIZE / (elapsed * 1024 * 1024));

    fslite_close(volume);
    free(data);
    CHECK(unlink(filename) == 0);
}

static void bench_memory_many_operations() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-memory-many-ops-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char data[128];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 5);

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

    printf("Memory for many operations (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

int main(void) {
    printf("=== FSLite Memory Usage Benchmarks ===\n");
    printf("Running %d iterations per benchmark\n\n", ITERATIONS);

    bench_memory_many_files();
    bench_memory_large_files();
    bench_memory_many_operations();

    printf("=== All memory benchmarks completed ===\n");
    return 0;
}