# Publishing Ticket Manager

[Italiano](../../tehnical/ticket-reverse-proxy.md) | **English**

Ticket Manager uses the `tickets` Compose service, the `punto/tickets` image and the
persistent `tickets_data` volume. Nginx Proxy Manager forwards requests to
`http://tickets:8083` on `private_app`; the `ticket-app` alias is equivalent.
Port 8083 is not published on the host. With the Keycloak overlay, the service
also has the private `auth_api` network, used for the OIDC backchannel.

## Addresses for this installation

- `https://ticket.localhost`: primary local address, configured together with the
  local certificate and hybrid Keycloak access. HTTP redirects to HTTPS.
- `https://ticket.tuaazienda.it`: additional public address prepared on
  request. It still requires DNS and a public certificate before use.
  The HTTP rule redirects to HTTPS and does not serve sign-in over HTTP.

The local certificate belongs to the CA in `data/keycloak/tls/ca.crt`. The CA must
be trusted by the device opening the site; do not disable certificate
verification. `localhost` and its subdomains refer to the browser's computer.

Ticket bootstrap credentials are in local file `ticket-app/.env`, excluded
from Git with 0600 permissions. Passwords are not included in verification logs
or documentation. Existing accounts are not replaced by
bootstrap. Company sign-in uses the explicit associations defined for Keycloak.

## Origins and sessions

```dotenv
TICKETS_PUBLIC_ORIGIN=https://ticket.localhost
TICKETS_ADDITIONAL_ORIGINS=https://ticket.tuaazienda.it
```

One primary origin and at most three additional origins are allowed, separated by
commas without spaces. Every origin is exact: no paths, wildcards or credentials.
The same Host cannot be associated with two different schemes; the HTTP→HTTPS transition
must be handled by the proxy. The `compose.tickets.yaml` overlay passes both variables.

The app checks Host and Origin, binds each session to its creation origin and sets
`Secure` for HTTPS origins. `X-Forwarded-*` headers do not change these
decisions. Ticket data is shared across domains, while sessions are
separate: one domain's cookie does not authenticate requests to the other.

OIDC sign-in is available only on the `OIDC_REDIRECT_URI` origin. In
hybrid mode, local sign-in remains on other origins. To move SSO to the
final domain, update the origin, redirect registered in the
Keycloak client and app configuration together.

## Provisioning Nginx rules

Before modifying NPM, create a consistent backup of its SQLite store.
After starting the service, using valid NPM credentials:

```sh
python3 scripts/configure_ticket_proxy.py \
  --env-file .env.docker --tickets-env-file ticket-app/.env
```

The command checks all conflicts before creating rules, preserves compatible
hosts and does not change existing hosts or certificates. It does not initialize NPM
accounts or issue certificates. For an HTTPS origin without a certificate it creates an
HTTP rule with a fixed redirect and explicitly reports that HTTPS still needs
completion. An existing rule does not prove Internet reachability.

For the public domain: configure DNS toward server ingress, make
Nginx reachable on the intended ports, install a valid certificate and
enable Force SSL in the dedicated rule. Application container and
database ports remain private.

## Verification

The suite uses temporary databases and includes workflows, permissions, OIDC, session
isolation across origins and idempotent proxy provisioning:

```sh
make test-tickets
```

On the running service, verify the TLS chain, HTTP redirect, page and
assets, `/healthz`, sign-in, authenticated APIs, CSRF and sign-out. Full write
verification can run on a temporary instance with a separate volume; do not
delete or replace the operational volume to clean up test data.

To update a hybrid installation, retain all overlays and configuration
files used to start it. Starting with only the Ticket overlay does not automatically retain
networks, mounts and OIDC variables from Keycloak overlays.

## Validation on 2026-10-11

- Container `projectmanament-tickets-1` healthy, `hybrid` mode, networks
  `private_app` and `auth_api`, no directly published ports.
- NPM: Proxy Host 4 for `ticket.localhost`, forwarding to `http://tickets:8083`,
  local certificate and Force SSL; Proxy Host 6 for `ticket.tuaazienda.it`, same
  forwarding and HTTP 308 redirect to HTTPS, public certificate still absent.
- 77 automated tests passed, including creation/editing/comment workflows,
  persistence, roles, OIDC, origins and proxy configuration.
- 35 deployment checks passed: TLS verification with CA, resources and language
  catalogs, local sign-in, session, lists and filters, CSRF/foreign-origin rejection,
  sign-out, public redirect and Punto app reachability.
- Browser opening verified from `http://ticket.localhost` to
  `https://ticket.localhost`, with local form and company sign-in available.
- No test tickets or users added to the operational store. The public
  domain was checked locally using the Host header; Internet
  reachability and its HTTPS are not yet available.

NPM backup and configuration before publication, test logs and final
report: `backups/ticket-publish-20261011T063230Z/`, a private directory excluded from Git.
