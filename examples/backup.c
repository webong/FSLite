#include "fslite.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s source.fslite new-backup.fslite\n", argv[0]);
        return 2;
    }
    fslite_volume *source = NULL;
    fslite_status status = fslite_open(argv[1], 0, &source);
    if (status == FSLITE_OK) status = fslite_export(source, argv[2]);
    fslite_close(source);
    if (status != FSLITE_OK) {
        fprintf(stderr, "FSLite: %s\n", fslite_status_string(status));
        return 1;
    }
    puts("Backup published");
    return 0;
}
