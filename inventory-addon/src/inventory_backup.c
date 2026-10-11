/* Consistent inventory snapshots, kept outside the live SQLite data directory. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "inventory_backup.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef SQLITE_OPEN_NOFOLLOW
#define SQLITE_OPEN_NOFOLLOW 0
#endif

#define BACKUP_RETRY_SECONDS 300
#define BACKUP_SCAN_LIMIT 10000
#define BACKUP_METADATA ".inventory-backup-status.json"

typedef struct {
    char name[80];
    time_t created;
    dev_t device;
    ino_t inode;
} Snapshot;

static struct {
    sqlite3 *source;
    int enabled, directory_fd, interval_hours, keep;
    char directory[PATH_MAX], latest_file[80], error[240];
    time_t last_success, last_attempt, next_due;
    unsigned int serial;
} state = {.directory_fd = -1};

static int number(const char *text, int fallback, int maximum) {
    if (!text || !*text) return fallback;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    return !errno && end && !*end && value >= 1 && value <= maximum ? (int)value : 0;
}

static json_object *timestamp(time_t value) {
    if (!value) return NULL;
    struct tm utc;
    char text[32];
    if (!gmtime_r(&value, &utc) || !strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc)) return NULL;
    return json_object_new_string(text);
}

json_object *inventory_backup_status(void) {
    json_object *result = json_object_new_object();
    json_object_object_add(result, "enabled", json_object_new_boolean(state.enabled));
    json_object_object_add(result, "interval_hours", json_object_new_int(state.interval_hours));
    json_object_object_add(result, "keep", json_object_new_int(state.keep));
    json_object_object_add(result, "last_success", timestamp(state.last_success));
    json_object_object_add(result, "last_attempt", timestamp(state.last_attempt));
    json_object_object_add(result, "next_due", state.enabled ? timestamp(state.next_due) : NULL);
    json_object_object_add(result, "latest_file", *state.latest_file ? json_object_new_string(state.latest_file) : NULL);
    json_object_object_add(result, "last_error", *state.error ? json_object_new_string(state.error) : NULL);
    return result;
}

static void failure(const char *message) {
    snprintf(state.error, sizeof(state.error), "%s", message);
    fprintf(stderr, "inventory backup: %s\n", state.error);
}

static int path_for(const char *name, char path[PATH_MAX]) {
    int length = snprintf(path, PATH_MAX, "%s/%s", state.directory, name);
    return length > 0 && length < PATH_MAX;
}

/* Never use a replaced directory or follow a symlink in its final component. */
static int same_directory(void) {
    struct stat opened, current;
    return state.directory_fd >= 0 && !fstat(state.directory_fd, &opened) &&
        !lstat(state.directory, &current) && S_ISDIR(current.st_mode) &&
        opened.st_dev == current.st_dev && opened.st_ino == current.st_ino;
}

static int snapshot_name(const char *name, time_t *created) {
    if (strlen(name) != 47 || strncmp(name, "inventory-", 10) ||
        name[30] != '-' || strcmp(name + 39, ".sqlite3")) return 0;
    for (int index = 10; index < 30; index++) if (name[index] < '0' || name[index] > '9') return 0;
    for (int index = 31; index < 39; index++) {
        char c = name[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    char epoch[21];
    memcpy(epoch, name + 10, 20); epoch[20] = 0;
    char *end;
    errno = 0;
    long long value = strtoll(epoch, &end, 10);
    if (errno || *end || value <= 0 || (long long)(time_t)value != value) return 0;
    *created = (time_t)value;
    return 1;
}

static int check_database(sqlite3 *database) {
    sqlite3_stmt *statement = NULL;
    int valid = sqlite3_prepare_v2(database, "PRAGMA integrity_check", -1, &statement, NULL) == SQLITE_OK;
    if (valid) {
        valid = sqlite3_step(statement) == SQLITE_ROW &&
            !strcmp((const char *)sqlite3_column_text(statement, 0), "ok") &&
            sqlite3_step(statement) == SQLITE_DONE;
    }
    sqlite3_finalize(statement);
    return valid;
}

static int verified_snapshot(Snapshot *snapshot) {
    char path[PATH_MAX];
    struct stat opened, current;
    int descriptor = openat(state.directory_fd, snapshot->name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return 0;
    int valid = !fstat(descriptor, &opened) && S_ISREG(opened.st_mode) && opened.st_uid == geteuid() &&
        !fstatat(state.directory_fd, snapshot->name, &current, AT_SYMLINK_NOFOLLOW) &&
        opened.st_ino == current.st_ino && opened.st_dev == current.st_dev && path_for(snapshot->name, path);
    sqlite3 *database = NULL;
    if (valid) valid = sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, NULL) == SQLITE_OK;
    if (valid) valid = check_database(database) &&
        !fstatat(state.directory_fd, snapshot->name, &current, AT_SYMLINK_NOFOLLOW) &&
        S_ISREG(current.st_mode) && opened.st_ino == current.st_ino && opened.st_dev == current.st_dev;
    if (database) sqlite3_close(database);
    close(descriptor);
    if (valid) { snapshot->device = opened.st_dev; snapshot->inode = opened.st_ino; }
    return valid;
}

static int newest_first(const void *left, const void *right) {
    const Snapshot *a = left, *b = right;
    if (a->created < b->created) return 1;
    if (a->created > b->created) return -1;
    return strcmp(b->name, a->name);
}

static Snapshot *snapshots(size_t *count) {
    *count = 0;
    if (!same_directory()) return NULL;
    int descriptor = openat(state.directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return NULL;
    DIR *directory = fdopendir(descriptor);
    if (!directory) { close(descriptor); return NULL; }
    Snapshot *items = NULL;
    struct dirent *entry;
    size_t scanned = 0;
    while ((entry = readdir(directory)) && scanned++ < BACKUP_SCAN_LIMIT) {
        Snapshot candidate = {0};
        if (!snapshot_name(entry->d_name, &candidate.created)) continue;
        snprintf(candidate.name, sizeof(candidate.name), "%s", entry->d_name);
        if (!verified_snapshot(&candidate)) continue;
        Snapshot *grown = realloc(items, (*count + 1) * sizeof(*items));
        if (!grown) { free(items); items = NULL; *count = 0; break; }
        items = grown;
        items[(*count)++] = candidate;
    }
    closedir(directory);
    if (*count > 1) qsort(items, *count, sizeof(*items), newest_first);
    return items;
}

static int write_all(int descriptor, const char *data, size_t length) {
    while (length) {
        ssize_t written = write(descriptor, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return 0;
        data += written; length -= (size_t)written;
    }
    return 1;
}

static int unique_file(char name[80], const char *prefix, time_t now) {
    /* O_EXCL provides collision safety; the suffix does not carry any secret. */
    for (int attempt = 0; attempt < 32; attempt++) {
        unsigned int suffix = (unsigned int)getpid() * 2654435761u ^ ++state.serial;
        snprintf(name, 80, "%s%020lld-%08x%s", prefix, (long long)now, suffix,
                 !strcmp(prefix, "inventory-") ? ".sqlite3" : ".tmp");
        int descriptor = openat(state.directory_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor >= 0 || errno != EEXIST) return descriptor;
    }
    return -1;
}

static void save_metadata(void) {
    if (!same_directory()) return;
    json_object *metadata = json_object_new_object();
    json_object_object_add(metadata, "last_success_epoch", json_object_new_int64(state.last_success));
    json_object_object_add(metadata, "last_attempt_epoch", json_object_new_int64(state.last_attempt));
    json_object_object_add(metadata, "next_due_epoch", json_object_new_int64(state.next_due));
    json_object_object_add(metadata, "latest_file", json_object_new_string(state.latest_file));
    json_object_object_add(metadata, "last_error", json_object_new_string(state.error));
    const char *body = json_object_to_json_string_ext(metadata, JSON_C_TO_STRING_PLAIN);
    char temporary[80];
    int descriptor = unique_file(temporary, ".inventory-state-", state.last_attempt ? state.last_attempt : time(NULL));
    if (descriptor >= 0) {
        int valid = write_all(descriptor, body, strlen(body)) && !fsync(descriptor);
        if (close(descriptor)) valid = 0;
        if (valid && !renameat(state.directory_fd, temporary, state.directory_fd, BACKUP_METADATA)) {
            (void)fsync(state.directory_fd);
        } else {
            unlinkat(state.directory_fd, temporary, 0);
            failure("Impossibile salvare lo stato persistente del backup.");
        }
    } else failure("Impossibile creare lo stato persistente del backup.");
    json_object_put(metadata);
}

static void load_metadata(time_t now) {
    int descriptor = openat(state.directory_fd, BACKUP_METADATA, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return;
    struct stat info;
    char body[2048];
    ssize_t length = !fstat(descriptor, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() ?
        read(descriptor, body, sizeof(body) - 1) : -1;
    close(descriptor);
    if (length <= 0 || length >= (ssize_t)sizeof(body) - 1) return;
    body[length] = 0;
    json_object *metadata = json_tokener_parse(body), *value;
    if (!metadata || !json_object_is_type(metadata, json_type_object)) { json_object_put(metadata); return; }
    if (!json_object_object_get_ex(metadata, "latest_file", &value) ||
        !json_object_is_type(value, json_type_string) || strcmp(json_object_get_string(value), state.latest_file)) {
        json_object_put(metadata); return;
    }
    if (json_object_object_get_ex(metadata, "last_attempt_epoch", &value) && json_object_is_type(value, json_type_int)) {
        int64_t attempt = json_object_get_int64(value);
        if (attempt > 0 && attempt <= (int64_t)now) state.last_attempt = (time_t)attempt;
    }
    if (json_object_object_get_ex(metadata, "last_error", &value) && json_object_is_type(value, json_type_string)) {
        snprintf(state.error, sizeof(state.error), "%s", json_object_get_string(value));
    }
    if (*state.error && state.last_attempt && now - state.last_attempt < BACKUP_RETRY_SECONDS &&
        state.next_due <= now) state.next_due = state.last_attempt + BACKUP_RETRY_SECONDS;
    json_object_put(metadata);
}

void inventory_backup_close(void) {
    if (state.directory_fd >= 0) close(state.directory_fd);
    memset(&state, 0, sizeof(state));
    state.directory_fd = -1;
}

void inventory_backup_open(sqlite3 *source) {
    inventory_backup_close();
    state.source = source;
    state.interval_hours = number(getenv("INVENTORY_BACKUP_INTERVAL_HOURS"), 24, 168);
    state.keep = number(getenv("INVENTORY_BACKUP_KEEP"), 14, 365);
    const char *directory = getenv("INVENTORY_BACKUP_DIR");
    if (!directory || !*directory) return;
    state.enabled = 1;
    if (!state.interval_hours || !state.keep || directory[0] != '/' || strlen(directory) > sizeof(state.directory) - 90) {
        failure("Configurazione del backup non valida.");
        return;
    }
    snprintf(state.directory, sizeof(state.directory), "%s", directory);
    size_t length = strlen(state.directory);
    while (length > 1 && state.directory[length - 1] == '/') state.directory[--length] = 0;
    if (mkdir(state.directory, 0700) && errno != EEXIST) {
        failure("Impossibile creare la directory dei backup.");
        return;
    }
    struct stat info;
    if (lstat(state.directory, &info) || !S_ISDIR(info.st_mode)) {
        failure("La directory dei backup deve essere una directory reale, non un collegamento simbolico.");
        return;
    }
    /* Canonicalize parent aliases such as macOS /var before SQLite's NOFOLLOW. */
    char canonical[PATH_MAX];
    if (!realpath(state.directory, canonical) || strlen(canonical) > sizeof(state.directory) - 90) {
        failure("Impossibile risolvere la directory dei backup.");
        return;
    }
    snprintf(state.directory, sizeof(state.directory), "%s", canonical);
    state.directory_fd = open(state.directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (state.directory_fd < 0 || fstat(state.directory_fd, &info) || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0022)) {
        if (state.directory_fd >= 0) close(state.directory_fd);
        state.directory_fd = -1;
        failure("La directory dei backup deve appartenere al servizio e non essere scrivibile da altri.");
        return;
    }
    size_t count;
    Snapshot *items = snapshots(&count);
    if (count) {
        state.last_success = items[0].created;
        snprintf(state.latest_file, sizeof(state.latest_file), "%s", items[0].name);
        state.next_due = state.last_success + state.interval_hours * 3600;
    }
    free(items);
    load_metadata(time(NULL));
}

static int create_snapshot(time_t now, char final_name[80]) {
    char temporary[80], path[PATH_MAX];
    int descriptor = unique_file(temporary, ".inventory-copy-", now);
    if (descriptor < 0) return 0;
    close(descriptor);
    sqlite3 *destination = NULL;
    int valid = path_for(temporary, path) &&
        sqlite3_open_v2(path, &destination, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, NULL) == SQLITE_OK;
    sqlite3_backup *backup = valid ? sqlite3_backup_init(destination, "main", state.source, "main") : NULL;
    valid = backup != NULL;
    if (backup) {
        struct timespec started, current, pause = {.tv_sec = 0, .tv_nsec = 20000000};
        clock_gettime(CLOCK_MONOTONIC, &started);
        int result;
        do {
            result = sqlite3_backup_step(backup, 256);
            if (result == SQLITE_BUSY || result == SQLITE_LOCKED) nanosleep(&pause, NULL);
            clock_gettime(CLOCK_MONOTONIC, &current);
        } while ((result == SQLITE_OK || result == SQLITE_BUSY || result == SQLITE_LOCKED) &&
                 current.tv_sec - started.tv_sec < 5);
        valid = result == SQLITE_DONE;
        if (sqlite3_backup_finish(backup) != SQLITE_OK) valid = 0;
    }
    if (valid) valid = sqlite3_exec(destination, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL) == SQLITE_OK && check_database(destination);
    if (destination && sqlite3_close(destination) != SQLITE_OK) valid = 0;
    descriptor = valid ? openat(state.directory_fd, temporary, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (descriptor >= 0) { valid = !fsync(descriptor); close(descriptor); }
    else valid = 0;
    if (valid) {
        /* Reserve the final name first: a restart can never overwrite an older copy. */
        descriptor = unique_file(final_name, "inventory-", now);
        if (descriptor < 0) valid = 0;
        else {
            close(descriptor);
            if (renameat(state.directory_fd, temporary, state.directory_fd, final_name)) {
                unlinkat(state.directory_fd, final_name, 0); valid = 0;
            } else (void)fsync(state.directory_fd);
        }
    }
    if (!valid) unlinkat(state.directory_fd, temporary, 0);
    return valid;
}

static void retain_snapshots(void) {
    size_t count;
    Snapshot *items = snapshots(&count);
    for (size_t index = (size_t)state.keep; index < count; index++) {
        struct stat current;
        if (fstatat(state.directory_fd, items[index].name, &current, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(current.st_mode) || current.st_dev != items[index].device || current.st_ino != items[index].inode) continue;
        if (unlinkat(state.directory_fd, items[index].name, 0)) failure("Impossibile applicare la conservazione dei backup.");
    }
    free(items);
    (void)fsync(state.directory_fd);
}

void inventory_backup_tick(time_t now) {
    if (!state.enabled || now <= 0 || (state.next_due && now < state.next_due)) return;
    state.last_attempt = now;
    state.next_due = now + BACKUP_RETRY_SECONDS;
    if (!state.source || !state.interval_hours || !state.keep || !same_directory()) {
        if (!*state.error) failure("Directory dei backup non disponibile.");
        save_metadata(); return;
    }
    char filename[80];
    if (!create_snapshot(now, filename)) {
        failure("Creazione o verifica del backup SQLite non riuscita.");
        save_metadata(); return;
    }
    state.last_success = now;
    state.next_due = now + state.interval_hours * 3600;
    snprintf(state.latest_file, sizeof(state.latest_file), "%s", filename);
    state.error[0] = 0;
    retain_snapshots();
    save_metadata();
}
