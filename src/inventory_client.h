#ifndef PUNTO_INVENTORY_CLIENT_H
#define PUNTO_INVENTORY_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include "db.h"

typedef enum {
    INVENTORY_STATUS,
    INVENTORY_ASSETS,
    INVENTORY_DETAIL,
    INVENTORY_HISTORY,
    INVENTORY_CREATE,
    INVENTORY_UPDATE,
    INVENTORY_RELATIONS,
    INVENTORY_RELATION_CREATE,
    INVENTORY_RELATION_REMOVE,
    INVENTORY_EXPORT,
    INVENTORY_IMPORT_PREVIEW,
    INVENTORY_IMPORT_COMMIT,
    INVENTORY_DUE,
    INVENTORY_PLACES,
    INVENTORY_PLACE_CREATE,
    INVENTORY_PLACE_UPDATE,
    INVENTORY_CONTEXT,
    INVENTORY_CHECKOUT,
    INVENTORY_CHECKIN,
    INVENTORY_VERIFY,
    INVENTORY_CUSTODY,
    INVENTORY_VERIFICATIONS,
    INVENTORY_DASHBOARD,
    INVENTORY_OPERATIONAL_DUE,
    INVENTORY_PEOPLE,
    INVENTORY_PERSON_CREATE,
    INVENTORY_PERSON_DETAIL,
    INVENTORY_PERSON_UPDATE,
    INVENTORY_PERSON_HISTORY,
    INVENTORY_PERSON_IDENTITIES,
    INVENTORY_PERSON_ASSETS,
    INVENTORY_IDENTITIES,
    INVENTORY_IDENTITY_CREATE,
    INVENTORY_IDENTITY_DETAIL,
    INVENTORY_IDENTITY_UPDATE,
    INVENTORY_IDENTITY_HISTORY,
    INVENTORY_IDENTITY_PEOPLE,
    INVENTORY_IDENTITY_PERSON_CREATE,
    INVENTORY_IDENTITY_PERSON_REMOVE,
    INVENTORY_IDENTITY_ASSETS,
    INVENTORY_SUBJECTS,
    INVENTORY_SUBJECT_CREATE,
    INVENTORY_SUBJECT_REMOVE
} InventoryOperation;

typedef struct { int status; char *body; } InventoryResponse;

/* Omitted configuration is allowed for native development. Partial config,
   public destinations, and unverified TLS are rejected at startup. */
bool inventory_client_open(void);
void inventory_client_close(void);
bool inventory_client_enabled(void);
/* The server supplies item_id and relation_id from the route, actor_id from the current
   authenticated account. Browser fields can never override actor_id. */
void inventory_client_call(InventoryOperation operation, DbInt item_id, DbInt relation_id,
                           DbInt actor_id, const char *input,
                           InventoryResponse *response);
/* Resolve active app accounts only after the complete browser form has been
   validated. Return 1 for an active account, 0 if absent/inactive, -1 on error. */
typedef int (*InventoryUserLookup)(DbInt user_id, char *name, size_t capacity);
void inventory_client_call_with_lookup(InventoryOperation operation, DbInt item_id,
                           DbInt relation_id, DbInt actor_id, const char *input,
                           InventoryUserLookup lookup, InventoryResponse *response);
void inventory_response_free(InventoryResponse *response);
struct json_object;
/* Fetch a complete, bounded deadline set; failures never return partial data. */
bool inventory_client_due(const char *today, int days_before, struct json_object **items);
bool inventory_client_operational_due(const char *today, const char *kind,
                                     int verification_days, struct json_object **items);

#endif
