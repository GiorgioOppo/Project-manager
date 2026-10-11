#ifndef TICKETS_RAW_SCHEMA_H
#define TICKETS_RAW_SCHEMA_H
/* Shared by the HTTP app and the local receiver. Only the app migrates schema. */
#define RAW_SCHEMA_SQL_CONFIG \
"CREATE TABLE IF NOT EXISTS raw_mailboxes(" \
"id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,address TEXT NOT NULL," \
"host TEXT NOT NULL DEFAULT '',port INTEGER NOT NULL DEFAULT 993 CHECK(port BETWEEN 1 AND 65535)," \
"username TEXT NOT NULL DEFAULT '',folder TEXT NOT NULL DEFAULT 'INBOX',secret_name TEXT NOT NULL DEFAULT ''," \
"auth_mode TEXT NOT NULL DEFAULT 'password' CHECK(auth_mode IN('password','xoauth2'))," \
"authserv_id TEXT NOT NULL DEFAULT '',trust_auth_results INTEGER NOT NULL DEFAULT 0 CHECK(trust_auth_results IN(0,1))," \
"active INTEGER NOT NULL DEFAULT 0 CHECK(active IN(0,1)),job_id INTEGER CHECK(job_id IS NULL OR job_id>0)," \
"revision INTEGER NOT NULL DEFAULT 1,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));" \
"CREATE TABLE IF NOT EXISTS raw_senders(" \
"id INTEGER PRIMARY KEY AUTOINCREMENT,mailbox_id INTEGER NOT NULL REFERENCES raw_mailboxes(id)," \
"email TEXT NOT NULL COLLATE NOCASE,name TEXT NOT NULL DEFAULT '',active INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1))," \
"valid_until INTEGER CHECK(valid_until IS NULL OR valid_until>0),job_id INTEGER CHECK(job_id IS NULL OR job_id>0)," \
"revision INTEGER NOT NULL DEFAULT 1,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),UNIQUE(mailbox_id,email));"
#define RAW_SCHEMA_SQL_MESSAGES \
"CREATE TABLE IF NOT EXISTS raw_mailbox_state(" \
"mailbox_id INTEGER NOT NULL REFERENCES raw_mailboxes(id),source_key TEXT NOT NULL,uidvalidity INTEGER NOT NULL DEFAULT 0," \
"last_uid INTEGER NOT NULL DEFAULT 0,last_polled_at INTEGER,last_error TEXT NOT NULL DEFAULT '',PRIMARY KEY(mailbox_id,source_key));" \
"CREATE TABLE IF NOT EXISTS raw_messages(" \
"id INTEGER PRIMARY KEY AUTOINCREMENT,mailbox_id INTEGER NOT NULL REFERENCES raw_mailboxes(id)," \
"source TEXT NOT NULL CHECK(source IN('import','imap')),source_key TEXT NOT NULL,delivery_identity TEXT NOT NULL UNIQUE," \
"raw_sha256 TEXT NOT NULL,raw_eml BLOB NOT NULL CHECK(length(raw_eml) BETWEEN 1 AND 10485760),received_at INTEGER NOT NULL," \
"from_email TEXT NOT NULL DEFAULT '',from_name TEXT NOT NULL DEFAULT '',subject TEXT NOT NULL DEFAULT ''," \
"message_id TEXT NOT NULL DEFAULT '',in_reply_to TEXT NOT NULL DEFAULT '',references_json TEXT NOT NULL DEFAULT '[]'," \
"headers_json TEXT NOT NULL DEFAULT '[]',body_text TEXT NOT NULL DEFAULT '',text_preview TEXT NOT NULL DEFAULT ''," \
"auth_status TEXT NOT NULL CHECK(auth_status IN('pass','unverified','fail')),authserv_id TEXT NOT NULL DEFAULT '',auth_reason TEXT NOT NULL DEFAULT ''," \
"parse_error TEXT NOT NULL DEFAULT '',state TEXT NOT NULL CHECK(state IN('new','reviewing','converted','linked','discarded','quarantine'))," \
"quarantine_reason TEXT NOT NULL DEFAULT '',job_id INTEGER CHECK(job_id IS NULL OR job_id>0)," \
"claimed_by INTEGER REFERENCES users(id),ticket_id INTEGER REFERENCES tickets(id),revision INTEGER NOT NULL DEFAULT 1," \
"created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),UNIQUE(mailbox_id,raw_sha256));" \
"CREATE INDEX IF NOT EXISTS raw_messages_queue_idx ON raw_messages(state,received_at DESC,id DESC);" \
"CREATE TABLE IF NOT EXISTS raw_attachments(" \
"id INTEGER PRIMARY KEY AUTOINCREMENT,raw_message_id INTEGER NOT NULL REFERENCES raw_messages(id),filename TEXT NOT NULL," \
"content_type TEXT NOT NULL DEFAULT 'application/octet-stream',size INTEGER NOT NULL CHECK(size BETWEEN 0 AND 5242880)," \
"sha256 TEXT NOT NULL,content BLOB NOT NULL CHECK(length(content)=size));" \
"CREATE INDEX IF NOT EXISTS raw_attachments_message_idx ON raw_attachments(raw_message_id,id);" \
"CREATE TABLE IF NOT EXISTS raw_receipts(" \
"mailbox_id INTEGER NOT NULL REFERENCES raw_mailboxes(id),source_key TEXT NOT NULL,uidvalidity INTEGER NOT NULL,uid INTEGER NOT NULL," \
"raw_message_id INTEGER NOT NULL REFERENCES raw_messages(id),received_at INTEGER NOT NULL,PRIMARY KEY(mailbox_id,source_key,uidvalidity,uid));"
#define RAW_SCHEMA_SQL_LINKS \
"CREATE TABLE IF NOT EXISTS ticket_raw_links(" \
"raw_message_id INTEGER PRIMARY KEY REFERENCES raw_messages(id),ticket_id INTEGER NOT NULL REFERENCES tickets(id)," \
"actor_id INTEGER NOT NULL REFERENCES users(id),action TEXT NOT NULL CHECK(action IN('converted','linked'))," \
"created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));" \
"CREATE INDEX IF NOT EXISTS ticket_raw_links_ticket_idx ON ticket_raw_links(ticket_id,raw_message_id);" \
"CREATE TABLE IF NOT EXISTS ticket_raw_attachments(" \
"ticket_id INTEGER NOT NULL REFERENCES tickets(id),raw_attachment_id INTEGER PRIMARY KEY REFERENCES raw_attachments(id));" \
"CREATE TABLE IF NOT EXISTS raw_history(" \
"id INTEGER PRIMARY KEY AUTOINCREMENT,raw_message_id INTEGER NOT NULL REFERENCES raw_messages(id),actor_id INTEGER NOT NULL REFERENCES users(id)," \
"actor_name TEXT NOT NULL,action TEXT NOT NULL,detail TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));"
#endif
