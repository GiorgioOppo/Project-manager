/* Private, authenticated producer data; never accept this JSON from a browser. */
#define GROUP_SNAPSHOT_LIMIT (4U * 1024U * 1024U)
#define GROUP_SNAPSHOT_AGE 120

typedef struct {
    const char *subject;
    const char *name;
    const char *parent;
    DbInt requested_id;
    DbInt local_id;
    json_object *members;
} ManagedGroup;

static bool managed_key(const char *text) {
    size_t length = text ? strlen(text) : 0;
    if (!length || length > 255) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr("-_.:@+~", *p))) return false;
    return true;
}

static bool managed_string(json_object *value, const char **out) {
    if (!json_object_is_type(value, json_type_string)) return false;
    const char *text = json_object_get_string(value);
    if (!text || (size_t)json_object_get_string_len(value) != strlen(text)) return false;
    *out = text;
    return true;
}

static bool managed_fields(json_object *object, const char *const *names, size_t count) {
    if (!json_object_is_type(object, json_type_object) || (size_t)json_object_object_length(object) != count)
        return false;
    for (size_t i = 0; i < count; i++) {
        json_object *value;
        if (!json_object_object_get_ex(object, names[i], &value)) return false;
    }
    return true;
}

static bool managed_email(const char *text) {
    size_t length = text ? strlen(text) : 0;
    const char *at = text ? strchr(text, '@') : NULL;
    if (!length || length > 254 || !at || at == text || !at[1] || strchr(at + 1, '@')) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 33 || *p > 126 || *p == '\\' || *p == '"') return false;
    return true;
}

static int managed_index(ManagedGroup *groups, size_t count, const char *subject) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(groups[i].subject, subject) == 0) return (int)i;
    return -1;
}

static bool managed_validate(json_object *root, ManagedGroup groups[GROUP_LIMIT], size_t *count,
                             json_object **bindings, time_t *generated, time_t now) {
    const char *const fields[] = {"version", "issuer", "generated_at", "groups", "user_bindings"};
    const char *const group_fields[] = {"id", "parent_id", "name", "local_id", "members"};
    json_object *value, *array;
    const char *issuer;
    if (!managed_fields(root, fields, 5)) return false;
    json_object_object_get_ex(root, "version", &value);
    if (!json_object_is_type(value, json_type_int) || json_object_get_int64(value) != 1) return false;
    json_object_object_get_ex(root, "issuer", &value);
    if (!managed_string(value, &issuer) || strcmp(issuer, groups_issuer) != 0) return false;
    json_object_object_get_ex(root, "generated_at", &value);
    if (!json_object_is_type(value, json_type_int)) return false;
    int64_t timestamp = json_object_get_int64(value);
    if (timestamp < 1 || timestamp > (int64_t)now + 5 || timestamp < (int64_t)now - GROUP_SNAPSHOT_AGE)
        return false;
    *generated = (time_t)timestamp;
    json_object_object_get_ex(root, "groups", &array);
    if (!json_object_is_type(array, json_type_array) || json_object_array_length(array) > GROUP_LIMIT)
        return false;
    *count = json_object_array_length(array);
    for (size_t i = 0; i < *count; i++) {
        ManagedGroup *group = &groups[i];
        json_object *item = json_object_array_get_idx(array, i);
        if (!managed_fields(item, group_fields, 5)) return false;
        json_object_object_get_ex(item, "id", &value);
        if (!managed_string(value, &group->subject) || !managed_key(group->subject)) return false;
        json_object_object_get_ex(item, "name", &value);
        if (!managed_string(value, &group->name) || !valid_text(group->name, 120, true, false)) return false;
        json_object_object_get_ex(item, "parent_id", &value);
        if (value && (!managed_string(value, &group->parent) || !managed_key(group->parent))) return false;
        json_object_object_get_ex(item, "local_id", &value);
        if (value) {
            if (!json_object_is_type(value, json_type_int) || (group->requested_id = json_object_get_int64(value)) <= 0)
                return false;
        }
        json_object_object_get_ex(item, "members", &group->members);
        if (!json_object_is_type(group->members, json_type_array) || json_object_array_length(group->members) > 200)
            return false;
        for (size_t j = 0; j < i; j++)
            if (strcmp(groups[j].subject, group->subject) == 0 ||
                (group->requested_id && group->requested_id == groups[j].requested_id)) return false;
        for (size_t j = 0; j < json_object_array_length(group->members); j++) {
            const char *member;
            if (!managed_string(json_object_array_get_idx(group->members, j), &member) || !managed_key(member))
                return false;
            for (size_t k = 0; k < j; k++)
                if (strcmp(member, json_object_get_string(json_object_array_get_idx(group->members, k))) == 0)
                    return false;
        }
    }
    for (size_t i = 0; i < *count; i++) {
        int current = (int)i;
        size_t depth = 0;
        while (current >= 0) {
            if (++depth > GROUP_DEPTH) return false;
            const char *parent = groups[current].parent;
            if (!parent) break;
            current = managed_index(groups, *count, parent);
            if (current < 0) return false;
        }
    }
    json_object_object_get_ex(root, "user_bindings", bindings);
    if (!json_object_is_type(*bindings, json_type_object) || json_object_object_length(*bindings) > 1024)
        return false;
    json_object_object_foreach(*bindings, subject, email_value) {
        const char *email;
        if (!managed_key(subject) || !managed_string(email_value, &email) || !managed_email(email)) return false;
    }
    return true;
}

bool organization_groups_managed(void) { return groups_managed; }

bool organization_groups_available(void) {
    if (!groups_managed) return true;
    time_t now = time(NULL);
    return groups_sync_ok && now != (time_t)-1 && groups_generated_at <= now + 5 &&
        groups_generated_at >= now - GROUP_SNAPSHOT_AGE;
}

static char *managed_read(size_t *length) {
    struct stat before, status;
    if (lstat(groups_snapshot_path, &before) || !S_ISREG(before.st_mode)) return NULL;
    int fd = open(groups_snapshot_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    if (fstat(fd, &status) || !S_ISREG(status.st_mode) || before.st_dev != status.st_dev ||
        before.st_ino != status.st_ino || status.st_size <= 0 ||
        status.st_size > GROUP_SNAPSHOT_LIMIT) { close(fd); return NULL; }
    size_t size = (size_t)status.st_size, used = 0;
    char *data = malloc(size + 1);
    if (!data) { close(fd); return NULL; }
    while (used < size) {
        ssize_t bytes = read(fd, data + used, size - used);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) { free(data); close(fd); return NULL; }
        used += (size_t)bytes;
    }
    char extra;
    ssize_t trailing;
    do { trailing = read(fd, &extra, 1); } while (trailing < 0 && errno == EINTR);
    close(fd);
    if (trailing != 0 || memchr(data, 0, size)) { free(data); return NULL; }
    data[size] = '\0'; *length = size;
    return data;
}

/* A signed email claim never selects an existing profile. Only the immutable
   identity projection or an explicit migration binding may resolve a member. */
static bool managed_member(const char *subject, json_object *bindings, DbInt *user_id) {
    DbStmt *statement = NULL;
    *user_id = 0;
    if (!prepare(&statement, "SELECT user_id FROM oidc_identities WHERE issuer=? AND subject=?") ||
        !bind_text(statement, 1, groups_issuer) || !bind_text(statement, 2, subject)) goto failure;
    int result = db_step(statement);
    if (result == DB_ROW) *user_id = db_column_int64(statement, 0);
    db_finalize(statement); statement = NULL;
    if (result != DB_ROW && result != DB_DONE) return false;
    if (*user_id) return true;
    json_object *bound_email;
    if (!json_object_object_get_ex(bindings, subject, &bound_email)) return true;
    if (!prepare(&statement, "SELECT id FROM users WHERE email=?") ||
        !bind_text(statement, 1, json_object_get_string(bound_email))) goto failure;
    result = db_step(statement);
    if (result == DB_ROW) *user_id = db_column_int64(statement, 0);
    db_finalize(statement); statement = NULL;
    if (result != DB_ROW && result != DB_DONE) return false;
    if (!*user_id) return true;
    if (!prepare(&statement, "SELECT user_id FROM oidc_identities WHERE user_id=?") ||
        !bind_id(statement, 1, *user_id)) goto failure;
    result = db_step(statement);
    db_finalize(statement); statement = NULL;
    if (result != DB_DONE) return false;
    if (!prepare(&statement, "INSERT INTO oidc_identities(issuer,subject,user_id) VALUES(?,?,?)") ||
        !bind_text(statement, 1, groups_issuer) || !bind_text(statement, 2, subject) ||
        !bind_id(statement, 3, *user_id) || db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement);
    return true;
failure:
    db_finalize(statement);
    return false;
}

static bool managed_bind_group(ManagedGroup *group) {
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT group_id FROM oidc_groups WHERE issuer=? AND subject=?") ||
        !bind_text(statement, 1, groups_issuer) || !bind_text(statement, 2, group->subject)) goto failure;
    int result = db_step(statement);
    if (result == DB_ROW) group->local_id = db_column_int64(statement, 0);
    db_finalize(statement); statement = NULL;
    if (result != DB_ROW && result != DB_DONE) return false;
    if (group->local_id) return !group->requested_id || group->requested_id == group->local_id;
    if (group->requested_id) {
        if (!prepare(&statement, "SELECT id FROM organization_groups WHERE id=?") ||
            !bind_id(statement, 1, group->requested_id) || db_step(statement) != DB_ROW) goto failure;
        db_finalize(statement); statement = NULL;
        if (!prepare(&statement, "SELECT group_id FROM oidc_groups WHERE group_id=?") ||
            !bind_id(statement, 1, group->requested_id) || db_step(statement) != DB_DONE) goto failure;
        db_finalize(statement); statement = NULL;
        group->local_id = group->requested_id;
    } else {
        if (!prepare(&statement, "INSERT INTO organization_groups(name,parent_id) VALUES(?,NULL)") ||
            !bind_text(statement, 1, group->name) || db_step(statement) != DB_DONE) goto failure;
        group->local_id = db_last_insert_rowid(database);
        db_finalize(statement); statement = NULL;
        if (group->local_id <= 0) return false;
    }
    if (!prepare(&statement, "INSERT INTO oidc_groups(issuer,subject,group_id,active) VALUES(?,?,?,1)") ||
        !bind_text(statement, 1, groups_issuer) || !bind_text(statement, 2, group->subject) ||
        !bind_id(statement, 3, group->local_id) || db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement);
    return true;
failure:
    db_finalize(statement);
    return false;
}

static bool managed_apply(ManagedGroup *groups, size_t count, json_object *bindings) {
    if (db_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != DB_OK) return false;
    DbStmt *statement = NULL;
    /* An issuer change needs an explicit migration; never mix old membership
       authority with the newly configured provider. */
    if (!prepare(&statement, "SELECT COUNT(*) FROM oidc_groups WHERE issuer<>?") ||
        !bind_text(statement, 1, groups_issuer) || db_step(statement) != DB_ROW ||
        db_column_int64(statement, 0) != 0) goto failure;
    db_finalize(statement); statement = NULL;
    /* Every pre-existing local group must be explicitly adopted. Otherwise
       switching modes would silently leave unrelated authorization behind. */
    if (!prepare(&statement, "SELECT g.id FROM organization_groups g LEFT JOIN oidc_groups o "
                             "ON o.group_id=g.id WHERE o.group_id IS NULL")) goto failure;
    int result;
    while ((result = db_step(statement)) == DB_ROW) {
        DbInt id = db_column_int64(statement, 0);
        bool found = false;
        for (size_t i = 0; i < count; i++) if (groups[i].requested_id == id) found = true;
        if (!found) goto failure;
    }
    db_finalize(statement); statement = NULL;
    if (result != DB_DONE) goto failure;
    for (size_t i = 0; i < count; i++) if (!managed_bind_group(&groups[i])) goto failure;
    if (!prepare(&statement, "UPDATE oidc_groups SET active=0 WHERE issuer=?") ||
        !bind_text(statement, 1, groups_issuer) || db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    if (!prepare(&statement, "DELETE FROM group_members WHERE group_id IN "
                             "(SELECT group_id FROM oidc_groups WHERE issuer=?)") ||
        !bind_text(statement, 1, groups_issuer) || db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    if (!prepare(&statement, "UPDATE organization_groups SET parent_id=NULL WHERE id IN "
                             "(SELECT group_id FROM oidc_groups WHERE issuer=?)") ||
        !bind_text(statement, 1, groups_issuer) || db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    for (size_t i = 0; i < count; i++) {
        ManagedGroup *group = &groups[i];
        DbInt parent = group->parent ? groups[managed_index(groups, count, group->parent)].local_id : 0;
        if (!prepare(&statement, "UPDATE organization_groups SET name=?,parent_id=? WHERE id=?") ||
            !bind_text(statement, 1, group->name) || !nullable_id(statement, 2, parent) ||
            !bind_id(statement, 3, group->local_id) || db_step(statement) != DB_DONE) goto failure;
        db_finalize(statement); statement = NULL;
        if (!prepare(&statement, "UPDATE oidc_groups SET active=1 WHERE issuer=? AND subject=?") ||
            !bind_text(statement, 1, groups_issuer) || !bind_text(statement, 2, group->subject) ||
            db_step(statement) != DB_DONE) goto failure;
        db_finalize(statement); statement = NULL;
        for (size_t j = 0; j < json_object_array_length(group->members); j++) {
            DbInt user_id;
            const char *subject = json_object_get_string(json_object_array_get_idx(group->members, j));
            if (!managed_member(subject, bindings, &user_id)) goto failure;
            if (!user_id) continue;
            if (!prepare(&statement, "INSERT INTO group_members(group_id,user_id) VALUES(?,?)") ||
                !bind_id(statement, 1, group->local_id) || !bind_id(statement, 2, user_id) ||
                db_step(statement) != DB_DONE) goto failure;
            db_finalize(statement); statement = NULL;
        }
    }
    char summary[96];
    (void)snprintf(summary, sizeof(summary), "{\"groups\":%zu,\"source\":\"keycloak\"}", count);
    if (!storage_audit(database, 0, "groups.keycloak_sync", "system", 0, NULL, summary) ||
        db_exec(database, "COMMIT", NULL, NULL, NULL) != DB_OK) goto failure;
    return true;
failure:
    db_finalize(statement); rollback();
    return false;
}

bool organization_sync_groups(void) {
    if (!groups_managed) return true;
    time_t now = time(NULL);
    if (!database || now == (time_t)-1) { groups_sync_ok = false; return false; }
    size_t length = 0;
    char *data = NULL;
    DbStmt *statement = NULL;
    DbInt identity_count = -1;
    if (!prepare(&statement, "SELECT COUNT(*) FROM oidc_identities") || db_step(statement) != DB_ROW)
        goto failure;
    identity_count = db_column_int64(statement, 0);
    db_finalize(statement); statement = NULL;
    /* A successful first SSO may create an identity between scheduler ticks.
       Its groups must be visible immediately, even within the same second. */
    if (groups_last_attempt == now && groups_last_attempt_identity_count == identity_count)
        return organization_groups_available();
    groups_last_attempt = now;
    groups_last_attempt_identity_count = identity_count;
    data = managed_read(&length);
    if (!data) goto failure;
    if (groups_sync_ok && groups_last_snapshot_length == length &&
        groups_last_identity_count == identity_count && !memcmp(groups_last_snapshot, data, length)) {
        free(data);
        return organization_groups_available();
    }
    json_tokener *tokener = json_tokener_new_ex(32);
    if (!tokener) goto failure;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, data, (int)length);
    bool valid = json_tokener_get_error(tokener) == json_tokener_success &&
        json_tokener_get_parse_end(tokener) == length;
    json_tokener_free(tokener);
    ManagedGroup groups[GROUP_LIMIT] = {{0}};
    size_t count = 0;
    json_object *bindings = NULL;
    time_t generated = 0;
    bool applied = valid && managed_validate(root, groups, &count, &bindings, &generated, now) &&
        managed_apply(groups, count, bindings);
    json_object_put(root);
    if (!applied) goto failure;
    free(groups_last_snapshot); groups_last_snapshot = data;
    groups_last_snapshot_length = length;
    /* An explicit legacy membership can create an identity projection. Count
       it now, so the unchanged snapshot does not cause an extra full import. */
    if (!prepare(&statement, "SELECT COUNT(*) FROM oidc_identities") || db_step(statement) != DB_ROW) {
        groups_sync_ok = false; db_finalize(statement); return false;
    }
    groups_last_identity_count = db_column_int64(statement, 0);
    groups_last_attempt_identity_count = groups_last_identity_count;
    db_finalize(statement);
    groups_generated_at = generated; groups_sync_ok = true;
    return true;
failure:
    db_finalize(statement); free(data); groups_sync_ok = false;
    return false;
}
