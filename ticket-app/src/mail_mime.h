#ifndef TICKETS_MAIL_MIME_H
#define TICKETS_MAIL_MIME_H
#include <stdbool.h>
#include <stddef.h>
#define RAW_MAIL_MAX (10U * 1024U * 1024U)
#define RAW_ATTACHMENT_MAX (5U * 1024U * 1024U)
#define RAW_ATTACHMENT_COUNT 20
typedef struct {
    char *filename, *content_type;
    unsigned char *content;
    size_t size;
    char sha256[65];
} RawAttachment;
typedef struct {
    char *from_email, *from_name, *subject, *message_id, *in_reply_to;
    char *references_json, *headers_json, *body_text, *text_preview;
    char auth_status[16], authserv_id[256], auth_reason[96], parse_error[96];
    RawAttachment attachments[RAW_ATTACHMENT_COUNT];
    size_t attachment_count;
} RawMail;
void raw_sha256(const void *data, size_t length, char output[65]);
bool raw_mime_parse(const void *data, size_t length, bool imap_source, bool trust_auth_results,
                    const char *authserv_id, RawMail *mail);
void raw_mime_free(RawMail *mail);
#endif
