#!/bin/bash
set -euo pipefail
# Secret values stay out of Compose, command arguments, and shell tracing.
export KC_DB_PASSWORD="$(cat "${KC_DB_PASSWORD_FILE:?Missing database secret file}")"
export KC_BOOTSTRAP_ADMIN_PASSWORD="$(cat "${KC_BOOTSTRAP_ADMIN_PASSWORD_FILE:?Missing bootstrap secret file}")"
test -n "$KC_DB_PASSWORD"
test -n "$KC_BOOTSTRAP_ADMIN_PASSWORD"
exec /opt/keycloak/bin/kc.sh "$@"
