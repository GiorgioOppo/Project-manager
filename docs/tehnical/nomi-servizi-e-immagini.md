# Nomi dei servizi e delle immagini Docker

**Italiano** | [English](../en/tehnical/nomi-servizi-e-immagini.md)

[Indice tecnico](README.md) · [Architettura](architettura.md) · [Rilascio](rilascio-e-compatibilita.md)

La nomenclatura usa nomi di servizio brevi e immagini costruite dal progetto nel formato `punto/<servizio>:<tag>`. Lo stack principale contiene **11 servizi**, compresi i tre addon C; l’overlay Ticket aggiunge il dodicesimo. Questa modifica aggiorna configurazione, script e documentazione. **Non applica automaticamente una migrazione o un deployment dell’installazione esistente.** L’esecuzione successiva sul progetto locale è registrata nel [rapporto di migrazione dell’11 ottobre](migrazione-container-2026-10-11.md), che registra la migrazione completata e i controlli eseguiti sul deployment.

## Mappa completa

Nella tabella `<tag>` corrisponde a `${PUNTO_IMAGE_TAG:-local}`. L’immagine Ticket è la stessa nell’overlay e nel Compose indipendente; i due deployment hanno progetti, reti e volumi distinti e non condividono automaticamente i dati.

| Servizio Compose attuale | Servizio precedente | Immagine | Alias interni conservati |
| --- | --- | --- | --- |
| `app` | `app` | `punto/app:<tag>` | `app` |
| `mail` | `mail-addon` | `punto/mail:<tag>` | `mail-addon`; `mail-api` sulla rete API |
| `calendar` | `calendar-addon` | `punto/calendar:<tag>` | `calendar-addon`; `calendar-api` sulla rete API |
| `inventory` | `inventory-addon` | `punto/inventory:<tag>` | `inventory-addon`; `inventory-api` sulla rete API |
| `tickets` | `ticket-app` | `punto/tickets:<tag>` | `ticket-app` |
| `proxy-web` | `nginx-proxy-manager` | `punto/proxy-web:<tag>` | `nginx-proxy-manager` |
| `proxy-egress` | `squid` | `punto/proxy-egress:<tag>` | `squid` |
| `proxy-db` | `db-proxy` | `punto/proxy-db:<tag>` | `db-proxy` |
| `mysql-1` | `db` | `mysql:8.4` | `db` |
| `mysql-2` | `db2` | `mysql:8.4` | `db2` |
| `mysql-3` | `db3` | `mysql:8.4` | `db3` |
| `mongodb` | `documents` | `mongo:8.0` | `documents` |

Fonti: [Compose principale](../../compose.yaml), [overlay Ticket](../../compose.tickets.yaml), [Compose Ticket indipendente](../../ticket-app/compose.yaml).

I nomi nuovi si usano nei comandi `docker compose build`, `up`, `logs`, `exec`, `stop` e `start`. Gli alias storici sono destinazioni di rete, non ulteriori servizi Compose. Per esempio il comando `docker compose exec proxy-db …` seleziona il servizio; l’app continua a collegarsi a `DB_HOST=db-proxy` per verificare la stessa identità TLS.

## Immagini e tag

Le immagini del progetto hanno `build`, `image` e `pull_policy: build`: Compose le costruisce dai sorgenti locali, riutilizzando la cache dove possibile, anche durante `up`. Questo evita di tentare il download dal namespace pubblico omonimo. I servizi MySQL e MongoDB continuano a usare le immagini ufficiali e la loro normale politica di download. Riferimenti: [politica Compose](https://docs.docker.com/reference/compose-file/services/#pull_policy) e [interazione fra build e image](https://docs.docker.com/reference/compose-file/build/).

Per assegnare un tag comune impostare `PUNTO_IMAGE_TAG` nel file dell’ambiente usato da Compose, per esempio `2026-10-11`. Il valore predefinito è `local`. Il tag identifica la build, non rende l’immagine immutabile: per un rilascio registrare anche gli ID effettivi. Questi nomi sono locali; una pubblicazione richiederebbe un registry e un namespace autorizzati e non è parte della modifica.

Le basi dei Dockerfile restano invariate: Debian Bookworm per le app C, Debian Trixie per Squid, `haproxy:3.2-alpine` per il proxy SQL e `jc21/nginx-proxy-manager:2.16.0` per il proxy web. Non viene eseguito un aggiornamento delle versioni dei fornitori.

```sh
docker compose --env-file .env.docker config --quiet
docker compose --env-file .env.docker config --services
docker compose --env-file .env.docker config --images
docker compose --env-file .env.docker build app mail calendar inventory proxy-web proxy-egress proxy-db
```

I controlli `--services` e `--images` mostrano soltanto i rispettivi elenchi. Non pubblicare l’output completo della configurazione risolta, che include le credenziali.

## Identità conservate

Non vengono aggiunti `name` al progetto Compose o `container_name` ai servizi. Conservare il **nome del progetto già in uso**, la directory di lavoro, gli eventuali `-p` / `COMPOSE_PROJECT_NAME` e lo stesso contesto Docker. Cambiare nome progetto può creare volumi nuovi apparentemente vuoti.

Restano uguali volumi nominati, bind mount, indirizzi statici, reti, porte, percorsi dei sorgenti e dei file TLS, secret e account. Restano anche gli hostname MySQL annunciati nel gruppo, le chiavi `db` / `db2` / `db3` in `data/cluster-state.json` e i riferimenti DNS usati per la verifica TLS. I controller traducono queste identità persistenti nei nuovi nomi di servizio. Non rinominare manualmente UUID, file di stato, SAN o directory per farli coincidere con i nomi Compose.

## Passaggio di un’installazione esistente

La procedura richiede una finestra di manutenzione e un [backup coerente](operazioni-backup-ripristino.md). I vecchi e nuovi container non devono usare contemporaneamente gli stessi IP o volumi. La sostituzione dell’identità Compose comporta quindi **arresto e rimozione dei vecchi container senza `-v`**, prima dell’avvio dei nuovi.

Il controller verifica questa condizione prima di `init`, `recover` e dell'avvio dei servizi; anche `configure_network.py apply` esegue il controllo. Se trova container legacy, inclusi quelli arrestati, nello stesso progetto, interrompe l'operazione e indica questa guida. Non rimuove container automaticamente e non considera gli omonimi di altri progetti. Se non può verificare l'inventario Docker, non procede all'avvio. Il controllo non si applica alla sola preparazione dei materiali (`prepare`) o ai comandi di consultazione e backup.

1. Registrare progetto Docker, contesto, servizi, immagini e volumi effettivi. Conservare la vecchia configurazione Compose nella stessa directory del progetto, insieme agli eventuali overlay, prima di aggiornarla. Un nome come `compose.before-renaming.yaml` mantiene la risoluzione dei bind mount relativi. Non spostare né copiare i dati persistenti.
2. Completare il backup coordinato **con il controller e la configurazione precedenti, prima di installare questa modifica**, poi preparare le immagini nuove. Il controller aggiornato usa i nomi nuovi anche per `status` e `backup`: non può interrogare un container ancora chiamato `db` attraverso il servizio `mysql-1`. Se i file sono già stati aggiornati, recuperare una copia della versione precedente per il backup dello stack legacy, senza ripristinare o modificare i dati. Conservare `.env.docker`, TLS, secret e `data/cluster-state.json` originali.
3. Con il vecchio file e lo stesso progetto arrestare e rimuovere i container. Gli esempi seguenti riguardano lo stack principale; se era attivo Ticket, includere anche il suo vecchio overlay e file ambiente.

   ```sh
   docker compose --env-file .env.docker -f compose.before-renaming.yaml stop
   docker compose --env-file .env.docker -f compose.before-renaming.yaml rm -f
   ```

4. Verificare che nessun container con i nomi precedenti sia ancora collegato alle reti. Non usare `down -v`, non cancellare volumi e non affidarsi a `up` per rimuovere automaticamente i servizi orfani.
5. Sui nuovi file avviare il recupero del cluster esistente attraverso il controller:

   ```sh
   python3 scripts/configure_network.py --env-file .env.docker prepare
   python3 scripts/manage_cluster.py --env-file .env.docker recover
   python3 scripts/manage_cluster.py --env-file .env.docker status
   docker compose --env-file .env.docker ps
   ```

   `recover` avvia i membri esistenti e poi i servizi applicativi. Rispetta quorum e GTID; non è un nuovo seed. Non forzare bootstrap, non cancellare i marcatori e non sostituire `recover` con un’inizializzazione di volumi vuoti. Se il controller segnala divergenze, fermare la procedura e diagnosticare lo stato.
6. Riavviare l’eventuale servizio `tickets` con il suo overlay, come nel [manuale Ticket](../../ticket-app/README.md). Verificare tre membri `ONLINE` / `PRIMARY`, salute dei servizi, login, dati delle commesse, PDF e API addon. La configurazione provider resta quella precedente.

Se i vecchi file non sono disponibili, individuare i container tramite le etichette `com.docker.compose.project` e `com.docker.compose.service`. Verificare il progetto originale e l’elenco concreto degli ID prima di fermarli e rimuoverli con `docker stop` e `docker rm`, **senza `-v`**. Non selezionare globalmente tutti i container del daemon. La tabella sopra fornisce i nomi legacy da ricercare; includere anche `app`, che deve essere fermata durante il passaggio del cluster.

Per Ticket indipendente applicare lo stesso criterio al suo progetto dedicato: conservare il vecchio `ticket-app/compose.yaml` nella stessa directory, arrestare e rimuovere `ticket-app` senza cancellare `tickets_data`, quindi avviare `tickets` con il nuovo file e lo stesso nome progetto. Non serve il controller MySQL per questa applicazione SQLite autonoma.

## Verifiche di preparazione — 11 ottobre 2026

- Configurazioni Compose principale, principale con Ticket e Ticket indipendente validate con credenziali sintetiche. Il confronto prima/dopo conferma gli stessi nomi progetto e volumi, mount, indirizzi, porte e protezioni; le differenze previste sono nomi servizi, dipendenze, immagini, politica di build, alias e ampliamento di `NO_PROXY`.
- Override `PUNTO_IMAGE_TAG` verificato in tutti e tre i Compose; overlay e Ticket indipendente producono lo stesso nome immagine.
- Compilate con successo le **otto immagini** `punto/app:local`, `punto/mail:local`, `punto/calendar:local`, `punto/inventory:local`, `punto/tickets:local`, `punto/proxy-web:local`, `punto/proxy-db:local` e `punto/proxy-egress:local`.
- **24 test** controller/preparazione/backup e **7 test** di protezione delle configurazioni NPM superati. Un test NPM runtime opt-in è stato saltato; le prove HA e rete sullo stack non sono state eseguite.
- Guardia verificata anche in sola lettura sul daemon locale: i container legacy dello stesso progetto vengono rilevati e l'avvio viene impedito senza modificare i container.

Durante questa fase di preparazione nessun servizio operativo era stato avviato, arrestato o ricreato: immagini precedenti e container esistenti erano stati conservati. La successiva migrazione del deployment è un’esecuzione distinta, documentata nel [rapporto operativo](migrazione-container-2026-10-11.md).
