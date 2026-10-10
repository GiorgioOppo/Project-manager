#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "documents.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool valid_key(const char *key) {
    if (!key || strlen(key) != 64) return false;
    for (size_t i = 0; i < 64; ++i)
        if (!((key[i] >= '0' && key[i] <= '9') ||
              (key[i] >= 'a' && key[i] <= 'f'))) return false;
    return true;
}

static bool valid_filename(const char *name) {
    if (!name) return false;
    size_t size = strlen(name);
    if (size < 5 || size > DOCUMENTS_FILENAME_MAX || name[0] == '.' ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        unsigned char c = *p++;
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == '"') return false;
        if (c < 128) continue;
        unsigned int count;
        uint32_t code;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; code = c & 0x1fU; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; code = c & 0x0fU; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; code = c & 0x07U; }
        else return false;
        for (unsigned int i = 0; i < count; ++i) {
            if ((*p & 0xc0U) != 0x80U) return false;
            code = (code << 6) | (*p++ & 0x3fU);
        }
        if ((count == 1 && code < 0x80U) || (count == 2 && code < 0x800U) ||
            (count == 3 && code < 0x10000U) || code > 0x10ffffU ||
            (code >= 0xd800U && code <= 0xdfffU)) return false;
    }
    return name[size - 4] == '.' && tolower((unsigned char)name[size - 3]) == 'p' &&
        tolower((unsigned char)name[size - 2]) == 'd' &&
        tolower((unsigned char)name[size - 1]) == 'f';
}

static bool valid_upload(const char *key, const char *filename,
                         const unsigned char *data, size_t size, const char *sha256) {
    return valid_key(key) && valid_filename(filename) && data && size >= 5 &&
        size <= DOCUMENTS_MAX_BYTES && memcmp(data, "%PDF-", 5) == 0 &&
        valid_key(sha256);
}

#ifdef PUNTO_MYSQL

#include <mongoc/mongoc.h>

static mongoc_client_t *client;
static mongoc_database_t *database;
static mongoc_gridfs_bucket_t *bucket;
static mongoc_collection_t *files;
static mongoc_collection_t *keys;
static bool initialized;

/* Driver messages can include connection details or server replies. */
static void silent_log(mongoc_log_level_t level, const char *domain,
                       const char *message, void *user_data) {
    (void)level; (void)domain; (void)message; (void)user_data;
}

static const char *setting(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

static bool valid_identifier(const char *value, size_t max) {
    if (!value || !*value || strlen(value) > max) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return false;
    return true;
}

static bson_value_t file_id(const char *key) {
    bson_value_t result = {0};
    result.value_type = BSON_TYPE_UTF8;
    result.value.v_utf8.str = (char *)key;
    result.value.v_utf8.len = 64;
    return result;
}

void documents_close(void) {
    if (keys) mongoc_collection_destroy(keys);
    if (files) mongoc_collection_destroy(files);
    if (bucket) mongoc_gridfs_bucket_destroy(bucket);
    if (database) mongoc_database_destroy(database);
    if (client) mongoc_client_destroy(client);
    keys = NULL; files = NULL; bucket = NULL; database = NULL; client = NULL;
    if (initialized) mongoc_cleanup();
    initialized = false;
}

bool documents_open(const char *db_path) {
    (void)db_path;
    documents_close();
    const char *host = setting("DOC_DB_HOST", "documents");
    const char *port_text = setting("DOC_DB_PORT", "27017");
    const char *name = setting("DOC_DB_NAME", "punto_documents");
    const char *user = setting("DOC_DB_USER", "punto_documents");
    const char *password = getenv("DOC_DB_PASSWORD");
    const char *ca = getenv("DOC_DB_SSL_CA");
    char *end = NULL;
    errno = 0;
    unsigned long port = strtoul(port_text, &end, 10);
    struct stat info;
    if (!valid_identifier(host, 253) || !valid_identifier(name, 63) ||
        !valid_identifier(user, 128) || !password || !*password ||
        strlen(password) > 4096 || errno || !end || *end || !port || port > 65535 ||
        !ca || ca[0] != '/' || stat(ca, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || access(ca, R_OK) != 0) return false;
    mongoc_log_set_handler(silent_log, NULL);
    mongoc_init();
    initialized = true;
    mongoc_uri_t *uri = mongoc_uri_new_for_host_port(host, (uint16_t)port);
    bool okay = uri && mongoc_uri_set_database(uri, name) &&
        mongoc_uri_set_auth_source(uri, name) &&
        mongoc_uri_set_username(uri, user) && mongoc_uri_set_password(uri, password) &&
        mongoc_uri_set_option_as_bool(uri, "tls", true) &&
        mongoc_uri_set_option_as_bool(uri, "tlsAllowInvalidCertificates", false) &&
        mongoc_uri_set_option_as_bool(uri, "tlsAllowInvalidHostnames", false) &&
        mongoc_uri_set_option_as_utf8(uri, "tlsCAFile", ca) &&
        mongoc_uri_set_option_as_utf8(uri, "appName", "punto-contracts") &&
        mongoc_uri_set_option_as_int32(uri, "connectTimeoutMS", 3000) &&
        mongoc_uri_set_option_as_int32(uri, "serverSelectionTimeoutMS", 3000) &&
        mongoc_uri_set_option_as_int32(uri, "socketTimeoutMS", 5000);
    if (okay) client = mongoc_client_new_from_uri(uri);
    if (uri) mongoc_uri_destroy(uri);
    if (client) database = mongoc_client_get_database(client, name);
    if (database) {
        bson_t options = BSON_INITIALIZER;
        bson_error_t error;
        BSON_APPEND_UTF8(&options, "bucketName", "contract_pdfs");
        BSON_APPEND_INT32(&options, "chunkSizeBytes", 255 * 1024);
        bucket = mongoc_gridfs_bucket_new(database, &options, NULL, &error);
        bson_destroy(&options);
        files = mongoc_client_get_collection(client, name, "contract_pdfs.files");
        /* A unique key reservation prevents parallel duplicate uploads from
           making GridFS abort cleanup delete another upload's chunks. */
        keys = mongoc_client_get_collection(client, name, "contract_pdf_keys");
    }
    if (!client || !database || !bucket || !files || !keys) {
        documents_close(); return false;
    }
    /* Client and bucket creation do not contact MongoDB. An unavailable server
       must not prevent SQL-only supplier and contract operations. */
    return true;
}

bool documents_health(void) {
    if (!database) return false;
    bson_t command = BSON_INITIALIZER;
    bson_error_t error;
    BSON_APPEND_INT32(&command, "ping", 1);
    bool okay = mongoc_database_command_simple(database, &command, NULL, NULL, &error);
    bson_destroy(&command);
    return okay;
}

static bool matching_file(const char *key, size_t expected_size, bool *exists) {
    bson_t filter = BSON_INITIALIZER;
    bson_t options = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&filter, "_id", key);
    BSON_APPEND_INT64(&options, "limit", 1);
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(files, &filter, &options, NULL);
    const bson_t *document = NULL;
    bson_error_t error;
    *exists = cursor && mongoc_cursor_next(cursor, &document);
    bool okay = cursor && !mongoc_cursor_error(cursor, &error);
    if (okay && *exists && expected_size) {
        bson_iter_t length;
        okay = bson_iter_init_find(&length, document, "length") &&
            (BSON_ITER_HOLDS_INT32(&length) || BSON_ITER_HOLDS_INT64(&length)) &&
            bson_iter_as_int64(&length) == (int64_t)expected_size;
    }
    if (cursor) mongoc_cursor_destroy(cursor);
    bson_destroy(&options); bson_destroy(&filter);
    return okay;
}

static bool release_key(const char *key) {
    bson_t filter = BSON_INITIALIZER;
    bson_error_t error;
    BSON_APPEND_UTF8(&filter, "_id", key);
    bool okay = mongoc_collection_delete_one(keys, &filter, NULL, NULL, &error);
    bson_destroy(&filter);
    return okay;
}

bool documents_store(const char *key, const char *filename,
                     const unsigned char *data, size_t size, const char *sha256) {
    if (!bucket || !valid_upload(key, filename, data, size, sha256)) return false;
    bool exists = false;
    if (!matching_file(key, 0, &exists) || exists) return false;
    bson_t claim = BSON_INITIALIZER;
    bson_error_t error;
    BSON_APPEND_UTF8(&claim, "_id", key);
    bool reserved = mongoc_collection_insert_one(keys, &claim, NULL, NULL, &error);
    bson_destroy(&claim);
    if (!reserved) return false;
    bson_t options = BSON_INITIALIZER;
    bson_t metadata;
    BSON_APPEND_DOCUMENT_BEGIN(&options, "metadata", &metadata);
    BSON_APPEND_UTF8(&metadata, "sha256", sha256);
    BSON_APPEND_UTF8(&metadata, "contentType", "application/pdf");
    bson_append_document_end(&options, &metadata);
    bson_value_t id = file_id(key);
    mongoc_stream_t *stream = mongoc_gridfs_bucket_open_upload_stream_with_id(
        bucket, &id, filename, &options, &error);
    bson_destroy(&options);
    bool okay = stream != NULL;
    size_t offset = 0;
    while (okay && offset < size) {
        size_t count = size - offset;
        if (count > 64U * 1024U) count = 64U * 1024U;
        ssize_t written = mongoc_stream_write(stream, (void *)(data + offset), count, 5000);
        if (written <= 0 || (size_t)written > count) okay = false;
        else offset += (size_t)written;
    }
    if (okay) okay = mongoc_stream_close(stream) == 0 &&
        !mongoc_gridfs_bucket_stream_error(stream, &error);
    bool cleaned = true;
    if (stream) {
        if (!okay) cleaned = mongoc_gridfs_bucket_abort_upload(stream);
        mongoc_stream_destroy(stream);
    }
    if (!okay) {
        bool remaining = false;
        if (!matching_file(key, 0, &remaining)) cleaned = false;
        else if (remaining && !mongoc_gridfs_bucket_delete_by_id(bucket, &id, &error))
            cleaned = false;
    }
    /* Keep successful claims until deletion. Failed cleanup leaves the claim
       in place, so a retry cannot reuse an identifier with orphaned chunks. */
    if (!okay && cleaned) (void)release_key(key);
    return okay;
}

bool documents_load(const char *key, size_t expected_size, unsigned char **data) {
    if (!data) return false;
    *data = NULL;
    if (!bucket || !valid_key(key) || !expected_size || expected_size > DOCUMENTS_MAX_BYTES)
        return false;
    bool exists = false;
    if (!matching_file(key, expected_size, &exists) || !exists) return false;
    bson_error_t error;
    bson_value_t id = file_id(key);
    mongoc_stream_t *stream = mongoc_gridfs_bucket_open_download_stream(bucket, &id, &error);
    if (!stream) return false;
    unsigned char *result = malloc(expected_size);
    bool okay = result != NULL;
    size_t offset = 0;
    while (okay && offset < expected_size) {
        size_t count = expected_size - offset;
        if (count > 64U * 1024U) count = 64U * 1024U;
        ssize_t got = mongoc_stream_read(stream, result + offset, count, 1, 5000);
        if (got <= 0 || (size_t)got > count) okay = false;
        else offset += (size_t)got;
    }
    unsigned char extra;
    if (okay) okay = mongoc_stream_read(stream, &extra, 1, 1, 5000) == 0 &&
        !mongoc_gridfs_bucket_stream_error(stream, &error);
    if (mongoc_stream_close(stream) != 0) okay = false;
    mongoc_stream_destroy(stream);
    if (okay) *data = result;
    else free(result);
    return okay;
}

bool documents_remove(const char *key) {
    if (!bucket || !valid_key(key)) return false;
    bson_error_t error;
    bson_value_t id = file_id(key);
    bool exists = false;
    if (!matching_file(key, 0, &exists)) return false;
    if (exists && !mongoc_gridfs_bucket_delete_by_id(bucket, &id, &error)) return false;
    return release_key(key);
}

#else

static int documents_dir = -1;
static unsigned long temporary_counter;

void documents_close(void) {
    if (documents_dir >= 0) (void)close(documents_dir);
    documents_dir = -1;
}

/* Walk directory components using O_NOFOLLOW, retaining a directory descriptor
   for all subsequent file operations. No filename from the user forms a path. */
static int private_directory(const char *path) {
    if (!path || !*path || strlen(path) >= PATH_MAX) return -1;
    char copy[PATH_MAX];
    memcpy(copy, path, strlen(path) + 1);
    int current = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) return -1;
    char *save = NULL;
    char *part = strtok_r(copy, "/", &save);
    bool visited = false;
    while (part) {
        char *next = strtok_r(NULL, "/", &save);
        if (strcmp(part, ".") == 0) { part = next; continue; }
        if (strcmp(part, "..") == 0) { (void)close(current); return -1; }
        int child = openat(current, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0 && errno == ENOENT && !next) {
            if (mkdirat(current, part, 0700) != 0 && errno != EEXIST) {
                (void)close(current); return -1;
            }
            child = openat(current, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        (void)close(current);
        if (child < 0) return -1;
        current = child; visited = true; part = next;
    }
    struct stat info;
    if (!visited || fstat(current, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || fchmod(current, 0700) != 0) {
        (void)close(current); return -1;
    }
    return current;
}

bool documents_open(const char *db_path) {
    documents_close();
    const char *directory = getenv("DOC_FILES_DIR");
    char path[PATH_MAX];
    if (!directory || !*directory) {
        if (!db_path || !*db_path || strlen(db_path) >= sizeof(path)) return false;
        const char *slash = strrchr(db_path, '/');
        char parent_path[PATH_MAX];
        size_t parent = slash ? (size_t)(slash - db_path + 1) : 0;
        if (parent) {
            memcpy(parent_path, db_path, parent);
            parent_path[parent] = '\0';
        } else memcpy(parent_path, ".", 2);
        /* DB paths are trusted application configuration. Canonicalize their
           already-open parent (macOS /var is a platform symlink), then reject
           symlinks in the actual PDF directory and every file operation. */
        if (!realpath(parent_path, path)) return false;
        parent = strlen(path);
        if (parent + sizeof("/documents") > sizeof(path)) return false;
        memcpy(path + parent, "/documents", sizeof("/documents"));
        directory = path;
    }
    documents_dir = private_directory(directory);
    return documents_dir >= 0;
}

bool documents_health(void) {
    struct stat info;
    return documents_dir >= 0 && fstat(documents_dir, &info) == 0 &&
        S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 0777) == 0700;
}

bool documents_store(const char *key, const char *filename,
                     const unsigned char *data, size_t size, const char *sha256) {
    if (!documents_health() || !valid_upload(key, filename, data, size, sha256)) return false;
    char temporary[128];
    (void)snprintf(temporary, sizeof(temporary), ".%s.%ld.%lu.tmp", key,
                   (long)getpid(), ++temporary_counter);
    int fd = openat(documents_dir, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool okay = true;
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { okay = false; break; }
        offset += (size_t)written;
    }
    if (okay && fsync(fd) != 0) okay = false;
    if (close(fd) != 0) okay = false;
    /* linkat publishes a complete file atomically and refuses an existing key.
       A regular rename would silently overwrite a previously stored document. */
    if (okay && linkat(documents_dir, temporary, documents_dir, key, 0) != 0) okay = false;
    (void)unlinkat(documents_dir, temporary, 0);
    if (okay && fsync(documents_dir) != 0) {
        (void)unlinkat(documents_dir, key, 0); okay = false;
    }
    return okay;
}

bool documents_load(const char *key, size_t expected_size, unsigned char **data) {
    if (!data) return false;
    *data = NULL;
    if (!documents_health() || !valid_key(key) || !expected_size ||
        expected_size > DOCUMENTS_MAX_BYTES) return false;
    int fd = openat(documents_dir, key, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info;
    bool okay = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_uid == geteuid() && (info.st_mode & 0777) == 0600 &&
        info.st_size >= 0 && (uint64_t)info.st_size == expected_size;
    unsigned char *result = okay ? malloc(expected_size) : NULL;
    okay = okay && result;
    size_t offset = 0;
    while (okay && offset < expected_size) {
        ssize_t got = read(fd, result + offset, expected_size - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) okay = false;
        else offset += (size_t)got;
    }
    unsigned char extra;
    if (okay) {
        ssize_t got;
        do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
        okay = got == 0;
    }
    if (close(fd) != 0) okay = false;
    if (okay) *data = result;
    else free(result);
    return okay;
}

bool documents_remove(const char *key) {
    if (!documents_health() || !valid_key(key)) return false;
    if (unlinkat(documents_dir, key, 0) != 0 && errno != ENOENT) return false;
    return fsync(documents_dir) == 0;
}

#endif
