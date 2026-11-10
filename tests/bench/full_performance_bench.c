#define _POSIX_C_SOURCE 200809L
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define ITERATIONS 30
/* Very short operations need more samples for a stable rate; clock() has
 * microsecond granularity and a 30-call loop finishes too close to it. */
#define LOOKUP_ITERATIONS 5000
#define READ_ITERATIONS 2000

static uint32_t random_number(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void bench_path_lookups() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-path-lookup-bench-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    OK(fslite_mkdir(volume, "/dir1"));
    OK(fslite_mkdir(volume, "/dir1/subdir1"));
    OK(fslite_mkdir(volume, "/dir1/subdir2"));

    clock_t start = clock();
    for (int iter = 0; iter < LOOKUP_ITERATIONS; ++iter) {
        fslite_info info;
        STATUS(fslite_stat(volume, "/dir1/subdir1", &info), FSLITE_OK);
        CHECK(info.kind == 2);
        CHECK(info.size == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Path lookup benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           LOOKUP_ITERATIONS, elapsed, (double)LOOKUP_ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_small_file_operations() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-small-file-ops-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char data[256];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)i;

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/file", data, sizeof(data)));

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, "/file", 0, read_buf, sizeof(data), &count));
        CHECK(count == sizeof(data));
        CHECK(memcmp(data, read_buf, sizeof(data)) == 0);

        OK(fslite_truncate(volume, "/file", 0));
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Small file operations benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_streaming_writes() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-streaming-bench-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    size_t stream_size = 5 * 1024 * 1024; // 5MB
    unsigned char *buffer = malloc(stream_size);
    CHECK(buffer != NULL);
    for (size_t i = 0; i < stream_size; ++i) buffer[i] = (unsigned char)(i & 0xFF);

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/stream.dat", buffer, stream_size));

        unsigned char *read_buf = malloc(stream_size);
        CHECK(read_buf != NULL);
        size_t count = 0;
        OK(fslite_read(volume, "/stream.dat", 0, read_buf, stream_size, &count));
        CHECK(count == stream_size);
        CHECK(memcmp(buffer, read_buf, stream_size) == 0);
        free(read_buf);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double total_bytes = (double)ITERATIONS * stream_size;

    printf("Streaming writes benchmark (iter=%d, size=%zuMB): %.3f seconds, %.1f MB/sec\n",
           ITERATIONS, stream_size / (1024 * 1024), elapsed, total_bytes / (elapsed * 1024 * 1024));

    fslite_close(volume);
    free(buffer);
    CHECK(unlink(filename) == 0);
}

static void bench_random_access_simple() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-random-simple-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    size_t file_size = 64 * 1024; // 64KB
    unsigned char *data = malloc(file_size);
    CHECK(data != NULL);
    for (size_t i = 0; i < file_size; ++i) data[i] = (unsigned char)(i * 3);

    OK(fslite_write(volume, "/file.dat", data, file_size));

    uint32_t state = 0x51f17e;
    clock_t start = clock();
    for (int iter = 0; iter < READ_ITERATIONS; ++iter) {
        uint32_t offset = (random_number(&state) % file_size);
        uint32_t length = (random_number(&state) % 256) + 1;
        if ((uint64_t)offset + length > file_size) length = (uint32_t)(file_size - offset);

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, "/file.dat", offset, read_buf, length, &count));
        CHECK(count == (size_t)length);
        CHECK(memcmp(data + offset, read_buf, length) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Random access reads benchmark (iter=%d, file_size=%zuKB): %.3f seconds, %.0f ops/sec\n",
           READ_ITERATIONS, file_size / 1024, elapsed, (double)READ_ITERATIONS / elapsed);

    fslite_close(volume);
    free(data);
    CHECK(unlink(filename) == 0);
}

int main(void) {
    printf("=== FSLite Full Performance Benchmark Suite ===\n");
    printf("Running iterations: file ops=%d, lookups=%d, reads=%d\n\n",
           ITERATIONS, LOOKUP_ITERATIONS, READ_ITERATIONS);

    bench_path_lookups();
    bench_small_file_operations();
    bench_streaming_writes();
    bench_random_access_simple();

    printf("=== All benchmarks completed ===\n");
    return 0;
}