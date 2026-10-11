#ifndef TICKETS_PUNTO_CLIENT_H
#define TICKETS_PUNTO_CLIENT_H
#include <stdbool.h>
#include <json-c/json.h>
bool punto_client_open(void);
void punto_client_close(void);
bool punto_client_configured(void);
const char *punto_client_origin(void);
int punto_client_get(const char *grant, const char *endpoint, const char *query, json_object **result);
#endif
