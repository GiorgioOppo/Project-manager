/* Persistent command-line harness for the public OIDC API. It never runs as
   part of either application and uses only disposable test credentials. */
#include "../auth/oidc.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static time_t time_offset;

/* Exercise the five-minute pending transaction boundary without a slow test. */
time_t time(time_t *out) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) return (time_t)-1;
    time_t result = now.tv_sec + time_offset;
    if (out) *out = result;
    return result;
}

static void print_json(struct json_object *value) {
    puts(json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN));
    fflush(stdout);
    json_object_put(value);
}

int main(void) {
    bool initialized = oidc_init();
    struct json_object *initial = json_object_new_object();
    json_object_object_add(initial, "initialized", json_object_new_boolean(initialized));
    json_object_object_add(initial, "enabled", json_object_new_boolean(oidc_enabled()));
    json_object_object_add(initial, "local", json_object_new_boolean(oidc_local_enabled()));
    print_json(initial);
    if (!initialized) return 1;
    char line[32768];
    while (fgets(line, sizeof(line), stdin)) {
        struct json_object *command = json_tokener_parse(line), *operation = NULL;
        struct json_object *response = json_object_new_object();
        int status = 400;
        if (command && json_object_object_get_ex(command, "op", &operation)) {
            const char *op = json_object_get_string(operation);
            if (op && !strcmp(op, "advance")) {
                struct json_object *seconds = NULL;
                if (json_object_object_get_ex(command, "seconds", &seconds)) {
                    time_offset += (time_t)json_object_get_int64(seconds);
                    status = 200;
                }
            } else if (op && !strcmp(op, "start")) {
                char location[8192], cookie[512];
                status = oidc_start(location, cookie);
                json_object_object_add(response, "location", json_object_new_string(location));
                json_object_object_add(response, "cookie", json_object_new_string(cookie));
            } else if (op && !strcmp(op, "logout")) {
                char location[8192];
                status = oidc_logout(location);
                json_object_object_add(response, "location", json_object_new_string(location));
            } else if (op && !strcmp(op, "management")) {
                struct json_object *email = NULL;
                json_object_object_get_ex(command, "email", &email);
                status = 200;
                json_object_object_add(response, "managed", json_object_new_boolean(oidc_users_managed()));
                json_object_object_add(response, "admin_url", json_object_new_string(oidc_users_admin_url()));
                json_object_object_add(response, "allowed", json_object_new_boolean(
                    oidc_local_account_allowed(email ? json_object_get_string(email) : NULL)));
            } else if (op && !strcmp(op, "identity")) {
                struct json_object *query = NULL, *cookies = NULL;
                OidcIdentity identity;
                int lifetime;
                json_object_object_get_ex(command, "query", &query);
                json_object_object_get_ex(command, "cookie", &cookies);
                status = oidc_callback_identity(query ? json_object_get_string(query) : NULL,
                    cookies ? json_object_get_string(cookies) : NULL, &identity, &lifetime);
                json_object_object_add(response, "issuer", json_object_new_string(identity.issuer));
                json_object_object_add(response, "subject", json_object_new_string(identity.subject));
                json_object_object_add(response, "email", json_object_new_string(identity.email));
                json_object_object_add(response, "name", json_object_new_string(identity.name));
                json_object_object_add(response, "role", json_object_new_string(identity.role));
                json_object_object_add(response, "bound_email", json_object_new_string(identity.bound_email));
                json_object_object_add(response, "lifetime", json_object_new_int(lifetime));
                json_object_object_add(response, "clear_cookie", json_object_new_string(oidc_clear_cookie()));
            } else if (op && !strcmp(op, "callback")) {
                struct json_object *query = NULL, *cookies = NULL;
                char email[255];
                int lifetime;
                json_object_object_get_ex(command, "query", &query);
                json_object_object_get_ex(command, "cookie", &cookies);
                status = oidc_callback(query ? json_object_get_string(query) : NULL,
                    cookies ? json_object_get_string(cookies) : NULL, email, &lifetime);
                json_object_object_add(response, "email", json_object_new_string(email));
                json_object_object_add(response, "lifetime", json_object_new_int(lifetime));
                json_object_object_add(response, "clear_cookie", json_object_new_string(oidc_clear_cookie()));
            }
        }
        json_object_object_add(response, "status", json_object_new_int(status));
        print_json(response);
        json_object_put(command);
    }
    oidc_close();
    return 0;
}
