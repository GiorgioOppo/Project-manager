# API private degli addon mail e calendario

Verificato sul codice il **10 ottobre 2026**. I servizi sono programmi C separati dall'applicazione, eseguiti in container dedicati. SMTP e credenziali Google sono predisposti, ma **la configurazione dei fornitori reali è rinviata**: i test usano servizi simulati.

Fonti: [mailer.c](../../addon/src/mailer.c), [mail_client.c](../../src/mail_client.c), [calendar.c](../../calendar-addon/src/calendar.c), [calendar_client.c](../../src/calendar_client.c), [scheduler alert](../../src/alerts.c), [Compose](../../compose.yaml). Guide dei componenti: [mail-addon](../../addon/README.md) e [calendar-addon](../../calendar-addon/README.md).

## Trasporto, autenticazione e confini

| Servizio | API privata | Sorgente applicativa | Rete API | Uscita |
| --- | --- | --- | --- | --- |
| `mail-addon` | `https://mail-api:8090`, bind `172.30.60.2` | `172.30.60.10` | `mail_api` | `172.30.40.11` su `app_egress`, verso Squid. |
| `calendar-addon` | `https://calendar-api:8091`, bind `172.30.70.2` | `172.30.70.10` | `calendar_api` | `172.30.40.12` su `app_egress`, verso Squid. |

Le API non pubblicano porte sull'host e non sono instradate da Nginx al browser. L'app usa una CA interna, verifica hostname e TLS almeno 1.2; non segue redirect e non usa proxy per questi collegamenti privati. Il servizio verifica separatamente l'IP sorgente e `Authorization: Bearer <token dedicato>`, con confronto a tempo costante. Non si tratta di mTLS: l'autenticazione applicativa è tramite token più controllo sorgente, mentre il certificato autentica il server.

Soltanto `GET /healthz` non richiede bearer. Il listener accetta sorgenti configurate, loopback o il proprio indirizzo per health check; sorgenti non ammesse vengono rifiutate/chiuse. Il fatto che un health check risponda `200` non dimostra che SMTP o Google siano configurati e raggiungibili.

Le risposte sono JSON, senza cache. I body dei POST sono form URL-encoded; contenuti testuali non sono interpolati in comandi di shell. Nessun endpoint accetta un URL remoto arbitrario da contattare. I contenuti persistenti, la cifratura e le tabelle sono descritti nel [modello dati](modello-dati.md); il controllo browser/sessione è nel [contratto API app](api-app.md).

## Mail-addon: contratti HTTP

| Metodo e percorso | Autenticazione | Risposta |
| --- | --- | --- |
| `GET /healthz` | Nessun bearer, sorgente ammessa. | `200 {"status":"ok"}`. |
| `GET /v1/status` | Bearer e IP app. | `200 {"smtp_configured":false,"transport":"smtp_tls","idempotency":true}`; flag `true` quando il provider è configurato. |
| `POST /v1/emails` | Bearer e IP app. | Esito sincrono SMTP, con idempotenza persistente. |

`POST /v1/emails` accetta esattamente `application/x-www-form-urlencoded` oppure `application/x-www-form-urlencoded; charset=UTF-8`. Nessuna query, allegato o batch destinatari è implementato.

| Campo | Validazione |
| --- | --- |
| `idempotency_key` | 1–128 caratteri ASCII tra lettere, cifre, `_`, `.`, `:`, `-`. |
| `to` | Un singolo indirizzo email valido, massimo 254 byte; niente liste, display name o CR/LF. |
| `subject` | UTF-8 non vuoto, fino a 250 byte, senza caratteri di controllo. |
| `body` | UTF-8 non vuoto, fino a 8192 byte; tab e ritorni a capo ammessi. |

Campi duplicati, sconosciuti o mancanti sono rifiutati. Il corpo HTTP codificato è limitato a 32768 byte e gli header a 8192 byte. Il messaggio generato è `text/plain`; il mittente deriva dalla configurazione del servizio e non dal form. Non c'è endpoint pubblico dell'app per spedire email arbitrarie: attualmente il chiamante è l'outbox degli alert.

| HTTP | Corpo/esito | Interpretazione |
| --- | --- | --- |
| `200` | `status:"sent"`, `idempotency_key`, `replayed` | Il server SMTP ha accettato il messaggio. Non certifica recapito in casella o lettura. |
| `503` | `error:"smtp_not_configured", status:"pending_configuration"` | Provider non configurato; nessuna chiave consumata e nessun tentativo SMTP. |
| `409` | `error:"idempotency_conflict"` | Chiave esistente con destinatario/oggetto/corpo differenti. |
| `409` | `error:"delivery_unknown", status:"unknown"`, chiave e `replayed` | Il messaggio potrebbe essere già stato accettato; non reinviare automaticamente con una nuova chiave. |
| `502` | `error:"smtp_delivery_failed", status:"failed"`, chiave e `replayed` | Tentativo concluso come fallito; terminale per quella chiave. |
| `503` | `error:"ledger_unavailable"` | Non è stato possibile riservare/verificare la chiave; ripetibile mantenendo contenuto e chiave. |
| `400` | `invalid_request` o `invalid_email` | Richiesta/form non valido. |
| `401`, `403` | `unauthorized`, `forbidden` | Token o sorgente non autorizzati. |
| `404`, `405`, `413`, `415` | Percorso, metodo, dimensione, formato non supportati. | Correggere la chiamata; non ritentare in un ciclo indiscriminato. |

Esempio di risposta già consegnata e richiesta nuovamente:

```json
{"status":"sent","idempotency_key":"ambiente:outbox:123","replayed":true}
```

### Idempotenza, timeout e outbox

Prima di SMTP il servizio salva in SQLite `sending`, con hash del payload, WAL e `synchronous=FULL`. Stessa chiave e stesso hash restituiscono l'esito persistito; hash diverso dà `409`. Al riavvio ogni `sending` residuo diventa `unknown`. Anche `failed` è terminale: la ripetizione della chiave non effettua un nuovo invio.

L'app usa `MAIL_API_NAMESPACE:outbox:ID`, dove `ID` è quello di `alert_email_outbox`. Non modificare il namespace o azzerare il ledger per risolvere un timeout: potrebbe duplicare una consegna già accettata. L'outbox persiste destinatario, testo e stato; il ledger conserva soltanto chiave, hash e tempi.

| Esito client mail | Stato outbox applicativo |
| --- | --- |
| Invio accettato | `sent` |
| SMTP non configurato | `pending_configuration` |
| Timeout/rete/risposta non riconosciuta | `retrying` con la stessa chiave |
| Consegna ambigua | `delivery_unknown` |
| Fallimento o conflitto di idempotenza | `delivery_failed` |

L'app verifica lo stato SMTP circa ogni 60 secondi e tratta al massimo una email per ciclo dello scheduler, circa ogni 30 secondi, con cursore circolare. Prima dell'invio ricontrolla gestore attivo e destinatario ancora ammesso. La chiamata avviene fuori dalla transazione MySQL; esito e audit sono salvati insieme successivamente.

Timeout client app→mail-addon: 1 secondo per connettersi, 3 secondi totali; risposta massima 8192 byte. Timeout SMTP del servizio: `MAIL_SMTP_TIMEOUT`, 1–60 secondi, default 20. Pertanto il client può perdere la risposta mentre SMTP continua; la chiave persistente consente di interrogare nuovamente lo stesso invio senza duplicarlo. Il server mail gestisce le richieste in sequenza; un invio lungo può ritardare la lettura dello stato.

Non esiste un endpoint di reinvio terminale o consultazione dell'intero ledger. Per `delivery_unknown` verificare il registro del provider e decidere esplicitamente un eventuale nuovo invio. Un backup coerente comprende MySQL, ledger e namespace: vedere [backup/ripristino](operazioni-backup-ripristino.md).

### Configurazione SMTP da completare

| Variabili | Funzione |
| --- | --- |
| `MAIL_BIND`, `MAIL_PORT`, `MAIL_ALLOWED_CLIENT` | Listener privato e sorgente autorizzata. |
| `MAIL_TLS_CERT_FILE`, `MAIL_TLS_KEY_FILE` | Certificato e chiave del listener. |
| `MAIL_API_TOKEN_FILE` | Token privato dell'app. |
| `MAIL_LEDGER_PATH` | File SQLite persistente. |
| `MAIL_SMTP_HOST`, `MAIL_SMTP_PORT`, `MAIL_SMTP_MODE` | Provider, porta 587/STARTTLS oppure 465/SMTPS nella policy Compose. |
| `MAIL_SMTP_FROM`, `MAIL_SMTP_USER`, `MAIL_SMTP_PASSWORD_FILE` | Mittente e autenticazione provider. Il secret è un file, non un campo HTTP. |
| `MAIL_PROXY_URL` | Proxy CONNECT obbligatorio nel percorso di uscita, `http://squid:3128`. |
| `MAIL_SMTP_TIMEOUT`, `MAIL_SMTP_CA_FILE` | Timeout e CA SMTP opzionale per un provider/fixture dedicato. |

La configurazione attuale lascia SMTP disabilitato e l'allowlist SMTP con un segnaposto. In seguito bisognerà aggiungere l'hostname esatto alla [allowlist SMTP](../../docker/squid/allowed-smtp-domains.txt), predisporre il secret e ricreare Squid e mail-addon secondo la [guida mail](../../addon/README.md). La configurazione parziale viene rifiutata all'avvio. L'abilitazione può elaborare anche email già accodate, quindi riguarda invii reali pendenti.

Il container non può contattare gli endpoint Google: la policy Squid della sua sorgente consente solo gli hostname SMTP autorizzati sulle porte ammesse. Nessun SMTP in chiaro sulla porta 25 è previsto nella policy corrente.

## Calendar-addon: contratti HTTP

Tutti i percorsi `/v1/users/{id}/...` ricevono l'identità dall'app. `{id}` è attualmente limitato a **1–2147483647** dal parser addon, anche se gli ID MySQL sono `BIGINT`. Il bearer è di servizio e autorizza il chiamante fidato a indicare l'utente; il servizio non interpreta il cookie dell'utente né i suoi ruoli globali. Non esporre questi percorsi al browser o a chiamanti non fidati.

| Metodo e percorso | Campi/query | Risposta principale |
| --- | --- | --- |
| `GET /healthz` | Nessuno; senza bearer. | `200`, disponibilità del processo HTTPS. |
| `GET /v1/status` | Nessuno. | `provider`, `oauth_configured`, `bidirectional`, `sync_interval`. |
| `GET /v1/users/{id}/status` | Nessuno. | Stato globale più connessione dell'utente, calendario e coda. |
| `POST /v1/users/{id}/oauth/start` | `session_binding` | URL consenso e durata state. |
| `POST /v1/users/{id}/oauth/callback` | `state`, `session_binding`, `code` oppure `error`. | `200 {"ok":true}` al completamento; errori OAuth altrimenti. |
| `GET /v1/users/{id}/calendars` | `page_token`, `limit` opzionali. | Una pagina Google, `items`, `next_page_token`. |
| `POST /v1/users/{id}/selection` | `calendar_id` | Selezione del calendario con controllo dei permessi Google. |
| `GET /v1/users/{id}/events` | `from`, `to`, `limit`, `after_id` opzionali. | Pagina cache con cursore, calendario e ultimo sync. |
| `GET /v1/users/{id}/changes` | Nessuno. | Ultime operazioni, `items` e `truncated`. |
| `POST /v1/users/{id}/events/create` | `idempotency_key`, `event`. | `202` nuova operazione o `200` replay. |
| `POST /v1/users/{id}/events/update` | `idempotency_key`, `event_id`, `etag`, `event`. | `202` nuova operazione o `200` replay. |
| `POST /v1/users/{id}/events/delete` | `idempotency_key`, `event_id`, `etag`. | `202` nuova operazione o `200` replay. |
| `POST /v1/users/{id}/sync` | Vuoto. | `202`, stato utente con `sync_requested:true`. |
| `POST /v1/users/{id}/disconnect` | Vuoto. | `200 {"disconnected":true,"remote_revoked":...}`. |

Il servizio richiede `Content-Type: application/x-www-form-urlencoded` per i POST privati (senza parametro charset); il bridge normalizza il formato. Header fino a 8192 byte, corpo privato fino a 65536 byte, form del bridge pubblico fino a 32768. Nessun parametro duplicato/sconosciuto; anche chiavi JSON duplicate negli eventi sono rifiutate.

### Stato e autorizzazione OAuth

Esempio senza credenziali Google e senza connessione dell'utente:

```json
{
  "provider": "google",
  "oauth_configured": false,
  "bidirectional": true,
  "sync_interval": 300,
  "connected": false,
  "calendar_id": "",
  "last_sync": 0,
  "last_error": "",
  "reauth_required": false,
  "sync_requested": false,
  "sync_running": false,
  "queued_changes": 0,
  "conflicts": 0,
  "unknown_changes": 0
}
```

`last_sync` è un istante Unix aggiornato solo dopo un pull completo riuscito. `connected:true` indica un record account presente: leggere anche `reauth_required`, perché il consenso può richiedere rinnovo. `queued_changes` non include `unknown_changes` né conflitti.

`session_binding` è SHA-256 esadecimale del token di sessione corrente, prodotto sul server app. All'avvio OAuth il servizio genera state casuale e PKCE S256, conserva hash state, legame utente/sessione e verificatore cifrato, con scadenza 600 secondi. Restituisce `authorization_url` e `expires_in:600`. Il client deve aprire quell'URL, non costruirne uno proprio con altri scope.

Gli scope implementati sono `https://www.googleapis.com/auth/calendar.events` e `https://www.googleapis.com/auth/calendar.calendarlist.readonly`; richiesta offline e consenso esplicito. La callback verifica utente, sessione e state, lo consuma prima dello scambio del codice e rifiuta replay. Il browser passa per `/api/calendar/oauth/callback`; solo l'app chiama il POST privato. La configurazione NPM protegge la callback dal salvataggio di codice/state nei log della location dedicata.

Ricollegare lo stesso account conserva calendario, cache e storico della coda. Un account diverso restituisce `409 google_account_mismatch`: serve prima una disconnessione esplicita. Il controllo usa l'identità del calendario primario ottenuta da Google. Ogni nuovo consenso/selezione cambia una generazione locale; risposte del worker relative a una vecchia generazione non possono aggiornare la nuova connessione.

### Calendari disponibili e selezione

`GET calendars`: `limit` 1–50, default 50; `page_token` restituito dalla pagina precedente. Una sola chiamata Google per pagina; `next_page_token:null` alla fine. Il risultato è limitato a 100 KiB: in caso di pagina eccessiva ridurre il limite. Non assumere che la prima pagina contenga tutti i calendari.

`selection` richiede un ID ottenuto dal provider e accesso `writer` o `owner`; sola lettura non basta per il sync bidirezionale. Operazioni pendenti `queued`/`unknown` impediscono di cambiare calendario (`409 pending_changes`). La selezione prepara il nuovo ciclo di sincronizzazione; leggere `/status` e `/events` per distinguere una scelta riuscita da una cache già aggiornata.

### Eventi accettati in scrittura

Il campo `event` contiene una stringa JSON. Proprietà ammesse:

| Proprietà | Vincolo |
| --- | --- |
| `summary` | Stringa UTF-8 obbligatoria non vuota, massimo 1000 byte. |
| `description` | Facoltativa, massimo 8192 byte. |
| `location` | Facoltativa, massimo 1024 byte. |
| `start`, `end` | Oggetti obbligatori con `date` oppure `dateTime`, mai entrambi; `timeZone` facoltativo. |
| `date` | `YYYY-MM-DD`, anno 1900–2199. Per eventi giornalieri `end.date` è esclusiva. |
| `dateTime` | RFC 3339 con secondi interi e `Z` o offset esplicito, per esempio `2026-10-12T09:00:00+02:00`. Niente frazioni di secondo in scrittura. |
| `timeZone` | Stringa non vuota fino a 100 byte, caratteri compatibili con nomi come `Europe/Rome`; il provider completa la validazione del fuso. |

Inizio/fine devono usare lo stesso tipo, fine successiva all'inizio, durata massima 366 giorni. Non sono ammessi `attendees`, allegati, notifiche, ricorrenze nuove, proprietà private arbitrarie o URL di webhook. Le scritture remote usano `sendUpdates=none`.

```json
{
  "summary": "Revisione requisiti",
  "description": "Verifica del perimetro della commessa",
  "start": {"dateTime":"2026-10-12T09:00:00+02:00","timeZone":"Europe/Rome"},
  "end": {"dateTime":"2026-10-12T10:00:00+02:00","timeZone":"Europe/Rome"}
}
```

`idempotency_key`: 8–128 caratteri ASCII dell'insieme `[A-Za-z0-9_.:-]`. `event_id`: 1–1024 caratteri alfanumerici, `_` o `-`. `etag`: stringa tra virgolette doppie, 2–200 byte, da copiare esattamente dalla lettura; l'ETag include le virgolette come parte del valore. Il bridge limita il JSON `event` a 24576 byte prima della codifica form.

Le operazioni update hanno gli stessi campi evento obbligatori della create: non accettano un oggetto contenente soltanto la proprietà cambiata. Il servizio normalizza il JSON ed esegue una PATCH Google con il sottoinsieme validato. Per rimuovere un campo opzionale remoto non fare affidamento sull'omissione: il contratto corrente non espone un'operazione dedicata di cancellazione proprietà.

### Lettura cache e ricorrenze

`GET events` accetta `limit` 1–100 (default 20), `after_id`, e `from`/`to` insieme. Estremi inclusivi, massimo 366 giorni; i confini del periodo sono giorni **UTC**, non il fuso della UI. Risposta: `items`, `calendar_id`, `last_sync`, `next_after_id` e `recurrence_mode:"masters_and_exceptions"`. Ogni pagina rispetta 100 KiB e può contenere meno elementi del limite richiesto; proseguire con `next_after_id` finché è `null`, conservando gli stessi filtri.

L'ordinamento è per ID evento, non per data. La paginazione non è uno snapshot immutabile tra richieste: una sincronizzazione può aggiornare la cache durante la consultazione. I campi pubblici comprendono `id`, `etag`, `status`, `summary`, `description`, `location`, `start`, `end`, `recurrence`, `recurringEventId`, `originalStartTime`, `updated`, `created`, `htmlLink`, `sequence`, `eventType`, se presenti. Proprietà private e credenziali non sono esposte.

Le serie ricorrenti rimangono master con eccezioni, **non vengono espanse**. Un master può apparire anche se l'inizio originale precede il filtro; tombstone `status:cancelled` possono non avere orari. Non interpretare quindi il numero degli elementi come numero di appuntamenti nel periodo. Modificare l'ID master può agire sull'intera serie; questa versione non implementa una scelta UI tra occorrenza e serie.

### Coda, conflitti e retry

Una nuova mutazione risponde prima dell'esecuzione remota:

```json
{
  "id": 123,
  "idempotency_key": "calendar-create-example-001",
  "event_id": "identificativo-generato",
  "state": "queued",
  "replayed": false
}
```

`GET changes` restituisce `items` con `id`, `idempotency_key`, `event_id`, `state`, `error`, `attempts`, `replayed:false`. Massimo 100 operazioni recenti in ordine ID decrescente e 90 KiB di dati; `truncated:true` segnala altri risultati, ma non è ancora presente un cursore per sfogliare l'intero storico. Il replay della stessa richiesta restituisce il singolo esito con `replayed:true`.

| Stato | Significato e trattamento |
| --- | --- |
| `queued` | Persistito, in attesa del worker o di un nuovo tentativo. |
| `unknown` | Scrittura remota iniziata/potenzialmente eseguita; prima di ritentare si verifica l'evento Google. |
| `applied` | Il servizio ha confermato l'applicazione o riconosciuto un risultato già applicato. |
| `conflict` | ETag divergente o conflitto non risolvibile automaticamente. Rileggere l'evento e decidere una nuova operazione consapevole. |
| `failed` | Errore definitivo/validazione remota/integrità; intervento del chiamante. |

La chiave è univoca per utente. Stesso contenuto normalizzato = replay `200`; contenuto differente = `409 idempotency_conflict`. Per le create l'ID Google deriva deterministicamente da utente e chiave, mentre il marcatore privato `puntoMutation` lega anche il payload; update/delete inviano `If-Match` con l'ETag. Un `412` Google porta a `conflict` senza sovrascrittura cieca.

Lo stato `unknown` è salvato prima della chiamata remota. Dopo crash/timeout il servizio consulta l'evento: marcatore coincidente prova una scrittura già applicata; per le cancellazioni verifica l'assenza/stato remoto; ETag diverso può richiedere conflitto. Attese esponenziali partono da 30 secondi e sono limitate a 3840 secondi; gli istanti sono soglie minime, non garanzie di esecuzione precisa. Nessun limite massimo di tentativi per gli errori temporanei è definito come politica utente.

Il worker passa ogni circa 5 secondi, sceglie un utente per turno con cursore circolare e tratta al massimo 20 modifiche per ciclo di sincronizzazione. Il polling remoto predefinito è 300 secondi. API e worker usano connessioni SQLite distinte; le chiamate Google non tengono aperte transazioni di scrittura lunghe. Il polling è implementato, i webhook push Google no.

Il download iniziale/incrementale segue tutte le pagine e cambia `sync_token` solo insieme alla transazione che aggiorna la cache. Un `410` Google avvia una nuova fotografia completa, conservando la precedente fino al successo. Limiti per sincronizzazione: 100 pagine, 20.000 eventi, 64 KiB per evento, 16 MiB di payload totali; oltre il limite la cache precedente resta valida e viene riportato errore. Una singola risposta HTTP provider è limitata a 4 MiB.

`POST sync` restituisce subito `202`; controllare `sync_running`, `last_sync`, `last_error` e coda. Non è una prova che tutte le operazioni siano state applicate né una promessa di exactly-once verso servizi esterni.

### Errori e disconnessione

| HTTP/codice | Azione del chiamante |
| --- | --- |
| `400 invalid_form`, `invalid_event`, `invalid_event_reference`, `invalid_idempotency_key` | Correggere il payload; nessuna scrittura remota valida è stata accodata. |
| `400 invalid_oauth_state`, `invalid_oauth_callback`, `oauth_cancelled` | Riavviare il consenso dalla sessione corretta quando appropriato. |
| `401 unauthorized`, `403 source_not_allowed` | Verificare token/routing privato. |
| `403` per calendario senza scrittura | Selezionare un calendario sul quale Google concede writer/owner. |
| `409 calendar_not_connected`, `calendar_not_selected` | Completare collegamento/selezione. |
| `409 pending_changes`, `google_account_mismatch` | Risolvere coda o cambiare account tramite disconnessione esplicita. |
| `409 idempotency_conflict` | Non riutilizzare una chiave per due intenti diversi. |
| `503 oauth_not_configured` | Configurare il provider prima del consenso. |
| `500`, `502`, `503` con altri codici | Dipendenza, archivio o provider indisponibile; conservare la chiave, rileggere stato e applicare un retry controllato. |

Il bridge pubblico app sanifica errori interni/non previsti in `503`; non aspettarsi la stessa superficie di errore del servizio privato. Una revoca/scadenza dei token imposta `reauth_required` e richiede nuovo consenso.

`disconnect` tenta la revoca del refresh token, poi elimina localmente account, eventi, coda e state OAuth. `remote_revoked:false` segnala che la revoca remota non è confermata, anche se `disconnected:true` conferma la pulizia locale. Una richiesta già inviata a Google può completarsi durante la disconnessione: la rimozione locale non annulla retroattivamente un evento creato sul provider. La disconnessione elimina anche lo storico di idempotenza dell'utente: non riutilizzare intenzionalmente vecchie chiavi per nuove operazioni dopo il ricollegamento.

### Configurazione Google da completare

| Variabili | Funzione |
| --- | --- |
| `CALENDAR_BIND`, `CALENDAR_PORT`, `CALENDAR_ALLOWED_CLIENT` | Listener privato e sorgente app. |
| `CALENDAR_TLS_CERT_FILE`, `CALENDAR_TLS_KEY_FILE` | Certificato API e chiave. |
| `CALENDAR_API_TOKEN_FILE` | Token di servizio condiviso con l'app. |
| `CALENDAR_TOKEN_KEY_FILE` | Chiave AES-256 di 64 caratteri esadecimali, separata dal database. |
| `CALENDAR_DB_PATH` | Archivio SQLite nel volume persistente. |
| `CALENDAR_PROXY_URL` | Proxy obbligatorio di uscita, `http://squid:3128`. |
| `CALENDAR_SYNC_INTERVAL` | Polling 30–86400 secondi in produzione, default 300. |
| `CALENDAR_HTTP_TIMEOUT` | Totale per chiamata Google 1–60 secondi, default 10; connessione 3 secondi. |
| `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET_FILE`, `GOOGLE_REDIRECT_URI` | Client OAuth Web e callback pubblica esatta. |
| `CALENDAR_GOOGLE_CA_FILE` | CA personalizzata opzionale, usata dai fixture di test. |

Per attivare: abilitare Calendar API nel progetto Google Cloud, configurare consenso/utenti/scopi e client Web, registrare per esempio `https://progetti.tuaazienda.it/api/calendar/oauth/callback`, predisporre ID, secret e redirect, quindi ricreare il servizio. Le impostazioni parziali vengono rifiutate all'avvio. Ogni utente dovrà concedere il proprio consenso. Il secret non va in URL, repository, payload frontend o log.

Le sole destinazioni di uscita previste sono `accounts.google.com`, `oauth2.googleapis.com`, `www.googleapis.com` su HTTPS 443 tramite Squid. Le chiamate del servizio verificano CA/hostname, richiedono TLS almeno 1.2, disabilitano redirect e forzano il proxy anche in presenza di `NO_PROXY`. Modificare la allowlist da sola non autorizza il codice a usare provider arbitrari. Outlook, CalDAV e altri provider non sono ancora implementati.

## Esempi di chiamate interne e prove isolate

L'esempio è per un processo già autorizzato sulla rete API, con una CA e un file privato di header predisposti dall'operatore. Il file indicato da `API_HEADER_FILE` contiene l'header bearer; non stamparlo né versionarlo. Dall'host le porte non sono pubblicate.

```sh
ADDON_CA='/percorso/privato/ca.crt'
API_HEADER_FILE='/percorso/privato/calendar-authorization.header'

curl --fail-with-body --cacert "$ADDON_CA" \
  --header "@$API_HEADER_FILE" \
  'https://calendar-api:8091/v1/users/123/status'

curl --fail-with-body --cacert "$ADDON_CA" \
  --header "@$API_HEADER_FILE" \
  --header 'Content-Type: application/x-www-form-urlencoded' \
  --data '' \
  'https://calendar-api:8091/v1/users/123/sync'
```

Usare normalmente il bridge pubblico autenticato dell'app; l'esempio interno serve a diagnosi e sviluppo. Non passare un ID ricevuto dal browser senza verificare l'identità applicativa.

Le suite [mail-addon](../../tests/test_mail_addon.py), [client mail](../../tests/test_mail_client.py), [calendar-addon](../../tests/test_calendar_addon.py), [client calendario](../../tests/test_calendar_client.py), [rete](../../tests/test_network_security.py) e [privacy proxy](../../tests/test_proxy_privacy.py) verificano i contratti con SMTP/OAuth/Calendar HTTPS simulati, certificati di test e proxy CONNECT locali. Non dimostrano una configurazione produttiva Google/SMTP già effettuata.

L'override `CALENDAR_GOOGLE_ORIGIN` viene accettato soltanto con `CALENDAR_TEST_MODE=1`, listener loopback e proxy loopback. Tale modalità non compare nella configurazione Compose produttiva; la verifica TLS resta attiva anche nei fixture. Non indebolire i controlli TLS o abilitare test mode per collegare un provider reale.

L'integrazione calendario esistente è a livello di API e servizio. UI calendario, selezione evento→commessa, contabilizzazione automatica delle ore, mapping tra task ed eventi e politiche avanzate di risoluzione conflitti sono ancora lavoro di prodotto futuro.
