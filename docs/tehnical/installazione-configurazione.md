# Installazione e configurazione

**Italiano** | [English](../en/tehnical/installazione-configurazione.md)

Guida verificata sui file del repository il **11 ottobre 2026**. I comandi presuppongono la directory radice del progetto. [Architettura](architettura.md) e [rete e sicurezza](rete-sicurezza.md) descrivono cosa viene avviato; il [README principale](../../README.md) offre l'ingresso rapido.

## Requisiti

Per Docker sono necessari Docker Engine **28 o successivo**, Docker Compose **2.36.0 o successivo**, Python 3 e OpenSSL. Le reti richiedono il gateway bridge `isolated`, `gw_priority` e `interface_name`. Su macOS il daemon è quello Linux di Docker Desktop. Gli script del cluster scelgono normalmente il contesto `desktop-linux` su macOS; accettano `--docker-context` per cambiarlo.

Occorrono spazio persistente per tre copie SQL, GridFS e gli archivi addon, oltre a una destinazione di backup. Le subnet `172.30.10.0/24` fino a `172.30.80.0/24` definite in Compose devono essere libere. Cambiarle richiede l'allineamento di Compose, listener, allowlist, controlli e SAN dei certificati; non basta modificare una sola subnet.

Le porte host predefinite sono 80 e 443 per l'app attraverso NPM e `127.0.0.1:81` per l'amministrazione. Nessun servizio database o addon pubblica una porta sull'host.

## Prima installazione Docker

1. Copiare il modello, senza sostituire un file esistente:

   ```sh
   cp .env.docker.example .env.docker
   chmod 600 .env.docker
   ```

2. Compilare nel file reale `PUNTO_EMAIL`, `PUNTO_PASSWORD`, `MYSQL_PASSWORD`, `MYSQL_ROOT_PASSWORD`, `NPM_ADMIN_EMAIL` e `NPM_ADMIN_PASSWORD`. Usare credenziali distinte. Configurare `PUBLIC_ORIGINS` con le origini effettive dell'app.
3. Lasciare vuote, se si desidera la generazione iniziale, `CLUSTER_REPLICATION_PASSWORD`, `CLUSTER_MONITOR_PASSWORD`, `DOC_DB_PASSWORD` e `DOC_DB_ROOT_PASSWORD`. `init` genera valori interni distinti e li salva nel file. La password del canale di recupero MySQL non può superare 32 byte.
4. Preparare i materiali e inizializzare:

   ```sh
   python3 scripts/configure_network.py --env-file .env.docker prepare
   python3 scripts/manage_cluster.py --env-file .env.docker init
   python3 scripts/manage_cluster.py --env-file .env.docker status
   docker compose --env-file .env.docker ps
   ```

5. Configurare gli inoltri NPM:

   ```sh
   python3 scripts/configure_proxy.py --env-file .env.docker
   ```

6. Verificare la risposta pubblica e aprire l'app:

   ```sh
   curl --fail http://localhost/healthz
   ```

`prepare` crea o verifica certificati e secret; non avvia lo stack. `init` ferma l'app, preserva il primo database, prepara i nuovi membri vuoti, crea il gruppo e avvia le dipendenze attraverso Compose. Conserva i marcatori in `data/cluster-state.json`. Se il cluster è già marcato come inizializzato, richiama il recupero senza ripetere il seed.

La procedura supporta anche la precedente installazione con un singolo MySQL e storico GTID vuoto. Rifiuta il seed se la sorgente ha già GTID o se un nuovo nodo contiene dati non riconducibili alla preparazione prevista. In quel caso occorre progettare un clone o un seed con checkpoint corretto, senza eliminare i controlli dello script.

Su un database privo di utenti viene creato il primo gestore. Cambiare in seguito `PUNTO_EMAIL` o `PUNTO_PASSWORD` non cambia l'account esistente. Analogamente, cambiare solo il file delle variabili non ruota gli account già presenti in MySQL o MongoDB.

## Dominio e HTTPS pubblico

Esempio non segreto di origini autorizzate:

```dotenv
PUBLIC_ORIGINS=http://localhost,http://127.0.0.1,https://progetti.example.com
```

Usare origini complete, senza percorso, credenziali, wildcard o slash finale; includere la porta quando non è quella standard. Sono ammesse al massimo 16 origini univoche. Dopo una modifica, ricreare l'app per rileggere l'ambiente.

`configure_proxy.py` usa l'API locale NPM, inizializza il suo amministratore se il pannello è nuovo e crea gli host mancanti verso `http://app:8080`. Mantiene separati i nomi locali e quelli pubblici, verifica gli host già esistenti e si ferma in caso di configurazioni incompatibili. Non richiede un certificato pubblico e non abilita automaticamente HTTPS o il redirect forzato.

Per il dominio reale configurare DNS e raggiungibilità dell'host, quindi assegnare il certificato nel pannello [NPM locale](http://127.0.0.1:81). Se si usa una porta amministrativa diversa, adattare l'indirizzo. I certificati della CA interna generati da `prepare` servono a database e addon; non sostituiscono il certificato pubblico del dominio.

## Variabili Docker

Il modello completo è [.env.docker.example](../../.env.docker.example). Le credenziali reali devono rimanere nel file locale o nei file secret dedicati, senza comparire in documentazione, shell history, ticket o Git. Evitare di pubblicare l'output completo di `docker compose config`: contiene valori risolti e password.

### Applicazione e ingresso

| Variabile modificabile | Significato e valore non segreto di esempio |
| --- | --- |
| `PUNTO_IMAGE_TAG` | Tag comune delle immagini locali del progetto; `local` se assente o vuoto |
| `PUNTO_EMAIL` | Email del primo gestore, per esempio `admin@example.test` |
| `PUNTO_PASSWORD` | Password iniziale del gestore; 8–1024 byte UTF-8, obbligatoria in Compose |
| `PUBLIC_ORIGINS` | Origini accettate dal controllo Host/Origin |
| `NPM_HTTP_PORT` / `NPM_HTTPS_PORT` | Porte host, `80` / `443` |
| `NPM_ADMIN_PORT` | Porta host locale, `81` |
| `NPM_TZ` | Fuso NPM, predefinito del modello `Australia/Brisbane` |
| `NPM_ADMIN_EMAIL` / `NPM_ADMIN_PASSWORD` | Credenziali lette dallo script proxy; password 8–64 caratteri |

Compose fissa per l'app `PORT=8080`, `BIND_HOST=172.30.20.10`, `PUBLIC_DIR=/app/public` e `TZ=Europe/Rome`. `PUBLIC_PORT` deriva da `NPM_HTTP_PORT`. L'ambiente del container non viene configurato copiando il file `.env.example` nativo.

### SQL e documenti

| Variabile | Scopo |
| --- | --- |
| `MYSQL_DATABASE` | Database applicativo, predefinito `punto` |
| `MYSQL_USER` / `MYSQL_PASSWORD` | Account applicativo SQL e password |
| `MYSQL_ROOT_PASSWORD` | Account amministrativo SQL, non fornito all'app |
| `CLUSTER_REPLICATION_PASSWORD` | Account `punto_recovery`, massimo 32 byte |
| `CLUSTER_MONITOR_PASSWORD` | Account `punto_monitor` per stato del gruppo |
| `DOC_DB_NAME` / `DOC_DB_USER` | Database e account documentali, entrambi predefiniti `punto_documents` |
| `DOC_DB_PASSWORD` / `DOC_DB_ROOT_PASSWORD` | Password documentale applicativa e amministrativa distinte |

Compose traduce le variabili SQL in `DB_HOST=db-proxy`, `DB_PORT=3306`, `DB_NAME`, `DB_USER` e `DB_PASSWORD`. Impone `DB_TLS_MODE=verify_identity` e `DB_SSL_CA=/etc/punto/tls/ca.crt`. Per MongoDB imposta host `documents`, porta `27017` e `DOC_DB_SSL_CA` sulla stessa CA. Non rimuovere la verifica dell'identità per aggirare errori di certificato.

### Email

| Variabile | Scopo |
| --- | --- |
| `MAIL_SMTP_HOST` | Host esatto; vuoto finché SMTP è rinviato |
| `MAIL_SMTP_PORT` / `MAIL_SMTP_MODE` | `587` / `starttls`, oppure `465` / `smtps` |
| `MAIL_SMTP_FROM` | Mittente autorizzato dal fornitore |
| `MAIL_SMTP_USER` | Utente SMTP se richiesto |
| `MAIL_API_NAMESPACE` | Prefisso stabile delle chiavi outbox, predefinito `punto-alerts-v1` |

La password SMTP proviene dal file `data/addon-secrets/smtp-password`, montato come secret. Il token privato è generato da `prepare` nel file `mail-api-token`. Il collegamento interno dell'app è fissato a `https://mail-api:8090`, con CA e token dedicati. L'addon usa il ledger `/var/lib/punto-mailer/delivery.sqlite3` e timeout SMTP di 20 secondi.

L'attivazione richiede anche l'hostname SMTP esatto in [allowed-smtp-domains.txt](../../docker/squid/allowed-smtp-domains.txt). Il segnaposto corrente non autorizza un provider reale. Prima dell'attivazione verificare l'outbox: abilitare SMTP può rendere inviabili anche alert già accodati. La procedura completa è nel [manuale email](../../addon/README.it.md).

### Calendar

| Variabile | Scopo |
| --- | --- |
| `GOOGLE_CLIENT_ID` | Client OAuth Web; vuoto fino alla configurazione Google |
| `GOOGLE_REDIRECT_URI` | URI pubblico esatto, per esempio `https://progetti.example.com/api/calendar/oauth/callback` |
| `CALENDAR_SYNC_INTERVAL` | Polling, predefinito `300` secondi |

Il client secret è nel file `data/addon-secrets/google-client-secret`. `prepare` genera separatamente il token `calendar-api-token` e la chiave AES `calendar-token-key`. Il file del client secret nasce vuoto. Il servizio privato usa `https://calendar-api:8091`, il database `/var/lib/punto-calendar/calendar.sqlite3`, timeout Google di 10 secondi e proxy Squid obbligatorio.

ID, redirect e segreto Google vanno configurati insieme; configurazioni parziali vengono rifiutate. OAuth resta disabilitato finché questa configurazione manca. Gli account Google richiedono poi il consenso individuale. Il [manuale Calendar](../../calendar-addon/README.it.md) descrive API, scope, limiti e attivazione. L'interfaccia grafica di collegamento non è ancora implementata: le funzionalità esposte in questa versione sono API.

## Aggiornamento dei servizi esistenti

Prima di aggiornare, salvare un backup coerente come descritto in [operazioni](operazioni-backup-ripristino.md), conservare le credenziali e verificare se il nuovo codice contiene migrazioni. Lo schema corrente è 10; avviare una versione precedente non esegue una migrazione inversa.

Se l’installazione usa ancora i precedenti nomi Compose, seguire prima la [migrazione della nomenclatura](nomi-servizi-e-immagini.md). Le identità nuove non devono partire accanto ai vecchi container con gli stessi IP e volumi.

Per una modifica solo applicativa:

```sh
docker compose --env-file .env.docker build app
docker compose --env-file .env.docker up -d --no-deps --force-recreate --wait app
```

Per l'aggiunta o l'aggiornamento congiunto dei servizi di integrazione sulle reti attuali:

```sh
python3 scripts/configure_network.py --env-file .env.docker prepare
docker compose --env-file .env.docker build proxy-egress mail calendar inventory app proxy-web
docker compose --env-file .env.docker up -d --no-deps --force-recreate --wait proxy-egress mail calendar inventory app proxy-web
```

Le ricreazioni interrompono temporaneamente le connessioni; quella dell'app invalida le sessioni in memoria. `--no-deps` evita la ricreazione implicita dei database, ma presuppone che le dipendenze siano già disponibili. Ricompilare e ricreare soltanto i servizi effettivamente modificati quando l'intervento è più ristretto.

Per migrare un cluster già inizializzato alla segmentazione di rete prevista, il comando dedicato è:

```sh
python3 scripts/configure_network.py --env-file .env.docker apply
```

`apply` verifica il motore e le sovrapposizioni con altre reti Docker, ferma app/NPM/HAProxy, salva SQL, ricrea i membri e richiama `recover`. È una manutenzione con interruzione, diversa dall'aggiornamento del solo binario. Non cambiare subnet di reti Docker esistenti senza pianificare anche la loro ricreazione e tutti i riferimenti correlati.

## Certificati e secret interni

`prepare` crea una CA privata RSA 3072/SHA-256 valida dieci anni e certificati di servizio validi un anno. Il materiale si trova in `data/network-tls`; la chiave CA non è montata nei container. Le directory locali hanno permessi `0700`, la chiave CA `0600`, i certificati pubblici `0644` e i file chiave montati `0444` per i diversi UID dei servizi. La protezione locale dipende anche dalla directory padre e dall'accesso all'host.

Su un bundle esistente, `prepare` verifica scadenza, corrispondenza fra chiavi e certificati, catena, hostname e IP. Aggiunge soltanto i leaf mancanti previsti; non rinnova automaticamente quelli esistenti. Si ferma se il bundle è incompleto, incoerente o prossimo alla scadenza, senza cancellarlo.

Il rinnovo è manuale: conservare i materiali originali, firmare i nuovi leaf con la CA prevista e gli stessi SAN necessari, verificarli, installarli in una finestra di manutenzione e ricreare i servizi coinvolti. Ruotare una CA richiede una transizione coordinata della fiducia di tutti i client. Cancellare la directory per rilanciare `prepare` non è una procedura di rinnovo.

La chiave `calendar-token-key` serve a decifrare il database già scritto. Non può essere sostituita come un normale token API: la rotazione richiede una migrazione esplicita, oggi non automatizzata. Lo stesso volume con una chiave differente non viene aperto. Vedere [sicurezza](rete-sicurezza.md) e [backup](operazioni-backup-ripristino.md).

## Modalità nativa per sviluppo

Servono un compilatore C11, make, Python 3 per i test, `pkg-config`, SQLite, libcurl, json-c e OpenSSL 3 (anche su macOS, per la verifica OIDC). La derivazione password usa Security/CommonCrypto su macOS e OpenSSL su Linux. Gli addon richiedono inoltre OpenSSL; Calendar richiede pthread. L’integrazione opzionale [Keycloak](keycloak.md) usa un realm comune per Punto e Ticket, con client e collegamenti utente separati.

```sh
make
make run
```

Il server non carica automaticamente `.env` o `.env.example`: esportare le variabili desiderate prima dell'avvio. Impostare esplicitamente credenziali di sviluppo dedicate, perché il sorgente contiene un fallback dimostrativo per l'avvio locale. Gli esempi di credenziali non devono essere usati per una distribuzione condivisa.

| Variabile nativa | Predefinito / comportamento |
| --- | --- |
| `PORT` | `8080`, intervallo 1024–65535 |
| `PUBLIC_PORT` | Uguale a `PORT`, intervallo 1–65535 |
| `BIND_HOST` | `127.0.0.1`, indirizzo IPv4 |
| `PUBLIC_DIR` | `public` |
| `DB_PATH` | `data/punto.db`, usato dal backend SQLite |
| `DOC_FILES_DIR` | Directory `documents` accanto al file SQL |
| `TZ` | `Europe/Rome` se non già definito |
| `PUBLIC_ORIGINS` | Origini aggiuntive autorizzate |

I tre parametri `CALENDAR_API_URL`, `CALENDAR_API_TOKEN_FILE`, `CALENDAR_API_CA_FILE` devono essere tutti presenti per collegare l'addon Calendar. Per l'email valgono `MAIL_API_URL`, `MAIL_API_TOKEN_FILE`, `MAIL_API_CA_FILE` e il namespace. Una configurazione incompleta non va utilizzata. Il container non pubblica le API private per l'uso nativo: occorre un ambiente di sviluppo dedicato, senza aprire porte di produzione come scorciatoia.
