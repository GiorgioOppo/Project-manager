#!/usr/bin/env python3
"""Add missing Ticket Manager hosts to an already initialized Nginx Proxy Manager.

Credentials are read locally and never printed. Existing hosts and certificates
are never changed. HTTPS origins without certificates receive only an HTTP
redirect: certificate issuance and public DNS remain separate deployment steps.
"""

import argparse
import http.client
import os
from pathlib import Path
import re
import sys

from configure_proxy import ConfigurationError, NpmClient, read_env


def ticket_origins(primary, additional=""):
    """Validate the complete host plan before authenticating or creating hosts."""
    raw_origins = [primary] + (additional.split(",") if additional else [])
    if len(raw_origins) > 4:
        raise ConfigurationError("Ticket Manager ammette al massimo quattro origini.")
    origins = []
    seen = set()
    for raw in raw_origins:
        if any(ord(char) < 32 or ord(char) == 127 for char in raw):
            raise ConfigurationError("Origine Ticket Manager non valida.")
        match = re.fullmatch(r"(http|https)://([A-Za-z0-9.-]+)(?::([0-9]+))?", raw.strip())
        if not match:
            raise ConfigurationError("Le origini Ticket Manager devono essere HTTP/HTTPS senza percorsi o credenziali.")
        scheme, hostname, port = match.groups()
        hostname = hostname.lower()
        if (len(hostname) > 253 or any(
                not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label)
                for label in hostname.split("."))):
            raise ConfigurationError("Nome host Ticket Manager non valido.")
        if port is not None and port != ("443" if scheme == "https" else "80"):
            raise ConfigurationError("Il proxy Ticket Manager supporta solo le porte pubbliche standard 80 e 443.")
        if hostname in seen:
            raise ConfigurationError("Ogni host Ticket Manager deve avere una sola origine e un solo protocollo.")
        seen.add(hostname)
        origins.append({"hostname": hostname, "scheme": scheme})
    return origins


def https_redirect(hostname):
    return f"if ($scheme = http) {{ return 308 https://{hostname}$request_uri; }}"


def is_ticket_host(host, origin):
    if not isinstance(host, dict):
        return False
    meta = host.get("meta")
    if not (host.get("forward_scheme") == "http" and
            host.get("forward_host") in ("tickets", "ticket-app") and
            host.get("forward_port") == 8083 and
            host.get("enabled") in (True, 1) and
            not host.get("locations") and not host.get("access_list_id") and
            isinstance(meta, dict) and meta.get("nginx_online") is True):
        return False
    certificate = host.get("certificate_id")
    if type(certificate) is not int or certificate < 0:
        return False
    advanced = (host.get("advanced_config") or "").strip()
    if origin["scheme"] == "http":
        return certificate == 0 and host.get("ssl_forced") in (False, 0) and not advanced
    redirect = https_redirect(origin["hostname"])
    if certificate > 0:
        return host.get("ssl_forced") in (True, 1) and advanced in ("", redirect)
    return host.get("ssl_forced") in (False, 0) and advanced == redirect


def configure(client, email, password, origins):
    status = client.wait_ready()
    if not isinstance(status, dict) or status.get("setup") is not True:
        raise ConfigurationError("Inizializza NPM prima di configurare Ticket Manager.")
    login = client.request("POST", "/api/tokens", {"identity": email, "secret": password})
    if not isinstance(login, dict) or not isinstance(login.get("token"), str) or not login["token"]:
        raise ConfigurationError("NPM non ha restituito una sessione amministrativa valida.")
    client.token = login["token"]
    hosts = client.request("GET", "/api/nginx/proxy-hosts")
    if not isinstance(hosts, list):
        raise ConfigurationError("Elenco dei Proxy Host NPM non valido.")
    existing = {}
    for host in hosts:
        if not isinstance(host, dict) or not isinstance(host.get("domain_names"), list):
            raise ConfigurationError("Elenco dei Proxy Host NPM non valido.")
        for name in host["domain_names"]:
            if not isinstance(name, str):
                raise ConfigurationError("Elenco dei Proxy Host NPM non valido.")
            existing.setdefault(name.lower(), []).append(host)
    # Validate every requested host before creating even the first missing one.
    for origin in origins:
        hostname = origin["hostname"]
        matches = existing.get(hostname, [])
        if len(matches) > 1 or any(not is_ticket_host(host, origin) for host in matches):
            raise ConfigurationError(f"Proxy Host incompatibile già presente per {hostname}; nessun inoltro modificato.")
    for origin in origins:
        hostname = origin["hostname"]
        if hostname in existing:
            active = existing[hostname][0]
            action = "già configurato"
        else:
            created = client.request("POST", "/api/nginx/proxy-hosts", {
                "domain_names": [hostname], "forward_scheme": "http", "forward_host": "tickets",
                "forward_port": 8083, "certificate_id": 0, "ssl_forced": False,
                "block_exploits": True, "caching_enabled": False,
                "allow_websocket_upgrade": False, "access_list_id": 0,
                "advanced_config": https_redirect(hostname) if origin["scheme"] == "https" else "",
                "enabled": True, "locations": [], "meta": {},
            })
            if not isinstance(created, dict) or type(created.get("id")) is not int:
                raise ConfigurationError("NPM non ha restituito il Proxy Host creato.")
            active = client.request("GET", f"/api/nginx/proxy-hosts/{created['id']}")
            if not is_ticket_host(active, origin) or active.get("domain_names") != [hostname]:
                raise ConfigurationError("Proxy Host creato ma non verificato attivo; verifica il pannello NPM.")
            action = "creato"
        print(f"Inoltro {action}: {hostname} → tickets:8083")
        if origin["scheme"] == "https" and not active.get("certificate_id"):
            print(f"HTTPS in attesa del certificato per {hostname}; HTTP reindirizza a HTTPS.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=Path(".env.docker"))
    parser.add_argument("--tickets-env-file", type=Path, default=Path("ticket-app/.env"))
    args = parser.parse_args()
    try:
        values = read_env(args.env_file)
        ticket_values = read_env(args.tickets_env_file)

        def setting(key, source, default=""):
            return os.environ.get(key, source.get(key, default))

        try:
            port = int(setting("NPM_ADMIN_PORT", values, "81"))
        except ValueError:
            raise ConfigurationError("NPM_ADMIN_PORT deve essere una porta numerica.") from None
        if not 1 <= port <= 65535:
            raise ConfigurationError("NPM_ADMIN_PORT deve essere compresa tra 1 e 65535.")
        email = setting("NPM_ADMIN_EMAIL", values).strip()
        password = setting("NPM_ADMIN_PASSWORD", values)
        if "@" not in email or not 8 <= len(password) <= 64:
            raise ConfigurationError("Imposta NPM_ADMIN_EMAIL e una NPM_ADMIN_PASSWORD di 8–64 caratteri.")
        origins = ticket_origins(setting("TICKETS_PUBLIC_ORIGIN", ticket_values),
                                 setting("TICKETS_ADDITIONAL_ORIGINS", ticket_values))
        configure(NpmClient(port), email, password, origins)
        return 0
    except (ConfigurationError, OSError, http.client.HTTPException) as error:
        # Never dump API bodies, tokens, payloads or environment values.
        detail = str(error) if isinstance(error, ConfigurationError) else "Connessione al pannello NPM non riuscita."
        print(f"Errore: {detail}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
