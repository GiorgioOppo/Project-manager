# Punto Ticket — gestione ticket in C

Il servizio Compose si chiama `tickets` e la sua immagine locale è `punto/tickets:${PUNTO_IMAGE_TAG:-local}`. Per aggiornare un’installazione con i nomi precedenti seguire la [guida nomenclatura Docker](../docs/tehnical/nomi-servizi-e-immagini.md); directory, volumi e alias di rete conservano i nomi già previsti.

App web autonoma in italiano, affiancata a Punto. Server HTTP in C11, archivio SQLite dedicato, interfaccia HTML/CSS/JavaScript senza npm. Account, sessioni e ticket sono indipendenti da quelli di Punto; non vengono importati dati o credenziali dell'app principale.

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
- Gestori e operatori con account personali. Il gestore crea, aggiorna e disattiva gli account; entrambi i profili gestiscono tutti i ticket del gruppo di lavoro.
- Controllo della revisione: una modifica concorrente restituisce un conflitto e richiede di ricaricare il ticket.

Il richiedente è un campo descrittivo, non un account esterno. Questa versione è una coda interna condivisa: non include portale clienti, allegati, notifiche email, SLA, sincronizzazione con commesse o accesso unico con Punto. I ticket si chiudono senza cancellare lo storico.

La bacheca iniziale mostra il lavoro aperto nelle colonne **Aperto**, **In corso** e **In attesa**. La coda **Tutti** include anche risolti e chiusi. Ogni colonna carica venti ticket e permette di mostrarne altri; i conteggi includono tutti i risultati dei filtri, anche quelli ancora da caricare. Aprire una scheda per modificare stato, priorità, assegnatario e scadenza. I contatori delle code rappresentano il carico globale del team, mentre il totale della vista e quelli delle colonne seguono i filtri selezionati.

Le code operative escludono risolti e chiusi. **Scaduti** contiene le scadenze precedenti a oggi; **In scadenza** comprende oggi e i sette giorni successivi. L’interfaccia usa la data di **Europe/Rome**. I link ai ticket richiedono comunque un account autenticato.

## Avvio nativo

Dipendenze: compilatore C11, `make`, `pkg-config`, SQLite, OpenSSL e json-c. Su macOS possono essere installate con `brew install pkg-config openssl@3 json-c sqlite`; su Debian con `apt install build-essential pkg-config libssl-dev libsqlite3-dev libjson-c-dev`.

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

I due file Compose descrivono modalità alternative. Quella indipendente e quella affiancata usano nomi di progetto e volumi effettivi potenzialmente diversi: non passare dall'una all'altra aspettandosi un'importazione automatica. Per spostare dati, arrestare l'app sorgente ed eseguire un backup e ripristino esplicito dell'archivio.

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

Le password sono conservate come derivazioni PBKDF2 con sale casuale. Le sessioni risiedono in memoria e scadono dopo otto ore; un riavvio richiede un nuovo accesso. Il browser riceve cookie HttpOnly e SameSite=Strict; con origine HTTPS viene aggiunto Secure. Le scritture richiedono il token CSRF restituito dalla sessione. Il server controlla Host e Origin, usa query preparate e registra ticket e storico nella stessa transazione.

Il server nativo parla HTTP. Il TLS pubblico viene terminato dal reverse proxy. Per l'esposizione remota configurare un'origine HTTPS dedicata e il relativo proxy.

## API

Le API restituiscono JSON. Gli errori hanno forma `{"ok":false,"error":"messaggio"}`. Login e creazione usano `POST`; gli aggiornamenti `PUT`. Per le mutazioni autenticate inviare `Content-Type: application/json`, il cookie di sessione e `X-CSRF-Token`.

| Metodo e percorso | Contenuto / risultato |
| --- | --- |
| `GET /healthz` | Salute del servizio, senza autenticazione |
| `POST /api/login` | `{email,password}` → `{ok,user,csrf_token}` e cookie |
| `GET /api/session` | Utente e token CSRF; `401` senza sessione |
| `POST /api/logout` | Termina la sessione corrente |
| `GET /api/users` | `{items}`; gli operatori vedono gli account attivi |
| `POST /api/users` | Solo gestori: `{name,email,password,role}` → `{user}` |
| `PUT /api/users/:id` | Solo gestori: campi `name`, `role`, `active`, `password` opzionali |
| `GET /api/tickets` | `{items,total,stats,queues,reference_date}`; parametri `search`, `status`, `priority`, `assignee_id`, `queue`, `sort`, `today`, `limit`, `offset` |
| `POST /api/tickets` | `{title,description,requester,priority,assignee_id,due_date}` → `{ticket}` |
| `GET /api/tickets/:id` | `{ticket,comments,history}` |
| `PUT /api/tickets/:id` | Campi del ticket, `status` opzionale e `revision` obbligatoria → `{ticket}` |
| `POST /api/tickets/:id/comments` | `{body}` → `{comment}` |

Ruoli: `admin`, `agent`. Stati: `open`, `in_progress`, `waiting`, `resolved`, `closed`. Priorità: `low`, `normal`, `high`, `urgent`. La scadenza è una data valida `YYYY-MM-DD` oppure `null`; l'assegnatario è l'ID di un account attivo oppure `null`. Il filtro `assignee_id=unassigned` seleziona i ticket non assegnati. I contatori `stats` e `queues` si riferiscono a tutti i ticket, indipendentemente dai filtri. La revisione protegge gli aggiornamenti del ticket; un commento aggiorna la data dell’ultima attività senza modificare la revisione.

`queue` accetta `all` (predefinito), `active`, `mine`, `unassigned`, `overdue` e `due_soon`. Tutte le code eccetto `all` includono solo gli stati attivi. `mine` usa l’utente della sessione. I filtri si combinano con AND. `today` è una data valida `YYYY-MM-DD`: il browser invia quella italiana; se omessa, l’API usa il giorno UTC e lo restituisce in `reference_date`. `due_soon` include gli estremi da `today` a `today + 7 giorni`.

`sort` accetta `updated` (predefinito, più recenti prima), `priority` (urgente, alta, normale, bassa, poi scadenza) e `due` (scadenze più vicine, poi priorità). Le scadenze assenti vengono dopo quelle presenti; aggiornamento e ID decrescenti risolvono le parità. `limit` va da 1 a 100, `offset` da 0 a 1.000.000. Parametri sconosciuti, duplicati o non validi restituiscono `400`.

## Verifica e backup

```sh
make test-tickets
```

I test compilano il server e usano database temporanei, credenziali sintetiche e porte loopback, senza scrivere nei dati di Punto. Coprono workflow, permessi, commenti, storico, revisioni, persistenza, code operative, confini delle scadenze, ordinamento e controlli HTTP.

Per il backup più semplice arrestare soltanto `tickets`, copiare l'intera directory dati o il volume dedicato, quindi riavviarlo. Per backup a caldo usare l'API SQLite Backup o il comando `.backup` di SQLite: copiare il solo file `.db` mentre il processo scrive può perdere dati presenti nel WAL. Conservare il backup in una posizione protetta perché contiene i ticket, i commenti e gli account.
