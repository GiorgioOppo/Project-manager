#ifndef PUNTO_ACCOUNTS_H
#define PUNTO_ACCOUNTS_H

#include <stdbool.h>
#include "db.h"
#include "access_control.h"
#include "../auth/oidc.h"

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
/* Resolve live functional permissions on the account database connection. */
int accounts_access_policy(DbInt id, AccessPolicy *out);
/* Only for a trusted, explicitly configured external identity binding. */
int accounts_lookup_email(const char *email, Account *out);
/* Verified provider identity: 1 resolved, 0 identity/email conflict, -1 failure.
   Persist the provider subject before issuing a session; email never links an
   existing account without an explicit administrator binding. */
int accounts_resolve_oidc(const OidcIdentity *identity, Account *out);
bool accounts_find(DbInt id, Account *out);

/* The caller authenticates requests and checks their origin and content type. */
bool accounts_handle(const char *method, const char *target, const char *query,
                     const char *body, const Account *actor,
                     WorkspaceResponse *response);

#endif
