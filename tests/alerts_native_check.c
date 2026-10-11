#include "alerts.h"
#include "inventory_client.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static time_t local_time(int year, int month, int day, int hour, int minute, int second) {
    struct tm value = {0};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    value.tm_isdst = -1;
    time_t result = mktime(&value);
    assert(result != (time_t)-1);
    return result;
}

static void assert_local(time_t value, const char *expected) {
    struct tm local;
    char actual[32];
    assert(localtime_r(&value, &local) != NULL);
    assert(strftime(actual, sizeof(actual), "%Y-%m-%dT%H:%M:%S%z", &local) > 0);
    assert(strcmp(actual, expected) == 0);
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    assert(setenv("TZ", "Europe/Rome", 1) == 0);
    tzset();
    if (strcmp(argv[1], "tick") == 0) {
        assert(argc == 4);
        errno = 0;
        char *end = NULL;
        long long epoch = strtoll(argv[3], &end, 10);
        assert(!errno && end && !*end);
        assert(inventory_client_open());
        assert(alerts_open(argv[2]));
        alerts_tick_at((time_t)epoch);
        alerts_close();
        inventory_client_close();
    } else if (strcmp(argv[1], "ordinary") == 0) {
        assert(!alerts_workday_due("2026-10-05", local_time(2026, 10, 7, 23, 59, 59)));
        assert(alerts_workday_due("2026-10-05", local_time(2026, 10, 8, 0, 0, 0)));
        assert(!alerts_workday_due("2026-10-10", local_time(2026, 10, 20, 12, 0, 0)));
        assert(!alerts_workday_due("2026-10-11", local_time(2026, 10, 20, 12, 0, 0)));
        assert(!alerts_workday_due("2026-10-20", local_time(2026, 10, 8, 12, 0, 0)));
        assert(!alerts_workday_due("2026-02-29", local_time(2026, 10, 8, 12, 0, 0)));
        assert(!alerts_workday_due("2026-1-01", local_time(2026, 10, 8, 12, 0, 0)));
        assert(!alerts_workday_due("2026-04-31", local_time(2026, 10, 8, 12, 0, 0)));
    } else if (strcmp(argv[1], "spring") == 0) {
        /* Friday ends at Saturday midnight. The Sunday change loses one local
           hour, so the 48 real-hour grace ends on Monday at 01:00. */
        assert(!alerts_workday_due("2026-03-27", local_time(2026, 3, 30, 0, 59, 59)));
        assert(alerts_workday_due("2026-03-27", local_time(2026, 3, 30, 1, 0, 0)));
    } else if (strcmp(argv[1], "autumn") == 0) {
        /* The repeated Sunday hour means the same grace ends at 23:00 Sunday. */
        assert(!alerts_workday_due("2026-10-23", local_time(2026, 10, 25, 22, 59, 59)));
        assert(alerts_workday_due("2026-10-23", local_time(2026, 10, 25, 23, 0, 0)));
    } else if (strcmp(argv[1], "next_run") == 0) {
        time_t next;
        assert(alerts_next_run(local_time(2026, 10, 10, 7, 59, 59), "08:00", false, &next));
        assert_local(next, "2026-10-10T08:00:00+0200");
        assert(alerts_next_run(local_time(2026, 10, 10, 8, 0, 0), "08:00", false, &next));
        assert_local(next, "2026-10-11T08:00:00+0200");
        assert(alerts_next_run(local_time(2026, 10, 10, 7, 0, 0), "08:00", true, &next));
        assert_local(next, "2026-10-11T08:00:00+0200");
        assert(alerts_next_run(local_time(2026, 3, 28, 23, 0, 0), "08:00", false, &next));
        assert_local(next, "2026-03-29T08:00:00+0200");
        assert(alerts_next_run(local_time(2026, 10, 24, 23, 0, 0), "08:00", false, &next));
        assert_local(next, "2026-10-25T08:00:00+0100");
        assert(!alerts_next_run(local_time(2026, 10, 10, 0, 0, 0), "24:00", false, &next));
        assert(!alerts_next_run(local_time(2026, 10, 10, 0, 0, 0), "8:00", false, &next));
        assert(!alerts_next_run(local_time(2026, 10, 10, 0, 0, 0), "08:00", false, NULL));
    } else {
        assert(!"unknown alerts test scenario");
    }
    return 0;
}
