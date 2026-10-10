#ifndef PUNTO_DOCUMENTS_H
#define PUNTO_DOCUMENTS_H

#include <stdbool.h>
#include <stddef.h>

#define DOCUMENTS_MAX_BYTES (10U * 1024U * 1024U)
#define DOCUMENTS_FILENAME_MAX 180U

/* The caller checks its user's permissions before invoking any of these APIs.
   Metadata remains in SQL; production PDF bytes are held only in GridFS. */
bool documents_open(const char *db_path);
void documents_close(void);
bool documents_health(void);
bool documents_store(const char *storage_key, const char *file_name,
                     const unsigned char *data, size_t size, const char *sha256);
/* On success the caller owns *data. On failure *data is always NULL. */
bool documents_load(const char *storage_key, size_t expected_size,
                    unsigned char **data);
bool documents_remove(const char *storage_key);

#endif
