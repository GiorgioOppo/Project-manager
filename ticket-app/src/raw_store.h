#ifndef TICKETS_RAW_STORE_H
#define TICKETS_RAW_STORE_H
#include "mail_mime.h"
#include <sqlite3.h>
#include <stdint.h>
typedef struct {
    sqlite3_int64 id, job_id, revision;
    char name[401], address[255], host[256], username[1021], folder[801], secret_name[128];
    char auth_mode[16], authserv_id[256];
    int port;
    bool active, trust_auth_results;
} RawMailbox;
typedef struct { sqlite3 *database; char error[128]; } RawStore;
bool raw_store_open(RawStore *store, const char *path);
void raw_store_close(RawStore *store);
bool raw_store_mailboxes(RawStore *store, sqlite3_int64 only, RawMailbox **items, size_t *count);
bool raw_store_checkpoint(RawStore *store, sqlite3_int64 mailbox, const char *source_key,
                          uint32_t *uidvalidity, uint32_t *last_uid);
bool raw_store_progress(RawStore *store, sqlite3_int64 mailbox, const char *source_key,
                        uint32_t uidvalidity, uint32_t last_uid, const char *error);
bool raw_store_ingest(RawStore *store, const RawMailbox *mailbox, const char *source_key,
                      uint32_t uidvalidity, uint32_t uid, const char *source,
                      const void *raw, size_t length, const RawMail *mail,
                      sqlite3_int64 *message_id, bool *replayed);
#endif
