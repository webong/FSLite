#include "fslite.hpp"

#include <cstdio>
#include <cstring>

namespace {

void report(const fslite::Status &status) {
    std::fprintf(stderr, "FSLite: %s\n", status.message());
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s volume.fslite\n", argv[0]);
        return 2;
    }

    fslite::Volume volume;
    fslite::Status status = volume.open(argv[1], FSLITE_OPEN_CREATE);
    if (!status) {
        report(status);
        return 1;
    }

    const char message[] = "Hello from FSLite C++!";
    status = volume.write("/hello.txt", message, sizeof message - 1);
    if (!status) {
        report(status);
        return 1;
    }

    fslite_file_info info;
    status = volume.inspect("/hello.txt", info);
    if (!status) {
        report(status);
        return 1;
    }

    char buffer[64];
    size_t count = 0;
    status = volume.read("/hello.txt", 0, buffer, sizeof buffer, count);
    if (!status || count != info.size) {
        report(status.ok() ? fslite::Status(FSLITE_INTERNAL_ERROR) : status);
        return 1;
    }

    std::fwrite(buffer, 1, count, stdout);
    std::putchar('\n');
    return 0;
}
