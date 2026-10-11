#ifndef PUNTO_CALENDAR_CLIENT_H
#define PUNTO_CALENDAR_CLIENT_H

#include <stdbool.h>
#include "db.h"

typedef enum {
    CALENDAR_STATUS,
    CALENDAR_OAUTH_START,
    CALENDAR_OAUTH_CALLBACK,
    CALENDAR_CALENDARS,
    CALENDAR_SELECTION,
    CALENDAR_EVENTS,
    CALENDAR_EVENT_CREATE,
    CALENDAR_EVENT_UPDATE,
    CALENDAR_EVENT_DELETE,
    CALENDAR_SYNC,
    CALENDAR_DISCONNECT,
    CALENDAR_CHANGES,
    CALENDAR_EVENT,
    CALENDAR_CHANGE
} CalendarOperation;

typedef struct { int status; char *body; } CalendarResponse;

/* Configuration is optional for native development. An incomplete or unsafe
   configuration fails startup; TLS and the private destination are mandatory. */
bool calendar_client_open(void);
void calendar_client_close(void);
bool calendar_client_enabled(void);
/* User identity and OAuth session binding come only from the authenticated
   server session. input is a bounded form (or query for CALENDAR_EVENTS). */
void calendar_client_call(CalendarOperation operation, DbInt user_id,
                          const char *input, const char *session_binding,
                          CalendarResponse *response);
void calendar_response_free(CalendarResponse *response);

#endif
