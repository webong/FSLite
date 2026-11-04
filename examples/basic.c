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
    const char *message = "Hello from FSLite!";
    status = fslite_write(volume, "/hello.txt", message, strlen(message));
    if (status != FSLITE_OK) goto done;
    status = fslite_commit(volume);
    if (status != FSLITE_OK) goto done;
    char buffer[64];
    size_t count = 0;
    status = fslite_read(volume, "/hello.txt", 0, buffer, sizeof buffer, &count);
    if (status != FSLITE_OK) goto done;
    fwrite(buffer, 1, count, stdout);
    putchar('\n');
done:
    if (status != FSLITE_OK) fprintf(stderr, "FSLite: %s\n", fslite_status_string(status));
    fslite_close(volume);
    return status == FSLITE_OK ? 0 : 1;
}
