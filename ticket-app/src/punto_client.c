#include "punto_client.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define JSON_LIMIT (512U * 1024U)
static char origin[512], host[256], resolve_entry[320];
static const char *ca_file;
static bool initialized, configured;
static bool port_number(const char *s, unsigned *out) {
    unsigned n = 0;
    if (!*s) return false;
    for (; *s; s++) { if (*s < '0' || *s > '9' || n > (65535U - (unsigned)(*s - '0')) / 10U) return false; n = n * 10U + (unsigned)(*s - '0'); }
    if (!n) return false;
    *out = n; return true;
}
static bool hostname(const char *s) {
    size_t n = strlen(s), label = 0;
    if (!n || n > 253 || s[0] == '.' || s[n - 1] == '.' || s[0] == '-' || s[n - 1] == '-') return false;
    for (const char *p = s; *p; p++) {
        if (*p == '.') { if (!label || p[-1] == '-' || p[1] == '-') return false; label = 0; }
        else if (!(isalnum((unsigned char)*p) || *p == '-') || ++label > 63) return false;
    }
    return true;
}
void punto_client_close(void) {
    if (initialized) curl_global_cleanup();
    initialized = configured = false; origin[0] = host[0] = resolve_entry[0] = 0; ca_file = NULL;
}
bool punto_client_open(void) {
    punto_client_close();
    const char *s = getenv("TICKETS_PUNTO_ORIGIN"), *ca = getenv("TICKETS_PUNTO_CA_FILE"), *ip = getenv("TICKETS_PUNTO_RESOLVE_IP");
    if (!s || !*s) return (!ca || !*ca) && (!ip || !*ip);
    bool https = !strncmp(s, "https://", 8), http = !strncmp(s, "http://", 7);
    size_t prefix = https ? 8 : http ? 7 : 0;
    if (!prefix || strlen(s) >= sizeof(origin) || !s[prefix] || strpbrk(s + prefix, "/?#@\\ \t\r\n")) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 33 || *p >= 127) return false;
    char authority[256];
    if (strlen(s + prefix) >= sizeof(authority)) return false;
    strcpy(authority, s + prefix);
    unsigned port = https ? 443U : 80U;
    char *port_text = NULL;
    if (authority[0] == '[') {
        char *end = strchr(authority, ']'); struct in6_addr addr;
        if (!end || (end[1] && end[1] != ':')) return false;
        if (end[1] == ':') port_text = end + 2;
        *end = 0;
        if (inet_pton(AF_INET6, authority + 1, &addr) != 1 || (http && strcmp(authority + 1, "::1"))) return false;
        snprintf(host, sizeof(host), "%s", authority + 1);
    } else {
        char *colon = strchr(authority, ':'); if (colon) { *colon = 0; port_text = colon + 1; }
        if (!hostname(authority)) return false;
        strcpy(host, authority);
        if (http && strcmp(host, "127.0.0.1") && strcmp(host, "localhost")) return false;
    }
    if (port_text && !port_number(port_text, &port)) return false;
    if (ca && *ca) {
        if (!https) return false;
        int flags = O_RDONLY;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        int fd = open(ca, flags); struct stat info;
        bool ok = fd >= 0 && !fstat(fd, &info) && S_ISREG(info.st_mode) && !(info.st_mode & (S_IWGRP | S_IWOTH));
        if (fd >= 0) close(fd);
        if (!ok) return false;
        ca_file = ca;
    }
    if (ip && *ip) {
        struct in_addr addr;
        if (strchr(host, ':') || inet_pton(AF_INET, ip, &addr) != 1 || (http && (ntohl(addr.s_addr) >> 24) != 127)) return false;
        snprintf(resolve_entry, sizeof(resolve_entry), "%s:%u:%s", host, port, ip);
    } else if (http && !strcmp(host, "localhost")) snprintf(resolve_entry, sizeof(resolve_entry), "localhost:%u:127.0.0.1", port);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
    initialized = configured = true; strcpy(origin, s); return true;
}
bool punto_client_configured(void) { return configured; }
const char *punto_client_origin(void) { return configured ? origin : NULL; }
typedef struct { char *data; size_t length; } Received;
static size_t receive_json(char *data, size_t size, size_t count, void *opaque) {
    Received *r = opaque;
    if (size && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if (bytes > JSON_LIMIT - r->length) return 0;
    memcpy(r->data + r->length, data, bytes); r->length += bytes; r->data[r->length] = 0;
    return bytes;
}
int punto_client_get(const char *grant, const char *endpoint, const char *query, json_object **result) {
    *result = NULL;
    if (!configured || !grant || strlen(grant) != 64) return 503;
    for (const char *p = grant; *p; p++) if (!isxdigit((unsigned char)*p)) return 503;
    if (strcmp(endpoint, "scope") && strcmp(endpoint, "jobs") && strcmp(endpoint, "assets") && strcmp(endpoint, "validate")) return 503;
    char url[8192], authorization[96];
    int n = snprintf(url, sizeof(url), "%s/api/integrations/tickets/%s%s%s", origin, endpoint, query && *query ? "?" : "", query ? query : "");
    if (n < 0 || (size_t)n >= sizeof(url)) return 503;
    snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", grant);
    CURL *curl = curl_easy_init(); Received r = {.data = calloc(1, JSON_LIMIT + 1)};
    if (!curl || !r.data) { if (curl) curl_easy_cleanup(curl); free(r.data); return 503; }
    struct curl_slist *headers = NULL, *resolve = NULL;
    headers = curl_slist_append(headers, authorization); headers = curl_slist_append(headers, "Accept: application/json");
    if (*resolve_entry) resolve = curl_slist_append(resolve, resolve_entry);
    bool ok = headers && (!*resolve_entry || resolve);
#define SET(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) ok = false; } while (0)
    SET(CURLOPT_URL, url); SET(CURLOPT_HTTPHEADER, headers); SET(CURLOPT_PROXY, ""); SET(CURLOPT_NOPROXY, "*");
    SET(CURLOPT_FOLLOWLOCATION, 0L); SET(CURLOPT_MAXREDIRS, 0L); SET(CURLOPT_SSL_VERIFYPEER, 1L); SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2); SET(CURLOPT_CONNECTTIMEOUT_MS, 3000L); SET(CURLOPT_TIMEOUT_MS, 5000L); SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive_json); SET(CURLOPT_WRITEDATA, &r);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "http,https"); SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    SET(CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS); SET(CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    if (ca_file) SET(CURLOPT_CAINFO, ca_file);
    if (resolve) SET(CURLOPT_RESOLVE, resolve);
#undef SET
    CURLcode code = ok ? curl_easy_perform(curl) : CURLE_FAILED_INIT; long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers); curl_slist_free_all(resolve); curl_easy_cleanup(curl);
    if (code == CURLE_OK && r.length && !memchr(r.data, 0, r.length) && (status == 200 || status == 400 || status == 401 || status == 403 || status == 404 || status == 503)) {
        json_tokener *tok = json_tokener_new_ex(32);
        if (tok) {
            json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
            json_object *o = json_tokener_parse_ex(tok, r.data, (int)r.length);
            size_t end = json_tokener_get_parse_end(tok);
            while (end < r.length && isspace((unsigned char)r.data[end])) end++;
            if (json_tokener_get_error(tok) == json_tokener_success && end == r.length && json_object_is_type(o, json_type_object)) *result = o;
            else if (o) json_object_put(o);
            json_tokener_free(tok);
        }
    }
    memset(r.data, 0, r.length); free(r.data);
    return *result ? (int)status : 503;
}
