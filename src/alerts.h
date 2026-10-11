#ifndef PUNTO_ALERTS_H
#define PUNTO_ALERTS_H
#include "workspace.h"
#include <time.h>

bool alerts_open(const char *db_path);
bool alerts_outbox_controls_schema(Db *db);
struct json_object;
void alerts_outbox_handle(Db *db,const char *method,struct json_object *fields,
                         DbInt actor_id,WorkspaceResponse *response);
void alerts_close(void);
bool alerts_handle(const char *method, const char *target, const char *query,
                   const char *body, DbInt actor_id, const char *role,
                   WorkspaceResponse *response);
void alerts_record_invalid_job(DbInt user_id, DbInt job_id,
                               const char *work_date, DbInt minutes);
void alerts_tick(void);
void alerts_tick_at(time_t now);
/* All calendar boundaries use the application's Europe/Rome timezone.
   Exposing the clock argument keeps DST and scheduler tests deterministic. */
bool alerts_workday_due(const char *date, time_t now);
bool alerts_next_run(time_t now, const char *run_time, bool today_already_run,
                     time_t *out);
#endif
