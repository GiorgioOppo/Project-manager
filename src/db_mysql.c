#include "db.h"

#ifdef PUNTO_MYSQL

#include <mysql.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLUMN_LIMIT (16UL * 1024UL * 1024UL)
#define INITIAL_COLUMN_BYTES 256UL

typedef struct {
    unsigned char *data;
    unsigned long length;
    long long integer;
    my_bool is_null;
} Parameter;

typedef struct {
    unsigned char *data;
    unsigned long capacity;
    unsigned long length;
    my_bool is_null;
    my_bool truncated;
    int type;
} Column;

typedef enum { SQL_OTHER, SQL_WRITE, SQL_BEGIN, SQL_COMMIT, SQL_ROLLBACK } SqlKind;

struct Db {
    MYSQL *connection;
    int error;
    int changes;
    DbInt insert_id;
    unsigned int statements;
    int flags;
    int busy_timeout_ms;
    bool in_transaction;
    bool broken;
};

struct DbStmt {
    Db *database;
    MYSQL_STMT *native;
    MYSQL_BIND *parameter_bindings;
    Parameter *parameters;
    unsigned long parameter_count;
    MYSQL_BIND *column_bindings;
    Column *columns;
    unsigned int column_count;
    SqlKind kind;
    bool executed;
    bool row;
    bool done;
    bool failed;
    int error;
};

static void wipe(void *pointer, size_t size) {
    volatile unsigned char *p = pointer;
    while (size--) *p++ = 0;
}

static int mysql_error_code(unsigned int code) {
    if (code == 1062) return DB_CONSTRAINT_UNIQUE;
    /* 3101 is a certified Group Replication rollback. 3098 instead signals
       unsupported schema/DML and must not be treated as a transient conflict. */
    if (code == 1205 || code == 1213 || code == 3101) return DB_BUSY;
    if (code == 2008 || code == 1037 || code == 1038) return DB_NOMEM;
    return DB_ERROR;
}

static bool lost_connection(unsigned int code) {
    return code == 2006 || code == 2013 || code == 2055;
}

/* Never include SQL text, bound values, server error text, or credentials in logs. */
static int connection_failure(Db *database, const char *operation) {
    unsigned int code = database && database->connection ? mysql_errno(database->connection) : 0;
    if (database) {
        database->error = mysql_error_code(code);
        if (lost_connection(code)) database->broken = true;
    }
    fprintf(stderr, "MySQL: %s non riuscita (codice %u).\n", operation, code);
    return database ? database->error : DB_ERROR;
}

static int statement_failure(DbStmt *statement, const char *operation) {
    unsigned int code = mysql_stmt_errno(statement->native);
    statement->error = mysql_error_code(code);
    statement->failed = true;
    statement->row = false;
    statement->database->error = statement->error;
    if (lost_connection(code)) statement->database->broken = true;
    fprintf(stderr, "MySQL: %s non riuscita (codice %u).\n", operation, code);
    return statement->error;
}

static int local_failure(Db *database, int error) {
    if (database) database->error = error;
    return error;
}

static int statement_local_failure(DbStmt *statement, int error) {
    statement->error = error;
    statement->failed = true;
    statement->row = false;
    return local_failure(statement->database, error);
}

static void refresh_transaction(Db *database) {
    unsigned int status = 0;
    if (database && database->connection && !database->broken &&
        mariadb_get_infov(database->connection, MARIADB_CONNECTION_SERVER_STATUS, &status) == 0)
        database->in_transaction = (status & SERVER_STATUS_IN_TRANS) != 0;
}

static bool word_equal(const char *text, size_t length, const char *word) {
    size_t expected = strlen(word);
    if (length != expected) return false;
    for (size_t i = 0; i < length; i++)
        if (toupper((unsigned char)text[i]) != toupper((unsigned char)word[i])) return false;
    return true;
}

static const char *skip_space(const char *sql) {
    while (isspace((unsigned char)*sql)) sql++;
    return sql;
}

static bool first_word(const char *sql, const char *word) {
    sql = skip_space(sql);
    const char *end = sql;
    while (isalpha((unsigned char)*end)) end++;
    return word_equal(sql, (size_t)(end - sql), word);
}

static SqlKind sql_kind(const char *sql) {
    if (first_word(sql, "INSERT") || first_word(sql, "UPDATE") ||
        first_word(sql, "DELETE") || first_word(sql, "REPLACE")) return SQL_WRITE;
    if (first_word(sql, "BEGIN") || first_word(sql, "START")) return SQL_BEGIN;
    if (first_word(sql, "COMMIT")) return SQL_COMMIT;
    if (first_word(sql, "ROLLBACK")) return SQL_ROLLBACK;
    return SQL_OTHER;
}

/* Transform fixed SQL syntax only. All user data still use native parameters. */
static char *normalize_sql(const char *sql, size_t size) {
    const char old[] = "COLLATE NOCASE", replacement[] = "COLLATE utf8mb4_general_ci";
    size_t occurrences = 0;
    char quote = 0;
    for (size_t i = 0; i < size; i++) {
        char c = sql[i];
        if (quote) {
            if (c == '\\' && i + 1 < size) i++;
            else if (c == quote) {
                if (i + 1 < size && sql[i + 1] == quote) i++;
                else quote = 0;
            }
        } else if (c == '\'' || c == '"' || c == '`') quote = c;
        else if (size - i >= sizeof(old) - 1 &&
                 word_equal(sql + i, sizeof(old) - 1, old)) {
            occurrences++;
            i += sizeof(old) - 2;
        }
    }
    size_t extra = sizeof(replacement) - sizeof(old);
    if (occurrences > (SIZE_MAX - size - 1) / extra) return NULL;
    size_t capacity = size + occurrences * extra + 1;
    char *result = malloc(capacity);
    if (!result) return NULL;
    size_t used = 0;
    quote = 0;
    for (size_t i = 0; i < size; i++) {
        char c = sql[i];
        if (quote) {
            result[used++] = c;
            if (c == '\\' && i + 1 < size) result[used++] = sql[++i];
            else if (c == quote) {
                if (i + 1 < size && sql[i + 1] == quote) result[used++] = sql[++i];
                else quote = 0;
            }
        } else if (c == '\'' || c == '"' || c == '`') {
            quote = c; result[used++] = c;
        } else if (size - i >= sizeof(old) - 1 &&
                   word_equal(sql + i, sizeof(old) - 1, old)) {
            memcpy(result + used, replacement, sizeof(replacement) - 1);
            used += sizeof(replacement) - 1;
            i += sizeof(old) - 2;
        } else result[used++] = c;
    }
    result[used] = '\0';
    /* BEGIN IMMEDIATE serializes SQLite writers. MySQL uses transaction locks. */
    const char *start = skip_space(result);
    size_t length = strlen(start);
    while (length && (isspace((unsigned char)start[length - 1]) || start[length - 1] == ';')) length--;
    if (word_equal(start, length, "BEGIN IMMEDIATE")) {
        free(result);
        result = malloc(sizeof("START TRANSACTION"));
        if (result) memcpy(result, "START TRANSACTION", sizeof("START TRANSACTION"));
    }
    return result;
}

static void record_execution(Db *database, SqlKind kind, my_ulonglong affected,
                              my_ulonglong insert_id) {
    database->error = DB_OK;
    if (kind == SQL_WRITE) {
        database->changes = affected > INT_MAX ? INT_MAX : (int)affected;
        if (insert_id && insert_id <= INT64_MAX) database->insert_id = (DbInt)insert_id;
    }
    if (kind == SQL_BEGIN) database->in_transaction = true;
    else if (kind == SQL_COMMIT || kind == SQL_ROLLBACK) database->in_transaction = false;
    /* DDL and deadlock handling can end a transaction implicitly on MySQL. */
    refresh_transaction(database);
}

static int open_connection(Db *database) {
    const char *host = getenv("DB_HOST"), *port_text = getenv("DB_PORT");
    const char *name = getenv("DB_NAME"), *user = getenv("DB_USER");
    const char *password = getenv("DB_PASSWORD");
    const char *tls_mode = getenv("DB_TLS_MODE"), *ca_path = getenv("DB_SSL_CA");
    if (!host || !*host) host = "db";
    if (!name || !*name) name = "punto";
    if (!user || !*user) user = "punto";
    if (!password || !*password) {
        fprintf(stderr, "MySQL: DB_PASSWORD è obbligatoria.\n");
        return DB_ERROR;
    }
    /* Connector/C 3.3 permits an authentication fallback without TLS when only
       SSL_ENFORCE is set. Requiring identity verification rejects that fallback
       before sending authentication material, as well as validating the peer. */
    if (!tls_mode || !*tls_mode) tls_mode = "verify_identity";
    my_bool enforce_tls = 1, verify_identity = 1;
    if (strcmp(tls_mode, "verify_identity") != 0) {
        fprintf(stderr, "MySQL: DB_TLS_MODE deve essere verify_identity.\n");
        return DB_ERROR;
    }
    if (!ca_path || !*ca_path) {
        fprintf(stderr, "MySQL: DB_SSL_CA è obbligatoria per verify_identity.\n");
        return DB_ERROR;
    }
    {
        /* Reject missing, empty, or unreadable trust stores before authentication.
           Connector/C validates the PEM certificates during the TLS handshake. */
        FILE *ca_file = ca_path[0] == '/' ? fopen(ca_path, "rb") : NULL;
        bool readable = ca_file && fgetc(ca_file) != EOF;
        if (ca_file) fclose(ca_file);
        if (!readable) {
            fprintf(stderr, "MySQL: DB_SSL_CA deve essere un file assoluto leggibile e non vuoto.\n");
            return DB_ERROR;
        }
    }
    unsigned int port = 3306;
    if (port_text && *port_text) {
        unsigned long parsed = 0;
        for (const char *p = port_text; *p; p++) {
            if (*p < '0' || *p > '9' || parsed > 65535UL / 10UL) {
                fprintf(stderr, "MySQL: DB_PORT non valida.\n"); return DB_ERROR;
            }
            parsed = parsed * 10UL + (unsigned long)(*p - '0');
        }
        if (!parsed || parsed > 65535UL) {
            fprintf(stderr, "MySQL: DB_PORT non valida.\n"); return DB_ERROR;
        }
        port = (unsigned int)parsed;
    }
    database->connection = mysql_init(NULL);
    if (!database->connection) return local_failure(database, DB_NOMEM);
    my_bool reconnect = 0, report_truncation = 1;
    unsigned int connect_timeout = 10, io_timeout = 15;
    unsigned int protocol = MYSQL_PROTOCOL_TCP;
    if (mysql_options(database->connection, MYSQL_OPT_RECONNECT, &reconnect) ||
        mysql_options(database->connection, MYSQL_REPORT_DATA_TRUNCATION, &report_truncation) ||
        mysql_options(database->connection, MYSQL_SET_CHARSET_NAME, "utf8mb4") ||
        mysql_options(database->connection, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout) ||
        mysql_options(database->connection, MYSQL_OPT_READ_TIMEOUT, &io_timeout) ||
        mysql_options(database->connection, MYSQL_OPT_WRITE_TIMEOUT, &io_timeout) ||
        mysql_options(database->connection, MYSQL_OPT_PROTOCOL, &protocol) ||
        mysql_options(database->connection, MYSQL_OPT_SSL_ENFORCE, &enforce_tls) ||
        mysql_options(database->connection, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify_identity) ||
        mysql_options(database->connection, MARIADB_OPT_TLS_VERSION, "TLSv1.2,TLSv1.3") ||
        mysql_options(database->connection, MYSQL_OPT_SSL_CA, ca_path) ||
        !mysql_real_connect(database->connection, host, user, password, name,
                            port, NULL, CLIENT_FOUND_ROWS)) {
        int error = connection_failure(database, "connessione");
        mysql_close(database->connection); database->connection = NULL;
        return error;
    }
    /* Defence in depth against connector/version behaviour: never execute any
       application query on a connection that did not negotiate TLS. */
    const char *cipher = mysql_get_ssl_cipher(database->connection);
    if (!cipher || !*cipher) {
        fprintf(stderr, "MySQL: connessione rifiutata perché TLS non è attivo.\n");
        mysql_close(database->connection); database->connection = NULL;
        return local_failure(database, DB_ERROR);
    }
    unsigned int seconds = database->busy_timeout_ms > 0 ?
        (unsigned int)((database->busy_timeout_ms - 1) / 1000 + 1) : 1;
    char timeout_sql[96];
    (void)snprintf(timeout_sql, sizeof(timeout_sql),
                   "SET SESSION innodb_lock_wait_timeout=%u", seconds);
    if (mysql_options(database->connection, MYSQL_OPT_RECONNECT, &reconnect) ||
        mysql_set_character_set(database->connection, "utf8mb4") ||
        mysql_query(database->connection, "SET SESSION time_zone='+00:00'") ||
        mysql_query(database->connection, "SET SESSION autocommit=1") ||
        mysql_query(database->connection, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED") ||
        mysql_query(database->connection, timeout_sql) ||
        ((database->flags & DB_OPEN_READONLY) &&
         mysql_query(database->connection, "SET SESSION TRANSACTION READ ONLY"))) {
        int error = connection_failure(database, "configurazione sessione");
        mysql_close(database->connection); database->connection = NULL;
        return error;
    }
    database->in_transaction = false;
    database->broken = false;
    database->error = DB_OK;
    database->changes = 0;
    database->insert_id = 0;
    return DB_OK;
}

/* Recovery occurs only between statements and outside a transaction. A failed
   command is never replayed, including a COMMIT whose outcome is uncertain. */
static int ensure_connection(Db *database) {
    if (!database) return DB_MISUSE;
    if (database->statements || database->in_transaction)
        return database->connection && !database->broken ? DB_OK :
            local_failure(database, DB_ERROR);
    if (!database->broken && database->connection) {
        if (mysql_ping(database->connection) == 0) return DB_OK;
        (void)connection_failure(database, "verifica connessione");
        database->broken = true;
    }
    if (database->connection) {
        mysql_close(database->connection);
        database->connection = NULL;
    }
    database->broken = true;
    return open_connection(database);
}

/* Every application transaction writes the same guard row. Local row locks
   serialize one member's writers; Group Replication certifies this shared
   write across primaries, rolling back conflicting transactions in full.
   READ COMMITTED then observes the latest data after acquiring the guard.
   Never replay a failed transaction or an ambiguously acknowledged COMMIT. */
static int guard_write_transaction(Db *database) {
    static const char sql[] =
        "UPDATE cluster_write_guard SET generation=generation+1 WHERE id=1";
    int error = DB_OK;
    if (mysql_real_query(database->connection, sql, sizeof(sql) - 1))
        error = connection_failure(database, "acquisizione protezione scritture");
    else if (mysql_affected_rows(database->connection) != 1) {
        fprintf(stderr, "MySQL: protezione scritture non inizializzata.\n");
        error = local_failure(database, DB_ERROR);
    }
    if (error != DB_OK) {
        /* db_exec discards a broken connection instead of reconnecting for
           ROLLBACK. Preserve the guard failure if cleanup changes last error. */
        (void)db_exec(database, "ROLLBACK", NULL, NULL, NULL);
        return local_failure(database, error);
    }
    refresh_transaction(database);
    database->error = DB_OK;
    return DB_OK;
}

int db_open_v2(const char *filename, Db **out, int flags, const char *vfs) {
    (void)filename;
    (void)vfs;
    if (!out) return DB_MISUSE;
    *out = NULL;
    Db *database = calloc(1, sizeof(*database));
    if (!database) return DB_NOMEM;
    database->flags = flags;
    database->busy_timeout_ms = 5000;
    int result = open_connection(database);
    if (result != DB_OK) { free(database); return result; }
    *out = database;
    return DB_OK;
}

int db_close(Db *database) {
    if (!database) return DB_OK;
    if (database->statements) return local_failure(database, DB_BUSY);
    if (database->connection) mysql_close(database->connection);
    free(database);
    return DB_OK;
}

int db_busy_timeout(Db *database, int milliseconds) {
    if (!database || milliseconds < 0) return DB_MISUSE;
    unsigned int seconds = milliseconds > 0 ? (unsigned int)((milliseconds - 1) / 1000 + 1) : 1;
    char sql[96];
    (void)snprintf(sql, sizeof(sql), "SET SESSION innodb_lock_wait_timeout=%u", seconds);
    int result = db_exec(database, sql, NULL, NULL, NULL);
    if (result == DB_OK) database->busy_timeout_ms = milliseconds;
    return result;
}

static void free_statement(DbStmt *statement) {
    if (!statement) return;
    if (statement->native) mysql_stmt_close(statement->native);
    if (statement->parameters) {
        for (unsigned long i = 0; i < statement->parameter_count; i++) {
            if (statement->parameters[i].data) {
                wipe(statement->parameters[i].data, statement->parameters[i].length);
                free(statement->parameters[i].data);
            }
        }
    }
    if (statement->columns)
        for (unsigned int i = 0; i < statement->column_count; i++) free(statement->columns[i].data);
    free(statement->parameters); free(statement->parameter_bindings);
    free(statement->columns); free(statement->column_bindings);
    free(statement);
}

int db_prepare_v2(Db *database, const char *sql, int bytes, DbStmt **out,
                  const char **tail) {
    if (out) *out = NULL;
    if (tail) *tail = sql;
    if (!database || !sql || !out) return local_failure(database, DB_MISUSE);
    int connection = ensure_connection(database);
    if (connection != DB_OK) return connection;
    size_t length = bytes < 0 ? strlen(sql) : strnlen(sql, (size_t)bytes);
    if (!length || length > ULONG_MAX) return local_failure(database, DB_MISUSE);
    char *normalized = normalize_sql(sql, length);
    if (!normalized) return local_failure(database, DB_NOMEM);
    DbStmt *statement = calloc(1, sizeof(*statement));
    if (!statement) { free(normalized); return local_failure(database, DB_NOMEM); }
    statement->database = database;
    statement->kind = sql_kind(normalized);
    statement->native = mysql_stmt_init(database->connection);
    if (!statement->native) {
        free(normalized); free_statement(statement); return local_failure(database, DB_NOMEM);
    }
    if (mysql_stmt_prepare(statement->native, normalized, (unsigned long)strlen(normalized))) {
        int error = statement_failure(statement, "preparazione query");
        free(normalized); free_statement(statement); return error;
    }
    free(normalized);
    statement->parameter_count = mysql_stmt_param_count(statement->native);
    if (statement->parameter_count) {
        statement->parameters = calloc(statement->parameter_count, sizeof(*statement->parameters));
        statement->parameter_bindings = calloc(statement->parameter_count, sizeof(*statement->parameter_bindings));
        if (!statement->parameters || !statement->parameter_bindings) {
            free_statement(statement); return local_failure(database, DB_NOMEM);
        }
        for (unsigned long i = 0; i < statement->parameter_count; i++) {
            statement->parameters[i].is_null = 1;
            statement->parameter_bindings[i].buffer_type = MYSQL_TYPE_NULL;
            statement->parameter_bindings[i].is_null = &statement->parameters[i].is_null;
        }
    }
    database->statements++;
    database->error = DB_OK;
    if (tail) *tail = sql + length;
    *out = statement;
    return DB_OK;
}

static bool valid_parameter(DbStmt *statement, int index) {
    return statement && !statement->executed && index >= 1 &&
           (unsigned long)index <= statement->parameter_count;
}

static void clear_parameter(DbStmt *statement, int index) {
    Parameter *parameter = &statement->parameters[index - 1];
    if (parameter->data) { wipe(parameter->data, parameter->length); free(parameter->data); }
    memset(parameter, 0, sizeof(*parameter));
    memset(&statement->parameter_bindings[index - 1], 0, sizeof(MYSQL_BIND));
    statement->parameter_bindings[index - 1].is_null = &parameter->is_null;
}

static int bind_bytes(DbStmt *statement, int index, const void *value, int bytes,
                       bool blob, DbDestructor destroy) {
    if (!valid_parameter(statement, index))
        return local_failure(statement ? statement->database : NULL, DB_MISUSE);
    if (!value) return db_bind_null(statement, index);
    if (blob && bytes < 0) return local_failure(statement->database, DB_MISUSE);
    size_t length = bytes < 0 ? strlen(value) : (size_t)bytes;
    if (length > ULONG_MAX || length == SIZE_MAX) return local_failure(statement->database, DB_NOMEM);
    unsigned char *copy = malloc(length + 1);
    if (!copy) return local_failure(statement->database, DB_NOMEM);
    memcpy(copy, value, length); copy[length] = '\0';
    clear_parameter(statement, index);
    Parameter *parameter = &statement->parameters[index - 1];
    MYSQL_BIND *binding = &statement->parameter_bindings[index - 1];
    parameter->data = copy; parameter->length = (unsigned long)length;
    binding->buffer_type = blob ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
    binding->buffer = copy; binding->buffer_length = (unsigned long)length;
    binding->length = &parameter->length;
    if (destroy && destroy != DB_TRANSIENT) destroy((void *)value);
    statement->database->error = DB_OK;
    return DB_OK;
}

int db_bind_text(DbStmt *statement, int index, const char *value, int bytes,
                 DbDestructor destroy) {
    return bind_bytes(statement, index, value, bytes, false, destroy);
}

int db_bind_blob(DbStmt *statement, int index, const void *value, int bytes,
                 DbDestructor destroy) {
    return bind_bytes(statement, index, value, bytes, true, destroy);
}

int db_bind_int64(DbStmt *statement, int index, DbInt value) {
    if (!valid_parameter(statement, index))
        return local_failure(statement ? statement->database : NULL, DB_MISUSE);
    clear_parameter(statement, index);
    Parameter *parameter = &statement->parameters[index - 1];
    MYSQL_BIND *binding = &statement->parameter_bindings[index - 1];
    parameter->integer = (long long)value;
    binding->buffer_type = MYSQL_TYPE_LONGLONG;
    binding->buffer = &parameter->integer;
    binding->buffer_length = sizeof(parameter->integer);
    binding->is_unsigned = 0;
    statement->database->error = DB_OK;
    return DB_OK;
}

int db_bind_int(DbStmt *statement, int index, int value) {
    return db_bind_int64(statement, index, value);
}

int db_bind_null(DbStmt *statement, int index) {
    if (!valid_parameter(statement, index))
        return local_failure(statement ? statement->database : NULL, DB_MISUSE);
    clear_parameter(statement, index);
    Parameter *parameter = &statement->parameters[index - 1];
    parameter->is_null = 1;
    statement->parameter_bindings[index - 1].buffer_type = MYSQL_TYPE_NULL;
    statement->database->error = DB_OK;
    return DB_OK;
}

static int column_type(enum enum_field_types type, unsigned int charset) {
    switch (type) {
        case MYSQL_TYPE_TINY: case MYSQL_TYPE_SHORT: case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG: case MYSQL_TYPE_INT24: case MYSQL_TYPE_YEAR:
            return DB_INTEGER;
        case MYSQL_TYPE_DECIMAL: case MYSQL_TYPE_NEWDECIMAL: case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE: return DB_FLOAT;
        case MYSQL_TYPE_NULL: return DB_NULL;
        default: return charset == 63 ? DB_BLOB : DB_TEXT;
    }
}

static int prepare_columns(DbStmt *statement) {
    statement->column_count = mysql_stmt_field_count(statement->native);
    if (!statement->column_count) return DB_OK;
    MYSQL_RES *metadata = mysql_stmt_result_metadata(statement->native);
    if (!metadata) return statement_failure(statement, "lettura metadati");
    statement->columns = calloc(statement->column_count, sizeof(*statement->columns));
    statement->column_bindings = calloc(statement->column_count, sizeof(*statement->column_bindings));
    if (!statement->columns || !statement->column_bindings) {
        mysql_free_result(metadata); return statement_local_failure(statement, DB_NOMEM);
    }
    MYSQL_FIELD *fields = mysql_fetch_fields(metadata);
    if (!fields) { mysql_free_result(metadata); return statement_local_failure(statement, DB_ERROR); }
    for (unsigned int i = 0; i < statement->column_count; i++) {
        Column *column = &statement->columns[i];
        MYSQL_BIND *binding = &statement->column_bindings[i];
        column->capacity = INITIAL_COLUMN_BYTES;
        column->data = malloc(column->capacity);
        if (!column->data) { mysql_free_result(metadata); return statement_local_failure(statement, DB_NOMEM); }
        column->data[0] = '\0';
        column->type = column_type(fields[i].type, fields[i].charsetnr);
        /* STRING output converts numbers to decimal text and preserves BLOB bytes.
           The returned length, rather than strlen(), is used for binary values. */
        binding->buffer_type = MYSQL_TYPE_STRING;
        binding->buffer = column->data;
        binding->buffer_length = column->capacity - 1;
        binding->length = &column->length;
        binding->is_null = &column->is_null;
        binding->error = &column->truncated;
    }
    mysql_free_result(metadata);
    if (mysql_stmt_bind_result(statement->native, statement->column_bindings))
        return statement_failure(statement, "associazione risultati");
    return DB_OK;
}

int db_step(DbStmt *statement) {
    if (!statement) return DB_MISUSE;
    statement->row = false;
    if (statement->failed) return statement->error;
    if (statement->done) return DB_DONE;
    if (!statement->executed) {
        /* START TRANSACTION would implicitly commit an existing transaction
           on MySQL. Match db_exec and refuse a nested prepared BEGIN too. */
        if (statement->kind == SQL_BEGIN && statement->database->in_transaction)
            return statement_local_failure(statement, DB_ERROR);
        statement->executed = true;
        if (statement->parameter_count &&
            mysql_stmt_bind_param(statement->native, statement->parameter_bindings))
            return statement_failure(statement, "associazione parametri");
        if (mysql_stmt_execute(statement->native)) return statement_failure(statement, "esecuzione query");
        record_execution(statement->database, statement->kind,
            mysql_stmt_affected_rows(statement->native), mysql_stmt_insert_id(statement->native));
        if (statement->kind == SQL_BEGIN) {
            int guarded = guard_write_transaction(statement->database);
            if (guarded != DB_OK) return statement_local_failure(statement, guarded);
        }
        if (!mysql_stmt_field_count(statement->native)) {
            statement->done = true; return DB_DONE;
        }
        if (mysql_stmt_store_result(statement->native)) return statement_failure(statement, "lettura risultati");
        int result = prepare_columns(statement);
        if (result != DB_OK) return result;
    }
    for (unsigned int i = 0; i < statement->column_count; i++) {
        statement->columns[i].is_null = 0;
        statement->columns[i].truncated = 0;
        statement->columns[i].length = 0;
    }
    int result = mysql_stmt_fetch(statement->native);
    if (result == MYSQL_NO_DATA) { statement->done = true; return DB_DONE; }
    if (result != 0 && result != MYSQL_DATA_TRUNCATED) return statement_failure(statement, "lettura riga");
    bool rebind = false;
    for (unsigned int i = 0; i < statement->column_count; i++) {
        Column *column = &statement->columns[i];
        MYSQL_BIND *binding = &statement->column_bindings[i];
        if (column->is_null) { column->length = 0; column->data[0] = '\0'; continue; }
        if (column->length > COLUMN_LIMIT) return statement_local_failure(statement, DB_NOMEM);
        if (column->length >= column->capacity) {
            unsigned long capacity = column->length + 1;
            unsigned char *grown = realloc(column->data, capacity);
            if (!grown) return statement_local_failure(statement, DB_NOMEM);
            column->data = grown; column->capacity = capacity;
            binding->buffer = grown; binding->buffer_length = capacity - 1;
            column->truncated = 0;
            if (mysql_stmt_fetch_column(statement->native, binding, i, 0))
                return statement_failure(statement, "lettura colonna");
            rebind = true;
        }
        if (column->truncated) return statement_local_failure(statement, DB_ERROR);
        column->data[column->length] = '\0';
    }
    /* Reallocation changes bound pointers; refresh them before the next fetch. */
    if (rebind && mysql_stmt_bind_result(statement->native, statement->column_bindings))
        return statement_failure(statement, "aggiornamento risultati");
    statement->row = true;
    statement->database->error = DB_OK;
    return DB_ROW;
}

static Column *get_column(DbStmt *statement, int index) {
    return statement && statement->row && index >= 0 && (unsigned int)index < statement->column_count ?
        &statement->columns[index] : NULL;
}

const unsigned char *db_column_text(DbStmt *statement, int index) {
    Column *column = get_column(statement, index);
    return column && !column->is_null ? column->data : NULL;
}

const void *db_column_blob(DbStmt *statement, int index) {
    return db_column_text(statement, index);
}

DbInt db_column_int64(DbStmt *statement, int index) {
    const unsigned char *value = db_column_text(statement, index);
    if (!value) return 0;
    errno = 0;
    char *end;
    long long number = strtoll((const char *)value, &end, 10);
    if (end == (const char *)value) return 0;
    if (errno == ERANGE) return number < 0 ? INT64_MIN : INT64_MAX;
    return (DbInt)number;
}

int db_column_int(DbStmt *statement, int index) {
    DbInt value = db_column_int64(statement, index);
    return value > INT_MAX ? INT_MAX : value < INT_MIN ? INT_MIN : (int)value;
}

int db_column_bytes(DbStmt *statement, int index) {
    Column *column = get_column(statement, index);
    return column && !column->is_null ? (int)column->length : 0;
}

int db_column_type(DbStmt *statement, int index) {
    Column *column = get_column(statement, index);
    return !column || column->is_null ? DB_NULL : column->type;
}

int db_finalize(DbStmt *statement) {
    if (!statement) return DB_OK;
    Db *database = statement->database;
    int result = statement->failed ? statement->error : DB_OK;
    if (database && database->statements) database->statements--;
    /* Keep the connection's last error available after finalization, like SQLite. */
    free_statement(statement);
    return result;
}

int db_exec(Db *database, const char *sql,
            int (*callback)(void *, int, char **, char **), void *argument,
            char **error_message) {
    if (error_message) *error_message = NULL;
    if (!database || !sql) return local_failure(database, DB_MISUSE);
    const char *start = skip_space(sql);
    if (first_word(start, "PRAGMA") &&
        (strstr(start, "foreign_keys") || strstr(start, "busy_timeout"))) return DB_OK;
    char *normalized = normalize_sql(sql, strlen(sql));
    if (!normalized) return local_failure(database, DB_NOMEM);
    SqlKind kind = sql_kind(normalized);
    if (kind == SQL_ROLLBACK && (database->broken || !database->connection)) {
        /* Closing a lost connection abandons its transaction. Do not reconnect
           just to send ROLLBACK, and do not turn an uncertain COMMIT into a retry. */
        database->in_transaction = false;
        database->broken = true;
        if (!database->statements && database->connection) {
            mysql_close(database->connection);
            database->connection = NULL;
        }
        free(normalized);
        return local_failure(database, DB_ERROR);
    }
    if (kind == SQL_BEGIN && database->in_transaction) {
        free(normalized);
        return local_failure(database, DB_ERROR);
    }
    int connection = ensure_connection(database);
    if (connection != DB_OK) { free(normalized); return connection; }
    /* CLIENT_MULTI_STATEMENTS is disabled: exactly one server statement is accepted. */
    int query_result = mysql_real_query(database->connection, normalized, (unsigned long)strlen(normalized));
    free(normalized);
    if (query_result) {
        int error = connection_failure(database, "esecuzione comando");
        if (kind == SQL_ROLLBACK) {
            /* ROLLBACK itself can lose the connection. Discard it so a stale
               transaction flag cannot prevent recovery on the next request. */
            database->in_transaction = false;
            database->broken = true;
            if (!database->statements && database->connection) {
                mysql_close(database->connection);
                database->connection = NULL;
            }
        }
        if (error_message) {
            const char generic[] = "Errore del database MySQL.";
            *error_message = malloc(sizeof(generic));
            if (*error_message) memcpy(*error_message, generic, sizeof(generic));
        }
        return error;
    }
    record_execution(database, kind, mysql_affected_rows(database->connection),
                      mysql_insert_id(database->connection));
    if (kind == SQL_BEGIN) {
        int guarded = guard_write_transaction(database);
        if (guarded != DB_OK) {
            if (error_message) {
                const char generic[] = "Errore del database MySQL.";
                *error_message = malloc(sizeof(generic));
                if (*error_message) memcpy(*error_message, generic, sizeof(generic));
            }
            return guarded;
        }
    }
    MYSQL_RES *result = mysql_store_result(database->connection);
    if (!result) return mysql_field_count(database->connection) ?
        connection_failure(database, "lettura comando") : DB_OK;
    int status = DB_OK;
    if (callback) {
        unsigned int count = mysql_num_fields(result);
        MYSQL_FIELD *fields = mysql_fetch_fields(result);
        char **names = calloc(count, sizeof(*names));
        if (!names || !fields || count > INT_MAX) status = local_failure(database, DB_NOMEM);
        else {
            for (unsigned int i = 0; i < count; i++) names[i] = fields[i].name;
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result))) {
                if (callback(argument, (int)count, row, names)) {
                    status = local_failure(database, DB_ERROR); break;
                }
            }
            if (status == DB_OK && mysql_errno(database->connection))
                status = connection_failure(database, "lettura comando");
        }
        free(names);
    }
    mysql_free_result(result);
    return status;
}

DbInt db_last_insert_rowid(Db *database) {
    return database ? database->insert_id : 0;
}

int db_changes(Db *database) {
    return database ? database->changes : 0;
}

int db_extended_errcode(Db *database) {
    return database ? database->error : DB_MISUSE;
}

int db_get_autocommit(Db *database) {
    refresh_transaction(database);
    return !database || !database->in_transaction;
}

#endif
