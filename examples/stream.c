#include "fslite.h"
#include <inttypes.h>
#include <stdio.h>

/* Import stdin without retaining the complete input in the host or engine. */
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s volume.fslite /volume/path < input\n", argv[0]);
        return 2;
    }
    fslite_volume *volume = NULL;
    fslite_writer *writer = NULL;
    int input_error = 0;
    uint64_t total = 0;
    fslite_status status = fslite_open(argv[1], FSLITE_OPEN_CREATE, &volume);
    if (status != FSLITE_OK) goto done;
    status = fslite_writer_open(volume, argv[2], &writer);
    if (status != FSLITE_OK) goto done;
    unsigned char buffer[16384];
    size_t count;
    while ((count = fread(buffer, 1, sizeof buffer, stdin)) != 0) {
        status = fslite_writer_write(writer, buffer, count);
        if (status != FSLITE_OK) goto done;
        total += count;
    }
    if (ferror(stdin)) {
        perror("reading stdin");
        input_error = 1;
        goto done;
    }
    status = fslite_writer_finish(writer);
    if (status == FSLITE_OK) printf("Stored %" PRIu64 " bytes\n", total);
done:
    if (status != FSLITE_OK) fprintf(stderr, "FSLite: %s\n", fslite_status_string(status));
    fslite_writer_close(writer); /* Aborts unfinished input, including fread errors. */
    fslite_close(volume);
    return status == FSLITE_OK && !input_error ? 0 : 1;
}
