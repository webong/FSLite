#define _POSIX_C_SOURCE 200809L
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define MAX_SIZE 1024
#define ITERATIONS 10000

static void bench_small_files() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-small-files-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char buffer[256];
    for (size_t i = 0; i < sizeof(buffer); ++i) buffer[i] = (unsigned char)i;

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        int size = (iter % 10) + 1;
        int file_num = iter % 10;

        char path[64];
        snprintf(path, sizeof(path), "/file%d", file_num);

        OK(fslite_write(volume, path, buffer, size));

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, path, 0, read_buf, sizeof(read_buf), &count));
        CHECK(count == (size_t)size);
        CHECK(memcmp(buffer, read_buf, size) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Small files benchmark (iter=%d, max size=%d, files=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, MAX_SIZE, 10, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_many_small_files() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-many-small-files-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char buffer[128];
    for (size_t i = 0; i < sizeof(buffer); ++i) buffer[i] = (unsigned char)(i * 2);

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        char path[64];
        snprintf(path, sizeof(path), "/file%d", iter);

        OK(fslite_write(volume, path, buffer, sizeof(buffer)));

        unsigned char read_buf[128];
        size_t count = 0;
        OK(fslite_read(volume, path, 0, read_buf, sizeof(read_buf), &count));
        CHECK(count == sizeof(buffer));
        CHECK(memcmp(buffer, read_buf, sizeof(buffer)) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Many small files benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_small_file_overwrites() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-overwrite-small-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char data[64];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)i;

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/file", data, sizeof(data)));

        unsigned char read_buf[64];
        size_t count = 0;
        OK(fslite_read(volume, "/file", 0, read_buf, sizeof(read_buf), &count));
        CHECK(count == sizeof(data));
        CHECK(memcmp(data, read_buf, sizeof(data)) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Small file overwrite benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

int main(void) {
    printf("=== FSLite Small Files Benchmarks ===\n");
    bench_small_files();
    bench_many_small_files();
    bench_small_file_overwrites();
    printf("=== All benchmarks completed ===\n");
    return 0;
}