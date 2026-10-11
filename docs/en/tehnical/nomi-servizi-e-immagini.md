# Docker service and image names

[Italiano](../../tehnical/nomi-servizi-e-immagini.md) | **English**

[Technical index](README.md) · [Architecture](architettura.md) · [Release](rilascio-e-compatibilita.md)

Naming uses short service names and project-built images in the `punto/<servizio>:<tag>` format. The main stack contains **11 services**, including three C addons; the Ticket overlay adds a twelfth. This change updates configuration, scripts and documentation. **It does not automatically migrate or deploy an existing installation.** Subsequent execution on the local project is recorded in the [11 October migration report](migrazione-container-2026-10-11.md), which records the completed migration and deployment checks.

## Complete map

In the table, `<tag>` corresponds to `${PUNTO_IMAGE_TAG:-local}`. The Ticket image is the same in the overlay and standalone Compose; the two deployments have distinct projects, networks and volumes and do not automatically share data.

| Current Compose service | Previous service | Image | Retained internal aliases |
| --- | --- | --- | --- |
| `app` | `app` | `punto/app:<tag>` | `app` |
| `mail` | `mail-addon` | `punto/mail:<tag>` | `mail-addon`; `mail-api` on the API network |
| `calendar` | `calendar-addon` | `punto/calendar:<tag>` | `calendar-addon`; `calendar-api` on the API network |
| `inventory` | `inventory-addon` | `punto/inventory:<tag>` | `inventory-addon`; `inventory-api` on the API network |
| `tickets` | `ticket-app` | `punto/tickets:<tag>` | `ticket-app` |
| `proxy-web` | `nginx-proxy-manager` | `punto/proxy-web:<tag>` | `nginx-proxy-manager` |
| `proxy-egress` | `squid` | `punto/proxy-egress:<tag>` | `squid` |
| `proxy-db` | `db-proxy` | `punto/proxy-db:<tag>` | `db-proxy` |
| `mysql-1` | `db` | `mysql:8.4` | `db` |
| `mysql-2` | `db2` | `mysql:8.4` | `db2` |
| `mysql-3` | `db3` | `mysql:8.4` | `db3` |
| `mongodb` | `documents` | `mongo:8.0` | `documents` |

Sources: [main Compose](../../../compose.yaml), [Ticket overlay](../../../compose.tickets.yaml), [standalone Ticket Compose](../../../ticket-app/compose.yaml).

New names are used in `docker compose build`, `up`, `logs`, `exec`, `stop` and `start` commands. Historical aliases are network destinations, not additional Compose services. For example, `docker compose exec proxy-db …` selects the service; the app still connects to `DB_HOST=db-proxy` to verify the same TLS identity.

## Images and tags

Project images have `build`, `image` and `pull_policy: build`: Compose builds them from local sources, reusing cache where possible, including during `up`. This avoids attempting downloads from the identically named public namespace. MySQL and MongoDB services still use official images and their normal pull policy. References: [Compose policy](https://docs.docker.com/reference/compose-file/services/#pull_policy) and [build/image interaction](https://docs.docker.com/reference/compose-file/build/).

To assign a shared tag, set `PUNTO_IMAGE_TAG` in the environment file used by Compose, for example `2026-10-11`. The default is `local`. The tag identifies the build but does not make the image immutable: also record actual IDs for a release. These names are local; publication would require an authorized registry and namespace and is not part of the change.

Dockerfile bases remain unchanged: Debian Bookworm for C apps, Debian Trixie for Squid, `haproxy:3.2-alpine` for the SQL proxy and `jc21/nginx-proxy-manager:2.16.0` for the web proxy. Vendor versions are not upgraded.

```sh
docker compose --env-file .env.docker config --quiet
docker compose --env-file .env.docker config --services
docker compose --env-file .env.docker config --images
docker compose --env-file .env.docker build app mail calendar inventory proxy-web proxy-egress proxy-db
```

`--services` and `--images` checks show only their respective lists. Do not publish complete resolved configuration output, which includes credentials.

## Retained identities

No project `name` or service `container_name` is added. Retain the **project name already in use**, working directory, any `-p` / `COMPOSE_PROJECT_NAME` and the same Docker context. Changing the project name may create new, apparently empty volumes.

Named volumes, bind mounts, static addresses, networks, ports, source and TLS file paths, secrets and accounts remain unchanged. So do MySQL hostnames announced within the group, `db` / `db2` / `db3` keys in `data/cluster-state.json` and DNS references used for TLS verification. Controllers translate these persistent identities into the new service names. Do not manually rename UUIDs, state files, SANs or directories to match Compose names.

## Transitioning an existing installation

The procedure requires a maintenance window and a [consistent backup](operazioni-backup-ripristino.md). Old and new containers must not use the same IPs or volumes simultaneously. Replacing Compose identities therefore requires **stopping and removing old containers without `-v`** before starting new ones.

The controller checks this condition before `init`, `recover` and service startup; `configure_network.py apply` also checks it. If it finds legacy containers, including stopped ones, in the same project, it stops the operation and points to this guide. It does not automatically remove containers or consider same-named containers in other projects. If it cannot verify Docker inventory, startup does not proceed. The check does not apply to materials-only preparation (`prepare`) or inspection and backup commands.

1. Record the actual Docker project, context, services, images and volumes. Keep the old Compose configuration in the same project directory, together with any overlays, before updating it. A name such as `compose.before-renaming.yaml` preserves relative bind mount resolution. Do not move or copy persistent data.
2. Complete the coordinated backup **with the previous controller and configuration, before installing this change**, then prepare new images. The updated controller also uses new names for `status` and `backup`: it cannot query a container still called `db` through service `mysql-1`. If files are already updated, retrieve a copy of the previous version to back up the legacy stack without restoring or modifying data. Retain the original `.env.docker`, TLS, secrets and `data/cluster-state.json`.
3. Using the old file and the same project, stop and remove containers. The following examples concern the main stack; if Ticket was active, also include its old overlay and environment file.

   ```sh
   docker compose --env-file .env.docker -f compose.before-renaming.yaml stop
   docker compose --env-file .env.docker -f compose.before-renaming.yaml rm -f
   ```

4. Verify that no containers with previous names are still attached to networks. Do not use `down -v`, delete volumes or rely on `up` to remove orphaned services automatically.
5. Using new files, start recovery of the existing cluster through the controller:

   ```sh
   python3 scripts/configure_network.py --env-file .env.docker prepare
   python3 scripts/manage_cluster.py --env-file .env.docker recover
   python3 scripts/manage_cluster.py --env-file .env.docker status
   docker compose --env-file .env.docker ps
   ```

   `recover` starts existing members and then application services. It respects quorum and GTIDs; it is not a new seed. Do not force bootstrap, clear markers or replace `recover` with initialization of empty volumes. If the controller reports divergence, stop the procedure and diagnose status.
6. Restart any `tickets` service with its overlay as in the [Ticket manual](../../../ticket-app/README.md). Verify three `ONLINE` / `PRIMARY` members, service health, sign-in, project data, PDFs and addon APIs. Provider configuration remains unchanged.

If old files are unavailable, identify containers through `com.docker.compose.project` and `com.docker.compose.service` labels. Verify the original project and actual ID list before stopping and removing them with `docker stop` and `docker rm`, **without `-v`**. Do not globally select all containers on the daemon. The table above provides legacy names to search for; also include `app`, which must be stopped during the cluster transition.

For standalone Ticket, apply the same principle to its dedicated project: retain the old `ticket-app/compose.yaml` in the same directory, stop and remove `ticket-app` without deleting `tickets_data`, then start `tickets` with the new file and same project name. The MySQL controller is not needed for this standalone SQLite application.

## Preparation checks — 11 October 2026

- Main Compose, main with Ticket and standalone Ticket configurations validated with synthetic credentials. The before/after comparison confirms unchanged project and volume names, mounts, addresses, ports and protections; expected differences are service names, dependencies, images, build policy, aliases and expanded `NO_PROXY`.
- `PUNTO_IMAGE_TAG` override verified in all three Compose configurations; overlay and standalone Ticket produce the same image name.
- Successfully built the **eight images** `punto/app:local`, `punto/mail:local`, `punto/calendar:local`, `punto/inventory:local`, `punto/tickets:local`, `punto/proxy-web:local`, `punto/proxy-db:local` and `punto/proxy-egress:local`.
- **24 tests** for controller/preparation/backup and **7 tests** protecting NPM configurations passed. One opt-in NPM runtime test was skipped; HA and network tests on the stack were not run.
- Guard also verified read-only on the local daemon: legacy containers in the same project are detected and startup is blocked without changing containers.

During this preparation phase no operational service had been started, stopped or recreated: previous images and existing containers had been retained. Subsequent deployment migration is a separate execution documented in the [operations report](migrazione-container-2026-10-11.md).
