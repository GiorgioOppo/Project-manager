#!/usr/bin/env python3
"""Read the Keycloak group directory and publish an atomic application projection.

The service account is read-only. Group identity is the native Keycloak ID;
display names and paths are never used to authorize a local identity.
"""
import argparse
import http.client
import json
import os
from pathlib import Path
import re
import ssl
import stat
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPSHandler, HTTPRedirectHandler, ProxyHandler, Request, build_opener

GROUP_LIMIT = 200
MEMBER_LIMIT = 200
DEPTH_LIMIT = 16
PAGE_SIZE = 100
MAX_RESPONSE = 1024 * 1024
MAX_SNAPSHOT = 4 * 1024 * 1024
GROUP_ID = re.compile(r"[A-Za-z0-9._~:@+-]{1,255}\Z")
EMAIL = re.compile(r"[^\s@\x00-\x1f\x7f-\uffff]+@[^\s@\x00-\x1f\x7f-\uffff]+\Z")


class SyncError(Exception):
    """Safe error category: remote responses and credentials are never echoed."""


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise SyncError("duplicate-json-key")
        value[key] = item
    return value


def decode_json(raw):
    try:
        return json.loads(raw, object_pairs_hook=unique_object)
    except (ValueError, UnicodeError):
        raise SyncError("invalid-json") from None


def read_file(path, limit):
    path = Path(path)
    try:
        if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode) or path.stat().st_size > limit:
            raise SyncError("invalid-runtime-file")
        with path.open("rb") as stream:
            value = stream.read(limit + 1)
        if len(value) > limit:
            raise SyncError("runtime-file-too-large")
        return value
    except OSError:
        raise SyncError("runtime-file-unavailable") from None


def identifier(value):
    if not isinstance(value, str) or not GROUP_ID.fullmatch(value):
        raise SyncError("invalid-identity")
    return value


def group_name(value):
    if (not isinstance(value, str) or not value.strip() or value != value.strip()
            or len(value) > 120 or any(ord(c) < 32 or ord(c) == 127 for c in value)):
        raise SyncError("invalid-group-name")
    return value


def realm_url(value):
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except (TypeError, ValueError):
        raise SyncError("invalid-realm-url") from None
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.query or parsed.fragment
            or parsed.path != "/realms/punto" or "\\" in value
            or any(ord(c) <= 32 or ord(c) >= 127 for c in value)
            or (port is not None and not 1 <= port <= 65535)):
        raise SyncError("invalid-realm-url")
    return value


def load_group_bindings(path):
    value = decode_json(read_file(path, 256 * 1024))
    if not isinstance(value, dict) or len(value) > GROUP_LIMIT:
        raise SyncError("invalid-group-bindings")
    ids = set()
    for key, local_id in value.items():
        identifier(key)
        if type(local_id) is not int or not 0 < local_id <= 9223372036854775807 or local_id in ids:
            raise SyncError("invalid-group-bindings")
        ids.add(local_id)
    return value


def load_user_bindings(path):
    value = decode_json(read_file(path, 256 * 1024))
    if not isinstance(value, dict) or len(value) > 1024:
        raise SyncError("invalid-user-bindings")
    for key, email in value.items():
        identifier(key)
        if not isinstance(email, str) or len(email) > 254 or not EMAIL.fullmatch(email) or email != email.lower():
            raise SyncError("invalid-user-bindings")
    return value


class NoRedirect(HTTPRedirectHandler):
    def http_error_302(self, _request, response, _code, _message, _headers):
        response.close()
        raise SyncError("redirect-rejected")

    http_error_301 = http_error_303 = http_error_307 = http_error_308 = http_error_302


class KeycloakReader:
    def __init__(self, backend, client_id, secret_file, ca_file):
        self.backend = realm_url(backend)
        if client_id != "punto-groups-sync":
            raise SyncError("invalid-sync-client")
        self.client_id = client_id
        self.secret_file = Path(secret_file)
        # No ambient proxy credentials or unverified HTTPS connections.
        try:
            context = ssl.create_default_context(cafile=str(ca_file))
        except (OSError, ssl.SSLError):
            raise SyncError("invalid-ca-file") from None
        self.opener = build_opener(ProxyHandler({}), HTTPSHandler(context=context), NoRedirect())
        self.token = None
        self.expires_at = 0

    def request(self, url, *, data=None, authenticated=False):
        headers = {"Accept": "application/json"}
        if authenticated:
            headers["Authorization"] = "Bearer " + self.access_token()
        if data is not None:
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        try:
            with self.opener.open(Request(url, data=data, headers=headers), timeout=5) as response:
                if response.status != 200:
                    raise SyncError("unexpected-http-status")
                raw = response.read(MAX_RESPONSE + 1)
        except HTTPError as error:
            code = error.code
            error.close()
            if code == 401 and authenticated:
                self.token = None
                self.expires_at = 0
            raise SyncError("http-" + str(code)) from None
        except (URLError, OSError, http.client.HTTPException):
            raise SyncError("https-unavailable") from None
        if len(raw) > MAX_RESPONSE:
            raise SyncError("response-too-large")
        return decode_json(raw)

    def access_token(self):
        if self.token is None or time.monotonic() >= self.expires_at:
            secret = read_file(self.secret_file, 256).decode("ascii").strip()
            if not re.fullmatch(r"[0-9a-f]{64}", secret):
                raise SyncError("invalid-client-secret")
            payload = urlencode({"grant_type": "client_credentials", "client_id": self.client_id,
                                 "client_secret": secret}).encode("ascii")
            result = self.request(self.backend + "/protocol/openid-connect/token", data=payload)
            if (not isinstance(result, dict) or not isinstance(result.get("access_token"), str)
                    or not 1 <= len(result["access_token"]) <= 16384
                    or any(ord(c) < 33 or ord(c) > 126 for c in result["access_token"])
                    or type(result.get("expires_in")) is not int or not 1 <= result["expires_in"] <= 86400
                    or not isinstance(result.get("token_type", "Bearer"), str)
                    or result.get("token_type", "Bearer").lower() != "bearer"):
                raise SyncError("invalid-token-response")
            self.token = result["access_token"]
            self.expires_at = time.monotonic() + max(1, result["expires_in"] - 15)
        return self.token

    def get(self, path, **query):
        base = self.backend.removesuffix("/realms/punto") + "/admin/realms/punto/"
        return self.request(base + path + ("?" + urlencode(query) if query else ""), authenticated=True)


def pages(reader, path, limit, **query):
    rows, seen = [], set()
    offset = 0
    while True:
        page = reader.get(path, first=offset, max=PAGE_SIZE, **query)
        if not isinstance(page, list) or len(page) > PAGE_SIZE:
            raise SyncError("invalid-page")
        for item in page:
            if not isinstance(item, dict):
                raise SyncError("invalid-directory-entry")
            key = identifier(item.get("id"))
            if key in seen:
                raise SyncError("duplicate-directory-identity")
            seen.add(key)
            rows.append(item)
        if len(rows) > limit:
            raise SyncError("directory-limit")
        if len(page) < PAGE_SIZE:
            return rows
        offset += len(page)


def directory(reader):
    count = reader.get("groups/count", top="false")
    if not isinstance(count, dict) or type(count.get("count")) is not int or not 0 <= count["count"] <= GROUP_LIMIT:
        raise SyncError("invalid-group-count")
    result, seen = [], set()

    def visit(rows, parent, depth):
        if rows and depth > DEPTH_LIMIT:
            raise SyncError("group-depth-limit")
        for row in rows:
            gid = identifier(row.get("id"))
            if gid in seen or len(seen) >= GROUP_LIMIT:
                raise SyncError("duplicate-group-or-limit")
            if row.get("parentId") not in (None, parent) or (parent is None and row.get("parentId") is not None):
                raise SyncError("inconsistent-group-parent")
            seen.add(gid)
            members = pages(reader, "groups/" + gid + "/members", MEMBER_LIMIT, briefRepresentation="true")
            # Disabled accounts remain in the directory but are excluded from
            # authorization memberships, independently of stale local profiles.
            selected = []
            for member in members:
                if type(member.get("enabled")) is not bool:
                    raise SyncError("missing-member-status")
                if member["enabled"]:
                    selected.append(identifier(member["id"]))
            result.append({"id": gid, "parent_id": parent, "name": group_name(row.get("name")),
                           "members": sorted(selected)})
            children = pages(reader, "groups/" + gid + "/children", GROUP_LIMIT,
                             briefRepresentation="true", subGroupsCount="false")
            visit(children, gid, depth + 1)

    roots = pages(reader, "groups", GROUP_LIMIT, populateHierarchy="false", briefRepresentation="true",
                  subGroupsCount="false")
    visit(roots, None, 1)
    after = reader.get("groups/count", top="false")
    if after != count or len(seen) != count["count"]:
        raise SyncError("inconsistent-group-count")
    return sorted(result, key=lambda group: group["id"])


def snapshot(reader, issuer, group_bindings, user_bindings, *, now=None):
    issuer = realm_url(issuer)
    first = directory(reader)
    # A second complete traversal detects rename, moves, membership changes,
    # pagination races and count-preserving identity replacements.
    if directory(reader) != first:
        raise SyncError("directory-changed-during-fetch")
    groups = [dict(group, local_id=group_bindings.get(group["id"])) for group in first]
    return {"version": 1, "issuer": issuer, "generated_at": int(time.time() if now is None else now),
            "groups": groups, "user_bindings": user_bindings}


def atomic_snapshot(path, value):
    path = Path(path)
    raw = (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    if len(raw) > MAX_SNAPSHOT or path.is_symlink() or not path.parent.is_dir():
        raise SyncError("invalid-snapshot-destination")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".groups-", delete=False) as stream:
            temporary = Path(stream.name)
            os.fchmod(stream.fileno(), 0o640)
            if os.geteuid() == 0:
                os.fchown(stream.fileno(), 10001, 10001)
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
        fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        raise SyncError("snapshot-write-failed") from None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def healthy(path, *, now=None):
    try:
        value = decode_json(read_file(path, MAX_SNAPSHOT))
        age = (time.time() if now is None else now) - value.get("generated_at", 0)
        return (value.get("version") == 1 and type(value.get("generated_at")) is int
                and -5 <= age < 120 and isinstance(value.get("groups"), list)
                and isinstance(value.get("user_bindings"), dict))
    except (SyncError, TypeError, AttributeError):
        return False


def required(env, name):
    value = env.get(name)
    if not value:
        raise SyncError("missing-runtime-configuration")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--health", action="store_true")
    args = parser.parse_args()
    try:
        env = os.environ
        output = Path(required(env, "KEYCLOAK_SYNC_OUTPUT"))
        if args.health:
            return 0 if healthy(output) else 1
        issuer = realm_url(required(env, "KEYCLOAK_SYNC_ISSUER_URL"))
        reader = KeycloakReader(required(env, "KEYCLOAK_SYNC_BACKCHANNEL_URL"),
                                env.get("KEYCLOAK_SYNC_CLIENT_ID", "punto-groups-sync"),
                                required(env, "KEYCLOAK_SYNC_SECRET_FILE"), required(env, "KEYCLOAK_SYNC_CA_FILE"))
        interval = int(env.get("KEYCLOAK_SYNC_INTERVAL", "30"))
        if not 5 <= interval <= 60:
            raise SyncError("invalid-sync-interval")
        group_path = required(env, "KEYCLOAK_SYNC_GROUP_BINDINGS_FILE")
        user_path = required(env, "KEYCLOAK_SYNC_USER_BINDINGS_FILE")
        while True:
            try:
                value = snapshot(reader, issuer, load_group_bindings(group_path), load_user_bindings(user_path))
                atomic_snapshot(output, value)
                print(json.dumps({"status": "ok", "groups": len(value["groups"])}), flush=True)
            except SyncError as error:
                print(json.dumps({"status": "error", "category": str(error)}), file=sys.stderr, flush=True)
                if args.once:
                    return 1
            if args.once:
                return 0
            time.sleep(interval)
    except (SyncError, ValueError, UnicodeError) as error:
        category = str(error) if isinstance(error, SyncError) else "invalid-runtime-configuration"
        print(json.dumps({"status": "error", "category": category}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
