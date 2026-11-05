#include "bench.h"
#include <sys/wait.h>

static void measure(int smoke, uint64_t bytes) {
    setup(smoke);
    unsigned char buffer[BUFFER_SIZE]; pattern(buffer, sizeof buffer, 53);
    // Peak RSS is process-wide, not live heap usage or engine-only allocation.
    double baseline = peak_rss_mib();
    double start = now_seconds(); stream_file("/memory", bytes, buffer, sizeof buffer);
    double elapsed = now_seconds() - start;
    double peak = peak_rss_mib(); // Capture before validation changes the cache.
    struct stat volume; CHECK(stat(bench_filename, &volume) == 0);
    report("memory_stream", 1, bytes, elapsed);
    printf("memory_file_bytes=%" PRIu64 " caller_buffer_bytes=%zu baseline_peak_rss_mib=%.3f peak_rss_mib=%.3f volume_bytes=%" PRId64 "\n",
        bytes, sizeof buffer, baseline, peak, (int64_t)volume.st_size);
    reopen(); verify_stream("/memory", bytes, buffer, sizeof buffer);
    cleanup();
}
int main(int argc, char **argv) {
    int smoke = options(argc, argv);
    const uint64_t sizes[] = {MIB, (smoke ? 4 : 16) * MIB, (smoke ? 8 : 128) * MIB};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        // A fresh child per size prevents earlier workloads contaminating peak RSS.
        CHECK(fflush(NULL) == 0);
        pid_t child = fork(); CHECK(child >= 0);
        if (child == 0) { measure(smoke, sizes[i]); exit(0); }
        int status; pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        CHECK(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    return 0;
}
