#include "bench.h"
int main(int argc, char **argv) {
    int smoke = options(argc, argv); setup(smoke);
    unsigned char buffer[BUFFER_SIZE]; pattern(buffer, sizeof buffer, 41);
    const size_t fragments[] = {4093, BUFFER_SIZE};
    uint64_t bytes = (smoke ? 2 : 128) * MIB + 17;
    for (size_t i = 0; i < sizeof fragments / sizeof *fragments; ++i) {
        for (int replacement = 0; replacement < 2; ++replacement) {
            char path[64]; snprintf(path, sizeof path, "/stream-%zu", fragments[i]);
            pattern(buffer, sizeof buffer, replacement ? 71 : 41);
            double start = now_seconds();
            stream_file(path, bytes, buffer, fragments[i]);
            double elapsed = now_seconds() - start;
            char name[100]; snprintf(name, sizeof name, "stream_%s_fragment_%zuB", replacement ? "replace" : "create", fragments[i]);
            report(name, 1, bytes, elapsed);
            reopen(); verify_stream(path, bytes, buffer, fragments[i]);
        }
    }
    cleanup(); return 0;
}
