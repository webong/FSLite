#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: %s (%d), expected %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)

#define CHUNK_SIZE 65536
#define LARGE_SIZE (10 * 1024 * 1024)  // 10 MB
#define ITERATIONS 20

static void bench_large_streaming_writes() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-streaming-large-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char *buffer = malloc(LARGE_SIZE);
    CHECK(buffer != NULL);
    for (int i = 0; i < LARGE_SIZE; ++i) buffer[i] = (unsigned char)(i & 0xFF);

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/large_file.dat", buffer, LARGE_SIZE));

        unsigned char *read_buf = malloc(LARGE_SIZE);
        CHECK(read_buf != NULL);
        size_t count = 0;
        OK(fslite_read(volume, "/large_file.dat", 0, read_buf, LARGE_SIZE, &count));
        CHECK(count == (size_t)LARGE_SIZE);
        CHECK(memcmp(buffer, read_buf, LARGE_SIZE) == 0);
        free(read_buf);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double total_bytes = (double)ITERATIONS * LARGE_SIZE;

    printf("Large streaming writes benchmark (iter=%d, size=%d bytes): %.3f seconds, %.1f MB/sec\n",
           ITERATIONS, LARGE_SIZE, elapsed, total_bytes / (elapsed * 1024 * 1024));

    fslite_close(volume);
    free(buffer);
    CHECK(unlink(filename) == 0);
}

static void bench_chunked_streaming_writes() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-streaming-chunked-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char chunk[CHUNK_SIZE];
    for (int i = 0; i < CHUNK_SIZE; ++i) chunk[i] = (unsigned char)(i * 3);

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/chunked.dat", chunk, CHUNK_SIZE));

        unsigned char read_buf[CHUNK_SIZE];
        size_t count = 0;
        OK(fslite_read(volume, "/chunked.dat", 0, read_buf, sizeof(read_buf), &count));
        CHECK(count == CHUNK_SIZE);
        CHECK(memcmp(chunk, read_buf, CHUNK_SIZE) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double total_bytes = (double)ITERATIONS * CHUNK_SIZE;

    printf("Chunked streaming writes benchmark (iter=%d, chunk=%d bytes): %.3f seconds, %.1f MB/sec\n",
           ITERATIONS, CHUNK_SIZE, elapsed, total_bytes / (elapsed * 1024 * 1024));

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_empty_streaming_writes() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-streaming-empty-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        OK(fslite_write(volume, "/empty.dat", NULL, 0));

        unsigned char read_buf[1];
        size_t count = 0;
        OK(fslite_read(volume, "/empty.dat", 0, read_buf, sizeof(read_buf), &count));
        CHECK(count == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Empty streaming writes benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

int main(void) {
    printf("=== FSLite Streaming Writes Benchmarks ===\n");
    bench_empty_streaming_writes();
    bench_chunked_streaming_writes();
    bench_large_streaming_writes();
    printf("=== All streaming benchmarks completed ===\n");
    return 0;
}