#ifndef PUNTO_PROCUREMENT_H
#define PUNTO_PROCUREMENT_H
#include <stddef.h>
#include "workspace.h"

bool procurement_open(const char *db_path);
void procurement_close(void);
bool procurement_handle(const char *method, const char *target, const char *query,
                        const char *body, DbInt actor_id, const char *role,
                        WorkspaceResponse *response);
void procurement_upload(DbInt contract_id, const char *filename,
                        const unsigned char *data, size_t size, DbInt actor_id,
                        const char *role, WorkspaceResponse *response);
int procurement_download(DbInt document_id, DbInt actor_id, const char *role,
                         unsigned char **data, size_t *size, char filename[181]);
#endif
