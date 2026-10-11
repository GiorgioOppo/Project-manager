#!/bin/sh
set -eu
umask 077

# Compose bind-backed secrets preserve host ownership, so copy only the material
# each non-root service needs into its own volume. Sources remain mode 0600.
copy_private() {
    source=$1 destination=$2 owner=$3
    cp "$source" "$destination"
    chmod 0400 "$destination"
    chown "$owner:$owner" "$destination"
}

for item in keycloak:1000 database:999 punto:10001 tickets:10006; do
    name=${item%:*} owner=${item#*:}
    mkdir -p "/runtime/$name"
    chmod 0700 "/runtime/$name"
    chown "$owner:$owner" "/runtime/$name"
done

# The optional groups overlay supplies a separate read-only provider credential
# and a writable snapshot directory shared with Punto, never provider admin keys.
if [ -f /source/groups-client-secret ]; then
    for name in groups groups-snapshot; do
        mkdir -p "/runtime/$name"
        chmod 0700 "/runtime/$name"
        chown 10001:10001 "/runtime/$name"
    done
    copy_private /source/groups-client-secret /runtime/groups/client-secret 10001
    copy_private /source/groups-bindings.json /runtime/groups/groups.json 10001
    copy_private /source/punto-bindings.json /runtime/groups/users.json 10001
    copy_private /source/ca.crt /runtime/groups/ca.crt 10001
fi

copy_private /source/database-password /runtime/database/database-password 999
copy_private /source/database-password /runtime/keycloak/database-password 1000
copy_private /source/bootstrap-password /runtime/keycloak/bootstrap-password 1000
copy_private /source/punto-realm.json /runtime/keycloak/punto-realm.json 1000
copy_private /source/keycloak.crt /runtime/keycloak/keycloak.crt 1000
copy_private /source/keycloak.key /runtime/keycloak/keycloak.key 1000

for item in punto:10001 tickets:10006; do
    name=${item%:*} owner=${item#*:}
    copy_private "/source/$name-client-secret" "/runtime/$name/client-secret" "$owner"
    copy_private "/source/$name-bindings.json" "/runtime/$name/bindings.json" "$owner"
    copy_private /source/ca.crt "/runtime/$name/ca.crt" "$owner"
done
