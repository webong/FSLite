#include "bench.h"

static void run_case(unsigned files, size_t size, int batch, int overwrite) {
    unsigned char data[4096]; pattern(data, size, overwrite ? 93 : 17);
    double start = now_seconds();
    if (batch) OK(fslite_begin(bench_volume));
    for (unsigned i = 0; i < files; ++i) {
        char path[64]; snprintf(path, sizeof path, "/%s-%zu-%u", batch ? "batch" : "auto", size, i);
        OK(fslite_write(bench_volume, path, data, size));
    }
    if (batch) OK(fslite_commit(bench_volume));
    double elapsed = now_seconds() - start;
    char name[100]; snprintf(name, sizeof name, "small_%s_%s_%zuB", overwrite ? "overwrite" : "create",
        batch ? "transaction" : "autocommit", size);
    report(name, files, (uint64_t)files * size, elapsed);
    // Readback and integrity validation are outside the write timing.
    reopen();
    for (unsigned i = 0; i < files; ++i) {
        char path[64]; snprintf(path, sizeof path, "/%s-%zu-%u", batch ? "batch" : "auto", size, i);
        verify_size(path, size); verify_bytes(path, 0, data, size);
    }
    OK(fslite_check(bench_volume));
}
int main(int argc, char **argv) {
    int smoke = options(argc, argv); setup(smoke);
    const size_t sizes[] = {0, 128, 1024, 4096};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i)
        for (int batch = 0; batch < 2; ++batch) {
            run_case(smoke ? 8 : 1000, sizes[i], batch, 0);
            run_case(smoke ? 8 : 1000, sizes[i], batch, 1);
        }
    cleanup(); return 0;
}
