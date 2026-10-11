#ifndef PUNTO_WORK_BOARD_H
#define PUNTO_WORK_BOARD_H

#include "workspace.h"

bool work_board_open(const char *db_path);
void work_board_close(void);
bool work_board_handle(const char *method, const char *target, const char *query,
                       const char *body, DbInt actor_id, const char *role,
                       WorkspaceResponse *response);

#endif
