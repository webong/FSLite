#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>

#define CHUNK_SIZE 65536
#define STREAM_SIZE (5 * 1024 * 1024)  // 5MB
#define ITERATIONS 30

static void create_benchmark_volume(const char *filename) {
    char *cmd = malloc(strlen(filename) + 32);
    strcpy(cmd, "rm -f ");
    strcat(cmd, filename);
    system(cmd);
    free(cmd);
}

static double measure_time(void (*benchmark_func)(void)) {
    clock_t start = clock();
    benchmark_func();
    clock_t end = clock();
    return (double)(end - start) / CLOCKS_PER_SEC;
}

static void bench_path_lookups() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-path-lookup-bench-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    // Create a directory structure
    OK(fslite_mkdir(volume, "/dir1"));
    OK(fslite_mkdir(volume, "/dir1/subdir1"));
    OK(fslite_mkdir(volume, "/dir1/subdir2"));

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        fslite_info info;
        STATUS(fslite_stat(volume, "/dir1/subdir1", &info), FSLITE_OK);
        CHECK(info.kind == FSLITE_FILE);
        CHECK(info.size == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Path lookup benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

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
    for (int i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)i;

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/file", data, sizeof(data)));

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, "/file", 0, read_buf, sizeof(read_buf), &count));
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

    unsigned char *buffer = malloc(STREAM_SIZE);
    CHECK(buffer != NULL);
    for (int i = 0; i < STREAM_SIZE; ++i) buffer[i] = (unsigned char)(i & 0xFF);

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/stream.dat", buffer, STREAM_SIZE));

        unsigned char *read_buf = malloc(STREAM_SIZE);
        CHECK(read_buf != NULL);
        size_t count = 0;
        OK(fslite_read(volume, "/stream.dat", 0, read_buf, STREAM_SIZE, &count));
        CHECK(count == STREAM_SIZE);
        CHECK(memcmp(buffer, read_buf, STREAM_SIZE) == 0);
        free(read_buf);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double total_bytes = (double)ITERATIONS * STREAM_SIZE;

    printf("Streaming writes benchmark (iter=%d, size=%dMB): %.3f seconds, %.1f MB/sec\n",
           ITERATIONS, STREAM_SIZE / (1024 * 1024), elapsed, total_bytes / (elapsed * 1024 * 1024));

    fslite_close(volume);
    free(buffer);
    CHECK(unlink(filename) == 0);
}

static void bench_random_access_writes() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-random-access-bench-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    uint32_t state = 0x51f17e;
    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        uint32_t offset = (uint32_t)random_number(&state) % (10 * 1024 * 1024);  // 10MB max
        uint32_t length = (uint32_t)random_number(&state) % (CHUNK_SIZE / 4);
        if (length > 10 * 1024 * 1024 - offset) length = 10 * 1024 * 1024 - offset;

        unsigned char buffer[256];
        for (int i = 0; i < length; ++i) buffer[i] = (unsigned char)iter;

        OK(fslite_write_at(volume, "/file.dat", offset, buffer, length));

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, "/file.dat", offset, read_buf, sizeof(read_buf), &count));
        CHECK(count == (size_t)length);
        CHECK(memcmp(buffer, read_buf, length) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Random access writes benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_concurrent_operations() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-concurrent-bench-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    uint32_t state = 0x12345678;
    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        if (random_number(&state) % 3 == 0) {
            unsigned char data[128];
            snprintf((char*)data, sizeof(data), "file_%d", iter);
            OK(fslite_write(volume, (char*)data, data, strlen((char*)data)));
        } else if (random_number(&state) % 3 == 1) {
            // Read existing file
            OK(fslite_write(volume, "/temp", "data", 4));
            unsigned char read_buf[128];
            size_t count = 0;
            OK(fslite_read(volume, "/temp", 0, read_buf, sizeof(read_buf), &count));
        } else {
            // List directory
            fslite_list(volume, "/", NULL, NULL);
        }
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Concurrent operations benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

int main(void) {
    printf("=== FSLite Full Performance Benchmark Suite ===\n");
    printf("Running %d iterations per benchmark\n\n", ITERATIONS);

    bench_path_lookups();
    bench_small_file_operations();
    bench_streaming_writes();
    bench_random_access_writes();
    bench_concurrent_operations();

    printf("=== All benchmarks completed ===\n");
    return 0;
}