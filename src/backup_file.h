#ifndef FSLITE_BACKUP_FILE_H
#define FSLITE_BACKUP_FILE_H
#include <stdint.h>
typedef struct fslite_backup_file fslite_backup_file;
int32_t fslite_backup_file_create(const char *destination, fslite_backup_file **output);
const char *fslite_backup_file_path(fslite_backup_file *file);
int32_t fslite_backup_file_publish(fslite_backup_file *file);
void fslite_backup_file_close(fslite_backup_file *file);
#endif
