# punto — Commesse e ore di lavoro in C

Web app in italiano con server HTTP in C e interfaccia responsive in HTML, CSS e JavaScript, senza pacchetti npm. Gestisce utenti, ruoli, commesse abilitate, registrazioni delle ore e storico delle modifiche.

Sono disponibili due modalità indipendenti: **Docker con MySQL**, usato dall’app come database applicativo, oppure **avvio nativo con SQLite**. Le due modalità non condividono dati e non eseguono importazioni automatiche.

## Avvio con Docker e cluster MySQL

Servono Docker, Docker Compose e Python 3. La configurazione locale usa **tre istanze MySQL 8.4 con Group Replication multi-primary**, un proxy TCP HAProxy 3.2 e l’app C. Dalla cartella del progetto:

```sh
cp .env.docker.example .env.docker
```

Apri `.env.docker`, scegli l’email del primo gestore e compila `PUNTO_PASSWORD`, `MYSQL_PASSWORD` e `MYSQL_ROOT_PASSWORD` con password diverse. Imposta anche `NPM_ADMIN_EMAIL` e una password distinta in `NPM_ADMIN_PASSWORD` per il pannello del reverse proxy. Puoi lasciare vuote `CLUSTER_REPLICATION_PASSWORD` e `CLUSTER_MONITOR_PASSWORD`: `init` genera e salva nel file due password casuali di 32 caratteri ASCII. Se scegli manualmente la password di replica, il limite è **32 byte**. Non riutilizzare la password del gestore. Il file contiene credenziali e viene escluso dal versionamento.

Il primo avvio, compreso il passaggio dal precedente database MySQL singolo, va eseguito con:

```sh
python3 scripts/manage_cluster.py --env-file .env.docker init
python3 scripts/manage_cluster.py --env-file .env.docker status
```

Lo script ferma l’app durante la preparazione, esegue un backup dei dati del primo nodo, inizializza i nuovi nodi vuoti con una copia coerente, crea il gruppo e avvia gli altri servizi. Il primo nodo conserva il volume `mysql_data` già usato dal progetto: non occorre eliminare il database per aggiungere le repliche. Il file locale `data/cluster-state.json` conserva i marcatori dei nodi già preparati, così il seed non viene ripetuto sui dati esistenti. Gli account interni vengono creati su ciascun nodo e il seed viene importato localmente, senza registrare le password nell’output del controller. Su macOS il comando usa per impostazione predefinita il contesto Docker `desktop-linux`; `--docker-context` permette di sceglierne un altro.

La migrazione iniziale supporta il precedente database singolo con GTID disattivati e storico `@@gtid_executed` vuoto. Se la sorgente ha già transazioni GTID, `init` si ferma esplicitamente: occorre un clone o un seed con il corretto checkpoint GTID per evitare transazioni duplicate. L’immagine MySQL ufficiale può generare GTID durante l’inizializzazione dei nuovi nodi; il controller li azzera prima dell’importazione soltanto se il nodo è `OFFLINE`, il database applicativo è vuoto, non esistono altre tabelle di dati utente e tutti i GTID appartengono al suo UUID locale. Un nodo che non soddisfa questi controlli non viene sovrascritto.

In `docker/mysql/cluster.cnf` le opzioni del plugin hanno il prefisso `loose-group_replication_`: consente la fase `mysqld --initialize`, durante la quale il plugin non è ancora caricato. Dopo l’avvio il controller verifica le variabili effettive del server prima di inizializzare o recuperare il gruppo.

Apri [http://127.0.0.1:8082](http://127.0.0.1:8082) e accedi con `PUNTO_EMAIL` e `PUNTO_PASSWORD` impostati nel file. `PUNTO_HTTP_PORT` cambia la porta pubblicata, che rimane limitata alla macchina locale. Lo schema e il primo gestore vengono creati quando necessario. Il container applicativo legge e scrive in MySQL e non utilizza `data/punto.db`.

### Servizi, dati e disponibilità

| Servizio | Funzione | Persistenza |
| --- | --- | --- |
| `db` | Primo membro MySQL, scrivibile | Volume esistente `mysql_data` |
| `db2` | Secondo membro MySQL, scrivibile | Volume `mysql_data_2` |
| `db3` | Terzo membro MySQL, scrivibile | Volume `mysql_data_3` |
| `db-proxy` | HAProxy: connessioni TCP distribuite sui membri disponibili, porta interna `3306` | Configurazione del progetto |
| `app` | Server C, con `DB_HOST=db-proxy` | Dati applicativi nel cluster |
| `nginx-proxy-manager` | Reverse proxy HTTP/HTTPS e pannello amministrativo | Directory locali NPM |

Ogni membro MySQL conserva una copia completa di utenti, commesse, abilitazioni, ore e storico. Tutti e tre possono ricevere scritture; HAProxy sceglie un nodo per ciascuna nuova connessione, mantenendo su quel nodo l’intera transazione. Il monitor ammette nel bilanciamento soltanto membri `ONLINE`, con ruolo `PRIMARY`, scrivibili e con almeno due membri disponibili. MySQL e HAProxy non pubblicano porte database sull’host; il loro traffico rimane sulla rete Compose `backend`.

Il **quorum è 2 su 3**: il gruppo può continuare a lavorare se un membro si arresta. Con due membri indisponibili le scritture vengono bloccate per proteggere la consistenza. Group Replication e le sue limitazioni sono descritte nel [manuale Oracle MySQL 8.4](https://dev.mysql.com/doc/refman/8.4/en/group-replication-multi-primary-mode.html).

Questa configurazione protegge dal guasto di una singola istanza MySQL. I tre container sullo stesso computer condividono ancora il guasto dell’host; l’app C e il singolo HAProxy restano punti di guasto. Per proteggere l’intero servizio servono anche host distinti e ridondanza dei componenti applicativi e di rete.

Il cluster usa isolamento `READ COMMITTED` e consistenza `BEFORE_AND_AFTER`. La tabella interna `cluster_write_guard` viene aggiornata nella stessa transazione delle modifiche applicative: crea un conflitto rilevabile tra scritture concorrenti e protegge controlli quali il limite delle ore giornaliere e l’ultimo gestore attivo. Questa scelta riduce la concorrenza delle scritture; i conflitti di certificazione, incluso l’errore MySQL `3101`, annullano la transazione e vengono trattati come errori temporanei. Il driver ristabilisce le connessioni fuori dalle transazioni e **non ripete un commit con esito incerto**.

Le comunicazioni XCom e il recupero dei membri sono cifrati con TLS `REQUIRED`; gli account interni `punto_recovery` e `punto_monitor` richiedono SSL. I certificati generati dalle singole istanze non verificano reciprocamente l’identità dei nodi: per questa verifica occorre configurare una CA condivisa e certificati adeguati. Il TLS del cluster è separato dal certificato HTTPS gestito da NPM. [TLS Group Replication](https://dev.mysql.com/doc/refman/8.4/en/group-replication-secure-socket-layer-support-ssl.html), [TLS del recupero](https://dev.mysql.com/doc/refman/8.4/en/group-replication-configuring-ssl-for-recovery.html)

### Stato, backup e riavvii

```sh
python3 scripts/manage_cluster.py --env-file .env.docker status
python3 scripts/manage_cluster.py --env-file .env.docker backup
docker compose --env-file .env.docker ps
curl --fail http://127.0.0.1:8082/healthz
docker compose --env-file .env.docker logs --tail=100 app db-proxy db db2 db3
```

`status` permette di controllare la composizione del gruppo. `backup` crea una copia dei dati senza svuotare il database e la salva in `backups/` con permessi `0600`; la directory è esclusa dal versionamento. Anche `data/cluster-state.json` è locale ed escluso dal versionamento: va conservato insieme ai volumi per gestire il cluster esistente. `/healthz` è accessibile senza login, verifica la connessione al backend ed è usato anche dal controllo di salute dell’app: restituisce HTTP 200 con `{"ok":true,"database":"mysql"}` quando il controllo riesce, oppure HTTP 503 quando fallisce. Il controllo del singolo processo MySQL non sostituisce quello di appartenenza al gruppo eseguito dal monitor.

Quando il gruppo è ancora attivo, un membro riavviato può rientrare e recuperare le transazioni mancanti. Per avviare o ricreare i container di un cluster già inizializzato:

```sh
docker compose --env-file .env.docker up --build -d
```

Se **tutti e tre i membri si arrestano**, per esempio dopo `docker compose down` o un riavvio del computer, occorre ricostituire il gruppo:

```sh
python3 scripts/manage_cluster.py --env-file .env.docker recover
python3 scripts/manage_cluster.py --env-file .env.docker status
```

`recover` avvia i tre server SQL e attende che siano raggiungibili. Se esiste già un gruppo con membri `ONLINE`, fa rientrare quelli offline senza creare un nuovo gruppo. Soltanto quando nessun membro è `ONLINE` esegue il bootstrap: confronta le transazioni eseguite e quelle certificate, quindi sceglie come fondatore un membro che contiene il set di tutti gli altri. Non sceglie sempre `db` e non forza il quorum su un membro isolato. Se non trova un candidato sicuro si ferma senza creare un secondo gruppo. La scelta segue la [procedura Oracle per il riavvio completo](https://dev.mysql.com/doc/refman/8.4/en/group-replication-restarting-group.html).

Per fermare e rimuovere i container conservando i dati:

```sh
docker compose --env-file .env.docker down
```

I tre volumi MySQL conservano i dati tra riavvii e ricostruzioni. Non aggiungere `--volumes` o `-v` se vuoi mantenerli: queste opzioni eliminano i volumi. La replica non sostituisce un backup, perché anche una modifica errata viene propagata agli altri membri.

`PUNTO_EMAIL` e `PUNTO_PASSWORD` creano soltanto il primo gestore su un database privo di utenti; cambiare i valori dopo l’inizializzazione non modifica gli account salvati. Anche le variabili `MYSQL_*` di inizializzazione non cambiano automaticamente le password dei volumi MySQL già esistenti. Non cambiare le password interne del cluster nel solo file `.env.docker` senza aggiornare anche gli account MySQL corrispondenti.

| Variabile in `.env.docker` | Funzione |
| --- | --- |
| `PUNTO_HTTP_PORT` | Porta locale dell’app, predefinita `8082` |
| `PUNTO_EMAIL`, `PUNTO_PASSWORD` | Credenziali iniziali del gestore |
| `PUBLIC_ORIGINS` | Origini HTTP/HTTPS autorizzate, separate da virgole |
| `MYSQL_DATABASE` | Nome del database, predefinito `punto` |
| `MYSQL_USER`, `MYSQL_PASSWORD` | Utente applicativo e relativa password |
| `MYSQL_ROOT_PASSWORD` | Password dell’amministratore MySQL, non usata dall’app |
| `CLUSTER_REPLICATION_PASSWORD` | Password dell’account interno `punto_recovery`, generata da `init` se vuota; massimo 32 byte |
| `CLUSTER_MONITOR_PASSWORD` | Password dell’account interno `punto_monitor`, generata da `init` se vuota |

Il file SQLite nativo resta separato e viene conservato: il passaggio a Docker non importa automaticamente utenti o registrazioni da SQLite.

## Reverse proxy con Nginx Proxy Manager

Il servizio `nginx-proxy-manager` usa l’immagine `jc21/nginx-proxy-manager:2.16.0`. Riceve il traffico sulle porte **80 HTTP** e **443 HTTPS**; il pannello di amministrazione è disponibile su [http://127.0.0.1:81](http://127.0.0.1:81), soltanto dalla macchina locale. Il servizio usa il controllo di salute `/usr/bin/check-health` documentato da [Nginx Proxy Manager](https://nginxproxymanager.com/advanced-config/#docker-healthcheck).

L’app e NPM condividono la rete Compose `proxy`, quindi la destinazione del proxy è **`http://app:8080`**. App, HAProxy e i tre membri MySQL condividono la rete `backend`; NPM non è collegato a questa rete. L’accesso diretto all’app su `127.0.0.1:8082` rimane disponibile. L’utilizzo del nome del servizio come destinazione sulla rete Docker segue la [configurazione ufficiale NPM](https://nginxproxymanager.com/advanced-config/#best-practice-use-a-docker-network).

### Configurazione del dominio

In `.env.docker`, autorizza gli indirizzi attraverso cui verrà aperta l’app:

```dotenv
PUBLIC_ORIGINS=http://localhost,http://127.0.0.1,http://progetti.tuaazienda.it,https://progetti.tuaazienda.it
```

Le origini devono essere complete e separate da virgole, senza percorsi, credenziali o wildcard. Le porte diverse da quelle predefinite vanno incluse, per esempio `http://localhost:8083`. Il server verifica `Host` e, per le operazioni di modifica, `Origin`. Non considera attendibili gli header `X-Forwarded-*` per autorizzare un dominio. Dopo una modifica delle origini, ricrea il container app:

```sh
docker compose --env-file .env.docker up --build -d
```

Il comando di configurazione del proxy legge `.env.docker`:

```sh
python3 scripts/configure_proxy.py --env-file .env.docker
```

Le credenziali amministrative `NPM_ADMIN_EMAIL` e `NPM_ADMIN_PASSWORD` appartengono al pannello NPM e sono diverse da quelle dell’app. Lo script le usa per inizializzare un’installazione NPM vuota; non vengono passate come variabili al container. Su un’installazione già configurata usa un account amministrativo esistente. I nomi da inoltrare sono ricavati da `PUBLIC_ORIGINS`: con l’esempio indicato sopra crea un Proxy Host per `progetti.tuaazienda.it` e uno per `localhost` e `127.0.0.1`, entrambi diretti al servizio `app` sulla porta `8080`. Lo script crea gli inoltri mancanti e rifiuta eventuali configurazioni incompatibili già presenti, senza sovrascriverle. È possibile eseguirlo nuovamente senza duplicare utenti o inoltri.

Per configurare lo stesso inoltro dal pannello NPM, crea due **Proxy Host** con questi valori:

| Campo | Valore |
| --- | --- |
| Domain Names | Primo host: `progetti.tuaazienda.it`; secondo host: `localhost`, `127.0.0.1` |
| Scheme | `http` |
| Forward Hostname / IP | `app` |
| Forward Port | `8080` |
| Advanced | Vuoto |

Per raggiungere il dominio da un browser, il relativo DNS deve puntare all’indirizzo del server. In locale puoi verificare l’inoltro senza cambiare il DNS:

```sh
curl --fail --resolve progetti.tuaazienda.it:80:127.0.0.1 http://progetti.tuaazienda.it/healthz
```

La porta 443 è pubblicata, ma **HTTPS diventa operativo dopo aver configurato un certificato valido** nella scheda SSL del Proxy Host. Per richiedere un certificato Let's Encrypt, il dominio e la relativa convalida devono essere raggiungibili correttamente. Richiedi il certificato per il solo dominio pubblico `progetti.tuaazienda.it`: `localhost` e `127.0.0.1` servono per l’accesso HTTP locale e hanno un Proxy Host separato. Le sessioni dell’app usano cookie `Secure` quando l’origine pubblica autorizzata è HTTPS. Configura il certificato prima di attivare **Force SSL** sul dominio pubblico.

### Persistenza e impostazioni del proxy

NPM conserva la propria configurazione in un database SQLite separato, `nginx-proxy-manager/data/database.sqlite`, e i certificati in `nginx-proxy-manager/letsencrypt/`. Queste directory sono montate nei percorsi `/data` e `/etc/letsencrypt` previsti dal [setup ufficiale](https://nginxproxymanager.com/setup/); sono escluse sia dal versionamento sia dal contesto di build dell’app. Le commesse e le ore continuano a essere salvate in MySQL.

| Variabile in `.env.docker` | Funzione |
| --- | --- |
| `NPM_HTTP_PORT` | Porta HTTP pubblica, predefinita `80` |
| `NPM_HTTPS_PORT` | Porta HTTPS pubblica, predefinita `443` |
| `NPM_ADMIN_PORT` | Porta locale del pannello, predefinita `81` |
| `NPM_TZ` | Fuso orario NPM, predefinito `Australia/Brisbane` come nel modello richiesto; puoi impostare `Europe/Rome` |
| `NPM_ADMIN_EMAIL`, `NPM_ADMIN_PASSWORD` | Credenziali lette dallo script di configurazione, non dall’app |

Per spostare una porta pubblicata, modifica il valore in `.env.docker` e ricrea il servizio. Cambiare `NPM_HTTP_PORT` o `NPM_HTTPS_PORT` non modifica automaticamente le origini autorizzate o il DNS. `docker compose down` conserva i tre volumi MySQL e le directory persistenti NPM; al successivo avvio completo usa il comando `recover` per ricostituire il gruppo.

## Avvio nativo con SQLite

La compilazione nativa usa SQLite per impostazione predefinita. Su macOS servono i Command Line Tools di Xcode. Su Linux servono compilatore C, Make e gli header di OpenSSL e SQLite, ad esempio `libssl-dev` e `libsqlite3-dev`.

```sh
make run
```

Apri [http://127.0.0.1:8080](http://127.0.0.1:8080). Su un database vuoto l’account demo è:

- **Email:** `demo@punto.local`
- **Password:** `Punto2026!`

`Ctrl+C` ferma il server. I dati rimangono in `data/punto.db`; `make clean` rimuove soltanto gli eseguibili compilati.

Le variabili sono lette dall’ambiente. `.env.example` documenta i valori, ma il server non carica automaticamente file `.env`:

```sh
PUNTO_EMAIL='nome@azienda.it' PUNTO_PASSWORD='UnaPasswordPersonale2026!' PORT=8081 make run
```

`PUNTO_EMAIL` e `PUNTO_PASSWORD` vengono usati solo quando non esistono utenti. Il pulsante demo della pagina di login compila sempre l’account demo predefinito.

### Compilazione nativa con MySQL

È possibile usare un server MySQL anche senza Docker. Servono gli header e la libreria MariaDB Connector/C e `pkg-config`, oltre alle dipendenze crittografiche della piattaforma: CommonCrypto/Security su macOS, OpenSSL su Linux. Su Debian i pacchetti di compilazione includono `build-essential`, `pkg-config`, `libmariadb-dev` e `libssl-dev`.

```sh
make clean
make DB_BACKEND=mysql
```

Esporta `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER` e `DB_PASSWORD` con i dati di connessione a un database esistente, quindi avvia `./build/punto`. `DB_PATH` è usato soltanto dal backend SQLite. Per tornare alla compilazione SQLite, esegui `make clean` e `make`.

| Variabile del server | Funzione |
| --- | --- |
| `PORT` | Porta HTTP, predefinita `8080` |
| `PUBLIC_PORT` | Porta locale pubblicata, predefinita uguale a `PORT`; Compose la allinea a `PUNTO_HTTP_PORT` per verificare Host e Origin |
| `PUBLIC_ORIGINS` | Elenco esatto delle origini HTTP/HTTPS autorizzate; necessario per usare un dominio attraverso il proxy |
| `BIND_HOST` | Indirizzo IPv4 di ascolto, predefinito `127.0.0.1`; Compose usa `0.0.0.0` all’interno del container |
| `PUBLIC_DIR` | Cartella degli asset, predefinita `public` |
| `DB_PATH` | File del backend SQLite, predefinito `data/punto.db` |
| `DB_HOST`, `DB_PORT` | Host e porta del backend MySQL |
| `DB_NAME`, `DB_USER`, `DB_PASSWORD` | Database e credenziali applicative MySQL |
| `PUNTO_EMAIL`, `PUNTO_PASSWORD` | Credenziali per creare il primo gestore |
| `TZ` | Fuso orario delle abilitazioni; in assenza di un valore viene usato `Europe/Rome` |

## Utenti e permessi

L’account iniziale è un **Gestore**. Dalla scheda **Utenti** può creare account con nome, email, password e ruolo, oppure modificarne nome, ruolo e stato attivo.

| Operazione | Gestore | Operatore |
| --- | --- | --- |
| Consultare commesse e stati | Tutte le commesse | Solo commesse attualmente abilitate |
| Creare e modificare commesse | Sì | No |
| Abilitare utenti alle commesse | Sì | No |
| Aggiungere ore e descrivere il lavoro svolto | Sì | Solo su commesse abilitate |
| Consultare le proprie registrazioni, anche dopo una revoca | Sì | Sì |
| Modificare o eliminare le proprie registrazioni | Sì | No |
| Gestire utenti e ruoli | Sì | No |
| Consultare lo storico delle modifiche | Sì | No |

I permessi sono verificati dal server a ogni richiesta, anche se l’utente tenta di inviare direttamente una chiamata API. Un cambio di ruolo vale per le sessioni già aperte; disattivare un account ne revoca le sessioni, che non vengono ripristinate riattivandolo. Il sistema impedisce di disattivare o declassare l’ultimo gestore attivo. L’email non è modificabile dall’interfaccia o dall’API.

Le password vengono salvate come derivazioni PBKDF2-HMAC-SHA256 con sale casuale. Le sessioni usano token casuali e cookie HttpOnly/SameSite, durano 8 ore oppure 30 giorni con “Ricordami” e terminano al riavvio del server. Per le origini HTTPS autorizzate il cookie include anche `Secure`; il certificato TLS viene gestito da Nginx Proxy Manager.

## Commesse e abilitazioni

Il gestore crea una commessa con codice univoco, nome, cliente e descrizione, e ne modifica dettagli e stato:

| Stato | Valore API |
| --- | --- |
| Da avviare | `planned` |
| In corso | `active` |
| In pausa | `paused` |
| Completata | `completed` |
| Annullata | `cancelled` |

Ricerca e filtro per stato aiutano a ritrovare le commesse. Le ore mostrate per commessa sono quelle registrate dall’utente autenticato, su tutte le date.

Dal comando **Abilitazioni** il gestore sceglie gli utenti autorizzati a utilizzare una commessa, con data di inizio e fine facoltative. Le date sono inclusive; un campo vuoto non pone un limite su quel lato dell’intervallo.

Per un operatore, l’abilitazione deve essere valida sia **oggi** sia **nella data del lavoro registrato**. Una commessa futura, scaduta o revocata non compare nell’elenco utilizzabile e il server rifiuta nuove ore. Le registrazioni precedenti restano consultabili. I gestori possono utilizzare tutte le commesse senza assegnazione; lo stato della commessa, da solo, non blocca la registrazione delle ore.

Gli utenti non vengono abilitati automaticamente a tutte le commesse quando si aggiorna un database esistente: il gestore assegna esplicitamente gli accessi.

## Registro ore

Seleziona una giornata e registra il tempo su una commessa disponibile, con una nota facoltativa. Le ore decimali vengono convertite in minuti interi: **1,5 ore = 1 ora e 30 minuti**. Puoi usare anche il formato `1:30`. Il gestore può correggere o eliminare le proprie registrazioni; l’operatore può soltanto aggiungerle.

Il registro mostra il totale giornaliero. Il server impedisce valori nulli o negativi, date inesistenti e un totale superiore a 24 ore nella stessa giornata. Sono ammesse più registrazioni sulla stessa commessa e date passate o future, rispettando le eventuali abilitazioni.

Ogni registrazione appartiene all’**identificativo stabile dell’utente** (`user_id`), attribuito dal server. L’email memorizzata insieme alle ore è un dato storico; non determina i permessi di lettura o modifica. Il modulo e l’API non consentono di attribuire ore a un altro utente.

## Storico delle modifiche e aggiornamenti dei dati

La scheda **Storico** è riservata ai gestori. Mostra creazioni e modifiche di utenti e commesse, aggiunte e correzioni delle ore, eliminazioni delle registrazioni e modifiche alle abilitazioni. Ogni evento conserva data UTC, autore e valori precedenti e successivi; non contiene password, hash o sali. I filtri per tipo e la paginazione consentono di consultare anche uno storico esteso.

Una modifica applicativa e il suo evento di audit vengono salvati nella stessa transazione. Se lo storico non può essere scritto, viene annullata anche la modifica. Le richieste rifiutate non producono eventi di modifica.

Gli aggiornamenti dello schema sono versionati. Per un precedente database SQLite, la migrazione conserva i dati e associa le ore agli utenti attraverso l’email, senza distinguere maiuscole e minuscole. Le righe senza un account corrispondente restano conservate ma non vengono esposte ad altri utenti; la creazione successiva dell’account corrispondente può associarle al suo `user_id`. Un database con versione futura non supportata viene rifiutato senza modificarlo.

La base attuale gestisce fino a 200 utenti, 1000 commesse e 1000 registrazioni per utente nella stessa giornata. Un superamento restituisce un errore esplicito senza troncare gli elenchi.

## Tabelle del database

Nel backend MySQL il database applicativo, predefinito `punto`, contiene sette tabelle InnoDB. Ogni tabella ha una chiave primaria; le relazioni usano chiavi esterne senza operazioni `CASCADE`.

| Tabella | Dati principali e struttura |
| --- | --- |
| `users` | ID utente `BIGINT`, email univoca, nome, ruolo e stato attivo; derivazione della password `BINARY(32)` e sale `BINARY(16)` |
| `jobs` | ID commessa `BIGINT`, codice univoco, nome, cliente, descrizione e stato |
| `job_assignments` | Chiave composta `(job_id,user_id)`, riferimenti a commessa e utente, date di validità facoltative |
| `time_entries` | ID registrazione `BIGINT`, riferimenti a commessa e utente, copia storica dell’email, giornata, minuti e nota |
| `audit_events` | ID evento `BIGINT`, autore, data UTC, azione, tipo e ID dell’elemento, JSON precedenti e successivi |
| `schema_metadata` | Versione dello schema, conservata in una singola riga |
| `cluster_write_guard` | Riga interna modificata dalle transazioni applicative per rilevare conflitti tra scritture sui membri del cluster |

I testi usano `utf8mb4`; email e codici sono univoci senza distinzione tra maiuscole e minuscole. Le giornate e gli intervalli sono stringhe ISO `YYYY-MM-DD`, le durate sono minuti interi e i JSON dello storico sono testo. I trigger proteggono lo storico da aggiornamenti ed eliminazioni e impediscono di cambiare l’utente già associato a una registrazione. La definizione completa si trova in `src/mysql_schema.c`.

SQLite conserva le tabelle funzionali della modalità nativa e usa `PRAGMA user_version` per la versione dello schema. `cluster_write_guard` appartiene alla modalità MySQL; il database SQLite di NPM contiene soltanto la configurazione del reverse proxy e non è una replica dei dati applicativi.

## API

Le risposte sono JSON. Gli endpoint di commesse, ore, utenti e storico richiedono una sessione. I POST usano `application/x-www-form-urlencoded`.

| Metodo e percorso | Funzione |
| --- | --- |
| `GET /healthz` | Controllo di salute senza login |
| `GET /api/session` | Identità, ruolo e permessi dell’utente autenticato |
| `GET /api/jobs` | Commesse visibili con totale minuti dell’utente |
| `POST /api/jobs` | Gestore: crea con `code`, `name`, `client`, `description`, `status` |
| `POST /api/jobs/<id>` | Gestore: modifica gli stessi campi |
| `GET /api/jobs/<id>/assignments` | Gestore: elenco utenti abilitati e relativi intervalli |
| `POST /api/jobs/<id>/assignments` | Gestore: crea o aggiorna abilitazione con `user_id`, `starts_on`, `ends_on` |
| `DELETE /api/jobs/<id>/assignments/<user_id>` | Gestore: revoca un’abilitazione, senza corpo della richiesta |
| `GET /api/time-entries?date=YYYY-MM-DD` | Registro giornaliero dell’utente e totale minuti |
| `POST /api/time-entries` | Registra tempo con `job_id`, `date`, `minutes`, `note` |
| `POST /api/time-entries/<id>` | Gestore: modifica una propria registrazione |
| `DELETE /api/time-entries/<id>` | Gestore: elimina una propria registrazione |
| `GET /api/users` | Gestore: elenco utenti senza credenziali |
| `POST /api/users` | Gestore: crea con `name`, `email`, `password`, `role` |
| `POST /api/users/<id>` | Gestore: aggiorna `name`, `role`, `active` (`1` oppure `0`) |
| `GET /api/audit?limit=30&before_id=123&entity_type=job` | Gestore: storico con filtri facoltativi |

`client`, `description` e `note` sono facoltativi. `starts_on` e `ends_on` accettano date `YYYY-MM-DD`, valori vuoti o campi omessi. L’utente abilitato deve esistere ed essere attivo. L’audit accetta `limit` da 1 a 100, un `before_id` positivo e `entity_type` tra `job`, `time_entry`, `assignment`, `user` e `system`; restituisce `next_before_id` per la pagina successiva.

I ruoli sono `manager` e `worker`. Le operazioni non autorizzate restituiscono HTTP 403. I dati delle ore sono sempre limitati all’utente autenticato, anche per un gestore.

## Verifica

La suite locale richiede Python 3 e avvia server SQLite su porte libere con database temporanei:

```sh
make test
```

Verifica autenticazione, sessioni, ruoli, commesse, abilitazioni, intervalli di date, isolamento tramite `user_id`, audit, rollback delle transazioni, migrazioni e persistenza. Il test MySQL viene saltato quando non è impostato `PUNTO_MYSQL_TEST_URL`.

Per verificare un’app MySQL già avviata, esporta le credenziali di un gestore esistente nelle variabili `PUNTO_MYSQL_TEST_EMAIL` e `PUNTO_MYSQL_TEST_PASSWORD`, quindi:

```sh
PUNTO_MYSQL_TEST_URL=http://127.0.0.1:8082 python3 tests/test_mysql.py -v
```

In alternativa il test legge `PUNTO_EMAIL` e `PUNTO_PASSWORD` dall’ambiente. Non assume credenziali demo e non carica automaticamente `.env.docker`. Crea dati di prova univoci con prefisso `codex-test-`, registra ore e verifica permessi, Unicode, aggiornamenti, audit e revoca senza perdita dello storico. Non svuota tabelle o elimina account esistenti; le fixture restano nel database. `PUNTO_MYSQL_TEST_MANIFEST` può indicare un file in cui salvare gli identificativi, senza password, per verifiche separate dopo un riavvio.

### Verifica della disponibilità del cluster

Sul cluster locale sono stati verificati tre membri `ONLINE` e `PRIMARY`, nove nuove connessioni attraverso HAProxy distribuite in misura `3/3/3` e il superamento della suite applicativa MySQL completa. Sono passate anche tutte e tre le prove HA:

- Scritture dirette sui tre nodi con identificativi distinti e rollback di una transazione concorrente in conflitto, senza duplicazioni.
- Arresto di un nodo con scritture applicative sui due superstiti e recupero automatico al rientro. Durante la pausa di due nodi, il membro isolato passa in sola lettura e l’app restituisce HTTP `503`; dopo il ripristino le scritture riprendono.
- Arresto completo dei tre nodi e recupero tramite confronto dei GTID, con conteggi applicativi invariati e login funzionante.

Al termine delle prove i tre membri e tutti i servizi sono stati ripristinati.

La suite dedicata è esclusa dalle esecuzioni ordinarie. Per abilitarla esplicitamente, dopo un backup e soltanto su un cluster di sviluppo:

```sh
PUNTO_CLUSTER_HA_TEST=1 python3 tests/test_cluster_ha.py -v
```

Il test legge `.env.docker`, arresta, mette in pausa e riavvia i membri per verificare il comportamento con guasti e perdita di quorum, poi ripristina i servizi nel blocco finale di pulizia. Durante le prove l’app può essere temporaneamente indisponibile. `PUNTO_CLUSTER_ENV_FILE` permette di scegliere un altro file di configurazione.

Per una verifica manuale, controlla inizialmente che `status` mostri tre membri `ONLINE` e `PRIMARY`. Verifica poi che una scrittura su ciascun membro compaia sugli altri, con identificativi distinti e uno storico coerente.

Arrestando un solo membro con `docker compose --env-file .env.docker stop db2`, l’app deve continuare a leggere e scrivere sui due membri rimasti. Dopo `docker compose --env-file .env.docker start db2`, attendi il rientro del nodo e ricontrolla lo stato. Se vengono arrestati due membri, il proxy deve escludere il membro rimasto e le scritture devono fallire senza modifiche parziali. Ripristina entrambi e verifica la convergenza. Dopo un arresto completo usa `recover`, quindi ripeti il controllo dello storico e dei dati.

Questi sono criteri di verifica operativa; `make test` da solo non simula guasti dei container o il recupero di un cluster completo.

## Struttura

```text
src/server.c           HTTP, sessioni e health check
src/accounts.c         Utenti, password e ruoli
src/workspace.c        Commesse, abilitazioni e ore
src/storage.c          Migrazioni e audit
src/db.h               Interfaccia comune SQLite/MySQL
src/db_mysql.c         Driver MySQL tramite MariaDB Connector/C
src/mysql_schema.c     Schema MySQL e protezione delle scritture concorrenti
public/               Login e area di lavoro
Makefile               Compilazione nativa e test
Dockerfile             Compilazione e runtime dell’app MySQL
compose.yaml           App, tre MySQL, HAProxy, NPM e reti dedicate
.env.docker.example    Modello delle impostazioni Docker
scripts/configure_proxy.py  Configurazione iniziale del reverse proxy
scripts/manage_cluster.py   Inizializzazione, stato, backup e recupero del cluster
tests/                 Test HTTP e migrazioni
tests/test_cluster_ha.py  Prove opzionali di guasto dei membri MySQL
data/punto.db          Database SQLite nativo, escluso dal versionamento
data/cluster-state.json  Marcatori locali di inizializzazione e seed, esclusi dal versionamento
backups/               Dump MySQL con permessi 0600, esclusi dal versionamento
nginx-proxy-manager/   Configurazione, SQLite e certificati NPM persistenti
```

L’integrazione con **Google Calendar** e le altre funzioni del piano — attività, Kanban, pianificazione, budget, approvazioni e area cliente — sono sviluppi successivi e non sono ancora implementati.
