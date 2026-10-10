#ifndef PUNTO_DB_H
#define PUNTO_DB_H

#ifdef PUNTO_MYSQL

#include <stdint.h>

typedef int64_t DbInt;
typedef struct Db Db;
typedef struct DbStmt DbStmt;
typedef void (*DbDestructor)(void *);

#define DB_OK 0
#define DB_ERROR 1
#define DB_BUSY 5
#define DB_NOMEM 7
#define DB_MISUSE 21
#define DB_CONSTRAINT_UNIQUE 2067
#define DB_ROW 100
#define DB_DONE 101
#define DB_INTEGER 1
#define DB_FLOAT 2
#define DB_TEXT 3
#define DB_BLOB 4
#define DB_NULL 5
#define DB_OPEN_READONLY 0x00000001
#define DB_OPEN_READWRITE 0x00000002
#define DB_OPEN_CREATE 0x00000004
#define DB_OPEN_FULLMUTEX 0x00010000
#define DB_STATIC ((DbDestructor)0)
#define DB_TRANSIENT ((DbDestructor)-1)

int db_open_v2(const char *filename, Db **database, int flags, const char *vfs);
int db_close(Db *database);
int db_busy_timeout(Db *database, int milliseconds);
int db_prepare_v2(Db *database, const char *sql, int bytes, DbStmt **statement,
                  const char **tail);
int db_bind_text(DbStmt *statement, int index, const char *value, int bytes,
                 DbDestructor destroy);
int db_bind_blob(DbStmt *statement, int index, const void *value, int bytes,
                 DbDestructor destroy);
int db_bind_int(DbStmt *statement, int index, int value);
int db_bind_int64(DbStmt *statement, int index, DbInt value);
int db_bind_null(DbStmt *statement, int index);
int db_step(DbStmt *statement);
const unsigned char *db_column_text(DbStmt *statement, int column);
const void *db_column_blob(DbStmt *statement, int column);
int db_column_int(DbStmt *statement, int column);
DbInt db_column_int64(DbStmt *statement, int column);
int db_column_bytes(DbStmt *statement, int column);
int db_column_type(DbStmt *statement, int column);
int db_finalize(DbStmt *statement);
int db_exec(Db *database, const char *sql,
            int (*callback)(void *, int, char **, char **), void *argument,
            char **error_message);
DbInt db_last_insert_rowid(Db *database);
int db_changes(Db *database);
int db_extended_errcode(Db *database);
int db_get_autocommit(Db *database);

#else

#include <sqlite3.h>

typedef sqlite3 Db;
typedef sqlite3_stmt DbStmt;
typedef sqlite3_int64 DbInt;
typedef sqlite3_destructor_type DbDestructor;

#define DB_OK SQLITE_OK
#define DB_ERROR SQLITE_ERROR
#define DB_BUSY SQLITE_BUSY
#define DB_NOMEM SQLITE_NOMEM
#define DB_MISUSE SQLITE_MISUSE
#define DB_CONSTRAINT_UNIQUE SQLITE_CONSTRAINT_UNIQUE
#define DB_ROW SQLITE_ROW
#define DB_DONE SQLITE_DONE
#define DB_INTEGER SQLITE_INTEGER
#define DB_FLOAT SQLITE_FLOAT
#define DB_TEXT SQLITE_TEXT
#define DB_BLOB SQLITE_BLOB
#define DB_NULL SQLITE_NULL
#define DB_OPEN_READONLY SQLITE_OPEN_READONLY
#define DB_OPEN_READWRITE SQLITE_OPEN_READWRITE
#define DB_OPEN_CREATE SQLITE_OPEN_CREATE
#define DB_OPEN_FULLMUTEX SQLITE_OPEN_FULLMUTEX
#define DB_STATIC SQLITE_STATIC
#define DB_TRANSIENT SQLITE_TRANSIENT
#define db_open_v2 sqlite3_open_v2
#define db_close sqlite3_close
#define db_busy_timeout sqlite3_busy_timeout
#define db_prepare_v2 sqlite3_prepare_v2
#define db_bind_text sqlite3_bind_text
#define db_bind_blob sqlite3_bind_blob
#define db_bind_int sqlite3_bind_int
#define db_bind_int64 sqlite3_bind_int64
#define db_bind_null sqlite3_bind_null
#define db_step sqlite3_step
#define db_column_text sqlite3_column_text
#define db_column_blob sqlite3_column_blob
#define db_column_int sqlite3_column_int
#define db_column_int64 sqlite3_column_int64
#define db_column_bytes sqlite3_column_bytes
#define db_column_type sqlite3_column_type
#define db_finalize sqlite3_finalize
#define db_exec sqlite3_exec
#define db_last_insert_rowid sqlite3_last_insert_rowid
#define db_changes sqlite3_changes
#define db_extended_errcode sqlite3_extended_errcode
#define db_get_autocommit sqlite3_get_autocommit

#endif
#endif
