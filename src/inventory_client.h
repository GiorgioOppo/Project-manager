#ifndef PUNTO_INVENTORY_CLIENT_H
#define PUNTO_INVENTORY_CLIENT_H

#include <stdbool.h>
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
    INVENTORY_DUE
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
void inventory_response_free(InventoryResponse *response);
struct json_object;
/* Fetch a complete, bounded deadline set; failures never return partial data. */
bool inventory_client_due(const char *today, int days_before, struct json_object **items);

#endif
