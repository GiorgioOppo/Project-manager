# Network and security

[Italiano](../../tehnical/rete-sicurezza.md) | **English**

The configuration described is the repository configuration as of **11 October 2026**. The main sources are [Compose](../../../compose.yaml), [MySQL configuration](../../../docker/mysql/cluster.cnf), [Squid](../../../docker/squid/squid.conf), [TLS preparation](../../../scripts/configure_network.py) and the C clients. For an overview, see [architecture](architettura.md) and the [main README](../../../README.md).

## Eight networks and addresses

Internal networks use both `internal: true` and `com.docker.network.bridge.gateway_mode_ipv4: isolated`. NPM and Squid each have a separate bridge with external connectivity. Interface names in containers are explicitly assigned by Compose.

| Network / interface | Subnet | Members and IPs | Type |
| --- | --- | --- | --- |
| `public` | `172.30.10.0/24` | NPM `.2` | Bridge with gateway; public ingress |
| `private_app` | `172.30.20.0/24` | NPM `.2`, app `.10` | Internal, isolated gateway |
| `backend_app` | `172.30.30.0/24` | app `.10`, HAProxy `.20`, mysql-1 `.21`, mysql-2 `.22`, mysql-3 `.23`, mongodb `.30` | Internal, isolated gateway |
| `app_egress` | `172.30.40.0/24` | Squid `.2`, app `.10`, mail `.11`, calendar `.12` | Internal, isolated gateway |
| `proxy_wan` | `172.30.50.0/24` | Squid `.2` | Bridge with gateway; proxy egress |
| `mail_api` | `172.30.60.0/24` | mail `.2`, app `.10` | Internal, isolated gateway |
| `calendar_api` | `172.30.70.0/24` | calendar `.2`, app `.10` | Internal, isolated gateway |
| `inventory_api` | `172.30.80.0/24` | inventory `.2`, app `.10` | Internal, isolated gateway |

The app has six interfaces. The `mail-api`, `calendar-api` and `inventory-api` aliases are defined only in their respective API networks. [Docker naming](nomi-servizi-e-immagini.md) retains historical service aliases for DNS and TLS; Compose commands use the new names. Addons do not belong to `backend_app` and receive no SQL or MongoDB credentials. NPM does not belong to database or addon networks. Only NPM publishes host ports. NPM also has direct egress through its `public` network, needed for proxy and certificate management: Squid policy applies to the app and addons and does not filter NPM traffic.

On Docker Desktop, bridge networks are inside the Linux VM; they are not interfaces directly reachable from the Mac. The host or Docker daemon administrator can still read volumes, configuration and memory or attach new containers: segmentation does not protect against that administrator.

## Listeners and allowed flows

| Source | Destination | Protocol / port | Control |
| --- | --- | --- | --- |
| Browser | NPM host | HTTP 80 or HTTPS 443 | Proxy host; public TLS must be configured |
| Local host | NPM panel | `127.0.0.1:81` by default | NPM authentication |
| NPM | `172.30.20.10:8080` | Private HTTP | App bind, authorized Host and Origin |
| App | `db-proxy:3306` | MySQL with TLS | CA, server identity, SQL account |
| HAProxy | db/db2/db3 `3306` | TCP forwarding | End-to-end TLS; group member selection |
| HAProxy monitor | db/db2/db3 `3306` | MySQL TLS | Monitor account and certificate verification |
| mysql-1/mysql-2/mysql-3 | Peers `33061` | Group Replication XCom with TLS | Three-IP allowlist and identity verification |
| mysql-1/mysql-2/mysql-3 | Peers `3306` | MySQL distributed recovery with TLS | Recovery account and server verification |
| App | `documents:27017` | MongoDB TLS | CA, name, authentication and document role |
| App `.60.10` | `mail-api:8090` | HTTPS | Source IP and dedicated bearer token |
| App `.70.10` | `calendar-api:8091` | HTTPS | Source IP and dedicated bearer token |
| App `.80.10` | `inventory-api:8092` | HTTPS | Source IP and dedicated bearer token |
| App and addons | `172.30.40.2:3128` | Explicit CONNECT proxy | Separate Squid policy per source IP |
| Squid | Authorized Google hosts `443` | TLS tunnel | Exact name, port and public destination |
| Squid | Authorized SMTP hosts `465/587` | SMTP TLS tunnel | Allowed only for the email service; provider not yet configured |

The app listens only on `172.30.20.10:8080`, so backend, egress, email, Calendar and inventory interfaces do not expose the HTTP server. The three addons listen only on their respective API interfaces; Squid only on `172.30.40.2:3128`.

MongoDB listens on loopback and `172.30.30.30`. MySQL X Protocol is disabled (`mysqlx=0`), so port `33060` is not among the intended services. The HAProxy monitor exposes its three HTTP endpoints only on `127.0.0.1:9201–9203` inside the container.

NPM exposes the Node administration API only on `127.0.0.1:3000` inside the container. The Nginx panel listens on loopback and `172.30.10.2:81`, while host publication of port 81 is limited to `127.0.0.1`. It does not listen on the `private_app` interface, preventing the app from reaching the panel from its own network.

The matrix describes intended flows, not a firewall for every pair on the same network. `backend_app` is shared: the app can technically open a direct connection to a member's MySQL port. The network does not enforce mandatory routing through HAProxy and does not apply per-port ACLs between all its members. Enforcing that requirement needs additional daemon/host policies or a different topology.

## Egress through Squid

The three accepted sources are the app, `mail` and `calendar` IPs on the egress network. Other sources, requests other than CONNECT, literal IPs and private/reserved destinations are forbidden. Disallowed hostnames are rejected before DNS resolution; the DNS response for an allowed name is still checked against [denied-destinations.txt](../../../docker/squid/denied-destinations.txt).

| Client | Allowed hosts | Ports |
| --- | --- | --- |
| App | `accounts.google.com`, `oauth2.googleapis.com`, `www.googleapis.com` | `443` |
| Calendar | The same three Google hosts | `443` |
| Email | Exact entries in `allowed-smtp-domains.txt` | `465`, `587` |

The SMTP list currently contains only `smtp-not-configured.invalid`. Domain lists do not allow wildcards, leading dots, schemes, ports or paths. `www.googleapis.com.example.org` does not match `www.googleapis.com`.

The proxy neither intercepts nor decrypts TLS. It can authorize the tunnel name and port but cannot inspect HTTPS URLs, request bodies or OAuth scopes. `www.googleapis.com` is not exclusive to Calendar: the addon code further restricts allowed origins, paths and operations.

The app receives `HTTP_PROXY`/`HTTPS_PROXY` and their lowercase variants. Internal services are in `NO_PROXY`. Addon API clients explicitly disable the proxy for private HTTPS; addons enforce Squid for external calls without allowing `NO_PROXY` to bypass it. Direct egress control comes from internal networks, not merely from environment variables.

Squid does not cache and is not a router. `tcp_outgoing_address` sets egress to `172.30.50.2`; container IP forwarding is disabled. Logs record source, method, domain and result without headers, paths or queries. Configuration is mounted read-only and Docker logs are limited to three 10 MB files.

## Internal TLS and cryptographic materials

[configure_network.py](../../../scripts/configure_network.py) generates a local CA and distinct leaf certificates with the historical names db, db2, db3, documents, mail-addon, calendar-addon and inventory-addon. The CA key stays on the host. The app receives only the public CA, while each server receives its own leaf certificate and key.

| Connection | Configured protection |
| --- | --- |
| App → SQL | `verify_identity`, mandatory local CA and verified name |
| SQL replication / recovery | TLS and peer verification; recovery account `REQUIRE SSL` |
| App → MongoDB | `requireTLS`, verified CA and name; TLS 1.0/1.1 disabled on the server |
| App → addons | HTTPS with verified CA and hostname; minimum TLS 1.2; redirects not followed |
| Addons → providers | TLS with name and CA verification; CONNECT through Squid |
| NPM → app | Plain HTTP on `private_app` |

SQL node certificates include their own name, IP and `db-proxy`, because HAProxy does not terminate TLS. SQL and document keys are not interchangeable. Addon certificates include the API network alias as well as the historical name and expected addresses. New Compose names are not automatically added to SANs: use the TLS aliases already configured.

Secrets are local files separate from the database, mounted read-only. The host directory has `0700` permissions; files intended for containers are readable by their respective unprivileged users. This solution is not a KMS, does not use an HSM and does not prevent a host administrator from reading the key. TLS renewal and key rotation require coordinated procedures; see [configuration](installazione-configurazione.md).

## Actual data encryption

| Data | Protection in place | Boundary to understand |
| --- | --- | --- |
| User passwords | PBKDF2-HMAC-SHA-256, 210,000 iterations, random 16-byte salt, 32-byte derived value | Irreversible derivation, not a recoverable encrypted password |
| Names, emails, roles and SQL management data | Authentication, application authorization and network TLS | No general application-level cell encryption |
| Original PDFs and contract metadata | Manager access, TLS, SHA-256 content verification | PDFs not individually encrypted by the app; a hash does not encrypt |
| OAuth tokens, PKCE verifiers, event payloads and Calendar changes | AES-256-GCM with random nonces and AAD binding user and row | Decrypted in memory by the authorized service |
| Calendar metadata | Service access controls | User/account/calendar IDs, event IDs, ETags, statuses, sync tokens, idempotency keys, fingerprints and dates remain in plaintext |
| Email ledger | SHA-256 content hash, key, status and timestamps | Does not retain recipient/subject/body; this content is in the SQL outbox in plaintext |
| SQL dumps and gzip MongoDB archive | Local `0600` permissions | Dumps and compression are not encryption |

The repository does not configure a general per-column key vault, per-field cryptographic authorization, homomorphic encryption, confidential computing enclaves or universal volume encryption at rest. Host disk and off-site copy encryption are additional infrastructure responsibilities. Data required for operations is processed in plaintext in process memory.

In Calendar, changing the user or row identity invalidates GCM authentication of the ciphertext. An encrypted check when opening the database prevents startup with a different key. The API bearer token lets the trusted app forward requests for users: browser isolation depends on the app bridge, which derives the user from the session and does not accept an alternative ID from the client.

## Application security and OAuth

Global profiles are manager and operator; groups and project roles add specific permissions. Checks run on the server. Hiding a frontend button neither grants nor revokes permission. Contracts, suppliers and costs are restricted to managers; personal entries always refer to the session. Details are in [application APIs](api-app.md) and the [data model](modello-dati.md).

Random sessions reside in memory. Cookies are `HttpOnly`, `SameSite=Lax` and receive `Secure` on requests recognized as HTTPS. Short lifetime: 8 hours; with remembered sign-in: 30 days. The app checks Host and Origin against authorized origins and returns security headers, including CSP and a prohibition on frame embedding. Module and API checks supplement domain permissions.

OAuth uses single-use state with a 600-second lifetime, bound to the user and a session fingerprint, plus PKCE S256. The bridge does not return Google tokens to the browser. Callbacks, permissions and per-user data are described in the [addon APIs](api-addon.md).

The derived NPM image removes queries and referrers from access logs. For the exact path `/api/calendar/oauth/callback`, it also disables access logging and error messages that could include the OAuth code. The transformation clones the root location, preserving ACLs, authentication and headers; it applies to configurations present at startup and those regenerated by NPM. Incompatible custom configurations are rejected: do not remove this protection to accept an unverified location.

## Container hardening

All services have `no-new-privileges` and IPv4/IPv6 forwarding disabled. They are not privileged, do not use host networking and do not mount the Docker socket.

The app, addons, Squid and HAProxy have `cap_drop: ALL`, a read-only root filesystem and `/tmp` on tmpfs with `noexec,nosuid`. Addons have only the required data volume. NPM, MySQL and MongoDB drop `NET_ADMIN` and `NET_RAW`, but retain other capabilities required by their images and do not have fully read-only root filesystems. They must not be described as having the same profile as the C processes.

Dockerfiles exclude local data, env files, backups, secrets, logs and Git directories from their contexts through their respective `.dockerignore` files. `.gitignore` excludes the same operational materials from version control. These exclusions do not replace checking what is added to Git.

## Verifying the matrix

The [test_network_security.py](../../../tests/test_network_security.py) suite is opt-in and checks networks, endpoints, forbidden attempts, TLS and egress. [test_squid_policy.py](../../../tests/test_squid_policy.py) uses an isolated container for DNS policy; [test_proxy_privacy.py](../../../tests/test_proxy_privacy.py) checks OAuth log protection; [test_mysql_tls.py](../../../tests/test_mysql_tls.py) covers driver rejection and downgrade handling.

A positive health check or failed ping is not enough to prove isolation. Repeat relevant checks after changes to networks, IPs, binds, images, allowlists, certificates or proxies. Test commands and effects are in [development and tests](sviluppo-test.md).
