#ifndef PUNTO_MYSQL_SCHEMA_H
#define PUNTO_MYSQL_SCHEMA_H

#include <stdbool.h>

/* Both functions use the MySQL connection configured through DB_* variables. */
bool mysql_schema_preflight(void);
bool mysql_schema_prepare(void);

#endif
