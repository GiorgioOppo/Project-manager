#ifndef PUNTO_ACCOUNTS_H
#define PUNTO_ACCOUNTS_H

#include <stdbool.h>
#include "db.h"

#include "workspace.h"

typedef struct {
    DbInt id;
    char email[255];
    /* A name contains at most 120 UTF-8 characters (four bytes each). */
    char name[481];
    char role[16];
} Account;

/* The bootstrap credentials are used only when the users table is empty. */
bool accounts_open(const char *db_path, const char *bootstrap_email,
                   const char *bootstrap_password);
void accounts_close(void);

/* Returns 1 for an active account, 0 for invalid credentials, -1 on failure. */
int accounts_authenticate(const char *email, const char *password, Account *out);
/* Active account: 1; missing/inactive: 0; database or row error: -1. */
int accounts_lookup(DbInt id, Account *out);
bool accounts_find(DbInt id, Account *out);

/* The caller authenticates requests and checks their origin and content type. */
bool accounts_handle(const char *method, const char *target, const char *query,
                     const char *body, const Account *actor,
                     WorkspaceResponse *response);

#endif
