#ifndef PUNTO_MAIL_CLIENT_H
#define PUNTO_MAIL_CLIENT_H

#include <stdbool.h>
#include <time.h>

typedef enum {
    MAIL_DELIVERY_SENT,
    MAIL_DELIVERY_UNCONFIGURED,
    MAIL_DELIVERY_UNKNOWN,
    MAIL_DELIVERY_FAILED,
    MAIL_DELIVERY_RETRY
} MailDelivery;

/* With no MAIL_API_* configuration the add-on is disabled. Partial or unsafe
   configuration fails startup, instead of weakening TLS or authentication. */
bool mail_client_open(void);
void mail_client_close(void);
bool mail_client_enabled(void);
bool mail_client_smtp_configured(void);
const char *mail_client_namespace(void);
void mail_client_refresh(time_t now);
MailDelivery mail_client_send(const char *key, const char *to,
                              const char *subject, const char *body);

#endif
