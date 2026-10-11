#include "mail_mime.h"
#include <gmime/gmime.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void raw_sha256(const void *data, size_t length, char output[65]) {
    unsigned char bytes[32]; unsigned int count = 0;
    EVP_Digest(data, length, bytes, &count, EVP_sha256(), NULL);
    for (size_t i = 0; i < 32; i++) snprintf(output + i * 2, 3, "%02x", bytes[i]);
}
static char *clean(const char *text, size_t limit) {
    char *utf8 = g_utf8_make_valid(text ? text : "", -1);
    GString *result = g_string_sized_new(128);
    for (const char *p = utf8; *p && result->len < limit;) {
        gunichar c = g_utf8_get_char(p); const char *next = g_utf8_next_char(p);
        if ((c >= 32 || c == '\n' || c == '\t') && c != 127 && c != 0x202e && c != 0x202d &&
            c != 0x2066 && c != 0x2067 && c != 0x2068 && c != 0x2069) {
            if (result->len + (size_t)(next - p) > limit) break;
            g_string_append_len(result, p, next - p);
        }
        p = next;
    }
    g_free(utf8);
    return g_string_free(result, FALSE);
}
static void unsafe(RawMail *mail, const char *reason) {
    if (!mail->parse_error[0]) snprintf(mail->parse_error, sizeof(mail->parse_error), "%s", reason);
}
static void warning(gint64 offset, GMimeParserWarning code, const gchar *item, gpointer context) {
    (void)offset; (void)item;
    if (code != GMIME_WARN_UNENCODED_8BIT_HEADER)
        unsafe(context, "malformed_mime_or_headers");
}
/* This is a text extractor, never an HTML renderer. No URL or resource is opened. */
static char *html_text(RawMail *mail, const char *html) {
    GString *out = g_string_sized_new(256); const char *p = html;
    char suppressed[24] = "";
    while (*p && out->len < 65536) {
        if (*p == '<') {
            const char *start = p++; bool closing = *p == '/'; if (closing) p++;
            char tag[24]; size_t n = 0;
            while (g_ascii_isalnum(*p) && n + 1 < sizeof(tag)) tag[n++] = g_ascii_tolower(*p++);
            tag[n] = 0; char quote = 0;
            while (*p) {
                if (quote) { if (*p == quote) quote = 0; }
                else if (*p == '\'' || *p == '"') quote = *p;
                else if (*p == '>') { p++; break; }
                p++;
            }
            if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "head")) {
                if (closing && !strcmp(suppressed, tag)) suppressed[0] = 0;
                else if (!closing && !suppressed[0]) snprintf(suppressed, sizeof(suppressed), "%s", tag);
            }
            if (!suppressed[0] && (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "br") ||
                                  !strcmp(tag, "li") || !strcmp(tag, "tr"))) g_string_append_c(out, '\n');
            if (p == start) p++;
            continue;
        }
        if (suppressed[0]) { p++; continue; }
        if (*p == '&') {
            const char *end = strchr(p, ';');
            if (end && end - p <= 12) {
                char entity[16]; memcpy(entity, p + 1, (size_t)(end - p - 1)); entity[end - p - 1] = 0;
                gunichar c = 0;
                if (!strcmp(entity, "amp")) c = '&'; else if (!strcmp(entity, "lt")) c = '<';
                else if (!strcmp(entity, "gt")) c = '>'; else if (!strcmp(entity, "quot")) c = '"';
                else if (!strcmp(entity, "apos")) c = '\''; else if (!strcmp(entity, "nbsp")) c = ' ';
                else if (entity[0] == '#') {
                    char *tail = NULL; const char *digits = entity + 1; int base = 10;
                    if (*digits == 'x' || *digits == 'X') { base = 16; digits++; }
                    unsigned long value = strtoul(digits, &tail, base);
                    if (*digits && tail && !*tail && value <= 0x10ffff && g_unichar_validate((gunichar)value)) c = (gunichar)value;
                }
                if (c >= 32 && c != 127) { g_string_append_unichar(out, c); p = end + 1; continue; }
            }
        }
        g_string_append_c(out, *p++);
    }
    if (*p) unsafe(mail, "body_text_truncated");
    char *result = clean(out->str, 65536); g_string_free(out, TRUE); return result;
}
typedef struct { RawMail *mail; GString *plain, *html; size_t bytes; unsigned parts; } Walk;
static void attachment(Walk *walk, const char *name, const char *type, const void *bytes, size_t size) {
    if (walk->mail->attachment_count >= RAW_ATTACHMENT_COUNT || size > RAW_ATTACHMENT_MAX ||
        walk->bytes > RAW_MAIL_MAX - size) { unsafe(walk->mail, "attachment_limits_exceeded"); return; }
    RawAttachment *a = &walk->mail->attachments[walk->mail->attachment_count++];
    a->filename = clean(name && *name ? name : "attachment.bin", 255);
    for (char *p = a->filename; *p; p++) if (*p == '/' || *p == '\\' || *p == '\n' || *p == '\t') *p = '_';
    a->content_type = clean(type ? type : "application/octet-stream", 128);
    a->content = g_malloc(size ? size : 1); if (size) memcpy(a->content, bytes, size); a->size = size;
    raw_sha256(bytes, size, a->sha256); walk->bytes += size;
}
static void walk_part(Walk *walk, GMimeObject *object, unsigned depth) {
    if (!object) return;
    if (depth > 16 || ++walk->parts > 128) { unsafe(walk->mail, "mime_structure_limits_exceeded"); return; }
    if (GMIME_IS_MULTIPART(object)) {
        GMimeMultipart *multipart = GMIME_MULTIPART(object);
        for (int i = 0; i < g_mime_multipart_get_count(multipart); i++)
            walk_part(walk, g_mime_multipart_get_part(multipart, i), depth + 1);
        return;
    }
    if (GMIME_IS_MESSAGE_PART(object)) {
        char *text = g_mime_object_to_string(object, NULL);
        if (text) { attachment(walk, "forwarded-message.eml", "message/rfc822", text, strlen(text)); g_free(text); }
        return;
    }
    if (!GMIME_IS_PART(object)) { unsafe(walk->mail, "unsupported_mime_part"); return; }
    GMimePart *part = GMIME_PART(object); const char *name = g_mime_part_get_filename(part);
    GMimeContentType *content_type = g_mime_object_get_content_type(object);
    if (g_mime_part_is_attachment(part) || (name && *name) || !GMIME_IS_TEXT_PART(part)) {
        GMimeStream *stream = g_mime_stream_mem_new();
        GMimeDataWrapper *wrapper = g_mime_part_get_content(part);
        if (!wrapper || g_mime_data_wrapper_write_to_stream(wrapper, stream) < 0) unsafe(walk->mail, "attachment_decode_failed");
        else {
            GByteArray *data = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(stream));
            char *type = g_mime_content_type_get_mime_type(content_type);
            attachment(walk, name, type, data->data, data->len); g_free(type);
        }
        g_object_unref(stream); return;
    }
    char *text = g_mime_text_part_get_text(GMIME_TEXT_PART(part));
    if (!text) { unsafe(walk->mail, "text_decode_failed"); return; }
    GString *target = g_mime_content_type_is_type(content_type, "text", "plain") ? walk->plain :
                      g_mime_content_type_is_type(content_type, "text", "html") ? walk->html : NULL;
    size_t limit = target == walk->plain ? 65536 : 262144;
    if (target && target->len < limit) {
        if (strlen(text) > limit - target->len) unsafe(walk->mail, "body_text_truncated");
        char *safe = clean(text, limit - target->len); g_string_append(target, safe); g_string_append_c(target, '\n'); g_free(safe);
    } else if (target && *text) {
        unsafe(walk->mail, "body_text_truncated");
    }
    g_free(text);
}
static char *uncomment(const char *text) {
    if (!text || strlen(text) > 8192) return NULL;
    GString *out = g_string_new(""); unsigned depth = 0; bool escape = false, quoted = false;
    for (const char *p = text; *p && out->len < 8192; p++) {
        if (escape) { if (!depth) g_string_append_c(out, *p); escape = false; continue; }
        if (*p == '\\') {
            if (!depth && !quoted) { g_string_free(out, TRUE); return NULL; }
            if (!depth) g_string_append_c(out, *p);
            escape = true; continue;
        }
        if (!depth && *p == '"') { quoted = !quoted; g_string_append_c(out, *p); continue; }
        if (!quoted && *p == '(') { if (++depth > 16) break; continue; }
        if (*p == ')' && depth) { depth--; g_string_append_c(out, ' '); continue; }
        if (!quoted && *p == ')' && !depth) { g_string_free(out, TRUE); return NULL; }
        if (!depth) g_string_append_c(out, g_ascii_tolower(*p));
    }
    if (depth || escape || quoted) { g_string_free(out, TRUE); return NULL; }
    return g_string_free(out, FALSE);
}
static char *token(char **cursor, bool *quoted) {
    *quoted = false;
    while (**cursor && g_ascii_isspace(**cursor)) (*cursor)++;
    if (!**cursor) return NULL;
    char *start = *cursor;
    if (**cursor == '=' || **cursor == ';') { (*cursor)++; return g_strndup(start, 1); }
    if (**cursor == '"') {
        *quoted = true; (*cursor)++; GString *result = g_string_new("");
        while (**cursor && **cursor != '"') {
            if (**cursor == '\\' && (*cursor)[1]) (*cursor)++;
            g_string_append_c(result, **cursor); (*cursor)++;
        }
        if (**cursor) (*cursor)++;
        return g_string_free(result, FALSE);
    }
    while (**cursor && !g_ascii_isspace(**cursor) && **cursor != '=' && **cursor != ';') (*cursor)++;
    return g_strndup(start, (gsize)(*cursor - start));
}
static void authentication(GMimeHeaderList *headers, RawMail *mail, bool imap, bool trust, const char *configured) {
    snprintf(mail->auth_status, sizeof(mail->auth_status), "unverified");
    snprintf(mail->auth_reason, sizeof(mail->auth_reason), "%s", !imap ? "local_import_untrusted" : !trust ? "auth_results_trust_disabled" : "trusted_auth_results_missing");
    if (!imap || !trust || !configured || !*configured) return;
    char *first = NULL; unsigned matching = 0;
    for (int i = 0; i < g_mime_header_list_get_count(headers); i++) {
        GMimeHeader *header = g_mime_header_list_get_header_at(headers, i);
        if (strcasecmp(g_mime_header_get_name(header), "Authentication-Results")) continue;
        char *value = uncomment(g_mime_header_get_value(header));
        if (!value) {
            snprintf(mail->auth_reason, sizeof(mail->auth_reason), "authentication_results_limit_or_syntax");
            unsafe(mail, "authentication_results_limit_or_syntax"); g_free(first); return;
        }
        bool quoted; char *cursor = value, *id = token(&cursor, &quoted);
        if (id && !quoted && !strcasecmp(id, configured)) matching++;
        if (!first) first = g_strdup(value);
        g_free(id); g_free(value);
    }
    if (!first) return;
    bool quoted; char *cursor = first, *id = token(&cursor, &quoted);
    if (!id || quoted || strcasecmp(id, configured) || matching != 1) {
        snprintf(mail->auth_reason, sizeof(mail->auth_reason), "%s", matching > 1 ? "duplicate_trusted_auth_results" : "first_auth_results_not_trusted");
        g_free(id); g_free(first); return;
    }
    snprintf(mail->authserv_id, sizeof(mail->authserv_id), "%s", id); g_free(id);
    const char *domain = strrchr(mail->from_email, '@'); domain = domain ? domain + 1 : "";
    unsigned methods = 0, aligned = 0, domains = 0; bool passed = false, segment_start = false, dmarc_segment = false;
    char *item;
    while ((item = token(&cursor, &quoted))) {
        if (!quoted && !strcmp(item, ";")) { segment_start = true; dmarc_segment = false; }
        else if (!quoted && segment_start && !strcmp(item, "dmarc")) {
            bool equal_quoted, result_quoted;
            char *equal = token(&cursor, &equal_quoted), *result = token(&cursor, &result_quoted); methods++;
            passed = equal && !equal_quoted && !strcmp(equal, "=") && result && !result_quoted && !strcmp(result, "pass");
            dmarc_segment = true; segment_start = false;
            g_free(equal); g_free(result);
        } else if (!quoted && dmarc_segment && !strcmp(item, "header.from")) {
            bool equal_quoted, value_quoted;
            char *equal = token(&cursor, &equal_quoted), *value = token(&cursor, &value_quoted);
            domains++;
            if (equal && !equal_quoted && !strcmp(equal, "=") && value && !strcasecmp(value, domain) && *domain) aligned++;
            g_free(equal); g_free(value);
        } else segment_start = false;
        g_free(item);
    }
    if (methods == 1 && passed && aligned == 1 && domains == 1) {
        snprintf(mail->auth_status, sizeof(mail->auth_status), "pass");
        snprintf(mail->auth_reason, sizeof(mail->auth_reason), "dmarc_domain_pass_allowlist_required");
    } else {
        snprintf(mail->auth_status, sizeof(mail->auth_status), "fail");
        snprintf(mail->auth_reason, sizeof(mail->auth_reason), "dmarc_not_pass_or_from_domain_mismatch");
    }
    g_free(first);
}
bool raw_mime_parse(const void *data, size_t length, bool imap, bool trust, const char *authserv, RawMail *mail) {
    memset(mail, 0, sizeof(*mail));
    if (!data || !length || length > RAW_MAIL_MAX) return false;
    GMimeStream *stream = g_mime_stream_mem_new_with_buffer(data, length);
    GMimeParser *parser = g_mime_parser_new_with_stream(stream); g_mime_parser_set_format(parser, GMIME_FORMAT_MESSAGE);
    GMimeParserOptions *options = g_mime_parser_options_new();
    g_mime_parser_options_set_address_compliance_mode(options, GMIME_RFC_COMPLIANCE_STRICT);
    g_mime_parser_options_set_allow_addresses_without_domain(options, FALSE);
#if GMIME_CHECK_VERSION(3, 2, 15)
    g_mime_parser_options_set_warning_callback(options, warning, mail, NULL);
#else
    g_mime_parser_options_set_warning_callback(options, warning, mail);
#endif
    GMimeMessage *message = g_mime_parser_construct_message(parser, options);
    g_mime_parser_options_free(options); g_object_unref(parser); g_object_unref(stream);
    mail->from_email = g_strdup(""); mail->from_name = g_strdup("");
    mail->subject = clean(message ? g_mime_message_get_subject(message) : "", 1024);
    mail->message_id = clean(message ? g_mime_message_get_message_id(message) : "", 1024);
    mail->in_reply_to = g_strdup(""); mail->references_json = g_strdup("[]"); mail->headers_json = g_strdup("[]");
    mail->body_text = g_strdup(""); mail->text_preview = g_strdup("");
    if (!message) { unsafe(mail, "message_parse_failed"); snprintf(mail->auth_status, sizeof(mail->auth_status), "unverified"); return true; }
    GMimeHeaderList *headers = g_mime_object_get_header_list(GMIME_OBJECT(message)); unsigned from_count = 0;
    json_object *items = json_object_new_array(); size_t header_bytes = 0;
    for (int i = 0; i < g_mime_header_list_get_count(headers); i++) {
        GMimeHeader *header = g_mime_header_list_get_header_at(headers, i); const char *name = g_mime_header_get_name(header);
        if (!strcasecmp(name, "From")) from_count++;
        if (i >= 200 || header_bytes >= 65536) { unsafe(mail, "header_limits_exceeded"); break; }
        char *value = clean(g_mime_header_get_value(header), 4096); header_bytes += strlen(value);
        json_object *item = json_object_new_object(); json_object_object_add(item, "name", json_object_new_string(name));
        json_object_object_add(item, "value", json_object_new_string(value)); json_object_array_add(items, item); g_free(value);
    }
    g_free(mail->headers_json); mail->headers_json = g_strdup(json_object_to_json_string_ext(items, JSON_C_TO_STRING_PLAIN)); json_object_put(items);
    InternetAddressList *from = g_mime_message_get_from(message);
    InternetAddress *address = from && internet_address_list_length(from) == 1 ? internet_address_list_get_address(from, 0) : NULL;
    if (from_count == 1 && address && INTERNET_ADDRESS_IS_MAILBOX(address)) {
        const char *email = internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(address));
        bool valid = email && strlen(email) <= 254 && strchr(email, '@') && !strchr(strchr(email, '@') + 1, '@');
        for (const unsigned char *p = (const unsigned char *)(email ? email : ""); *p; p++) if (*p <= 32 || *p >= 127) valid = false;
        if (valid) { g_free(mail->from_email); mail->from_email = g_ascii_strdown(email, -1); g_free(mail->from_name); mail->from_name = clean(internet_address_get_name(address), 200); }
        else unsafe(mail, "invalid_single_from");
    } else unsafe(mail, "invalid_single_from");
    GMimeReferences *references = g_mime_references_parse(NULL, g_mime_object_get_header(GMIME_OBJECT(message), "References"));
    items = json_object_new_array();
    if (references) { for (int i = 0; i < g_mime_references_length(references) && i < 50; i++) {
        char *value = clean(g_mime_references_get_message_id(references, i), 1024); json_object_array_add(items, json_object_new_string(value)); g_free(value);
    } g_mime_references_free(references); }
    g_free(mail->references_json); mail->references_json = g_strdup(json_object_to_json_string_ext(items, JSON_C_TO_STRING_PLAIN)); json_object_put(items);
    references = g_mime_references_parse(NULL, g_mime_object_get_header(GMIME_OBJECT(message), "In-Reply-To"));
    if (references) { if (g_mime_references_length(references)) { g_free(mail->in_reply_to); mail->in_reply_to = clean(g_mime_references_get_message_id(references, 0), 1024); } g_mime_references_free(references); }
    authentication(headers, mail, imap, trust, authserv);
    Walk walk = {mail, g_string_new(""), g_string_new(""), 0, 0}; walk_part(&walk, g_mime_message_get_mime_part(message), 0);
    g_free(mail->body_text); mail->body_text = walk.plain->len ? clean(walk.plain->str, 65536) : html_text(mail, walk.html->str);
    g_free(mail->text_preview); mail->text_preview = clean(mail->body_text, 4096);
    g_string_free(walk.plain, TRUE); g_string_free(walk.html, TRUE); g_object_unref(message); return true;
}
void raw_mime_free(RawMail *mail) {
    g_free(mail->from_email); g_free(mail->from_name); g_free(mail->subject); g_free(mail->message_id); g_free(mail->in_reply_to);
    g_free(mail->references_json); g_free(mail->headers_json); g_free(mail->body_text); g_free(mail->text_preview);
    for (size_t i = 0; i < mail->attachment_count; i++) { g_free(mail->attachments[i].filename); g_free(mail->attachments[i].content_type); g_free(mail->attachments[i].content); }
    memset(mail, 0, sizeof(*mail));
}
