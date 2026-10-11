# Addon Google Calendar

Il servizio Compose si chiama `calendar` e la sua immagine locale è `punto/calendar:${PUNTO_IMAGE_TAG:-local}`. Per aggiornare un’installazione con i nomi precedenti seguire la [guida nomenclatura Docker](../docs/tehnical/nomi-servizi-e-immagini.md); directory, volumi e identità TLS conservano i nomi già previsti.

`punto-calendar` è un servizio in C indipendente dall'app, con API HTTPS privata e una coda persistente per le modifiche. Importa gli eventi Google nel proprio archivio e invia a Google creazioni, modifiche e cancellazioni richieste dall'app. Non accede ai database MySQL o MongoDB.

Le credenziali Google non sono ancora configurate: il container e le API funzionano, ma lo stato espone `oauth_configured: false`. Nessun account viene collegato e nessuna chiamata a Google viene effettuata in questa configurazione.

## API privata

Il servizio ascolta su `https://calendar-api:8091`. L'app deve verificare il certificato con la CA interna e inviare `Authorization: Bearer <token dedicato>`. L'indirizzo IP dell'app è verificato separatamente dal token. Soltanto `GET /healthz` non richiede autenticazione e resta limitato alle sorgenti ammesse o al container stesso.

L'ID utente nel percorso è quello dell'utente autenticato dall'app. Il browser non può chiamare direttamente questa API né scegliere un altro ID attraverso il bridge pubblico dell'app.

| Metodo e percorso | Uso |
| --- | --- |
| `GET /healthz` | Verifica HTTPS e disponibilità del processo. |
| `GET /v1/status` | Stato globale, provider, intervallo e configurazione OAuth. |
| `GET /v1/users/<id>/status` | Collegamento, calendario selezionato, sincronizzazione, coda e conflitti dell'utente. |
| `POST /v1/users/<id>/oauth/start` | Crea un consenso Google con state monouso e PKCE. |
| `POST /v1/users/<id>/oauth/callback` | Completa il consenso attraverso l'app. |
| `GET /v1/users/<id>/calendars` | Una pagina dei calendari disponibili. |
| `POST /v1/users/<id>/selection` | Seleziona un calendario con permesso Google `writer` o `owner`. |
| `GET /v1/users/<id>/events` | Legge una pagina della copia locale degli eventi. |
| `GET /v1/users/<id>/changes` | Operazioni recenti e relativi esiti, fino a 100 e 90 KiB; `truncated` segnala altri risultati. |
| `POST /v1/users/<id>/events/create` | Accoda un nuovo evento. |
| `POST /v1/users/<id>/events/update` | Accoda una modifica con controllo ETag. |
| `POST /v1/users/<id>/events/delete` | Accoda una cancellazione con controllo ETag. |
| `POST /v1/users/<id>/sync` | Richiede una sincronizzazione in background; risponde subito `202`. |
| `POST /v1/users/<id>/disconnect` | Revoca il consenso quando possibile e cancella l'archivio locale dell'utente. |

Tutti i POST usano `application/x-www-form-urlencoded`, con i soli campi descritti di seguito. Parametri sconosciuti o duplicati sono rifiutati. Anche gli oggetti JSON con chiavi duplicate sono rifiutati.

- `oauth/start`: `session_binding`, SHA-256 esadecimale della sessione corrente dell'app. Restituisce `authorization_url` e `expires_in: 600`.
- `oauth/callback`: `state`, `code`, `session_binding`; in caso di rifiuto del consenso, `error` sostituisce `code`. La sessione e l'utente devono coincidere con l'avvio. Lo state viene consumato prima dello scambio e non può essere riutilizzato.
- `selection`: `calendar_id`, ricavato dall'elenco Google. Una coda con operazioni `queued` o `unknown` impedisce di cambiare calendario.
- `events/create`: `idempotency_key` ed `event`.
- `events/update`: `idempotency_key`, `event_id`, `etag` ed `event`.
- `events/delete`: `idempotency_key`, `event_id`, `etag`.
- `sync` e `disconnect`: corpo vuoto.

`event` è un oggetto JSON con `summary`, `start`, `end` e, facoltativamente, `description` e `location`. Per gli eventi con orario, `start` e `end` contengono `dateTime` RFC 3339 con secondi interi e offset esplicito o `Z` e possono contenere `timeZone`. Per gli eventi giornalieri contengono `date: YYYY-MM-DD`; la data finale è esclusiva, come nell'API Google. La fine deve seguire l'inizio. Non sono accettati partecipanti, notifiche, allegati o proprietà private arbitrarie; le scritture usano `sendUpdates=none`.

Esempio di valore del campo `event`:

```json
{
  "summary": "Riunione di progetto",
  "start": {"dateTime": "2026-10-12T09:00:00+02:00", "timeZone": "Europe/Rome"},
  "end": {"dateTime": "2026-10-12T10:00:00+02:00", "timeZone": "Europe/Rome"}
}
```

`GET calendars` accetta `page_token` e `limit` da 1 a 50, con valore predefinito 50. La risposta contiene `items` e `next_page_token`, oppure `null` alla fine. Viene eseguita una sola richiesta Google per pagina. La risposta ha un limite di 100 KiB: se una pagina eccezionalmente grande supera il limite, il chiamante deve ridurre `limit`.

`GET events` accetta `limit` da 1 a 100, predefinito 20, e `after_id`. Restituisce `items`, `next_after_id`, `last_sync`, `calendar_id` e `recurrence_mode`. Il limite effettivo può essere inferiore per rispettare 100 KiB; il cursore indica come proseguire. Gli eventuali filtri `from` e `to` vanno forniti insieme, nel formato `YYYY-MM-DD`, con estremi inclusivi e intervallo massimo di 366 giorni. Il filtro confronta gli istanti degli eventi con orario e le date degli eventi giornalieri; gli estremi del filtro sono giorni UTC.

Una singola sincronizzazione ammette al massimo 100 pagine, 20.000 eventi, 64 KiB per evento e 16 MiB complessivi di payload. Superare un limite produce un errore conservando la cache precedente e il relativo token.

Gli eventi ricorrenti sono conservati come **serie ed eccezioni**, non vengono espansi in singole occorrenze. Una serie ricorrente rimane visibile anche quando il suo evento iniziale precede il filtro; gli eventi cancellati restano riconoscibili tramite `status: cancelled`. Non si tratta quindi di una vista delle sole occorrenze nel periodo. Le creazioni di nuove serie ricorrenti non fanno parte di questa prima versione; modificare un ID di serie Google può agire sulla serie intera.

## Sincronizzazione e conflitti

Il worker usa una connessione SQLite distinta da quella dell'API. Le chiamate a Google avvengono fuori dalle transazioni di scrittura; health check e letture dello stato restano disponibili durante la sincronizzazione. Le modifiche accodate vengono elaborate in background, mentre il polling periodico predefinito è di 300 secondi. Lo stato espone `sync_requested` e `sync_running`; `last_sync` cambia soltanto dopo un aggiornamento completo riuscito della cache.

Il primo download e quelli successivi rispettano tutte le pagine Google. Il token incrementale cambia soltanto insieme alla transazione che aggiorna gli eventi. Se Google risponde `410`, viene scaricata una nuova copia completa e la precedente resta disponibile finché il download non è terminato con successo. Il ripristino della cache non elimina la coda delle modifiche.

Ogni operazione ha una chiave di idempotenza per utente. Riutilizzarla con lo stesso contenuto restituisce lo stato già registrato; un contenuto differente restituisce `409`. I contenuti accettati sono normalizzati prima di calcolarne l'impronta. Le creazioni usano un ID Google deterministico. Un marcatore privato lega utente, chiave e contenuto richiesto. Modifiche e cancellazioni inviano `If-Match` e un ETag obsoleto produce uno stato `conflict`, senza sovrascrivere la versione remota.

Prima di una scrittura remota lo stato viene salvato come `unknown`. Dopo un arresto, un timeout o una risposta ambigua il servizio verifica l'evento remoto prima di ritentare. Un marcatore coincidente conferma una scrittura già eseguita; un ETag differente richiede la risoluzione del conflitto. Gli errori temporanei usano attese progressive; gli stati `failed` e `conflict` richiedono un intervento del chiamante. Non viene promessa una consegna esattamente una volta di qualsiasi operazione esterna.

Ogni nuovo consenso e ogni selezione di calendario cambiano la generazione della connessione: le risposte di un worker avviato prima del cambio non possono aggiornare cache, token o stato della nuova connessione.

Ricollegare lo stesso account Google conserva calendario, cache, coda e storico di idempotenza. Un account Google differente viene rifiutato con `google_account_mismatch`: occorre prima una disconnessione esplicita. La disconnessione cancella i dati locali di quell'utente e tenta la revoca del refresh token. Una richiesta remota già in corso può terminare durante la disconnessione; non vengono avviate nuove operazioni dopo la rimozione dell'account.

## Sicurezza e conservazione

I token OAuth, il verificatore PKCE, i payload degli eventi e i payload delle modifiche sono cifrati con **AES-256-GCM**, nonce casuale e dati autenticati che includono l'utente e l'identità della riga. Trapiantare un ciphertext fra utenti, eventi o chiavi di idempotenza ne impedisce la decifratura. Un controllo cifrato all'avvio impedisce di aprire l'archivio con una chiave differente e di azzerarlo inavvertitamente.

Restano in chiaro i metadati operativi: ID utente, account/calendario Google (spesso un indirizzo email), ID evento, ETag, stato, token di sincronizzazione incrementale, chiave di idempotenza, impronte, contatori e date. La chiave crittografica non è nel database. Il servizio decifra in memoria i soli dati dell'utente richiesto dall'app; questa soluzione non esegue calcoli su dati cifrati.

Il servizio usa solo le destinazioni Google previste, HTTPS con verifica della CA e del nome, TLS minimo 1.2, nessun redirect automatico e un tunnel CONNECT forzato attraverso Squid. `NO_PROXY` non può aggirare il proxy. Il container non ha una porta pubblicata, accesso diretto a Internet o credenziali dei database dell'app. Il token API e la chiave di cifratura sono secret montati in sola lettura.

Per il backup, salvare il volume `calendar_addon_data` in modo consistente con SQLite, includendo il WAL oppure usando la funzione SQLite Backup. Custodire **separatamente** la chiave `calendar-token-key` e il token API. Senza la chiave i dati non sono recuperabili. Non cancellare o sostituire chiave e volume separatamente; una futura rotazione richiede una migrazione esplicita dei ciphertext. Il file secret locale è una prima configurazione, non un KMS. Il container è una singola istanza: l'HA del cluster MySQL non rende automaticamente ridondante questo archivio.

## Configurazione Google, da completare in seguito

1. In Google Cloud abilitare Calendar API, configurare la schermata di consenso e creare un client OAuth di tipo **Web application**.
2. Registrare l'URI di ritorno pubblico esatto dell'app, ad esempio `https://progetti.tuaazienda.it/api/calendar/oauth/callback`.
3. Impostare `GOOGLE_CLIENT_ID` e `GOOGLE_REDIRECT_URI`; inserire il segreto nel file predisposto per `GOOGLE_CLIENT_SECRET_FILE`, senza inserirlo nel repository o nell'interfaccia chat.
4. Ricreare il servizio con la configurazione completa, poi avviare il consenso dall'app per ogni utente. Configurazioni parziali sono rifiutate all'avvio.

Gli scope sono `calendar.events` e `calendar.calendarlist.readonly`. Il consenso richiede accesso offline e PKCE `S256`. Refresh token revocati o scaduti richiedono un nuovo consenso. Le regole Google per applicazioni in test, utenti autorizzati e verifica degli scope vanno configurate nel progetto Google prima dell'uso reale.

Variabili principali: `CALENDAR_BIND`, `CALENDAR_PORT`, `CALENDAR_ALLOWED_CLIENT`, `CALENDAR_TLS_CERT_FILE`, `CALENDAR_TLS_KEY_FILE`, `CALENDAR_API_TOKEN_FILE`, `CALENDAR_TOKEN_KEY_FILE`, `CALENDAR_DB_PATH`, `CALENDAR_PROXY_URL`, `CALENDAR_SYNC_INTERVAL`, `CALENDAR_HTTP_TIMEOUT`, `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET_FILE`, `GOOGLE_REDIRECT_URI`.

## Compilazione e collaudo

```sh
make -C calendar-addon
python3 -m unittest discover -s tests -p 'test_calendar_addon.py' -v
```

Dipendenze: compilatore C11, pthread, OpenSSL, libcurl, SQLite, json-c e pkg-config. Il Dockerfile usa un'immagine Debian con utente non privilegiato UID/GID 10004.

I test usano server OAuth/Calendar HTTPS e proxy CONNECT locali. L'override `CALENDAR_GOOGLE_ORIGIN` è consentito solo con `CALENDAR_TEST_MODE=1`, listener loopback e proxy loopback. `CALENDAR_GOOGLE_CA_FILE` permette di verificare la CA del fixture; la verifica TLS resta obbligatoria. Le prove non usano account Google e non creano eventi reali.

Questa aggiunta espone il servizio e il bridge API. Non aggiunge ancora una nuova interfaccia grafica del calendario, associazioni automatiche alle commesse o registrazioni automatiche delle ore.

Riferimenti: [OAuth web server Google](https://developers.google.com/identity/protocols/oauth2/web-server), [sincronizzazione incrementale](https://developers.google.com/workspace/calendar/api/guides/sync), [versioni ed ETag](https://developers.google.com/workspace/calendar/api/guides/version-resources).
