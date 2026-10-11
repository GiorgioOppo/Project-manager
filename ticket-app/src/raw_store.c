#include "raw_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static bool failure(RawStore *s, const char *reason) { snprintf(s->error, sizeof(s->error), "%s", reason); return false; }
static sqlite3_stmt *prepare(RawStore *s, const char *sql) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(s->database, sql, -1, &statement, NULL) != SQLITE_OK) failure(s, "raw_storage_unavailable");
    return statement;
}
static bool execute(RawStore *s, const char *sql) { return sqlite3_exec(s->database, sql, NULL, NULL, NULL) == SQLITE_OK; }
static const char *text(sqlite3_stmt *s, int index) { const char *v = (const char *)sqlite3_column_text(s, index); return v ? v : ""; }
static void bind_text(sqlite3_stmt *s, int index, const char *value) { sqlite3_bind_text(s, index, value ? value : "", -1, SQLITE_TRANSIENT); }
bool raw_store_open(RawStore *s, const char *path) {
    memset(s, 0, sizeof(*s));
    if (sqlite3_open_v2(path, &s->database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK)
        return failure(s, "ticket_database_unavailable_initialize_app_first");
    sqlite3_busy_timeout(s->database, 5000);
    sqlite3_stmt *version = prepare(s, "PRAGMA user_version");
    int schema = version && sqlite3_step(version) == SQLITE_ROW ? sqlite3_column_int(version, 0) : -1;
    bool supported = schema == 4 || schema == 5;
    sqlite3_finalize(version);
    if (!supported) return failure(s, "ticket_schema_4_or_5_required_initialize_app_first");
    if (!execute(s, "PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL"))
        return failure(s, "raw_storage_unavailable");
    return true;
}
void raw_store_close(RawStore *s) { if (s->database) sqlite3_close(s->database); s->database = NULL; }
bool raw_store_mailboxes(RawStore *s, sqlite3_int64 only, RawMailbox **items, size_t *count) {
    *items = NULL; *count = 0;
    sqlite3_stmt *q = prepare(s, "SELECT id,name,address,host,port,username,folder,secret_name,auth_mode,authserv_id,trust_auth_results,active,COALESCE(job_id,0),revision FROM raw_mailboxes WHERE (?=0 AND active=1) OR id=? ORDER BY id LIMIT 1001");
    if (!q) return false;
    sqlite3_bind_int64(q, 1, only); sqlite3_bind_int64(q, 2, only);
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        if (*count >= 1000) { sqlite3_finalize(q); free(*items); *items = NULL; return failure(s, "mailbox_limit_exceeded"); }
        RawMailbox *grown = realloc(*items, (*count + 1) * sizeof(**items));
        if (!grown) { sqlite3_finalize(q); free(*items); *items = NULL; return failure(s, "allocation_failed"); }
        *items = grown; RawMailbox *m = &grown[(*count)++]; memset(m, 0, sizeof(*m));
        m->id = sqlite3_column_int64(q, 0); m->port = sqlite3_column_int(q, 4); m->job_id = sqlite3_column_int64(q, 12);
        m->active = sqlite3_column_int(q, 11) != 0; m->trust_auth_results = sqlite3_column_int(q, 10) != 0;
        m->revision = sqlite3_column_int64(q, 13);
#define COPY(member, index) do { \
    if (strlen(text(q, index)) >= sizeof(m->member)) { sqlite3_finalize(q); free(*items); *items = NULL; *count = 0; return failure(s, "mailbox_configuration_too_long"); } \
    snprintf(m->member, sizeof(m->member), "%s", text(q, index)); \
} while (0)
        COPY(name, 1); COPY(address, 2); COPY(host, 3); COPY(username, 5); COPY(folder, 6);
        COPY(secret_name, 7); COPY(auth_mode, 8); COPY(authserv_id, 9);
#undef COPY
    }
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) { free(*items); *items = NULL; return failure(s, "raw_storage_unavailable"); }
    return true;
}
bool raw_store_checkpoint(RawStore *s, sqlite3_int64 mailbox, const char *key, uint32_t *validity, uint32_t *uid) {
    *validity = 0; *uid = 0;
    sqlite3_stmt *q = prepare(s, "SELECT uidvalidity,last_uid FROM raw_mailbox_state WHERE mailbox_id=? AND source_key=?");
    if (!q) return false;
    sqlite3_bind_int64(q, 1, mailbox); bind_text(q, 2, key); int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) { *validity = (uint32_t)sqlite3_column_int64(q, 0); *uid = (uint32_t)sqlite3_column_int64(q, 1); }
    sqlite3_finalize(q); return rc == SQLITE_ROW || rc == SQLITE_DONE ? true : failure(s, "raw_storage_unavailable");
}
bool raw_store_progress(RawStore *s, sqlite3_int64 mailbox, const char *key, uint32_t validity, uint32_t uid, const char *error) {
    sqlite3_stmt *q = prepare(s, "INSERT INTO raw_mailbox_state(mailbox_id,source_key,uidvalidity,last_uid,last_polled_at,last_error) VALUES(?,?,?,?,?,?) ON CONFLICT(mailbox_id,source_key) DO UPDATE SET uidvalidity=excluded.uidvalidity,last_uid=excluded.last_uid,last_polled_at=excluded.last_polled_at,last_error=excluded.last_error");
    if (!q) return false;
    sqlite3_bind_int64(q, 1, mailbox); bind_text(q, 2, key); sqlite3_bind_int64(q, 3, validity); sqlite3_bind_int64(q, 4, uid);
    sqlite3_bind_int64(q, 5, (sqlite3_int64)time(NULL)); bind_text(q, 6, error);
    bool ok = sqlite3_step(q) == SQLITE_DONE; sqlite3_finalize(q); return ok ? true : failure(s, "raw_storage_unavailable");
}
bool raw_store_ingest(RawStore *s, const RawMailbox *m, const char *key, uint32_t validity, uint32_t uid,
                      const char *source, const void *raw, size_t length, const RawMail *mail,
                      sqlite3_int64 *message_id, bool *replayed) {
    *message_id = 0; *replayed = false;
    if (!raw || !length || length > RAW_MAIL_MAX || (strcmp(source, "imap") && strcmp(source, "import"))) return failure(s, "invalid_raw_message");
    char sha[65], identity[256]; raw_sha256(raw, length, sha);
    if (!strcmp(source, "imap")) snprintf(identity, sizeof(identity), "imap:%lld:%s:%u:%u", (long long)m->id, key, validity, uid);
    else snprintf(identity, sizeof(identity), "import:%lld:%s", (long long)m->id, sha);
    if (!execute(s, "BEGIN IMMEDIATE")) return failure(s, "raw_storage_busy");
    if (!strcmp(source, "imap")) {
        sqlite3_stmt *receipt = prepare(s, "SELECT m.id,m.raw_sha256 FROM raw_receipts r JOIN raw_messages m ON m.id=r.raw_message_id WHERE r.mailbox_id=? AND r.source_key=? AND r.uidvalidity=? AND r.uid=?");
        if (!receipt) goto rollback;
        sqlite3_bind_int64(receipt, 1, m->id); bind_text(receipt, 2, key); sqlite3_bind_int64(receipt, 3, validity); sqlite3_bind_int64(receipt, 4, uid);
        int found = sqlite3_step(receipt);
        if (found == SQLITE_ROW) {
            if (strcmp(text(receipt, 1), sha)) { sqlite3_finalize(receipt); failure(s, "delivery_identity_content_changed"); goto rollback; }
            *message_id = sqlite3_column_int64(receipt, 0); *replayed = true;
        }
        sqlite3_finalize(receipt);
        if (found != SQLITE_ROW && found != SQLITE_DONE) goto rollback;
    }
    sqlite3_stmt *q = prepare(s, "SELECT id,raw_sha256 FROM raw_messages WHERE delivery_identity=? OR (mailbox_id=? AND raw_sha256=?) ORDER BY delivery_identity=? DESC LIMIT 1");
    if (!q) goto rollback;
    bind_text(q, 1, identity); sqlite3_bind_int64(q, 2, m->id); bind_text(q, 3, sha); bind_text(q, 4, identity);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW && !*message_id) {
        if (strcmp(text(q, 1), sha)) { sqlite3_finalize(q); failure(s, "delivery_identity_content_changed"); goto rollback; }
        *message_id = sqlite3_column_int64(q, 0); *replayed = true;
    }
    sqlite3_finalize(q);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto rollback;
    if (!*message_id) {
        bool active = false, trusted = false, sender_active = false, same_configuration = false; sqlite3_int64 job = 0, sender_job = 0, until = 0;
        char current_authserv[256] = "";
        q = prepare(s, "SELECT active,trust_auth_results,authserv_id,COALESCE(job_id,0),revision,host,port,username,folder,secret_name,auth_mode FROM raw_mailboxes WHERE id=?");
        if (!q) goto rollback;
        sqlite3_bind_int64(q, 1, m->id); rc = sqlite3_step(q);
        if (rc == SQLITE_ROW) { active = sqlite3_column_int(q, 0) != 0; trusted = sqlite3_column_int(q, 1) != 0;
            snprintf(current_authserv, sizeof(current_authserv), "%s", text(q, 2)); job = sqlite3_column_int64(q, 3);
            same_configuration = sqlite3_column_int64(q, 4) == m->revision && !strcmp(text(q, 5), m->host) && sqlite3_column_int(q, 6) == m->port &&
                !strcmp(text(q, 7), m->username) && !strcmp(text(q, 8), m->folder) && !strcmp(text(q, 9), m->secret_name) && !strcmp(text(q, 10), m->auth_mode);
        }
        sqlite3_finalize(q); if (rc != SQLITE_ROW) goto rollback;
        q = prepare(s, "SELECT active,COALESCE(valid_until,0),COALESCE(job_id,0) FROM raw_senders WHERE mailbox_id=? AND email=? COLLATE NOCASE");
        if (!q) goto rollback;
        sqlite3_bind_int64(q, 1, m->id); bind_text(q, 2, mail->from_email); rc = sqlite3_step(q);
        if (rc == SQLITE_ROW) { sender_active = sqlite3_column_int(q, 0) != 0; until = sqlite3_column_int64(q, 1); sender_job = sqlite3_column_int64(q, 2); }
        sqlite3_finalize(q); if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto rollback;
        const char *reason = "";
        if (!active) reason = "mailbox_inactive";
        else if (strcmp(source, "imap")) reason = "local_import_untrusted";
        else if (!same_configuration) { reason = "mailbox_configuration_changed_during_fetch"; job = m->job_id; }
        else if (mail->parse_error[0]) reason = mail->parse_error;
        else if (!trusted || strcmp(current_authserv, mail->authserv_id) || strcmp(mail->auth_status, "pass")) reason = mail->auth_reason[0] ? mail->auth_reason : "authentication_not_verified";
        else if (!sender_active) reason = "sender_not_authorized";
        else if (until && until <= (sqlite3_int64)time(NULL)) reason = "sender_authorization_expired";
        else if (job && sender_job && job != sender_job) reason = "sender_mailbox_job_mismatch";
        if (!job) job = sender_job;
        q = prepare(s, "INSERT INTO raw_messages(mailbox_id,source,source_key,delivery_identity,raw_sha256,raw_eml,received_at,from_email,from_name,subject,message_id,in_reply_to,references_json,headers_json,body_text,text_preview,auth_status,authserv_id,auth_reason,parse_error,state,quarantine_reason,job_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!q) goto rollback;
        sqlite3_bind_int64(q, 1, m->id); bind_text(q, 2, source); bind_text(q, 3, key); bind_text(q, 4, identity); bind_text(q, 5, sha);
        sqlite3_bind_blob(q, 6, raw, (int)length, SQLITE_TRANSIENT); sqlite3_bind_int64(q, 7, (sqlite3_int64)time(NULL));
        const char *values[] = {mail->from_email,mail->from_name,mail->subject,mail->message_id,mail->in_reply_to,mail->references_json,mail->headers_json,mail->body_text,mail->text_preview,mail->auth_status,mail->authserv_id,mail->auth_reason,mail->parse_error,*reason ? "quarantine" : "new",reason};
        for (int i = 0; i < 15; i++) bind_text(q, 8 + i, values[i]);
        if (job) sqlite3_bind_int64(q, 23, job); else sqlite3_bind_null(q, 23);
        rc = sqlite3_step(q); sqlite3_finalize(q); if (rc != SQLITE_DONE) goto rollback;
        *message_id = sqlite3_last_insert_rowid(s->database);
        for (size_t i = 0; i < mail->attachment_count; i++) {
            const RawAttachment *a = &mail->attachments[i];
            q = prepare(s, "INSERT INTO raw_attachments(raw_message_id,filename,content_type,size,sha256,content) VALUES(?,?,?,?,?,?)");
            if (!q) goto rollback;
            sqlite3_bind_int64(q, 1, *message_id); bind_text(q, 2, a->filename); bind_text(q, 3, a->content_type);
            sqlite3_bind_int64(q, 4, (sqlite3_int64)a->size); bind_text(q, 5, a->sha256); sqlite3_bind_blob(q, 6, a->content, (int)a->size, SQLITE_TRANSIENT);
            rc = sqlite3_step(q); sqlite3_finalize(q); if (rc != SQLITE_DONE) goto rollback;
        }
    }
    if (!strcmp(source, "imap")) {
        q = prepare(s, "INSERT INTO raw_receipts(mailbox_id,source_key,uidvalidity,uid,raw_message_id,received_at) VALUES(?,?,?,?,?,?) ON CONFLICT(mailbox_id,source_key,uidvalidity,uid) DO NOTHING");
        if (!q) goto rollback;
        sqlite3_bind_int64(q, 1, m->id); bind_text(q, 2, key); sqlite3_bind_int64(q, 3, validity); sqlite3_bind_int64(q, 4, uid);
        sqlite3_bind_int64(q, 5, *message_id); sqlite3_bind_int64(q, 6, (sqlite3_int64)time(NULL)); rc = sqlite3_step(q); sqlite3_finalize(q);
        if (rc != SQLITE_DONE) goto rollback;
    }
    if (execute(s, "COMMIT")) { s->error[0] = 0; return true; }
rollback:
    execute(s, "ROLLBACK"); if (!s->error[0]) failure(s, "raw_ingest_failed"); *message_id = 0; return false;
}
