# Punto Ticket — gestione ticket in C

L’interfaccia supporta italiano, inglese, cinese semplificato, spagnolo, francese e tedesco. Il selettore Lingua è disponibile prima e dopo l’accesso; la scelta resta nel browser e segue i collegamenti verso Punto. I cataloghi condivisi si aggiornano con `python3 scripts/sync_locales.py` dalla radice del progetto.

Il servizio Compose si chiama `tickets` e la sua immagine locale è `punto/tickets:${PUNTO_IMAGE_TAG:-local}`. Per aggiornare un’installazione con i nomi precedenti seguire la [guida nomenclatura Docker](../docs/tehnical/nomi-servizi-e-immagini.md); directory, volumi e alias di rete conservano i nomi già previsti.

App web autonoma in italiano, affiancata a Punto. Server HTTP in C11, archivio SQLite dedicato, interfaccia HTML/CSS/JavaScript senza npm. Account, sessioni e ticket hanno un archivio dedicato. Il collegamento facoltativo a Punto permette di associare commesse e asset ai ticket, rispettando i permessi della sessione Punto collegata. Il login può usare Keycloak in comune con le altre app, associando esplicitamente l’identità aziendale a un account Ticket locale attivo.

## Funzioni

- Apertura ticket con descrizione, richiedente, priorità, assegnatario e scadenza.
- Stati **Aperto**, **In lavorazione**, **In attesa**, **Risolto**, **Chiuso**, con possibilità di riapertura.
- Priorità **Bassa**, **Normale**, **Alta**, **Urgente**.
- Bacheca per stato ed elenco paginato, con ricerca e filtri combinabili.
- Code rapide **Da lavorare**, **I miei**, **Non assegnati**, **Scaduti** e **In scadenza** nei prossimi sette giorni.
- Ordinamento per ultimo aggiornamento, priorità o scadenza; date e avvisi leggibili anche su mobile.
- Preferenze di vista e filtri conservate per account nel browser, senza salvare il testo delle ricerche.
- Collegamenti diretti ai ticket e azione **Prendi in carico** per assegnarsi una richiesta e avviarne la lavorazione.
- Commenti e storico delle modifiche, conservati insieme ai ticket.
- Riferimenti a commesse e asset reali, filtri dedicati e collegamenti in entrambe le direzioni.
- Gestori e operatori con account personali. Il gestore gestisce gli account Ticket; i ticket collegati seguono le abilitazioni della sessione Punto, anche per l’amministratore Ticket.
- Controllo della revisione: una modifica concorrente restituisce un conflitto e richiede di ricaricare il ticket.

Il richiedente è un campo descrittivo, non un account esterno. Questa versione è una coda interna: non include portale clienti, allegati, notifiche email o SLA. I ticket si chiudono senza cancellare lo storico. Il collegamento ai cataloghi usa una delega di lettura della sessione Punto; l'accesso aziendale OIDC resta una configurazione separata.

La bacheca iniziale mostra il lavoro aperto nelle colonne **Aperto**, **In corso** e **In attesa**. La coda **Tutti** include anche risolti e chiusi. Ogni colonna carica venti ticket e permette di mostrarne altri; i conteggi includono tutti i risultati dei filtri, anche quelli ancora da caricare. Aprire una scheda per modificare stato, priorità, assegnatario e scadenza. I contatori delle code rappresentano tutti i ticket accessibili alla sessione, mentre il totale della vista e quelli delle colonne seguono i filtri selezionati.

Le code operative escludono risolti e chiusi. **Scaduti** contiene le scadenze precedenti a oggi; **In scadenza** comprende oggi e i sette giorni successivi. L’interfaccia usa la data di **Europe/Rome**. I link ai ticket richiedono comunque un account autenticato.

## Avvio nativo

Dipendenze: compilatore C11, `make`, `pkg-config`, SQLite, OpenSSL, json-c e libcurl. Su macOS possono essere installate con `brew install pkg-config openssl@3 json-c sqlite curl`; su Debian con `apt install build-essential pkg-config libssl-dev libsqlite3-dev libjson-c-dev libcurl4-openssl-dev`.

Dalla radice del repository:

```sh
make tickets
cd ticket-app
export TICKETS_EMAIL='gestore@example.test'
export TICKETS_NAME='Gestore'
read -rs 'TICKETS_PASSWORD?Password iniziale (almeno 12 caratteri): '
export TICKETS_PASSWORD
./build/punto-tickets
```

Il comando `read` mostrato è per zsh, la shell predefinita di macOS. In bash usare `read -rs -p 'Password iniziale: ' TICKETS_PASSWORD` e poi `export TICKETS_PASSWORD`.

Aprire [http://localhost:8083](http://localhost:8083) e accedere con le credenziali scelte. Il server ascolta sul loopback; `Ctrl+C` lo arresta. Il database predefinito è `ticket-app/data/tickets.db` quando si avvia dalla cartella `ticket-app`. Per il primo avvio servono email e password di almeno 12 caratteri; su un database già inizializzato queste variabili non cambiano gli account. Le password si aggiornano dalla gestione utenti.

Per avviare nuovamente dalla radice:

```sh
make run-tickets
```

## Docker indipendente

```sh
cp ticket-app/.env.example ticket-app/.env
```

Compilare `TICKETS_EMAIL` e `TICKETS_PASSWORD` nel file locale, quindi:

```sh
docker compose --env-file ticket-app/.env -f ticket-app/compose.yaml up --build -d
docker compose --env-file ticket-app/.env -f ticket-app/compose.yaml ps
```

Aprire [http://localhost:8083](http://localhost:8083). La porta è pubblicata soltanto su `127.0.0.1`. Il volume `tickets_data` conserva SQLite; l'app gira con UID/GID `10006`, filesystem in sola lettura e nessuna dipendenza dai database di Punto. La modalità autonoma usa una rete bridge standard per consentire la pubblicazione della porta locale; il container può raggiungere la rete esterna. L'overlay affiancato a Punto usa invece la rete privata interna dello stack.

`TICKETS_HOST_PORT` cambia la porta host: aggiornare anche `TICKETS_PUBLIC_ORIGIN`, per esempio `http://localhost:8183`. L'origine è esatta, senza slash finale. Quando è impostata, usare nel browser il nome configurato.

## Affiancamento allo stack Punto e Nginx Proxy Manager

L'overlay facoltativo [compose.tickets.yaml](../compose.tickets.yaml) aggiunge il servizio `tickets` alla rete privata già usata da NPM, all'indirizzo `172.30.20.11:8083`, senza pubblicare porte host e senza collegarlo alle reti dei database. Lo stack Punto deve essere già configurato e avviato secondo il [README principale](../README.md).

1. Preparare `ticket-app/.env` come sopra e impostare `TICKETS_PUBLIC_ORIGIN` all'origine pubblica dedicata, ad esempio `https://ticket.tuaazienda.it`.
2. Avviare soltanto il nuovo servizio dalla radice del repository:

   ```sh
   docker compose --env-file .env.docker --env-file ticket-app/.env \
     -f compose.yaml -f compose.tickets.yaml up --build -d tickets
   ```

3. In Nginx Proxy Manager creare un **Proxy Host** per il nome scelto: schema `http`, destinazione `ticket-app` (alias del servizio `tickets`), porta `8083`. Conservare l'header Host del browser, come nella configurazione standard di NPM. Configurare il certificato per il dominio e l'inoltro HTTPS coerente con l'origine scelta.
4. Aprire l'origine configurata. Per consultare lo stato usare gli stessi file Compose con `ps tickets`.

Per configurare più indirizzi, sessioni separate e regole NPM ripetibili, vedere
la [guida alla pubblicazione di Ticket](../docs/tehnical/ticket-reverse-proxy.md).
`TICKETS_ADDITIONAL_ORIGINS` ammette fino a tre origini aggiuntive esatte, separate
da virgole senza spazi. Ogni Host deve avere un solo schema; gli ingressi HTTP
di un sito HTTPS vengono reindirizzati da Nginx. Il login aziendale rimane
disponibile soltanto sull'origine della redirect URI OIDC configurata.

I due file Compose descrivono modalità alternative. Quella indipendente e quella affiancata usano nomi di progetto e volumi effettivi potenzialmente diversi: non passare dall'una all'altra aspettandosi un'importazione automatica. Per spostare dati, arrestare l'app sorgente ed eseguire un backup e ripristino esplicito dell'archivio.

## Collegamento a commesse e asset

Configurare su Punto `TICKETS_APP_ORIGIN` con l’origine pubblica di Ticket e su Ticket `TICKETS_PUNTO_ORIGIN` con quella di Punto, senza slash finale. In produzione entrambe devono essere HTTPS. L’overlay `compose.tickets.yaml` passa la prima variabile a Punto e le variabili del connettore a Ticket. Con NPM sulla rete privata impostare anche `TICKETS_PUNTO_RESOLVE_IP=172.30.20.2`: il client raggiunge NPM direttamente mantenendo Host, SNI e verifica del certificato del dominio pubblico. Le CA pubbliche sono già installate nell’immagine. Per una CA privata montare il PEM in sola lettura con un override Compose e impostare `TICKETS_PUNTO_CA_FILE` nel servizio Ticket.

Dopo la modifica ricompilare e ricreare **entrambi** i servizi, usando gli stessi file e segreti dello stack:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  -f compose.yaml -f compose.tickets.yaml up --build -d app tickets
```

Per una prova nativa: Punto su `http://127.0.0.1:8080` con `TICKETS_APP_ORIGIN=http://127.0.0.1:8083`; Ticket con `TICKETS_PUNTO_ORIGIN=http://127.0.0.1:8080`. HTTP è ammesso soltanto per origini loopback. Non usare `127.0.0.1` per collegare due container.

1. Accedere a Ticket e premere **Collega Punto**. Il browser apre Punto e, se necessario, richiede l’accesso.
2. Tornando a Ticket, il riquadro mostra il nome dell’account Punto collegato. Il modulo ticket permette di cercare e scegliere una commessa e/o un asset. Non viene creata una relazione di proprietà fra commessa e asset.
3. **Ticket** e **+ Ticket** nella riga commessa, oppure **Ticket collegati** e **Nuovo ticket** nella scheda asset, aprono la coda filtrata o un modulo già contestualizzato. Nel dettaglio del ticket i riferimenti riportano alla commessa o all’asset.

Senza una sessione Punto collegata sono visibili soltanto i ticket senza riferimenti. Con il collegamento attivo sono visibili i ticket degli asset e delle commesse consultabili dall’account Punto. I permessi vengono verificati sul server a ogni richiesta, anche per dettagli, commenti, modifiche e contatori. Non si associano account per email o ID. La delega è di sola lettura, dura al massimo otto ore, resta in memoria ed è revocata da logout/scadenza della sessione Punto o dal riavvio di uno dei servizi; ripetere **Collega Punto**. **Scollega** la rimuove dalla sessione Ticket corrente.

Il server risolve ID e nomi dai cataloghi di Punto, attraverso le sue API di lettura; non accede ai database principali e non riceve il token privato dell’inventario. Codice e nome vengono conservati nel ticket come riferimenti storici. Modificare altri campi non sostituisce questi riferimenti. In caso di indisponibilità di Punto la lettura dei ticket collegati fallisce senza ampliare l’accesso; i ticket non collegati restano utilizzabili. Un asset non disponibile non impedisce di conservare un collegamento già salvato.

La migrazione SQLite dalla versione 1 alla 2 aggiunge riferimenti nullable e indici in una transazione; i ticket esistenti restano senza collegamenti. Effettuare il backup descritto sotto prima dell’aggiornamento. Il ripristino di una versione precedente dell’eseguibile richiede anche il ripristino del relativo database.

## Configurazione

| Variabile | Predefinito nativo | Uso |
| --- | --- | --- |
| `TICKETS_BIND` | `127.0.0.1` | Indirizzo IP numerico di ascolto |
| `TICKETS_PORT` | `8083` | Porta HTTP interna |
| `TICKETS_PUBLIC_DIR` | `public` | Cartella dell'interfaccia, relativa alla directory di avvio |
| `TICKETS_DB_PATH` | `data/tickets.db` | Database dedicato; la directory viene creata se necessario |
| `TICKETS_PUBLIC_ORIGIN` | Origini loopback sulla porta configurata | Origine HTTP/HTTPS esatta autorizzata, senza percorso |
| `TICKETS_EMAIL` | Nessuno | Email del primo gestore |
| `TICKETS_PASSWORD` | Nessuno | Password del primo gestore, almeno 12 caratteri |
| `TICKETS_NAME` | `Gestore` | Nome del primo gestore |
| `TICKETS_PUNTO_ORIGIN` | Disabilitato | Origine HTTPS di Punto; HTTP ammesso solo sul loopback |
| `TICKETS_PUNTO_RESOLVE_IP` | Risoluzione DNS | IPv4 fissa facoltativa per raggiungere l’origine HTTPS, mantenendo la verifica TLS |
| `TICKETS_PUNTO_CA_FILE` | CA di sistema | CA PEM privata facoltativa, leggibile dal processo |
| `AUTH_MODE` | `local` | `local`, `hybrid` (locale + Keycloak), `oidc` (solo Keycloak) |
| `OIDC_ISSUER_URL` | Nessuno | Issuer pubblico esatto del realm Keycloak, ad esempio `https://auth.example.it/realms/punto` |
| `OIDC_BACKCHANNEL_URL` | Issuer pubblico | URL del realm raggiungibile dal server per token e chiavi; deve corrispondere allo stesso realm |
| `OIDC_CLIENT_ID` | Nessuno | Client OIDC dedicato a Ticket |
| `OIDC_CLIENT_SECRET_FILE` | Nessuno | File del secret del client, leggibile dal processo Ticket |
| `OIDC_REDIRECT_URI` | Nessuno | URL pubblico esatto di `/auth/callback` su Ticket |
| `OIDC_BINDINGS_FILE` | Nessuno | File JSON che associa i subject Keycloak a email locali Ticket |
| `OIDC_CA_FILE` | CA di sistema | CA PEM per le connessioni HTTPS al provider |
| `OIDC_ALLOW_HTTP` | `0` | Abilita HTTP esplicitamente solo per prove locali |

Le password sono conservate come derivazioni PBKDF2 con sale casuale. Le sessioni risiedono in memoria e scadono dopo otto ore; quelle create tramite Keycloak hanno anche il limite di durata del token verificato. Un riavvio richiede un nuovo accesso. Il browser riceve cookie HttpOnly e SameSite=Strict; con origine HTTPS viene aggiunto Secure. Le scritture richiedono il token CSRF restituito dalla sessione. Il server controlla Host e Origin, usa query preparate e registra ticket e storico nella stessa transazione.

In `hybrid` e `oidc` il browser apre il login Keycloak e torna al callback con un codice monouso; token e secret restano sul server. La transazione usa state, nonce, PKCE e un cookie temporaneo HttpOnly SameSite=Lax. Il modulo condiviso verifica il token e risolve il subject tramite il file di associazioni; Ticket richiede poi che l’account locale sia attivo. I ruoli `admin`/`agent` rimangono quelli dell’archivio Ticket. L’email dichiarata nel token non crea o collega account automaticamente. Una configurazione incompleta impedisce l’avvio prima dell’apertura del database.

Il file `OIDC_BINDINGS_FILE` contiene un oggetto JSON, per esempio `{"subject-keycloak-del-gestore":"gestore@example.test"}`. Usare il valore `sub` stabile dell’utente nel realm configurato, non il nome utente o l’email Keycloak. Il file è specifico dell’app: la stessa identità può essere associata a un account diverso in Punto. Conservare e montare in sola lettura sia questo file sia il secret del client; una modifica viene applicata riavviando Ticket.

Per Docker multi app usare gli overlay Keycloak e la configurazione `KEYCLOAK_*` generata da `scripts/configure_keycloak.py`. L’overlay Ticket usa il backchannel TLS `https://keycloak:8443/realms/punto` e monta `/run/punto-auth/client-secret`, `/run/punto-auth/bindings.json` e `/run/punto-auth/ca.crt`. Le variabili della tabella sono quelle lette dal processo; per un avvio nativo esportarle con percorsi assoluti dell’host. Il Compose Ticket indipendente mantiene il login locale.

Il primo account e gli altri utenti si preparano come prima, senza cambiare gli ID o importare credenziali da Punto. In `oidc` il form locale è nascosto e `POST /api/login` restituisce `403`; le password locali conservate nell’archivio non sono utilizzabili finché questa modalità resta attiva. Il pulsante **Esci** chiude soltanto la sessione Ticket: la sessione Keycloak e le altre app restano attive, quindi un nuovo accesso aziendale può essere immediato.

Il server nativo parla HTTP. Il TLS pubblico viene terminato dal reverse proxy. Per l'esposizione remota configurare un'origine HTTPS dedicata e il relativo proxy.

## API

Le API restituiscono JSON. Gli errori hanno forma `{"ok":false,"error":"messaggio"}`. Login e creazione usano `POST`; gli aggiornamenti `PUT`. Per le mutazioni autenticate inviare `Content-Type: application/json`, il cookie di sessione e `X-CSRF-Token`.

| Metodo e percorso | Contenuto / risultato |
| --- | --- |
| `GET /healthz` | Salute del servizio, senza autenticazione |
| `GET /api/auth/config` | `{sso,local}`; modalità di accesso pubbliche, senza secret |
| `GET /auth/login` | Avvia l’accesso aziendale con redirect HTTP `303` e cookie temporaneo |
| `GET /auth/callback` | Verifica il codice OIDC, crea una sessione Ticket e redirige a `/` |
| `POST /api/login` | `{email,password}` → `{ok,user,csrf_token}` e cookie; `403` in modalità `oidc` |
| `GET /api/session` | Utente e token CSRF; `401` senza sessione |
| `POST /api/logout` | Termina la sessione corrente |
| `GET /api/users` | `{items}`; gli operatori vedono gli account attivi |
| `POST /api/users` | Solo gestori: `{name,email,password,role}` → `{user}` |
| `PUT /api/users/:id` | Solo gestori: campi `name`, `role`, `active`, `password` opzionali |
| `GET /api/integrations/punto/status` | `{configured,connected,user_name,expires_at,origin}` della sessione corrente |
| `POST /api/integrations/punto/start` | `{}` → `{state,url}` per aprire Punto |
| `POST /api/integrations/punto/connect` | `{state,grant}` → stato; richiede lo state pendente della stessa sessione |
| `POST /api/integrations/punto/disconnect` | `{}` → stato scollegato |
| `GET /api/integrations/punto/jobs` | Catalogo commesse abilitate; `search`, `limit`, `offset` |
| `GET /api/integrations/punto/assets` | Catalogo asset; `q`, `limit`, `after_id` |
| `GET /api/integrations/punto/validate` | `job_id` e/o `asset_id` → `{job,asset}` canonici |
| `GET /api/tickets` | `{items,total,stats,queues,reference_date}`; parametri `search`, `status`, `priority`, `assignee_id`, `job_id`, `asset_id`, `queue`, `sort`, `today`, `limit`, `offset` |
| `POST /api/tickets` | `{title,description,requester,priority,assignee_id,due_date,job_id,asset_id}` → `{ticket}` |
| `GET /api/tickets/:id` | `{ticket,comments,history}` |
| `PUT /api/tickets/:id` | Campi del ticket, `status` opzionale e `revision` obbligatoria → `{ticket}` |
| `POST /api/tickets/:id/comments` | `{body}` → `{comment}` |

Ruoli: `admin`, `agent`. Stati: `open`, `in_progress`, `waiting`, `resolved`, `closed`. Priorità: `low`, `normal`, `high`, `urgent`. La scadenza è una data valida `YYYY-MM-DD` oppure `null`; l'assegnatario è l'ID di un account attivo oppure `null`. Il filtro `assignee_id=unassigned` seleziona i ticket non assegnati. I contatori `stats` e `queues` si riferiscono a tutti i ticket visibili all’account, indipendentemente dai filtri. La revisione protegge gli aggiornamenti del ticket; un commento aggiorna la data dell’ultima attività senza modificare la revisione.

`job_id` e `asset_id` sono ID interi positivi oppure `null` nelle scritture; nei filtri sono ID positivi. I nomi non sono accettati dal browser: il server aggiunge `job_code`, `job_name`, `asset_tag`, `asset_name` dai cataloghi. La ricerca comprende questi riferimenti. Un aggiornamento che omette gli ID conserva i riferimenti esistenti; `null` li rimuove. Le risposte dei cataloghi contengono soltanto ID, codice/tag, nome, stato e tipologia dell’asset; nessuna credenziale dell’inventario.

Punto espone `POST /api/integrations/tickets/grant` alla sua sessione autenticata, con form `state` di 64 caratteri esadecimali minuscoli e verifica di origine. Le letture `/api/integrations/tickets/{scope,jobs,assets,validate}` richiedono la delega opaca Bearer, mai un ID utente scelto dal client. Nessuna di queste API modifica commesse o asset.

`queue` accetta `all` (predefinito), `active`, `mine`, `unassigned`, `overdue` e `due_soon`. Tutte le code eccetto `all` includono solo gli stati attivi. `mine` usa l’utente della sessione. I filtri si combinano con AND. `today` è una data valida `YYYY-MM-DD`: il browser invia quella italiana; se omessa, l’API usa il giorno UTC e lo restituisce in `reference_date`. `due_soon` include gli estremi da `today` a `today + 7 giorni`.

`sort` accetta `updated` (predefinito, più recenti prima), `priority` (urgente, alta, normale, bassa, poi scadenza) e `due` (scadenze più vicine, poi priorità). Le scadenze assenti vengono dopo quelle presenti; aggiornamento e ID decrescenti risolvono le parità. `limit` va da 1 a 100, `offset` da 0 a 1.000.000. Parametri sconosciuti, duplicati o non validi restituiscono `400`.

## Verifica e backup

```sh
make test-tickets
```

I test compilano i server e usano database temporanei, credenziali sintetiche e porte loopback, senza scrivere nei dati di Punto. Coprono workflow, permessi, commenti, storico, revisioni, persistenza, code operative, scadenze, ordinamento, controlli HTTP, migrazione dello schema e collegamenti ai cataloghi con revoca dei permessi e indisponibilità delle sorgenti.

La suite `tests/test_tickets_oidc.py` usa un provider OIDC locale con vere firme RSA; non richiede un Keycloak esterno. Verifica collegamento esplicito agli account, ruoli locali, CSRF, durata e revoca della sessione, riuso dei callback, modalità `oidc`, account disattivati, subject sconosciuti e firme non valide.

Per il backup più semplice arrestare soltanto `tickets`, copiare l'intera directory dati o il volume dedicato, quindi riavviarlo. Per backup a caldo usare l'API SQLite Backup o il comando `.backup` di SQLite: copiare il solo file `.db` mentre il processo scrive può perdere dati presenti nel WAL. Conservare il backup in una posizione protetta perché contiene i ticket, i commenti e gli account.
