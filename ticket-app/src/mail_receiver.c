#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "raw_store.h"
#include <curl/curl.h>
#include <gmime/gmime.h>
#include <openssl/crypto.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct { unsigned char *data; size_t length, limit; bool exceeded; } Buffer;
static volatile sig_atomic_t stopping;
static void stop(int signal_number) { (void)signal_number; stopping = 1; }
static const char *environment(const char *name, const char *fallback) { const char *v = getenv(name); return v && *v ? v : fallback; }
static size_t receive(void *data, size_t size, size_t count, void *context) {
    Buffer *b = context;
    if ((size && count > SIZE_MAX / size) || size * count > b->limit - b->length) { b->exceeded = true; return 0; }
    size_t length = size * count; unsigned char *grown = realloc(b->data, b->length + length + 1);
    if (!grown) return 0;
    b->data = grown; memcpy(grown + b->length, data, length); b->length += length; grown[b->length] = 0; return length;
}
static void clear(Buffer *b) { free(b->data); b->data = NULL; b->length = 0; b->exceeded = false; }
static bool secret_name(const char *name) {
    if (!*name || strlen(name) > 100 || name[0] == '.' || strstr(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p >= 128 || (!isalnum(*p) && !strchr("_-.", *p))) return false;
    return true;
}
static char *read_secret(const char *directory, const char *name) {
    if (!secret_name(name)) return NULL;
    int parent = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (parent < 0) return NULL;
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW); close(parent);
    if (fd < 0) return NULL;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || (info.st_mode & 077) ||
        (info.st_uid != geteuid() && info.st_uid != 0) || info.st_size < 1 || info.st_size > 8192) { close(fd); return NULL; }
    char *value = malloc((size_t)info.st_size + 1);
    if (!value) { close(fd); return NULL; }
    ssize_t got = read(fd, value, (size_t)info.st_size); close(fd);
    if (got != info.st_size || memchr(value, 0, (size_t)got)) { free(value); return NULL; }
    while (got && (value[got - 1] == '\r' || value[got - 1] == '\n')) got--;
    value[got] = 0;
    if (!got || strchr(value, '\r') || strchr(value, '\n')) { OPENSSL_cleanse(value, (size_t)info.st_size); free(value); return NULL; }
    return value;
}
static bool host_valid(const char *host) {
    if (!*host || strlen(host) > 253) return false;
    unsigned char address[16];
    if (inet_pton(AF_INET, host, address) == 1 || inet_pton(AF_INET6, host, address) == 1) return true;
    if (host[0] == '.' || host[0] == '-' || host[strlen(host) - 1] == '-' || host[strlen(host) - 1] == '.' || strstr(host, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)host; *p; p++) if (*p >= 128 || (!isalnum(*p) && *p != '.' && *p != '-')) return false;
    return true;
}
static bool mailbox_valid(const RawMailbox *m) {
    if (!m->active || !host_valid(m->host) || m->port < 1 || m->port > 65535 || !*m->username || !*m->folder || !secret_name(m->secret_name) ||
        (strcmp(m->auth_mode, "password") && strcmp(m->auth_mode, "xoauth2"))) return false;
    for (const unsigned char *p = (const unsigned char *)m->username; *p; p++) if (*p < 32 || *p == 127) return false;
    /* libcurl escapes the URL and quotes the decoded mailbox before sending it. */
    for (const unsigned char *p = (const unsigned char *)m->folder; *p; p++) if (*p < 32 || *p == 127) return false;
    return true;
}
static bool proxy_valid(const char *value) {
    if (!*value) return true;
    CURLU *url = curl_url(); char *scheme = NULL, *host = NULL, *user = NULL, *path = NULL, *query = NULL, *fragment = NULL;
    bool ok = url && curl_url_set(url, CURLUPART_URL, value, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK && !strcmp(scheme, "http") &&
        curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK && host && *host &&
        curl_url_get(url, CURLUPART_USER, &user, 0) != CURLUE_OK &&
        curl_url_get(url, CURLUPART_QUERY, &query, 0) != CURLUE_OK && curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) != CURLUE_OK;
    if (ok && curl_url_get(url, CURLUPART_PATH, &path, 0) == CURLUE_OK && strcmp(path, "/")) ok = false;
    curl_free(scheme); curl_free(host); curl_free(user); curl_free(path); curl_free(query); curl_free(fragment); if (url) curl_url_cleanup(url); return ok;
}
static CURL *client(const RawMailbox *m, const char *secret, Buffer *body, Buffer *headers, const char *proxy) {
    CURL *curl = curl_easy_init(); if (!curl) return NULL;
#define SET(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) goto fail; } while (0)
    SET(CURLOPT_USERNAME, m->username);
    if (!strcmp(m->auth_mode, "xoauth2")) { SET(CURLOPT_XOAUTH2_BEARER, secret); SET(CURLOPT_LOGIN_OPTIONS, "AUTH=XOAUTH2"); }
    else { SET(CURLOPT_PASSWORD, secret); }
    SET(CURLOPT_SSL_VERIFYPEER, 1L); SET(CURLOPT_SSL_VERIFYHOST, 2L); SET(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    const char *ca = getenv("TICKETS_IMAP_CA_FILE"); if (ca && *ca) SET(CURLOPT_CAINFO, ca);
    SET(CURLOPT_USE_SSL, (long)CURLUSESSL_ALL); SET(CURLOPT_FOLLOWLOCATION, 0L); SET(CURLOPT_MAXREDIRS, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "imaps"); SET(CURLOPT_REDIR_PROTOCOLS_STR, "imaps");
#else
    SET(CURLOPT_PROTOCOLS, CURLPROTO_IMAPS); SET(CURLOPT_REDIR_PROTOCOLS, CURLPROTO_IMAPS);
#endif
    SET(CURLOPT_PROXY, proxy); SET(CURLOPT_NOPROXY, *proxy ? "" : "*"); SET(CURLOPT_HTTPPROXYTUNNEL, *proxy ? 1L : 0L);
    SET(CURLOPT_CONNECTTIMEOUT, 5L); SET(CURLOPT_TIMEOUT, 20L); SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive); SET(CURLOPT_WRITEDATA, body); SET(CURLOPT_HEADERFUNCTION, receive); SET(CURLOPT_HEADERDATA, headers);
#undef SET
    return curl;
fail:
    curl_easy_cleanup(curl); return NULL;
}
typedef struct {
    CURL *curl;
    unsigned tag;
    unsigned char input[16384];
    size_t position, length;
    time_t deadline;
} Connection;
static time_t monotonic_seconds(void) { struct timespec value; clock_gettime(CLOCK_MONOTONIC, &value); return value.tv_sec; }
static bool wait_socket(Connection *c, short events) {
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(c->curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK || socket == CURL_SOCKET_BAD) return false;
    while (!stopping && monotonic_seconds() < c->deadline) {
        struct pollfd fd = {socket, events, 0}; int rc = poll(&fd, 1, 250);
        if (rc > 0) return !(fd.revents & POLLNVAL);
        if (rc < 0 && errno != EINTR) return false;
    }
    return false;
}
static bool send_bytes(Connection *c, const char *data, size_t length) {
    while (length && !stopping && monotonic_seconds() < c->deadline) {
        size_t sent = 0; CURLcode rc = curl_easy_send(c->curl, data, length, &sent);
        if (rc == CURLE_AGAIN) { if (!wait_socket(c, POLLOUT)) return false; continue; }
        if (rc != CURLE_OK || !sent) return false;
        data += sent; length -= sent;
    }
    return length == 0;
}
static bool fill(Connection *c) {
    if (c->position < c->length) return true;
    while (!stopping && monotonic_seconds() < c->deadline) {
        size_t size = 0; CURLcode rc = curl_easy_recv(c->curl, c->input, sizeof(c->input), &size);
        if (rc == CURLE_AGAIN) { if (!wait_socket(c, POLLIN)) return false; continue; }
        if (rc != CURLE_OK || !size) return false;
        c->position = 0; c->length = size; return true;
    }
    return false;
}
static bool read_line(Connection *c, char line[65536]) {
    size_t n = 0;
    while (n < 65535 && fill(c)) {
        unsigned char byte = c->input[c->position++];
        if (!byte) return false;
        line[n++] = (char)byte;
        if (byte == '\n') { line[n] = 0; return n >= 2 && line[n - 2] == '\r'; }
    }
    return false;
}
static bool fetch_metadata(const char *line, int *depth, bool *uid_seen, unsigned expected) {
    bool quoted = false, escape = false;
    for (const char *p = line; *p; p++) {
        if (escape) { escape = false; continue; }
        if (quoted && *p == '\\') { escape = true; continue; }
        if (*p == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (*p == '(') { if (++*depth > 16) return false; }
        else if (*p == ')') { if (--*depth < 0) return false; }
        else if ((p == line || isspace((unsigned char)p[-1]) || p[-1] == '(') && !strncasecmp(p, "UID", 3) && isspace((unsigned char)p[3])) {
            p += 3; while (isspace((unsigned char)*p)) p++;
            char *end = NULL; errno = 0; unsigned long uid = strtoul(p, &end, 10);
            if (errno || end == p || uid != expected || *uid_seen || (*end && !isspace((unsigned char)*end) && *end != ')')) return false;
            *uid_seen = true; p = end - 1;
        }
    }
    return !quoted && !escape;
}
/* libcurl owns TLS, proxy negotiation and IMAP authentication. Parsing literals
 * explicitly avoids libcurl versions that return only a custom FETCH's first line. */
static CURLcode request(Connection *c, const char *url, const char *command, Buffer *body, Buffer *headers) {
    clear(body); clear(headers); c->deadline = monotonic_seconds() + 20;
    if (!c->tag) {
        CURLcode rc = curl_easy_setopt(c->curl, CURLOPT_URL, url);
        if (rc == CURLE_OK) rc = curl_easy_setopt(c->curl, CURLOPT_CONNECT_ONLY, 1L);
        if (rc == CURLE_OK) rc = curl_easy_perform(c->curl);
        if (rc != CURLE_OK) return rc;
        c->tag = 1000;
    }
    char tag[32], outgoing[2300]; snprintf(tag, sizeof(tag), "T%u", ++c->tag);
    int n = snprintf(outgoing, sizeof(outgoing), "%s %s\r\n", tag, command);
    if (n < 0 || (size_t)n >= sizeof(outgoing) || !send_bytes(c, outgoing, (size_t)n)) return CURLE_SEND_ERROR;
    bool fetch = !strncasecmp(command, "UID FETCH ", 10), literal_seen = false, uid_seen = false;
    int fetch_depth = 0;
    unsigned requested = 0;
    if (fetch && sscanf(command, "UID FETCH %u", &requested) != 1) return CURLE_BAD_FUNCTION_ARGUMENT;
    char line[65536];
    for (unsigned lines = 0; lines < 2000 && read_line(c, line); lines++) {
        if (!strncmp(line, tag, strlen(tag)) && line[strlen(tag)] == ' ')
            return !strncasecmp(line + strlen(tag) + 1, "OK ", 3) ? (fetch && (!literal_seen || !uid_seen || fetch_depth) ? CURLE_REMOTE_FILE_NOT_FOUND : CURLE_OK) : CURLE_REMOTE_ACCESS_DENIED;
        if (!strncasecmp(line, "* BYE", 5)) return CURLE_RECV_ERROR;
        if (fetch) {
            char *brace = strrchr(line, '{');
            if (brace) {
                char *end = NULL; errno = 0; unsigned long long size = strtoull(brace + 1, &end, 10);
                unsigned sequence = 0;
                if (errno || end == brace + 1 || strcmp(end, "}\r\n") || literal_seen ||
                    sscanf(line, "* %u FETCH (", &sequence) != 1 || !strstr(line, " FETCH (") || !strstr(line, "BODY[]") ||
                    !fetch_metadata(line, &fetch_depth, &uid_seen, requested) || fetch_depth != 1) return CURLE_WEIRD_SERVER_REPLY;
                if (size > body->limit) { body->exceeded = true; return CURLE_WRITE_ERROR; }
                literal_seen = true;
                while (size) {
                    if (!fill(c)) return CURLE_PARTIAL_FILE;
                    size_t part = c->length - c->position; if (part > size) part = (size_t)size;
                    if (receive(c->input + c->position, 1, part, body) != part) return CURLE_WRITE_ERROR;
                    c->position += part; size -= part;
                }
            } else if (literal_seen && fetch_depth && !fetch_metadata(line, &fetch_depth, &uid_seen, requested)) return CURLE_WEIRD_SERVER_REPLY;
        } else if (receive(line, 1, strlen(line), body) != strlen(line)) return CURLE_WRITE_ERROR;
    }
    return CURLE_RECV_ERROR;
}
static bool number(const char *p, uint32_t *value, const char **end) {
    if (!isdigit((unsigned char)*p)) return false;
    uint64_t n = 0;
    while (isdigit((unsigned char)*p)) { n = n * 10 + (unsigned)(*p++ - '0'); if (n > UINT32_MAX) return false; }
    if (!n) return false;
    *value = (uint32_t)n; if (end) *end = p; return true;
}
static bool status_value(const Buffer *body, const char *name, uint32_t *value) {
    const char *p = (const char *)body->data; if (!p) return false;
    size_t length = strlen(name); unsigned found = 0;
    while (*p) {
        if ((p == (const char *)body->data || !isalnum((unsigned char)p[-1])) && !strncasecmp(p, name, length) && isspace((unsigned char)p[length])) {
            p += length; while (isspace((unsigned char)*p)) p++;
            const char *end; if (!number(p, value, &end) || (*end && !isspace((unsigned char)*end) && *end != ')' && *end != ']')) return false;
            found++; p = end;
        } else p++;
    }
    return found == 1;
}
static int uid_compare(const void *left, const void *right) { uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right; return a < b ? -1 : a > b; }
static bool search_uids(const Buffer *body, uint32_t first, uint32_t last, uint32_t uids[1000], size_t *count) {
    *count = 0; const char *line = (const char *)body->data; unsigned searches = 0;
    if (!line) return false;
    while (*line) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end) return false;
        if (line_end - line >= 8 && !strncasecmp(line, "* SEARCH", 8) &&
            (line_end == line + 8 || isspace((unsigned char)line[8]))) {
            if (++searches != 1) return false;
            const char *p = line + 8;
            while (p < line_end) {
                while (p < line_end && isspace((unsigned char)*p)) p++;
                if (p == line_end) break;
                uint32_t uid; const char *end;
                if (*count == 1000 || !number(p, &uid, &end) || end > line_end ||
                    (end < line_end && !isspace((unsigned char)*end)) || uid < first || uid > last) return false;
                uids[(*count)++] = uid; p = end;
            }
        }
        line = line_end + 2;
    }
    if (searches != 1) return false;
    qsort(uids, *count, sizeof(*uids), uid_compare);
    for (size_t i = 1; i < *count; i++) if (uids[i] == uids[i - 1]) return false;
    return true;
}
static bool poll_mailbox(RawStore *store, const RawMailbox *m, const char *directory, const char *proxy) {
    char origin[320], source_input[4096], source_key[65], reported_error[128];
    snprintf(origin, sizeof(origin), strchr(m->host, ':') ? "imaps://[%s]:%d" : "imaps://%s:%d", m->host, m->port);
    snprintf(source_input, sizeof(source_input), "%zu:%s\n%zu:%s\n%zu:%s", strlen(origin), origin, strlen(m->username), m->username, strlen(m->folder), m->folder);
    raw_sha256(source_input, strlen(source_input), source_key);
    uint32_t validity = 0, last_uid = 0;
    if (!raw_store_checkpoint(store, m->id, source_key, &validity, &last_uid)) return false;
    const char *error = NULL; char *secret = NULL, *folder = NULL; CURL *curl = NULL;
    Buffer body = {NULL,0,65536,false}, headers = {NULL,0,65536,false};
    if (!mailbox_valid(m)) { error = "mailbox_configuration_incomplete_or_invalid"; goto done; }
    secret = read_secret(directory, m->secret_name);
    if (!secret) { error = "mailbox_secret_missing_or_unsafe"; goto done; }
    curl = client(m, secret, &body, &headers, proxy);
    if (!curl) { error = "imap_client_configuration_failed"; goto done; }
    folder = curl_easy_escape(curl, m->folder, 0);
    if (!folder) { error = "allocation_failed"; goto done; }
    char url[4096], command[2048];
    snprintf(url, sizeof(url), "%s/", origin);
    /* Mailbox quoting is independently constrained; URL encoding handles all path delimiters. */
    char quoted[1610]; size_t n = 0; quoted[n++] = '"';
    for (const char *p = m->folder; *p; p++) { if (*p == '"' || *p == '\\') quoted[n++] = '\\'; quoted[n++] = *p; }
    quoted[n++] = '"'; quoted[n] = 0;
    snprintf(command, sizeof(command), "STATUS %s (UIDVALIDITY UIDNEXT)", quoted);
    Connection connection = {curl,0,{0},0,0,0};
    if (request(&connection, url, command, &body, &headers) != CURLE_OK) { error = "imap_status_failed"; goto done; }
    uint32_t current_validity, next_uid;
    if (!status_value(&body, "UIDVALIDITY", &current_validity) || !status_value(&body, "UIDNEXT", &next_uid)) { error = "imap_status_malformed"; goto done; }
    if (current_validity != validity) { validity = current_validity; last_uid = 0; }
    snprintf(url, sizeof(url), "%s/%s;UIDVALIDITY=%u", origin, folder, validity);
    snprintf(command, sizeof(command), "EXAMINE %s", quoted);
    if (request(&connection, url, command, &body, &headers) != CURLE_OK || !status_value(&body, "UIDVALIDITY", &current_validity) || current_validity != validity) {
        error = "imap_uidvalidity_changed_retry"; goto done;
    }
    unsigned processed = 0;
    for (unsigned window = 0; !stopping && window < 64 && last_uid < next_uid - 1 && processed < 100; window++) {
        uint32_t first = last_uid + 1, last = next_uid - 1;
        if (last - first >= 999) last = first + 999;
        snprintf(command, sizeof(command), "UID SEARCH UID %u:%u", first, last);
        body.limit = 65536;
        if (request(&connection, url, command, &body, &headers) != CURLE_OK) { error = "imap_search_failed"; goto done; }
        uint32_t uids[1000]; size_t count;
        if (!search_uids(&body, first, last, uids, &count)) { error = "imap_search_malformed"; goto done; }
        for (size_t i = 0; i < count && !stopping && processed < 100; i++) {
            snprintf(command, sizeof(command), "UID FETCH %u (BODY.PEEK[])", uids[i]); body.limit = RAW_MAIL_MAX;
            CURLcode rc = request(&connection, url, command, &body, &headers);
            if (rc != CURLE_OK || !body.length) {
                snprintf(reported_error, sizeof(reported_error), "%s_uid_%u", body.exceeded ? "imap_message_exceeds_10mb_limit" : "imap_fetch_failed", uids[i]);
                error = reported_error; goto done;
            }
            RawMail mail;
            if (!raw_mime_parse(body.data, body.length, true, m->trust_auth_results, m->authserv_id, &mail)) { error = "raw_parse_failed"; goto done; }
            sqlite3_int64 id; bool replay;
            bool saved = raw_store_ingest(store, m, source_key, validity, uids[i], "imap", body.data, body.length, &mail, &id, &replay);
            raw_mime_free(&mail);
            if (!saved) { error = store->error; goto done; }
            last_uid = uids[i]; processed++;
            if (!raw_store_progress(store, m->id, source_key, validity, last_uid, "")) { error = store->error; goto done; }
            printf("mailbox=%lld raw_message=%lld replayed=%d\n", (long long)m->id, (long long)id, replay ? 1 : 0);
        }
        if (processed < 100 && !stopping) last_uid = last;
    }
done:
    if (curl) curl_easy_cleanup(curl);
    curl_free(folder);
    if (secret) { OPENSSL_cleanse(secret, strlen(secret)); free(secret); }
    clear(&body); clear(&headers);
    char safe_error[128]; snprintf(safe_error, sizeof(safe_error), "%s", error ? error : "");
    bool stored = raw_store_progress(store, m->id, source_key, validity, last_uid, safe_error);
    if (error) fprintf(stderr, "mailbox=%lld error=%s uid=%u\n", (long long)m->id, safe_error, last_uid + (last_uid != UINT32_MAX));
    return !error && stored;
}
static bool import_file(RawStore *store, const RawMailbox *m, const char *path) {
    FILE *file = fopen(path, "rb"); if (!file) return false;
    Buffer body = {NULL,0,RAW_MAIL_MAX,false}; unsigned char block[16384]; size_t size; bool ok = true;
    while ((size = fread(block, 1, sizeof(block), file))) if (receive(block, 1, size, &body) != size) { ok = false; break; }
    if (ferror(file)) ok = false;
    fclose(file);
    if (!ok || !body.length) { clear(&body); return false; }
    RawMail mail;
    ok = raw_mime_parse(body.data, body.length, false, false, "", &mail);
    if (ok) { sqlite3_int64 id; bool replay;
        ok = raw_store_ingest(store, m, "local-import", 0, 0, "import", body.data, body.length, &mail, &id, &replay);
        if (ok) printf("raw_message=%lld source=import replayed=%d\n", (long long)id, replay ? 1 : 0);
        raw_mime_free(&mail);
    }
    clear(&body); return ok;
}
int main(int argc, char **argv) {
    bool once = false; const char *import = NULL; sqlite3_int64 only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = true;
        else if (!strcmp(argv[i], "--import-file") && i + 1 < argc) { import = argv[++i]; once = true; }
        else if (!strcmp(argv[i], "--mailbox-id") && i + 1 < argc) { const char *p = argv[++i]; char *end = NULL; errno = 0; long long n = strtoll(p, &end, 10); if (errno || !*p || *end || n <= 0) return 2; only = n; }
        else { fprintf(stderr, "Usage: punto-ticket-receiver [--once] [--mailbox-id ID] [--import-file PATH]\n"); return 2; }
    }
    if (import && !only) { fprintf(stderr, "Import requires --mailbox-id.\n"); return 2; }
    const char *proxy = environment("TICKETS_IMAP_PROXY_URL", "");
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 2;
    if (!proxy_valid(proxy)) { fprintf(stderr, "Invalid TICKETS_IMAP_PROXY_URL.\n"); curl_global_cleanup(); return 2; }
    g_mime_init(); signal(SIGTERM, stop); signal(SIGINT, stop);
    RawStore store;
    if (!raw_store_open(&store, environment("TICKETS_DB_PATH", "data/tickets.db"))) { fprintf(stderr, "%s\n", store.error); raw_store_close(&store); g_mime_shutdown(); curl_global_cleanup(); return 2; }
    char *end = NULL; long seconds = strtol(environment("TICKETS_IMAP_POLL_SECONDS", "30"), &end, 10);
    if (!end || *end || seconds < 5 || seconds > 3600) { fprintf(stderr, "Invalid polling interval.\n"); raw_store_close(&store); g_mime_shutdown(); curl_global_cleanup(); return 2; }
    int result = 0;
    do {
        RawMailbox *items = NULL; size_t count = 0;
        if (!raw_store_mailboxes(&store, only, &items, &count) || (only && count != 1)) { fprintf(stderr, "mailbox_lookup_failed\n"); result = 1; free(items); break; }
        for (size_t i = 0; i < count && !stopping; i++) {
            bool ok = import ? import_file(&store, &items[i], import) : poll_mailbox(&store, &items[i], environment("TICKETS_IMAP_SECRET_DIR", "/run/secrets/ticket-imap"), proxy);
            if (!ok) result = 1;
        }
        free(items);
        if (!once) { result = 0; struct timespec delay = {seconds,0}; while (!stopping && nanosleep(&delay, &delay) && errno == EINTR) {} }
    } while (!once && !stopping);
    raw_store_close(&store); g_mime_shutdown(); curl_global_cleanup(); return result;
}
