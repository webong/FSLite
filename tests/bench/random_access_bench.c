#include "bench.h"
#define PATCH_SIZE 4096
static uint32_t random_number(uint32_t *state) {
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5; return *state;
}
int main(int argc, char **argv) {
    int smoke = options(argc, argv); setup(smoke);
    const size_t size = (smoke ? 2 : 32) * (size_t)MIB;
    const unsigned operations = smoke ? 16 : 1000;
    unsigned char *model = calloc(size, 1); CHECK(model != NULL);
    uint64_t *offsets = malloc(operations * sizeof *offsets); CHECK(offsets != NULL);
    unsigned char *payloads = malloc((size_t)operations * PATCH_SIZE); CHECK(payloads != NULL);
    uint32_t state = 0x51f17e;
    for (unsigned i = 0; i < operations; ++i) {
        offsets[i] = random_number(&state) % (size - PATCH_SIZE + 1);
        // Every fourth patch crosses a 64 KiB chunk boundary.
        if (i % 4 == 0) offsets[i] = (1 + random_number(&state) % (size / BUFFER_SIZE - 1)) * BUFFER_SIZE - PATCH_SIZE / 2;
        pattern(payloads + (size_t)i * PATCH_SIZE, PATCH_SIZE, random_number(&state) & 255);
    }
    printf("# random_seed=0x51f17e file_bytes=%zu patch_bytes=%d operations=%u\n", size, PATCH_SIZE, operations);
    for (int batch = 0; batch < 2; ++batch) {
        memset(model, 0, size);
        // Create and densely allocate the file before timing patches.
        unsigned char zero[BUFFER_SIZE] = {0}; stream_file("/random", size, zero, sizeof zero);
        double start = now_seconds();
        if (batch) OK(fslite_begin(bench_volume));
        for (unsigned i = 0; i < operations; ++i)
            OK(fslite_write_at(bench_volume, "/random", offsets[i], payloads + (size_t)i * PATCH_SIZE, PATCH_SIZE));
        if (batch) OK(fslite_commit(bench_volume));
        double elapsed = now_seconds() - start;
        report(batch ? "random_write_transaction" : "random_write_autocommit", operations,
            (uint64_t)operations * PATCH_SIZE, elapsed);
        for (unsigned i = 0; i < operations; ++i)
            memcpy(model + offsets[i], payloads + (size_t)i * PATCH_SIZE, PATCH_SIZE);
        reopen(); verify_size("/random", size); verify_bytes("/random", 0, model, size);
        OK(fslite_check(bench_volume));
    }
    free(model); free(offsets); free(payloads); cleanup(); return 0;
}
