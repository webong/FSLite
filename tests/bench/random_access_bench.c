#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define STATUS(call, expected) do { fslite_status actual = (call); if (actual != (expected)) { \
    fprintf(stderr, "%s:%d: %s: %s (%d), expected %d\n", __FILE__, __LINE__, \
        #call, fslite_status_string(actual), actual, (expected)); exit(1); \
} } while (0)
#define OK(call) STATUS(call, FSLITE_OK)

#define CHUNK_SIZE 65536
#define FILE_SIZE (100 * 1024 * 1024)  // 100 MB
#define ITERATIONS 20

static uint32_t random_number(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223); 
    return *state;
}

static void bench_random_access_writes() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-random-access-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char *buffer = malloc(CHUNK_SIZE);
    CHECK(buffer != NULL);
    for (int i = 0; i < CHUNK_SIZE; ++i) buffer[i] = (unsigned char)(i * 7);

    uint32_t state = 0x51f17e;
    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        uint32_t offset = random_number(&state) % FILE_SIZE;
        uint32_t length = random_number(&state) % (CHUNK_SIZE / 4);
        if (length > FILE_SIZE - offset) length = FILE_SIZE - offset;

        OK(fslite_write_at(volume, "/file.dat", offset, buffer, length));

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, "/file.dat", offset, read_buf, sizeof(read_buf), &count));
        CHECK(count == (size_t)length);
        CHECK(memcmp(buffer, read_buf, length) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Random access writes benchmark (iter=%d, file_size=%dMB): %.3f seconds, %.1f ops/sec\n",
           ITERATIONS, FILE_SIZE / (1024 * 1024), elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    free(buffer);
    CHECK(unlink(filename) == 0);
}

static void bench_random_access_reads() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-random-access-reads-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    unsigned char data[CHUNK_SIZE];
    for (int i = 0; i < CHUNK_SIZE; ++i) data[i] = (unsigned char)(i * 11);

    OK(fslite_write(volume, "/file.dat", data, CHUNK_SIZE));

    uint32_t state = 0x51f17e;
    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        uint32_t offset = random_number(&state) % CHUNK_SIZE;
        uint32_t length = random_number(&state) % (CHUNK_SIZE / 2);
        if (length > CHUNK_SIZE - offset) length = CHUNK_SIZE - offset;

        unsigned char read_buf[256];
        size_t count = 0;
        OK(fslite_read(volume, "/file.dat", offset, read_buf, sizeof(read_buf), &count));
        CHECK(count == (size_t)length);
        CHECK(memcmp(data + offset, read_buf, length) == 0);
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Random access reads benchmark (iter=%d, file_size=%dKB): %.3f seconds, %.1f ops/sec\n",
           ITERATIONS, CHUNK_SIZE / 1024, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

static void bench_mixed_random_access() {
    char filename[64];
    strcpy(filename, "/tmp/fslite-random-access-mixed-XXXXXX");
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    fslite_volume *volume = NULL;
    OK(fslite_open(filename, FSLITE_OPEN_CREATE, &volume));

    uint32_t state = 0x51f17e;
    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        if (random_number(&state) % 2 == 0) {
            uint32_t offset = random_number(&state) % CHUNK_SIZE;
            uint32_t length = random_number(&state) % (CHUNK_SIZE / 4);
            unsigned char buffer[64];
            for (int i = 0; i < length; ++i) buffer[i] = (unsigned char)iter;
            OK(fslite_write_at(volume, "/file.dat", offset, buffer, length));

            unsigned char read_buf[64];
            size_t count = 0;
            OK(fslite_read(volume, "/file.dat", offset, read_buf, sizeof(read_buf), &count));
            CHECK(count == (size_t)length);
            CHECK(memcmp(buffer, read_buf, length) == 0);
        } else {
            unsigned char buffer[256];
            OK(fslite_write(volume, "/file.dat", buffer, sizeof(buffer)));

            unsigned char read_buf[256];
            size_t count = 0;
            OK(fslite_read(volume, "/file.dat", 0, read_buf, sizeof(read_buf), &count));
            CHECK(count == sizeof(buffer));
        }
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("Mixed random access benchmark (iter=%d): %.3f seconds, %.0f ops/sec\n",
           ITERATIONS, elapsed, (double)ITERATIONS / elapsed);

    fslite_close(volume);
    CHECK(unlink(filename) == 0);
}

int main(void) {
    printf("=== FSLite Random Access Benchmarks ===\n");
    bench_random_access_reads();
    bench_random_access_writes();
    bench_mixed_random_access();
    printf("=== All random access benchmarks completed ===\n");
    return 0;
}