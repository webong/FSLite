#define _POSIX_C_SOURCE 200809L
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define LARGE_FILE_SIZE (256 * 1024 * 1024)  // 256MB
#define ITERATIONS 3

static uint32_t random_number(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void bench_large_file_create() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-large-create-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    // Generate data in chunks to avoid large memory allocation
    unsigned char *chunk = malloc(65536);
    CHECK(chunk != NULL);
    for (size_t i = 0; i < 65536; ++i) chunk[i] = (unsigned char)(i * 3);

    // Create the file first with a small write
    OK(fslite_write(volume, "/large.dat", chunk, 65536));

    clock_t start = clock();
    for (uint64_t offset = 0; offset < LARGE_FILE_SIZE; offset += 65536) {
        OK(fslite_write_at(volume, "/large.dat", offset, chunk, 65536));
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Large file create (iter=1, size=256MB): %.3f seconds, %.1f MB/sec\n",
           elapsed, (double)LARGE_FILE_SIZE / (elapsed * 1024 * 1024));

    fslite_close(volume);
    free(chunk);
    CHECK(unlink(filename) == 0);
}

static void bench_large_file_streaming() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-large-stream-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    // Create the file first
    unsigned char *chunk = malloc(65536);
    CHECK(chunk != NULL);
    for (size_t i = 0; i < 65536; ++i) chunk[i] = (unsigned char)(i * 5);
    OK(fslite_write(volume, "/large_stream.dat", chunk, 65536));

    clock_t start = clock();
    for (uint64_t offset = 0; offset < LARGE_FILE_SIZE; offset += 65536) {
        OK(fslite_write(volume, "/large_stream.dat", chunk, 65536));
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Large file streaming (iter=1, size=256MB): %.3f seconds, %.1f MB/sec\n",
           elapsed, (double)LARGE_FILE_SIZE / (elapsed * 1024 * 1024));

    fslite_close(volume);
    free(chunk);
    CHECK(unlink(filename) == 0);
}

static void bench_large_file_random_access() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-large-random-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    // Pre-create the file
    unsigned char *chunk = malloc(65536);
    CHECK(chunk != NULL);
    for (size_t i = 0; i < 65536; ++i) chunk[i] = (unsigned char)(i * 7);
    OK(fslite_write(volume, "/large_rand.dat", chunk, 65536));
    for (uint64_t offset = 0; offset < LARGE_FILE_SIZE; offset += 65536) {
        OK(fslite_write_at(volume, "/large_rand.dat", offset, chunk, 65536));
    }

    uint32_t state = 0x51f17e;
    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        uint32_t offset = random_number(&state) % (LARGE_FILE_SIZE - 65536);
        size_t count = 65536;
        OK(fslite_read(volume, "/large_rand.dat", offset, chunk, 65536, &count));
        CHECK(count == 65536);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    uint64_t total_bytes = (uint64_t)ITERATIONS * 65536;

    printf("Large file random reads (iter=%d, file_size=256MB): %.3f seconds, %.1f MB/sec\n",
           ITERATIONS, elapsed, (double)total_bytes / (elapsed * 1024 * 1024));

    fslite_close(volume);
    free(chunk);
    CHECK(unlink(filename) == 0);
}

int main(void) {
    printf("=== FSLite Large File Benchmarks ===\n");
    printf("File size: 256MB per operation\n\n");

    bench_large_file_create();
    bench_large_file_streaming();
    bench_large_file_random_access();

    printf("=== All large file benchmarks completed ===\n");
    return 0;
}