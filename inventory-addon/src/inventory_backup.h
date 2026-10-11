#ifndef PUNTO_INVENTORY_BACKUP_H
#define PUNTO_INVENTORY_BACKUP_H

#include <json-c/json.h>
#include <sqlite3.h>
#include <time.h>

/* A backup failure is reported in status/logs and never closes the live database. */
void inventory_backup_open(sqlite3 *source);
void inventory_backup_tick(time_t now);
void inventory_backup_close(void);
/* The caller owns the returned object. No filesystem paths are exposed. */
json_object *inventory_backup_status(void);

#endif
