#include "fslite.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s volume.fslite\n", argv[0]);
        return 2;
    }
    fslite_volume *volume = NULL;
    fslite_status status = fslite_open(argv[1], FSLITE_OPEN_CREATE, &volume);
    if (status != FSLITE_OK) goto done;
    status = fslite_begin(volume);
    if (status != FSLITE_OK) goto done;
    status = fslite_write(volume, "/edited.bin", "Hello world", 11);
    if (status != FSLITE_OK) goto done;
    status = fslite_write_at(volume, "/edited.bin", 6, "FSLite", 6);
    if (status != FSLITE_OK) goto done;
    status = fslite_truncate(volume, "/edited.bin", 9);
    if (status != FSLITE_OK) goto done;
    status = fslite_truncate(volume, "/edited.bin", 12);
    if (status != FSLITE_OK) goto done;
    status = fslite_commit(volume);
    if (status != FSLITE_OK) goto done;
    unsigned char bytes[12]; size_t count = 0;
    status = fslite_read(volume, "/edited.bin", 0, bytes, sizeof bytes, &count);
    if (status != FSLITE_OK) goto done;
    const unsigned char expected[12] = { 'H','e','l','l','o',' ','F','S','L',0,0,0 };
    if (count != sizeof expected || memcmp(bytes, expected, sizeof expected) != 0) {
        fprintf(stderr, "Unexpected content after resizing\n");
        fslite_close(volume);
        return 1;
    }
    puts("Patch, shrink, and zero-filled regrowth verified");
done:
    if (status != FSLITE_OK) fprintf(stderr, "FSLite: %s\n", fslite_status_string(status));
    fslite_close(volume); /* Rolls back the transaction on any earlier failure. */
    return status == FSLITE_OK ? 0 : 1;
}
