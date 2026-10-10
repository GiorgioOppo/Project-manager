#!/bin/sh
set -eu

if [ -z "${CLUSTER_MONITOR_PASSWORD:-}" ]; then
    echo "CLUSTER_MONITOR_PASSWORD obbligatoria per il controllo del cluster." >&2
    exit 1
fi

python3 /usr/local/bin/punto-db-health.py serve &
health_pid=$!

# HAProxy remains PID 1. A failed monitor makes every check fail and stops the
# container, allowing the normal restart policy to restore the health service.
(
    while kill -0 "$health_pid" 2>/dev/null; do
        sleep 2
    done
    echo "Il monitor MySQL si è arrestato; arresto il proxy." >&2
    kill -TERM 1
) &

exec "$@"
