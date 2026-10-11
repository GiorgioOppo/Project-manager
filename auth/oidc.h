#ifndef PUNTO_OIDC_H
#define PUNTO_OIDC_H

#include <stdbool.h>

/* Optional, server-side OIDC authentication. Configuration is immutable after
   initialization. Legacy identities select an explicitly bound local email;
   managed identities carry signed claims and immutable issuer + subject.
   Application account status and permissions remain the caller's responsibility.
   The module is designed for the applications' single-threaded request loops. */
bool oidc_init(void);
bool oidc_enabled(void);
bool oidc_local_enabled(void);
/* Immutable, validated callback URL; empty when OIDC is disabled. */
const char *oidc_redirect_uri(void);

typedef struct {
    char issuer[1024];
    char subject[256];
    char email[255];
    char name[121];
    char role[16];
    char bound_email[255];
} OidcIdentity;

/* Managed identities are resolved by immutable issuer + subject in the caller.
   role contains the configured OIDC_ADMIN_ROLE or OIDC_USER_ROLE. bound_email
   is optional and only represents an explicit legacy administrator binding.
   Signed email claims must never automatically select an existing account. */
int oidc_users_managed(void);
int oidc_local_account_allowed(const char *email);
const char *oidc_users_admin_url(void);
/* Available in managed mode only (otherwise 404). As with oidc_callback, the
   caller clears the transaction cookie and issues its own application session. */
int oidc_callback_identity(const char *query, const char *cookie, OidcIdentity *out, int *lifetime);

/* Populate a redirect destination and a complete Set-Cookie header (including
   CRLF). Return 200 on success or an HTTP error status. No tokens leave the
   module. The caller redirects only on success. */
int oidc_start(char location[8192], char cookie[512]);

/* Consume a browser-bound, encrypted-cookie transaction after verifying its ID
   token and resolve its subject through OIDC_BINDINGS_FILE. Return 200 with a canonical
   local email and bounded session lifetime, or an HTTP error status. The caller
   must verify that this local account is active before issuing its own session.
   Clear the transaction cookie on every callback, including errors. Starts use
   no shared pending slots. Verified IdP responses retain replay tombstones until
   cookie expiry; a quota limits only the authenticated subject exceeding it. */
int oidc_callback(const char *query, const char *cookie, char email[255], int *lifetime);
const char *oidc_clear_cookie(void);
void oidc_close(void);

#endif
