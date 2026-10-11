#include "mail_client.h"

#include <curl/curl.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RESPONSE_LIMIT 8192U

static char api_url[256];
static char namespace_value[65];
static char authorization[160];
static char *ca_file;
static bool initialized, enabled, smtp_configured;
static time_t last_refresh;

static void erase(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool identifier(const char *text, size_t minimum, size_t maximum) {
    size_t length = text ? strlen(text) : 0;
    if (length < minimum || length > maximum) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 128 || (!isalnum(c) && c != '_' && c != '-')) return false;
    }
    return true;
}

static bool safe_url(const char *url) {
    if (!url) return false;
    if (strcmp(url, "https://mail-api:8090") == 0 ||
        strcmp(url, "https://mail-addon:8090") == 0) return true;
    /* Loopback HTTPS supports isolated TLS tests without accepting arbitrary
       destinations, paths, userinfo, queries, or credentials. */
    const char *port = NULL;
    if (strncmp(url, "https://127.0.0.1:", 18) == 0) port = url + 18;
    else if (strncmp(url, "https://localhost:", 18) == 0) port = url + 18;
    if (!port || !*port || strlen(port) > 5) return false;
    unsigned value = 0;
    for (; *port; port++) {
        if (*port < '0' || *port > '9') return false;
        value = value * 10 + (unsigned)(*port - '0');
    }
    return value > 0 && value <= 65535;
}

static int open_regular(const char *path) {
    if (!path || !*path) return -1;
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int file = open(path, flags);
    if (file < 0) return -1;
    struct stat status;
    if (fstat(file, &status) != 0 || !S_ISREG(status.st_mode) ||
        (status.st_mode & (S_IWGRP | S_IWOTH))) {
        close(file);
        return -1;
    }
    return file;
}

bool mail_client_open(void) {
    mail_client_close();
    const char *url = getenv("MAIL_API_URL");
    const char *token_file = getenv("MAIL_API_TOKEN_FILE");
    const char *ca_path = getenv("MAIL_API_CA_FILE");
    const char *name = getenv("MAIL_API_NAMESPACE");
    if ((!url || !*url) && (!token_file || !*token_file) &&
        (!ca_path || !*ca_path) && (!name || !*name)) return true;
    if (!safe_url(url) || !identifier(name, 1, 64)) return false;
    int ca = open_regular(ca_path);
    if (ca < 0) return false;
    close(ca);
    int file = open_regular(token_file);
    if (file < 0) return false;
    char token[130] = {0};
    ssize_t count = read(file, token, sizeof(token) - 1);
    char extra;
    bool complete = count > 0 && read(file, &extra, 1) == 0;
    close(file);
    if (count > 0 && token[count - 1] == '\n') token[--count] = '\0';
    bool valid = complete && count > 0 && identifier(token, 32, 128) &&
                 (size_t)count == strlen(token);
    if (!valid) { erase(token, sizeof(token)); return false; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        erase(token, sizeof(token));
        return false;
    }
    initialized = true;
    ca_file = strdup(ca_path);
    if (!ca_file) { erase(token, sizeof(token)); mail_client_close(); return false; }
    (void)snprintf(api_url, sizeof(api_url), "%s", url);
    (void)snprintf(namespace_value, sizeof(namespace_value), "%s", name);
    (void)snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", token);
    erase(token, sizeof(token));
    enabled = true;
    return true;
}

void mail_client_close(void) {
    if (initialized) curl_global_cleanup();
    initialized = enabled = smtp_configured = false;
    last_refresh = 0;
    free(ca_file);
    ca_file = NULL;
    erase(authorization, sizeof(authorization));
    api_url[0] = namespace_value[0] = '\0';
}

bool mail_client_enabled(void) { return enabled; }
bool mail_client_smtp_configured(void) { return smtp_configured; }
const char *mail_client_namespace(void) { return namespace_value; }

typedef struct { char data[RESPONSE_LIMIT + 1]; size_t size; } Response;

static size_t receive(void *data, size_t size, size_t count, void *context) {
    Response *response = context;
    if (size && count > RESPONSE_LIMIT / size) return 0;
    size_t length = size * count;
    if (length > RESPONSE_LIMIT - response->size || memchr(data, '\0', length)) return 0;
    memcpy(response->data + response->size, data, length);
    response->size += length;
    response->data[response->size] = '\0';
    return length;
}

static CURLcode request(const char *path, const char *form, Response *response,
                        long *status) {
    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_OUT_OF_MEMORY;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, authorization);
    if (!headers) { curl_easy_cleanup(curl); return CURLE_OUT_OF_MEMORY; }
    struct curl_slist *list = curl_slist_append(headers, "Accept: application/json");
    if (!list) { curl_slist_free_all(headers); curl_easy_cleanup(curl); return CURLE_OUT_OF_MEMORY; }
    headers = list;
    char url[288];
    (void)snprintf(url, sizeof(url), "%s%s", api_url, path);
    CURLcode result = CURLE_OK;
#define SET(option, value) do { if (result == CURLE_OK) result = curl_easy_setopt(curl, option, value); } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NOPROXY, "*");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_MAXREDIRS, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    SET(CURLOPT_CAINFO, ca_file);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "https");
    SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    SET(CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    SET(CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    SET(CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    SET(CURLOPT_TIMEOUT_MS, 3000L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, response);
    if (form) {
        SET(CURLOPT_POST, 1L);
        SET(CURLOPT_POSTFIELDS, form);
        SET(CURLOPT_POSTFIELDSIZE, (long)strlen(form));
    }
    if (result == CURLE_OK) result = curl_easy_perform(curl);
    if (result == CURLE_OK) result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
#undef SET
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

static void whitespace(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') (*p)++;
}

/* Strict flat JSON is the add-on's contract. Unknown scalar fields are ignored;
   strings with escapes are not accepted as status, boolean, or field names. */
static bool json_string(const char **p, const char **start, size_t *length) {
    if (*(*p)++ != '"') return false;
    *start = *p;
    while (**p && **p != '"') {
        if ((unsigned char)**p < 32 || **p == '\\') return false;
        (*p)++;
    }
    if (**p != '"') return false;
    *length = (size_t)(*p - *start);
    (*p)++;
    return true;
}

static bool field(const char *json, const char *wanted, char *out, size_t capacity,
                  bool *quoted) {
    const char *p = json;
    bool found = false;
    whitespace(&p);
    if (*p++ != '{') return false;
    whitespace(&p);
    if (*p == '}') return false;
    while (*p) {
        const char *name, *value;
        size_t name_length, value_length;
        if (!json_string(&p, &name, &name_length)) return false;
        whitespace(&p);
        if (*p++ != ':') return false;
        whitespace(&p);
        bool string_value = *p == '"';
        if (string_value) {
            if (!json_string(&p, &value, &value_length)) return false;
        } else {
            value = p;
            while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) p++;
            value_length = (size_t)(p - value);
            if (!((value_length == 4 && memcmp(value, "true", 4) == 0) ||
                  (value_length == 5 && memcmp(value, "false", 5) == 0) ||
                  (value_length == 4 && memcmp(value, "null", 4) == 0))) return false;
        }
        if (name_length == strlen(wanted) && memcmp(name, wanted, name_length) == 0) {
            if (found || value_length >= capacity) return false;
            memcpy(out, value, value_length);
            out[value_length] = '\0';
            if (quoted) *quoted = string_value;
            found = true;
        }
        whitespace(&p);
        if (*p == '}') { p++; whitespace(&p); return found && !*p; }
        if (*p++ != ',') return false;
        whitespace(&p);
    }
    return false;
}

void mail_client_refresh(time_t now) {
    if (!enabled || now == (time_t)-1 ||
        (last_refresh && now >= last_refresh && difftime(now, last_refresh) < 60.0)) return;
    last_refresh = now;
    Response response = {0};
    long status = 0;
    char configured[16];
    bool quoted = false;
    smtp_configured = request("/v1/status", NULL, &response, &status) == CURLE_OK &&
        status == 200 && field(response.data, "smtp_configured", configured, sizeof(configured), &quoted) && !quoted &&
        strcmp(configured, "true") == 0;
}

MailDelivery mail_client_send(const char *key, const char *to,
                              const char *subject, const char *body) {
    return mail_client_send_threaded(key, to, subject, body, "", "", "");
}

MailDelivery mail_client_send_threaded(const char *key, const char *to,
                                      const char *subject, const char *body,
                                      const char *message_id, const char *in_reply_to,
                                      const char *references) {
    if (!enabled || !smtp_configured) return MAIL_DELIVERY_UNCONFIGURED;
    if (!key || !to || !subject || !body || strlen(key) > 128 ||
        strlen(to) > 254 || strlen(subject) > 1024 || strlen(body) > 32768 ||
        !message_id || !in_reply_to || !references || strlen(message_id) > 255 ||
        strlen(in_reply_to) > 255 || strlen(references) > 1024 ||
        strpbrk(message_id, "\r\n") || strpbrk(in_reply_to, "\r\n") || strpbrk(references, "\r\n"))
        return MAIL_DELIVERY_FAILED;
    CURL *encoder = curl_easy_init();
    if (!encoder) return MAIL_DELIVERY_RETRY;
    const char *values[] = {key, to, subject, body, message_id, in_reply_to, references};
    const char *names[] = {"idempotency_key", "to", "subject", "body", "message_id", "in_reply_to", "references"};
    size_t count = (*message_id || *in_reply_to || *references) ? 7 : 4;
    char *encoded[7] = {0};
    size_t needed = 1;
    bool okay = true;
    for (size_t i = 0; i < count; i++) {
        encoded[i] = curl_easy_escape(encoder, values[i], 0);
        if (!encoded[i]) { okay = false; break; }
        needed += strlen(names[i]) + strlen(encoded[i]) + 2;
    }
    char *form = okay ? malloc(needed) : NULL;
    if (form) {
        form[0] = '\0';
        for (size_t i = 0; i < count; i++) {
            strcat(form, i ? "&" : ""); strcat(form, names[i]);
            strcat(form, "="); strcat(form, encoded[i]);
        }
    }
    for (size_t i = 0; i < count; i++) curl_free(encoded[i]);
    curl_easy_cleanup(encoder);
    if (!form) return MAIL_DELIVERY_RETRY;
    Response response = {0};
    long status = 0;
    CURLcode result = request("/v1/emails", form, &response, &status);
    erase(form, needed);
    free(form);
    if (result != CURLE_OK) return MAIL_DELIVERY_RETRY;
    char state[32] = "", error[48] = "";
    bool has_state = field(response.data, "status", state, sizeof(state), NULL);
    bool has_error = field(response.data, "error", error, sizeof(error), NULL);
    if (status == 200 && has_state && strcmp(state, "sent") == 0) return MAIL_DELIVERY_SENT;
    if (status == 503 && has_error && strcmp(error, "smtp_not_configured") == 0) {
        smtp_configured = false;
        return MAIL_DELIVERY_UNCONFIGURED;
    }
    if ((status == 409 || status == 503) && has_error && strcmp(error, "delivery_unknown") == 0)
        return MAIL_DELIVERY_UNKNOWN;
    if ((status == 502 && has_error && strcmp(error, "smtp_delivery_failed") == 0) ||
        (status == 409 && has_error && strcmp(error, "idempotency_conflict") == 0))
        return MAIL_DELIVERY_FAILED;
    return MAIL_DELIVERY_RETRY;
}
