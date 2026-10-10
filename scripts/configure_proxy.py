#!/usr/bin/env python3
"""Initialize local Nginx Proxy Manager and add missing Punto proxy hosts.

Uses only the Python standard library. Credentials and tokens are never printed.
Existing accounts and proxy hosts are not modified.
"""

import argparse
import http.client
import json
import os
from pathlib import Path
import shlex
import sys
import time
from urllib.parse import urlsplit


class ConfigurationError(Exception):
    pass


def read_env(path):
    values = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        raise ConfigurationError("Impossibile leggere il file delle variabili.") from None
    for line_number, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, raw_value = line.partition("=")
        if not separator or not key.strip().isidentifier():
            raise ConfigurationError(f"Variabile non valida alla riga {line_number}.")
        try:
            parts = shlex.split(raw_value, comments=True, posix=True)
        except ValueError:
            raise ConfigurationError(f"Valore non valido alla riga {line_number}.") from None
        if len(parts) > 1:
            raise ConfigurationError(f"Racchiudi il valore tra virgolette alla riga {line_number}.")
        values[key.strip()] = parts[0] if parts else ""
    return values


class NpmClient:
    def __init__(self, port):
        self.port = port
        self.token = None

    def request(self, method, path, payload=None):
        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        body = None
        if payload is not None:
            headers["Content-Type"] = "application/json"
            body = json.dumps(payload).encode("utf-8")
        # Connect directly to loopback: do not send admin credentials via HTTP proxies.
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            raw = response.read()
            status = response.status
        finally:
            connection.close()
        if not 200 <= status < 300:
            raise ConfigurationError(f"NPM ha rifiutato {method} {path} (HTTP {status}).")
        try:
            return json.loads(raw)
        except (ValueError, UnicodeError):
            raise ConfigurationError("Risposta NPM non valida.") from None

    def wait_ready(self):
        deadline = time.monotonic() + 60
        while True:
            try:
                status = self.request("GET", "/api/")
                if isinstance(status, dict) and status.get("status") == "OK":
                    return status
            except (OSError, http.client.HTTPException, ConfigurationError):
                pass
            if time.monotonic() >= deadline:
                raise ConfigurationError("NPM non è pronto: verifica che Compose sia avviato.")
            time.sleep(0.5)


def domain_groups(origins):
    names = []
    for origin in origins.split(","):
        try:
            parsed = urlsplit(origin.strip())
            port = parsed.port
        except ValueError:
            raise ConfigurationError("PUBLIC_ORIGINS contiene un'origine non valida.") from None
        if (parsed.scheme not in ("http", "https") or not parsed.hostname or
                parsed.username is not None or parsed.password is not None or
                parsed.path or parsed.query or parsed.fragment or
                (port is not None and not 1 <= port <= 65535)):
            raise ConfigurationError("PUBLIC_ORIGINS deve contenere solo origini HTTP/HTTPS complete.")
        name = parsed.hostname.lower()
        if not name.isascii() or any(char not in "abcdefghijklmnopqrstuvwxyz0123456789.-" for char in name):
            raise ConfigurationError("PUBLIC_ORIGINS contiene un nome host non supportato.")
        if name not in names:
            names.append(name)
    local = [name for name in names if name in ("localhost", "127.0.0.1")]
    # Keep public domains separate so their certificates never include localhost.
    return ([local] if local else []) + [[name] for name in names if name not in local]


def is_punto_host(host):
    return (host.get("forward_scheme") == "http" and host.get("forward_host") == "app" and
            host.get("forward_port") == 8080 and host.get("enabled") in (True, 1) and
            not host.get("advanced_config") and not host.get("locations") and
            not host.get("access_list_id") and host.get("meta", {}).get("nginx_online") is not False)


def configure(client, email, password, groups):
    status = client.wait_ready()
    if status.get("setup") is False:
        client.request("POST", "/api/users", {
            "name": "Amministratore Punto", "nickname": "Admin", "email": email,
            "roles": ["admin"], "auth": {"type": "password", "secret": password},
        })
        print("Account amministrativo NPM inizializzato.")
    elif status.get("setup") is not True:
        raise ConfigurationError("Stato di inizializzazione NPM non riconosciuto.")
    login = client.request("POST", "/api/tokens", {"identity": email, "secret": password})
    if not isinstance(login, dict) or not isinstance(login.get("token"), str) or not login["token"]:
        raise ConfigurationError("NPM non ha restituito una sessione amministrativa valida.")
    client.token = login["token"]
    hosts = client.request("GET", "/api/nginx/proxy-hosts")
    if not isinstance(hosts, list):
        raise ConfigurationError("Elenco dei Proxy Host NPM non valido.")
    existing = {}
    for host in hosts:
        for name in host.get("domain_names", []):
            existing.setdefault(name.lower(), []).append(host)
    # Validate every requested name before making changes to the proxy hosts.
    for group in groups:
        for name in group:
            matches = existing.get(name, [])
            if len(matches) > 1 or any(not is_punto_host(host) for host in matches):
                raise ConfigurationError(f"Proxy Host incompatibile già presente per {name}; nessun inoltro modificato.")
    for group in groups:
        missing = [name for name in group if name not in existing]
        if not missing:
            print(f"Inoltro già configurato: {', '.join(group)} → app:8080")
            continue
        created = client.request("POST", "/api/nginx/proxy-hosts", {
            "domain_names": missing, "forward_scheme": "http", "forward_host": "app",
            "forward_port": 8080, "certificate_id": 0, "ssl_forced": False,
            "block_exploits": True, "caching_enabled": False,
            "allow_websocket_upgrade": False, "access_list_id": 0,
            "advanced_config": "", "enabled": True, "locations": [], "meta": {},
        })
        if not isinstance(created, dict) or not isinstance(created.get("id"), int):
            raise ConfigurationError("NPM non ha restituito il Proxy Host creato.")
        active = client.request("GET", f"/api/nginx/proxy-hosts/{created['id']}")
        if not isinstance(active, dict) or active.get("meta", {}).get("nginx_online") is not True:
            raise ConfigurationError("Proxy Host creato ma non attivo; verifica il pannello NPM.")
        print(f"Inoltro creato: {', '.join(missing)} → app:8080")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=Path(".env.docker"))
    args = parser.parse_args()
    try:
        values = read_env(args.env_file)
        def setting(key, default=""):
            return os.environ.get(key, values.get(key, default))
        try:
            port = int(setting("NPM_ADMIN_PORT", "81"))
        except ValueError:
            raise ConfigurationError("NPM_ADMIN_PORT deve essere una porta numerica.") from None
        if not 1 <= port <= 65535:
            raise ConfigurationError("NPM_ADMIN_PORT deve essere compresa tra 1 e 65535.")
        email = setting("NPM_ADMIN_EMAIL").strip()
        password = setting("NPM_ADMIN_PASSWORD")
        if "@" not in email or not 8 <= len(password) <= 64:
            raise ConfigurationError("Imposta NPM_ADMIN_EMAIL e una NPM_ADMIN_PASSWORD di 8–64 caratteri.")
        groups = domain_groups(setting("PUBLIC_ORIGINS", "http://localhost,http://127.0.0.1"))
        configure(NpmClient(port), email, password, groups)
        print(f"Pannello: http://127.0.0.1:{port} — credenziali nel file delle variabili.")
        return 0
    except (ConfigurationError, OSError, http.client.HTTPException) as error:
        # Never dump API bodies, tokens, payloads or environment values.
        detail = str(error) if isinstance(error, ConfigurationError) else "Connessione al pannello NPM non riuscita."
        print(f"Errore: {detail}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
