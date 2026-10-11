#include "inventory_client.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_LIMIT (128U * 1024U)
#define INPUT_LIMIT 32768U
#define IMPORT_FORM_LIMIT (1024U * 1024U)
#define CSV_LIMIT (256U * 1024U)
#define FIELD_LIMIT 16U

static char api_url[256], authorization[160];
static char *ca_file;
static bool initialized, enabled;

static void erase(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool identifier(const char *value, size_t minimum, size_t maximum) {
    size_t length = value ? strlen(value) : 0;
    if (length < minimum || length > maximum) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c >= 128 || (!isalnum(c) && c != '_' && c != '-')) return false;
    }
    return true;
}

static bool safe_url(const char *url) {
    if (!url) return false;
    if (!strcmp(url, "https://inventory-api:8092") ||
        !strcmp(url, "https://inventory-addon:8092")) return true;
    const char *port = NULL;
    if (!strncmp(url, "https://127.0.0.1:", 18)) port = url + 18;
    else if (!strncmp(url, "https://localhost:", 18)) port = url + 18;
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
    if (fstat(file, &status) || !S_ISREG(status.st_mode) ||
        (status.st_mode & (S_IWGRP | S_IWOTH))) {
        close(file); return -1;
    }
    return file;
}

bool inventory_client_open(void) {
    inventory_client_close();
    const char *url = getenv("INVENTORY_API_URL");
    const char *token_path = getenv("INVENTORY_API_TOKEN_FILE");
    const char *ca_path = getenv("INVENTORY_API_CA_FILE");
    if ((!url || !*url) && (!token_path || !*token_path) &&
        (!ca_path || !*ca_path)) return true;
    if (!safe_url(url)) return false;
    int file = open_regular(ca_path);
    if (file < 0) return false;
    close(file);
    file = open_regular(token_path);
    if (file < 0) return false;
    char token[131] = {0}, extra;
    ssize_t count = read(file, token, sizeof(token) - 1);
    bool complete = count > 0 && read(file, &extra, 1) == 0;
    close(file);
    while (count > 0 && (token[count - 1] == '\n' || token[count - 1] == '\r')) token[--count] = '\0';
    if (!complete || count <= 0 || !identifier(token, 32, 128) ||
        (size_t)count != strlen(token)) {
        erase(token, sizeof(token)); return false;
    }
    /* libcurl's global initialization is reference counted. This client owns
       exactly one reference, independently of the alert mail client. */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        erase(token, sizeof(token)); return false;
    }
    initialized = true;
    ca_file = strdup(ca_path);
    if (!ca_file) {
        erase(token, sizeof(token)); inventory_client_close(); return false;
    }
    snprintf(api_url, sizeof(api_url), "%s", url);
    snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", token);
    erase(token, sizeof(token));
    enabled = true;
    return true;
}

void inventory_client_close(void) {
    if (initialized) curl_global_cleanup();
    initialized = enabled = false;
    free(ca_file); ca_file = NULL;
    erase(authorization, sizeof(authorization));
    api_url[0] = '\0';
}

bool inventory_client_enabled(void) { return enabled; }

void inventory_response_free(InventoryResponse *response) {
    if (!response) return;
    if (response->body) {
        erase(response->body, strlen(response->body));
        free(response->body);
    }
    response->body = NULL; response->status = 0;
}

static void reply(InventoryResponse *response, int status, const char *body) {
    response->status = status;
    response->body = strdup(body);
    if (!response->body) response->status = 500;
}

typedef struct { char *data; size_t size; } Received;

static size_t receive(void *data, size_t size, size_t count, void *context) {
    Received *response = context;
    if (size && count > RESPONSE_LIMIT / size) return 0;
    size_t length = size * count;
    if (length > RESPONSE_LIMIT - response->size || memchr(data, '\0', length)) return 0;
    memcpy(response->data + response->size, data, length);
    response->size += length;
    response->data[response->size] = '\0';
    return length;
}

static CURLcode request(const char *path, const char *form, Received *received, long *status, long timeout_ms) {
    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_OUT_OF_MEMORY;
    struct curl_slist *headers = curl_slist_append(NULL, authorization);
    if (!headers) { curl_easy_cleanup(curl); return CURLE_OUT_OF_MEMORY; }
    struct curl_slist *list = curl_slist_append(headers, "Accept: application/json");
    if (!list) { curl_slist_free_all(headers); curl_easy_cleanup(curl); return CURLE_OUT_OF_MEMORY; }
    headers = list;
    char url[8192];
    int needed = snprintf(url, sizeof(url), "%s%s", api_url, path);
    if (needed < 0 || (size_t)needed >= sizeof(url)) {
        curl_slist_free_all(headers); curl_easy_cleanup(curl); return CURLE_URL_MALFORMAT;
    }
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
    SET(CURLOPT_TIMEOUT_MS, timeout_ms);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, received);
    if (form) {
        SET(CURLOPT_POST, 1L);
        SET(CURLOPT_POSTFIELDS, form);
        SET(CURLOPT_POSTFIELDSIZE, (long)strlen(form));
    }
    if (result == CURLE_OK) result = curl_easy_perform(curl);
    if (result == CURLE_OK) result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
#undef SET
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    return result;
}

static struct json_object *parse_json(const char *input, size_t length) {
    struct json_tokener *tokener = json_tokener_new_ex(16);
    if (!tokener) return NULL;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *object = json_tokener_parse_ex(tokener, input, (int)length);
    bool valid = json_tokener_get_error(tokener) == json_tokener_success;
    size_t end = json_tokener_get_parse_end(tokener);
    while (end < length && isspace((unsigned char)input[end])) end++;
    valid = valid && end == length;
    json_tokener_free(tokener);
    if (!valid && object) { json_object_put(object); object = NULL; }
    return object;
}

static char *encode(const char *value) {
    size_t length = strlen(value);
    char *result = malloc(length * 3 + 1);
    if (!result) return NULL;
    const char digits[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            result[used++] = (char)*p;
        } else {
            result[used++] = '%'; result[used++] = digits[*p >> 4]; result[used++] = digits[*p & 15];
        }
    }
    result[used] = '\0';
    return result;
}

static bool public_json(struct json_object *value, unsigned depth, unsigned *members) {
    if (depth > 12 || ++*members > 16384) return false;
    if (json_object_is_type(value, json_type_object)) {
        json_object_object_foreach(value, name, item) {
            if (!strcasecmp(name, "access_token") || !strcasecmp(name, "refresh_token") ||
                !strcasecmp(name, "id_token") || !strcasecmp(name, "client_secret") ||
                !strcasecmp(name, "code_verifier") || !strcasecmp(name, "session_binding") ||
                !strcasecmp(name, "authorization") || !strcasecmp(name, "password") ||
                !strcasecmp(name, "token") || !strcasecmp(name, "code")) return false;
            if (!public_json(item, depth + 1, members)) return false;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t length = json_object_array_length(value);
        if (length > 2000) return false;
        for (size_t i = 0; i < length; i++)
            if (!public_json(json_object_array_get_idx(value, i), depth + 1, members)) return false;
    } else if (json_object_is_type(value, json_type_string)) {
        if (strlen(json_object_get_string(value)) != (size_t)json_object_get_string_len(value)) return false;
    }
    return true;
}

static bool positive_number(const char *value, unsigned long long maximum) {
    if (!value || !*value) return false;
    unsigned long long result = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < '0' || *p > '9' || result > maximum / 10 ||
            (result == maximum / 10 && (unsigned)(*p - '0') > maximum % 10)) return false;
        result = result * 10 + (unsigned)(*p - '0');
    }
    return result > 0;
}

static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool utf8(const unsigned char *p) {
    while (*p) {
        unsigned c = *p++;
        if (c < 0x80) continue;
        unsigned remaining, minimum;
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; minimum = 0x800; c &= 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; minimum = 0x10000; c &= 0x07; }
        else return false;
        while (remaining--) {
            if ((*p & 0xc0) != 0x80) return false;
            c = (c << 6) | (*p++ & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}

static char *decode(const char *start, size_t length) {
    char *value = malloc(length + 1);
    if (!value) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)start[i];
        if (c == '+') c = ' ';
        else if (c == '%') {
            if (i + 2 >= length || hex((unsigned char)start[i + 1]) < 0 ||
                hex((unsigned char)start[i + 2]) < 0) { free(value); return NULL; }
            c = (unsigned char)(hex((unsigned char)start[i + 1]) * 16 + hex((unsigned char)start[i + 2]));
            i += 2;
        }
        if (!c || c == 127 || (c < 32 && c != '\n' && c != '\r' && c != '\t')) {
            free(value); return NULL;
        }
        value[used++] = (char)c;
    }
    value[used] = '\0';
    if (!utf8((const unsigned char *)value)) { free(value); return NULL; }
    return value;
}

static bool due_date(const char *value);

static bool allowed_field(InventoryOperation operation, const char *name, const char *value) {
    size_t maximum = 0;
    bool catalog = operation == INVENTORY_ASSETS || operation == INVENTORY_EXPORT || operation == INVENTORY_DASHBOARD;
    bool person_write = operation == INVENTORY_PERSON_CREATE || operation == INVENTORY_PERSON_UPDATE;
    bool identity_write = operation == INVENTORY_IDENTITY_CREATE || operation == INVENTORY_IDENTITY_UPDATE;
    bool people_list = operation == INVENTORY_PEOPLE;
    bool identity_list = operation == INVENTORY_IDENTITIES;
    bool subject_write = operation == INVENTORY_SUBJECT_CREATE;
    bool identity_link_write = operation == INVENTORY_IDENTITY_PERSON_CREATE;
    bool directory_page = people_list || identity_list || operation == INVENTORY_PERSON_HISTORY ||
        operation == INVENTORY_PERSON_IDENTITIES || operation == INVENTORY_PERSON_ASSETS ||
        operation == INVENTORY_IDENTITY_HISTORY || operation == INVENTORY_IDENTITY_PEOPLE ||
        operation == INVENTORY_IDENTITY_ASSETS || operation == INVENTORY_SUBJECTS;
    if ((people_list || identity_list) && !strcmp(name,"q")) maximum=200;
    if ((people_list || identity_list || person_write || identity_write) && !strcmp(name,"active"))
        return !strcmp(value,"1") || !strcmp(value,"0");
    if ((identity_list || identity_write) && !strcmp(name,"kind"))
        return !strcmp(value,"personal") || !strcmp(value,"shared") || !strcmp(value,"service");
    if ((identity_list || identity_link_write) && !strcmp(name,"person_id")) return positive_number(value,LLONG_MAX);
    if ((person_write || identity_write || identity_link_write || subject_write) && !strcmp(name,"notes")) maximum=2000;
    if ((person_write || identity_write) && !strcmp(name,"display_name")) maximum=160;
    if (person_write) {
        if (!strcmp(name,"person_code")) maximum=64;
        if (!strcmp(name,"email")) maximum=254;
        if (!strcmp(name,"organization")) maximum=160;
        if (!strcmp(name,"job_title")) maximum=120;
    }
    if (identity_write && (!strcmp(name,"realm") || !strcmp(name,"login"))) maximum=160;
    if ((operation == INVENTORY_PERSON_UPDATE || operation == INVENTORY_IDENTITY_UPDATE ||
         identity_link_write || operation == INVENTORY_IDENTITY_PERSON_REMOVE || subject_write ||
         operation == INVENTORY_SUBJECT_REMOVE) && !strcmp(name,"version")) return positive_number(value,LLONG_MAX);
    if (subject_write && !strcmp(name,"subject_kind")) return !strcmp(value,"person") || !strcmp(value,"identity");
    if (subject_write && !strcmp(name,"subject_id")) return positive_number(value,LLONG_MAX);
    if ((identity_link_write || subject_write) && !strcmp(name,"role")) return !strcmp(value,"owner") ||
        !strcmp(value,"user") || (subject_write && !strcmp(value,"administrator"));
    if (catalog && !strcmp(name, "q")) maximum = 200;
    if (catalog && !strcmp(name, "mine")) return !strcmp(value, "1");
    if (catalog && (!strcmp(name,"site_id") || !strcmp(name,"department_id") ||
        !strcmp(name,"person_id") || !strcmp(name,"identity_id"))) return positive_number(value,LLONG_MAX);
    if (catalog && !strcmp(name,"attention")) return !strcmp(value,"open_custody") || !strcmp(value,"overdue_returns") ||
        !strcmp(value,"never_verified") || !strcmp(value,"verification_due") || !strcmp(value,"location_mismatch");
    if ((catalog || operation == INVENTORY_OPERATIONAL_DUE) && !strcmp(name,"verification_days")) return positive_number(value,365);
    if ((catalog || operation == INVENTORY_OPERATIONAL_DUE) && !strcmp(name,"today")) return due_date(value);
    if (operation == INVENTORY_OPERATIONAL_DUE && !strcmp(name,"kind")) return !strcmp(value,"return") || !strcmp(value,"verification");
    if ((catalog && operation != INVENTORY_DASHBOARD) || operation == INVENTORY_HISTORY || operation == INVENTORY_RELATIONS ||
        operation == INVENTORY_DUE || operation == INVENTORY_OPERATIONAL_DUE ||
        operation == INVENTORY_PLACES || operation == INVENTORY_CUSTODY || operation == INVENTORY_VERIFICATIONS || directory_page) {
        if (!strcmp(name, "limit")) return positive_number(value, 100);
        if (!strcmp(name, "after_id")) return positive_number(value, LLONG_MAX);
    }
    bool place_write = operation == INVENTORY_PLACE_CREATE || operation == INVENTORY_PLACE_UPDATE;
    bool asset_action = operation >= INVENTORY_CONTEXT && operation <= INVENTORY_VERIFY;
    if ((place_write || asset_action) && !strcmp(name, "version"))
        return operation != INVENTORY_PLACE_CREATE && positive_number(value, LLONG_MAX);
    if (operation == INVENTORY_PLACES || place_write) {
        if (!strcmp(name,"kind")) return !strcmp(value,"site") || !strcmp(value,"department") || !strcmp(value,"location");
        if (!strcmp(name,"active")) return !strcmp(value,"1") || !strcmp(value,"0");
        if (place_write && !strcmp(name,"parent_id")) return !*value || positive_number(value,LLONG_MAX);
        if (place_write && !strcmp(name,"name")) maximum=160;
    }
    if (operation == INVENTORY_CONTEXT) {
        if (!strcmp(name,"assignee_user_id") || !strcmp(name,"assignee_person_id") || !strcmp(name,"site_id") ||
            !strcmp(name,"department_id") || !strcmp(name,"location_id"))
            return !*value || positive_number(value,LLONG_MAX);
    }
    if (operation == INVENTORY_CHECKOUT) {
        if (!strcmp(name,"user_id") || !strcmp(name,"person_id")) return positive_number(value,LLONG_MAX);
        if (!strcmp(name,"checkout_on") || !strcmp(name,"due_on")) maximum=10;
        if (!strcmp(name,"condition_out")) return !strcmp(value,"good") || !strcmp(value,"fair") || !strcmp(value,"damaged");
    }
    if (operation == INVENTORY_CHECKIN) {
        if (!strcmp(name,"returned_on")) maximum=10;
        if (!strcmp(name,"condition_in")) return !strcmp(value,"good") || !strcmp(value,"fair") || !strcmp(value,"damaged");
        if (!strcmp(name,"status")) return !strcmp(value,"available") || !strcmp(value,"maintenance");
    }
    if (operation == INVENTORY_VERIFY) {
        if (!strcmp(name,"verified_on")) maximum=10;
        if (!strcmp(name,"condition")) return !strcmp(value,"good") || !strcmp(value,"fair") || !strcmp(value,"damaged");
        if (!strcmp(name,"observed_location_id")) return !*value || positive_number(value,LLONG_MAX);
    }
    if (asset_action && !strcmp(name,"notes")) maximum=2000;
    if (operation == INVENTORY_DUE) {
        if (!strcmp(name,"days_before")) return positive_number(value,365);
        if (!strcmp(name,"today")) {
            if (strlen(value)!=10 || value[4]!='-' || value[7]!='-') return false;
            for (int i=0;i<10;i++) if (i!=4 && i!=7 && (value[i]<'0' || value[i]>'9')) return false;
            return true;
        }
    }
    if (operation == INVENTORY_IMPORT_PREVIEW) {
        if (!strcmp(name,"csv")) return *value && strlen(value)<=CSV_LIMIT;
        if (!strcmp(name,"mode")) return !strcmp(value,"create_only") || !strcmp(value,"upsert");
    }
    if (operation == INVENTORY_IMPORT_COMMIT && !strcmp(name,"preview_id")) {
        if (strlen(value)!=32) return false;
        for (const char *p=value;*p;p++) if (!isxdigit((unsigned char)*p)) return false;
        return true;
    }
    if (operation == INVENTORY_CREATE || operation == INVENTORY_UPDATE) {
        if (!strcmp(name, "asset_tag")) maximum = 64;
        else if (!strcmp(name, "name") || !strcmp(name, "location") || !strcmp(name, "assignee")) maximum = 160;
        else if (!strcmp(name, "category")) maximum = 80;
        else if (!strcmp(name, "serial_number") || !strcmp(name, "manufacturer") || !strcmp(name, "model")) maximum = 120;
        else if (!strcmp(name, "purchase_date") || !strcmp(name, "warranty_until")) maximum = 10;
        else if (!strcmp(name, "notes")) maximum = 4000;
        else if (!strcmp(name, "details")) {
            if (strlen(value) > 4096) return false;
            struct json_object *details = parse_json(value, strlen(value));
            bool valid = details && json_object_is_type(details, json_type_object);
            if (details) json_object_put(details);
            return valid;
        }
        else if (!strcmp(name, "version") && operation == INVENTORY_UPDATE)
            return positive_number(value, LLONG_MAX);
    }
    if (!strcmp(name, "asset_type") && (catalog ||
        operation == INVENTORY_CREATE || operation == INVENTORY_UPDATE)) {
        return !strcmp(value, "it_device") || !strcmp(value, "network_device") ||
            !strcmp(value, "physical") || !strcmp(value, "software") || !strcmp(value, "license");
    }
    if ((operation == INVENTORY_RELATION_CREATE || operation == INVENTORY_RELATION_REMOVE) &&
        !strcmp(name, "version")) return positive_number(value, LLONG_MAX);
    if (operation == INVENTORY_RELATION_CREATE) {
        if (!strcmp(name, "relation_type")) return !strcmp(value, "installed_on") ||
            !strcmp(value, "connected_to") || !strcmp(value, "licensed_to");
        if (!strcmp(name, "target_id") || !strcmp(name,"person_id") || !strcmp(name,"identity_id"))
            return positive_number(value, LLONG_MAX);
        if (!strcmp(name, "quantity")) return positive_number(value, 1000000);
        if (!strcmp(name, "assignee")) maximum = 160;
        else if (!strcmp(name, "installed_version") || !strcmp(name, "source_port") || !strcmp(name, "target_port")) maximum = 80;
        else if (!strcmp(name, "notes")) maximum = 2000;
    }
    if (!strcmp(name, "status") && (catalog ||
        operation == INVENTORY_CREATE || operation == INVENTORY_UPDATE)) {
        return !strcmp(value, "available") || !strcmp(value, "assigned") ||
            !strcmp(value, "maintenance") || !strcmp(value, "retired") || !strcmp(value, "lost");
    }
    if (!maximum || strlen(value) > maximum) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
        if (*p < 32 && strcmp(name, "notes")) return false;
    return true;
}

/* Decode before checking names so escaped duplicates and identity fields are
   rejected. Re-encode every value before appending the trusted audit actor. */
static bool write_operation(InventoryOperation operation) {
    return operation == INVENTORY_CREATE || operation == INVENTORY_UPDATE ||
        operation == INVENTORY_RELATION_CREATE || operation == INVENTORY_RELATION_REMOVE ||
        operation == INVENTORY_IMPORT_PREVIEW || operation == INVENTORY_IMPORT_COMMIT ||
        operation == INVENTORY_PLACE_CREATE || operation == INVENTORY_PLACE_UPDATE ||
        (operation >= INVENTORY_CONTEXT && operation <= INVENTORY_VERIFY) ||
        operation == INVENTORY_PERSON_CREATE || operation == INVENTORY_PERSON_UPDATE ||
        operation == INVENTORY_IDENTITY_CREATE || operation == INVENTORY_IDENTITY_UPDATE ||
        operation == INVENTORY_IDENTITY_PERSON_CREATE || operation == INVENTORY_IDENTITY_PERSON_REMOVE ||
        operation == INVENTORY_SUBJECT_CREATE || operation == INVENTORY_SUBJECT_REMOVE;
}

static InventoryPersonLookup person_lookup;
void inventory_client_set_person_lookup(InventoryPersonLookup lookup) { person_lookup=lookup; }

static char *build_form(InventoryOperation operation, const char *input, DbInt actor_id,
                       InventoryUserLookup lookup, int *error_status) {
    struct { char *name, *value; } fields[FIELD_LIMIT] = {{0}};
    size_t count = 0;
    size_t input_limit = operation == INVENTORY_IMPORT_PREVIEW ? IMPORT_FORM_LIMIT : INPUT_LIMIT;
    bool valid = input && strlen(input) <= input_limit;
    const char *p = input ? input : "";
    unsigned present = 0;
    while (valid && *p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *equals = memchr(p, '=', (size_t)(end - p));
        if (!equals || equals == p || count == FIELD_LIMIT) { valid = false; break; }
        fields[count].name = decode(p, (size_t)(equals - p));
        fields[count].value = decode(equals + 1, (size_t)(end - equals - 1));
        const char *name = fields[count].name, *value = fields[count].value;
        if (!name || !value || !allowed_field(operation, name, value)) valid = false;
        for (size_t i = 0; valid && i < count; i++)
            if (!strcmp(fields[i].name, name)) valid = false;
        if (valid) {
            if (!strcmp(name, "asset_tag") && *value) present |= 1;
            if (!strcmp(name, "name") && *value) present |= 2;
            if (!strcmp(name, "version")) present |= 4;
            if (!strcmp(name, "relation_type") && *value) present |= 8;
            if (!strcmp(name, "csv") && *value) present |= 16;
            if (!strcmp(name, "preview_id") && *value) present |= 32;
            if (!strcmp(name, "person_code") && *value) present |= 64;
            if (!strcmp(name, "display_name") && *value) present |= 128;
            if (!strcmp(name, "realm") && *value) present |= 256;
            if (!strcmp(name, "login") && *value) present |= 512;
            if (!strcmp(name, "person_id") && *value) present |= 1024;
            if (!strcmp(name, "subject_kind") && *value) present |= 2048;
            if (!strcmp(name, "subject_id") && *value) present |= 4096;
        }
        count++;
        p = *end ? end + 1 : end;
        if (*end && !*p) valid = false;
    }
    bool write = write_operation(operation);
    unsigned required = operation == INVENTORY_CREATE ? 3U : operation == INVENTORY_UPDATE ? 7U :
        operation == INVENTORY_RELATION_CREATE ? 12U : operation == INVENTORY_IMPORT_PREVIEW ? 16U :
        operation == INVENTORY_IMPORT_COMMIT ? 32U : operation == INVENTORY_PLACE_CREATE ? 2U :
        operation == INVENTORY_PLACE_UPDATE ? 6U : operation == INVENTORY_PERSON_CREATE ? 192U :
        operation == INVENTORY_PERSON_UPDATE ? 196U : operation == INVENTORY_IDENTITY_CREATE ? 768U :
        operation == INVENTORY_IDENTITY_UPDATE ? 772U : operation == INVENTORY_IDENTITY_PERSON_CREATE ? 1028U :
        operation == INVENTORY_SUBJECT_CREATE ? 6148U : 4U;
    if (write && ((present & required) != required || actor_id <= 0)) valid = false;
    /* Validate recipient alternatives before any application-user lookup. People
       and inventory identities have their own ID spaces and never use it. */
    if (valid && (operation == INVENTORY_CONTEXT || operation == INVENTORY_CHECKOUT)) {
        const char *user_key=operation == INVENTORY_CONTEXT ? "assignee_user_id" : "user_id";
        const char *person_key=operation == INVENTORY_CONTEXT ? "assignee_person_id" : "person_id";
        bool user=false, person=false;
        for (size_t i=0;i<count;i++) {
            if (!strcmp(fields[i].name,user_key) && *fields[i].value) user=true;
            if (!strcmp(fields[i].name,person_key) && *fields[i].value) person=true;
        }
        if ((user && person) || (operation == INVENTORY_CHECKOUT && !user && !person)) valid=false;
    }
    if (valid && operation == INVENTORY_RELATION_CREATE) {
        unsigned recipients=0;
        bool structured=false, license=false;
        for (size_t i=0;i<count;i++) {
            const char *name=fields[i].name;
            if (!strcmp(name,"relation_type")) license=!strcmp(fields[i].value,"licensed_to");
            if ((!strcmp(name,"target_id") || !strcmp(name,"assignee") ||
                 !strcmp(name,"person_id") || !strcmp(name,"identity_id")) && *fields[i].value) recipients++;
            if (!strcmp(name,"person_id") || !strcmp(name,"identity_id")) structured=true;
        }
        if (recipients != 1 || (structured && !license)) valid=false;
    }
    char user_name[481]={0};
    bool user_field=false;
    const char *trusted_name_field=operation == INVENTORY_CONTEXT ? "assignee_user_name" : "user_name";
    if (valid && (operation == INVENTORY_CONTEXT || operation == INVENTORY_CHECKOUT)) {
        const char *id_field=operation == INVENTORY_CONTEXT ? "assignee_user_id" : "user_id";
        for (size_t i=0;i<count;i++) if (!strcmp(fields[i].name,id_field)) {
            user_field=true;
            if (*fields[i].value) {
                int found=lookup ? lookup((DbInt)strtoll(fields[i].value,NULL,10),user_name,sizeof(user_name)) : 0;
                if (found!=1) { valid=false; *error_status=found<0 ? 503 : 422; }
                if (valid && (!*user_name || !utf8((unsigned char *)user_name))) valid=false;
            }
        }
    }
    char *form = valid ? calloc(1, input_limit * 3 + 1800) : NULL;
    if (form) {
        for (size_t i = 0; i < count; i++) {
            char *encoded = encode(fields[i].value);
            if (!encoded) { free(form); form = NULL; break; }
            if (*form) strcat(form, "&");
            const char *field_name=!strcmp(fields[i].name,"mine") ? "assignee_user_id" : fields[i].name;
            if (!strcmp(fields[i].name,"mine")) {
                if (actor_id<=0) { free(encoded); free(form); form=NULL; break; }
                char current_id[32]; snprintf(current_id,sizeof(current_id),"%lld",(long long)actor_id);
                free(encoded); encoded=encode(current_id);
                if (!encoded) { free(form); form=NULL; break; }
            }
            strcat(form, field_name); strcat(form, "="); strcat(form, encoded);
            free(encoded);
            if (!strcmp(fields[i].name,"mine") && person_lookup) {
                DbInt person=person_lookup(actor_id);
                if(person<0){free(form);form=NULL;*error_status=503;break;}
                if(person>0){char suffix[80];snprintf(suffix,sizeof(suffix),"&mapped_person_id=%lld",(long long)person);strcat(form,suffix);}
            }
        }
        if (form && user_field) {
            char *encoded=encode(user_name);
            if (!encoded) { free(form); form=NULL; }
            else { strcat(form,"&"); strcat(form,trusted_name_field); strcat(form,"="); strcat(form,encoded); free(encoded); }
        }
        if (form && write) {
            char actor[64];
            snprintf(actor, sizeof(actor), "&actor_id=%lld", (long long)actor_id);
            strcat(form, actor);
        }
    }
    erase(user_name,sizeof(user_name));
    for (size_t i = 0; i < count; i++) {
        free(fields[i].name);
        if (fields[i].value) { erase(fields[i].value, strlen(fields[i].value)); free(fields[i].value); }
    }
    return form;
}

static void client_call(InventoryOperation operation, DbInt item_id, DbInt relation_id, DbInt actor_id,
                        const char *input, InventoryUserLookup lookup, InventoryResponse *response, long timeout_ms) {
    if (!response) return;
    response->status = 0; response->body = NULL;
    bool write = write_operation(operation);
    bool relations = operation == INVENTORY_RELATIONS || operation == INVENTORY_RELATION_CREATE ||
        operation == INVENTORY_RELATION_REMOVE;
    bool operational = operation >= INVENTORY_CONTEXT && operation <= INVENTORY_VERIFICATIONS;
    bool person_item = operation == INVENTORY_PERSON_DETAIL || operation == INVENTORY_PERSON_UPDATE ||
        operation == INVENTORY_PERSON_HISTORY || operation == INVENTORY_PERSON_IDENTITIES || operation == INVENTORY_PERSON_ASSETS;
    bool identity_item = operation == INVENTORY_IDENTITY_DETAIL || operation == INVENTORY_IDENTITY_UPDATE ||
        operation == INVENTORY_IDENTITY_HISTORY || operation == INVENTORY_IDENTITY_PEOPLE ||
        operation == INVENTORY_IDENTITY_PERSON_CREATE || operation == INVENTORY_IDENTITY_PERSON_REMOVE || operation == INVENTORY_IDENTITY_ASSETS;
    bool subjects = operation == INVENTORY_SUBJECTS || operation == INVENTORY_SUBJECT_CREATE || operation == INVENTORY_SUBJECT_REMOVE;
    bool item = operation == INVENTORY_DETAIL || operation == INVENTORY_HISTORY || operation == INVENTORY_UPDATE || relations || operational || subjects;
    bool remove = operation == INVENTORY_RELATION_REMOVE || operation == INVENTORY_IDENTITY_PERSON_REMOVE || operation == INVENTORY_SUBJECT_REMOVE;
    if (operation < INVENTORY_STATUS || operation > INVENTORY_SUBJECT_REMOVE ||
        ((item || person_item || identity_item || operation == INVENTORY_PLACE_UPDATE) && item_id <= 0) || (remove && relation_id <= 0)) {
        reply(response, 400, "{\"ok\":false,\"error\":\"Richiesta inventario non valida.\"}"); return;
    }
    int error_status=400;
    char *form = build_form(operation, input ? input : "", actor_id, lookup, &error_status);
    if (!form) { reply(response, error_status, error_status == 503 ? "{\"ok\":false,\"error\":\"Impossibile verificare l’utente. Riprova.\"}" :
        error_status == 422 ? "{\"ok\":false,\"error\":\"Seleziona un utente attivo dell’app.\"}" :
        "{\"ok\":false,\"error\":\"Campi inventario non validi o duplicati.\"}"); return; }
    if (!enabled) {
        reply(response, operation == INVENTORY_STATUS ? 200 : 503,
            operation == INVENTORY_STATUS ? "{\"ok\":true,\"configured\":false}" :
            "{\"ok\":false,\"error\":\"Il servizio Asset inventory non è configurato.\"}");
        erase(form, strlen(form)); free(form); return;
    }
    char path[8192];
    int length;
    if (operation == INVENTORY_PEOPLE || operation == INVENTORY_PERSON_CREATE ||
        operation == INVENTORY_IDENTITIES || operation == INVENTORY_IDENTITY_CREATE)
        length=snprintf(path,sizeof(path),"/v1/%s%s%s",
            operation == INVENTORY_PEOPLE || operation == INVENTORY_PERSON_CREATE ? "people" : "identities",
            !write && *form ? "?" : "",write ? "" : form);
    else if (person_item || identity_item) {
        const char *suffix = operation == INVENTORY_PERSON_HISTORY || operation == INVENTORY_IDENTITY_HISTORY ? "/history" :
            operation == INVENTORY_PERSON_IDENTITIES ? "/identities" :
            operation == INVENTORY_PERSON_ASSETS || operation == INVENTORY_IDENTITY_ASSETS ? "/assets" :
            operation == INVENTORY_IDENTITY_PEOPLE || operation == INVENTORY_IDENTITY_PERSON_CREATE ? "/people" : "";
        if (operation == INVENTORY_IDENTITY_PERSON_REMOVE)
            length=snprintf(path,sizeof(path),"/v1/identities/%lld/people/%lld/remove",(long long)item_id,(long long)relation_id);
        else length=snprintf(path,sizeof(path),"/v1/%s/%lld%s%s%s",person_item ? "people" : "identities",
            (long long)item_id,suffix,!write && *form ? "?" : "",write ? "" : form);
    } else if (subjects) {
        if (operation == INVENTORY_SUBJECT_REMOVE)
            length=snprintf(path,sizeof(path),"/v1/assets/%lld/subjects/%lld/remove",(long long)item_id,(long long)relation_id);
        else length=snprintf(path,sizeof(path),"/v1/assets/%lld/subjects%s%s",(long long)item_id,
            !write && *form ? "?" : "",write ? "" : form);
    } else if (operation == INVENTORY_PLACE_UPDATE)
        length=snprintf(path,sizeof(path),"/v1/places/%lld",(long long)item_id);
    else if (operation == INVENTORY_PLACES || operation == INVENTORY_PLACE_CREATE)
        length=snprintf(path,sizeof(path),"/v1/places%s%s",!write && *form ? "?" : "",write ? "" : form);
    else if (operational) {
        static const char *names[]={"context","checkout","checkin","verify","custody","verifications"};
        length=snprintf(path,sizeof(path),"/v1/assets/%lld/%s%s%s",(long long)item_id,
            names[operation-INVENTORY_CONTEXT],!write && *form ? "?" : "",write ? "" : form);
    } else if (operation == INVENTORY_IMPORT_PREVIEW || operation == INVENTORY_IMPORT_COMMIT)
        length = snprintf(path,sizeof(path),"/v1/import/%s",operation == INVENTORY_IMPORT_PREVIEW ? "preview" : "commit");
    else if (operation == INVENTORY_EXPORT || operation == INVENTORY_DUE ||
             operation == INVENTORY_DASHBOARD || operation == INVENTORY_OPERATIONAL_DUE)
        length = snprintf(path,sizeof(path),"/v1/%s%s%s",operation == INVENTORY_EXPORT ? "assets/export" :
            operation == INVENTORY_DUE ? "due" : operation == INVENTORY_DASHBOARD ? "dashboard" : "operational-due",*form ? "?" : "",form);
    else if (operation == INVENTORY_RELATION_REMOVE) length = snprintf(path, sizeof(path),
        "/v1/assets/%lld/relations/%lld/remove", (long long)item_id, (long long)relation_id);
    else if (relations) length = snprintf(path, sizeof(path), "/v1/assets/%lld/relations%s%s", (long long)item_id,
        !write && *form ? "?" : "", write ? "" : form);
    else if (item) length = snprintf(path, sizeof(path), "/v1/assets/%lld%s%s%s", (long long)item_id,
        operation == INVENTORY_HISTORY ? "/history" : "", !write && *form ? "?" : "", write ? "" : form);
    else length = snprintf(path, sizeof(path), "/v1/%s%s%s", operation == INVENTORY_STATUS ? "status" : "assets",
        !write && *form ? "?" : "", write ? "" : form);
    Received received = {.data = calloc(1, RESPONSE_LIMIT + 1)};
    long status = 0;
    CURLcode result = length < 0 || (size_t)length >= sizeof(path) || !received.data ? CURLE_OUT_OF_MEMORY :
        request(path, write ? form : NULL, &received, &status, timeout_ms);
    erase(form, strlen(form)); free(form);
    struct json_object *object = result == CURLE_OK && received.size ? parse_json(received.data, received.size) : NULL;
    unsigned members = 0;
    bool okay = object && json_object_is_type(object, json_type_object) && public_json(object, 0, &members);
    if (okay && (status == 200 || status == 201 || status == 400 || status == 403 || status == 404 ||
        status == 409 || status == 413 || status == 422 || status == 429 || status == 503)) {
        reply(response, (int)status, json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN));
    } else reply(response, 503, "{\"ok\":false,\"error\":\"Il servizio Asset inventory non è raggiungibile. Riprova.\"}");
    if (object) json_object_put(object);
    if (received.data) { erase(received.data, received.size); free(received.data); }
}

void inventory_client_call(InventoryOperation operation, DbInt item_id, DbInt relation_id, DbInt actor_id,
                           const char *input, InventoryResponse *response) {
    client_call(operation,item_id,relation_id,actor_id,input,NULL,response,5000L);
}

void inventory_client_call_with_lookup(InventoryOperation operation, DbInt item_id, DbInt relation_id,
                           DbInt actor_id, const char *input, InventoryUserLookup lookup,
                           InventoryResponse *response) {
    client_call(operation,item_id,relation_id,actor_id,input,lookup,response,5000L);
}

static bool due_date(const char *value) {
    if (!value || strlen(value)!=10 || value[4]!='-' || value[7]!='-') return false;
    for (int i=0;i<10;i++) if (i!=4 && i!=7 && !isdigit((unsigned char)value[i])) return false;
    int year=atoi(value), month=atoi(value+5), day=atoi(value+8);
    static const int lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if (year<1 || month<1 || month>12) return false;
    int maximum=lengths[month-1]+(month==2 && year%4==0 && (year%100!=0 || year%400==0));
    return day>=1 && day<=maximum;
}

static bool due_text(struct json_object *object,const char *name,size_t maximum,const char **value) {
    struct json_object *item=NULL;
    if (!json_object_object_get_ex(object,name,&item) || !json_object_is_type(item,json_type_string)) return false;
    *value=json_object_get_string(item);
    size_t length=strlen(*value);
    if (!length || length>maximum*4 || length!=(size_t)json_object_get_string_len(item)) return false;
    size_t characters=0;
    for (const unsigned char *p=(const unsigned char *)*value;*p;p++) if ((*p & 0xc0)!=0x80) characters++;
    return characters<=maximum;
}

static long long monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)!=0) return -1;
    return (long long)now.tv_sec*1000+now.tv_nsec/1000000;
}

bool inventory_client_due(const char *today,int days_before,struct json_object **items) {
    if (!items) return false;
    *items=NULL;
    if (!enabled || !due_date(today) || days_before<1 || days_before>365) return false;
    struct json_object *all=json_object_new_array();
    if (!all) return false;
    long long started=monotonic_ms(), after=0;
    bool complete=false;
    for (unsigned page=0;page<100 && started>=0;page++) {
        long long remaining=8000-(monotonic_ms()-started);
        if (remaining<=0) break;
        char query[160];
        snprintf(query,sizeof(query),"today=%s&days_before=%d&limit=100%s",today,days_before,after ? "&after_id=" : "");
        if (after) snprintf(query+strlen(query),sizeof(query)-strlen(query),"%lld",after);
        InventoryResponse response={0};
        client_call(INVENTORY_DUE,0,0,0,query,NULL,&response,(long)(remaining<5000 ? remaining : 5000));
        struct json_object *data=response.status==200 && response.body ? parse_json(response.body,strlen(response.body)) : NULL;
        struct json_object *okay=NULL,*rows=NULL,*next=NULL;
        bool valid=data && json_object_object_get_ex(data,"ok",&okay) &&
            json_object_is_type(okay,json_type_boolean) && json_object_get_boolean(okay) &&
            json_object_object_get_ex(data,"items",&rows) && json_object_is_type(rows,json_type_array) &&
            json_object_object_get_ex(data,"next_after_id",&next);
        size_t length=valid ? json_object_array_length(rows) : 0;
        if (length>400 || json_object_array_length(all)+length>10000) valid=false;
        long long previous=after ? after-1 : LLONG_MAX;
        unsigned kinds=0;
        for (size_t i=0;valid && i<length;i++) {
            struct json_object *row=json_object_array_get_idx(rows,i),*id=NULL,*days=NULL;
            const char *tag=NULL,*name=NULL,*type=NULL,*kind=NULL,*date=NULL;
            valid=json_object_is_type(row,json_type_object) && json_object_object_get_ex(row,"asset_id",&id) &&
                json_object_is_type(id,json_type_int) && json_object_get_int64(id)>0 &&
                json_object_get_int64(id)<=previous && due_text(row,"asset_tag",64,&tag) &&
                due_text(row,"name",160,&name) && due_text(row,"asset_type",32,&type) &&
                due_text(row,"due_kind",32,&kind) && due_text(row,"due_on",10,&date) && due_date(date) &&
                json_object_object_get_ex(row,"days_remaining",&days) && json_object_is_type(days,json_type_int) &&
                json_object_get_int64(days)>=-3660000 && json_object_get_int64(days)<=days_before;
            if (!valid) break;
            valid=!strcmp(type,"it_device") || !strcmp(type,"network_device") || !strcmp(type,"physical") ||
                !strcmp(type,"software") || !strcmp(type,"license");
            unsigned bit=!strcmp(kind,"warranty_until") ? 1U : !strcmp(kind,"maintenance_due") ? 2U :
                !strcmp(kind,"expires_on") ? 4U : !strcmp(kind,"renewal_on") ? 8U : 0;
            long long current=json_object_get_int64(id);
            if (current!=previous) kinds=0;
            if (!bit || (kinds & bit)) valid=false;
            kinds|=bit; previous=current;
            if (valid && json_object_array_add(all,json_object_get(row))!=0) valid=false;
        }
        bool last=valid && json_object_is_type(next,json_type_null);
        long long cursor=valid && json_object_is_type(next,json_type_int) ? json_object_get_int64(next) : 0;
        if (valid && !last && (!length || cursor<=0 || cursor!=previous || (after && cursor>=after))) valid=false;
        if (data) json_object_put(data);
        inventory_response_free(&response);
        if (!valid) break;
        if (last) { complete=true; break; }
        after=cursor;
    }
    if (!complete) { json_object_put(all); return false; }
    *items=all;
    return true;
}

/* Gregorian day arithmetic avoids DST and process-timezone dependencies. */
static long long due_day(const char *value) {
    int year=atoi(value),month=atoi(value+5),day=atoi(value+8);
    year-=month<=2;
    int era=year/400,year_of_era=year-era*400;
    int day_of_year=(153*(month+(month>2 ? -3 : 9))+2)/5+day-1;
    return (long long)era*146097+year_of_era*365+year_of_era/4-year_of_era/100+day_of_year;
}

bool inventory_client_operational_due(const char *today,const char *kind,
                                     int verification_days,struct json_object **items) {
    if (!items) return false;
    *items=NULL;
    if (!enabled || !due_date(today) || !kind || (strcmp(kind,"return") && strcmp(kind,"verification")) ||
        verification_days<1 || verification_days>365) return false;
    bool returns=!strcmp(kind,"return");
    struct json_object *all=json_object_new_array();
    if (!all) return false;
    long long started=monotonic_ms(),after=0;
    bool complete=false;
    for (unsigned page=0;page<100 && started>=0;page++) {
        long long elapsed=monotonic_ms();
        long long remaining=elapsed<0 ? 0 : 8000-(elapsed-started);
        if (remaining<=0) break;
        char query[192];
        snprintf(query,sizeof(query),"kind=%s&today=%s&verification_days=%d&limit=100%s",kind,today,
            verification_days,after ? "&after_id=" : "");
        if (after) snprintf(query+strlen(query),sizeof(query)-strlen(query),"%lld",after);
        InventoryResponse response={0};
        client_call(INVENTORY_OPERATIONAL_DUE,0,0,0,query,NULL,&response,(long)(remaining<5000 ? remaining : 5000));
        struct json_object *data=response.status==200 && response.body ? parse_json(response.body,strlen(response.body)) : NULL;
        struct json_object *okay=NULL,*rows=NULL,*next=NULL;
        bool valid=data && json_object_object_get_ex(data,"ok",&okay) &&
            json_object_is_type(okay,json_type_boolean) && json_object_get_boolean(okay) &&
            json_object_object_get_ex(data,"items",&rows) && json_object_is_type(rows,json_type_array) &&
            json_object_object_get_ex(data,"next_after_id",&next);
        size_t length=valid ? json_object_array_length(rows) : 0;
        if (length>100 || json_object_array_length(all)+length>10000) valid=false;
        long long previous=after ? after : LLONG_MAX;
        for (size_t i=0;valid && i<length;i++) {
            struct json_object *row=json_object_array_get_idx(rows,i),*id=NULL,*days=NULL,*source=NULL;
            const char *tag=NULL,*name=NULL,*type=NULL,*event_kind=NULL,*date=NULL;
            valid=json_object_is_type(row,json_type_object) && json_object_object_get_ex(row,"asset_id",&id) &&
                json_object_is_type(id,json_type_int) && json_object_get_int64(id)>0 &&
                (json_object_get_int64(id)<previous || (!after && !i && json_object_get_int64(id)==LLONG_MAX)) &&
                due_text(row,"asset_tag",64,&tag) && due_text(row,"name",160,&name) &&
                due_text(row,"asset_type",32,&type) && due_text(row,"due_kind",32,&event_kind) &&
                due_text(row,"due_on",10,&date) && due_date(date) &&
                json_object_object_get_ex(row,"days_remaining",&days) && json_object_is_type(days,json_type_int) &&
                json_object_get_int64(days)==due_day(date)-due_day(today) &&
                json_object_get_int64(days)<=(returns ? -1 : 0) &&
                json_object_object_get_ex(row,"source_id",&source) && json_object_is_type(source,json_type_int) &&
                json_object_get_int64(source)>=(returns ? 1 : 0);
            if (!valid) break;
            valid=(!strcmp(type,"it_device") || !strcmp(type,"network_device") || !strcmp(type,"physical")) &&
                !strcmp(event_kind,returns ? "return_due" : "verification_due");
            previous=json_object_get_int64(id);
            if (valid && json_object_array_add(all,json_object_get(row))!=0) valid=false;
        }
        bool last=valid && json_object_is_type(next,json_type_null);
        long long cursor=valid && json_object_is_type(next,json_type_int) ? json_object_get_int64(next) : 0;
        if (valid && !last && (!length || cursor<=0 || cursor!=previous || (after && cursor>=after))) valid=false;
        if (data) json_object_put(data);
        inventory_response_free(&response);
        if (!valid) break;
        if (last) { complete=true; break; }
        after=cursor;
    }
    if (!complete) { json_object_put(all); return false; }
    *items=all;
    return true;
}
