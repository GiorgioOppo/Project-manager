# Add-on email di punto

**Italiano** | [English](README.md)

Il servizio Compose si chiama `mail` e la sua immagine locale è `punto/mail:${PUNTO_IMAGE_TAG:-local}`. Per aggiornare un’installazione con i nomi precedenti seguire la [guida nomenclatura Docker](../docs/tehnical/nomi-servizi-e-immagini.md); directory, volumi e identità TLS conservano i nomi già previsti.

Microservizio scritto in C che riceve richieste HTTPS dall’app e invia messaggi di testo tramite SMTP con TLS. Usa OpenSSL, libcurl e SQLite; `make -C addon` compila l’eseguibile indipendente `addon/build/punto-mailer`. Il [Dockerfile](Dockerfile) produce il servizio Compose `mail`, eseguito con utente non privilegiato, filesystem in sola lettura e un volume dedicato al registro degli invii.

**SMTP non è ancora configurato.** Il servizio è operativo come API, ma non invia email reali. Le notifiche interne continuano a funzionare; le email degli alert restano in `alert_email_outbox` con stato `pending_configuration`.

## API privata

L’app usa `https://mail-api:8090`. Il nome `mail-api` è un alias presente soltanto sulla rete interna `mail_api`; il certificato include questo nome. Il client verifica CA, nome del server e TLS, senza seguire redirect e senza attraversare Squid per questa chiamata interna.

Le API `/v1/*` richiedono `Authorization: Bearer <token>` e una sorgente autorizzata da `MAIL_ALLOWED_CLIENT`, configurata in Compose come indirizzo dell’app `172.30.60.10`. Il token condiviso viene letto da `MAIL_API_TOKEN_FILE`; non va inserito in URL, log, sorgenti o comandi di collaudo visibili. `/healthz` non richiede autenticazione, verifica che il processo risponda e non certifica la raggiungibilità del fornitore SMTP.

| Metodo e percorso | Risultato |
| --- | --- |
| `GET /healthz` | `200 {"status":"ok"}` |
| `GET /v1/status` | `200`, con `smtp_configured`, `transport: "smtp_tls"` e `idempotency: true` |
| `POST /v1/emails` | Invio o recupero del risultato già registrato per la stessa chiave |

`smtp_configured` indica che la configurazione SMTP richiesta è presente; non esegue un collegamento di prova al fornitore.

Il POST accetta `Content-Type: application/x-www-form-urlencoded`, anche con `charset=UTF-8`, e i campi seguenti (i primi quattro sono obbligatori):

| Campo | Contenuto |
| --- | --- |
| `idempotency_key` | Da 1 a 128 caratteri ASCII tra lettere, numeri, `_`, `.`, `:`, `-` |
| `to` | Un singolo indirizzo email, fino a 254 byte |
| `subject` | Testo UTF-8 non vuoto, fino a 250 byte, senza caratteri di controllo |
| `body` | Testo UTF-8 non vuoto, fino a 8192 byte; sono ammessi ritorni a capo e tabulazioni |
| `message_id` | Facoltativo, un ID canonico `<id@dominio>` fino a 255 byte |
| `in_reply_to` | Facoltativo, un ID canonico della richiesta fino a 255 byte |
| `references` | Facoltativo, ID canonici separati da uno spazio, fino a 1024 byte e 32 ID |

Campi sconosciuti, duplicati, indirizzi multipli e contenuti non validi vengono rifiutati. Il corpo HTTP codificato non può superare 32768 byte. Il servizio genera messaggi `text/plain`; allegati, HTML e invii multipli nella stessa richiesta non sono implementati.

Le intestazioni della conversazione non accettano ritorni a capo o sintassi ambigue e partecipano all’hash di idempotenza. Gli hash delle richieste preesistenti senza queste intestazioni restano compatibili; riusare una chiave cambiando il thread restituisce un conflitto.

| HTTP | Risposta significativa | Comportamento dell’app |
| --- | --- | --- |
| `200` | `status: "sent"`, chiave e `replayed` | Memorizza `sent` e termina i tentativi |
| `503` | `error: "smtp_not_configured"`, `status: "pending_configuration"` | Conserva la richiesta in attesa; nessuna chiave viene consumata |
| `409` | `error: "delivery_unknown"`, `status: "unknown"` | Memorizza `delivery_unknown`; richiede verifica manuale |
| `502` | `error: "smtp_delivery_failed"`, `status: "failed"` | Memorizza `delivery_failed`; richiede verifica manuale |
| `409` | `error: "idempotency_conflict"` | Blocca la richiesta con `delivery_failed`; richiede verifica manuale |
| `503` | `error: "ledger_unavailable"` | Riprova successivamente con la stessa chiave |
| `400`, `401`, `403`, `405`, `413`, `415` | Richiesta, autenticazione, sorgente, metodo, dimensione o formato non validi | Correggere configurazione o chiamante |

Un errore di rete o una risposta non riconosciuta lascia l’elemento ripetibile (`retrying`), mantenendo chiave e contenuto. L’app interroga lo stato SMTP circa ogni minuto e tratta al massimo una email per ciclo del pianificatore, circa ogni 30 secondi. Prima di inviare controlla nuovamente che il gestore sia attivo e che l’indirizzo sia ancora tra i destinatari della regola. Le chiamate di rete avvengono fuori dalla transazione MySQL; l’aggiornamento dell’outbox e il relativo audit avvengono insieme in una transazione successiva.

`sent` significa che **il server SMTP ha accettato il messaggio**. Non conferma la consegna nella casella, l’assenza di rimbalzi o la lettura da parte del destinatario.

## Idempotenza e ripristino

Il registro persistente è `/var/lib/punto-mailer/delivery.sqlite3`, nel volume `mail_addon_data`. La tabella `deliveries` conserva chiave, hash SHA-256 del contenuto, stato e orari; non conserva destinatario, oggetto o testo in chiaro. Questi contenuti rimangono nell’outbox applicativa. Il registro usa WAL e sincronizzazione `FULL`.

Prima di contattare SMTP, il servizio registra e conferma su disco lo stato `sending`. Una richiesta ripetuta con la stessa chiave e lo stesso contenuto restituisce il risultato memorizzato, senza un altro invio; con contenuto differente restituisce `idempotency_conflict`. Al riavvio, gli invii rimasti `sending` diventano `unknown`, perché il fornitore potrebbe aver già accettato il messaggio. Anche `failed` e `unknown` sono terminali per quella chiave.

L’app genera chiavi nella forma `MAIL_API_NAMESPACE:outbox:ID`. **Mantieni stabile `MAIL_API_NAMESPACE`** insieme al database applicativo e al registro dell’add-on. Non cambiare namespace per forzare un tentativo e non cancellare il registro per risolvere un errore: entrambe le operazioni possono reinviare email già accettate.

Il backup MySQL da solo non include questo volume. Per un backup coerente sospendi tutte le scritture e gli invii, ferma app e add-on, salva MySQL e una copia consistente del registro SQLite, poi ripristina insieme outbox, registro e namespace. Per SQLite usa un backup consistente oppure, a servizio fermo, conserva l’intera directory del volume con gli eventuali file WAL. Proteggi anche token, certificati e credenziali di ripristino. Una nuova installazione con database e registro nuovi deve usare un namespace distinto; non riutilizzare chiavi storiche.

Gli esiti `unknown` e `failed` richiedono un controllo del registro del fornitore e una decisione esplicita prima di un eventuale nuovo invio. Non è presente un comando automatico di reinvio degli esiti terminali.

## Configurare SMTP in seguito

In `.env.docker` impostare i dati del fornitore:

| Variabile | Uso |
| --- | --- |
| `MAIL_SMTP_HOST` | Hostname esatto del server SMTP |
| `MAIL_SMTP_PORT` | `587` per STARTTLS oppure `465` per TLS immediato |
| `MAIL_SMTP_MODE` | `starttls` oppure `smtps` |
| `MAIL_SMTP_FROM` | Indirizzo mittente autorizzato dal fornitore |
| `MAIL_SMTP_USER` | Utente SMTP, se il fornitore richiede autenticazione |

Inserire la password soltanto nel file locale `data/addon-secrets/smtp-password`, montato come secret `/run/secrets/mail_smtp_password`. Non inserirla nel file `.env.docker` o nel repository. `configure_network.py prepare` crea i secret mancanti senza stamparli e conserva quelli esistenti; la directory sull’host ha permessi `0700`, mentre i file montati devono essere leggibili dagli utenti non privilegiati dei container e non scrivibili da gruppo o altri.

Sostituire il segnaposto in [allowed-smtp-domains.txt](../docker/squid/allowed-smtp-domains.txt) con il solo hostname SMTP autorizzato, senza schema, porta, percorso, wildcard o punto iniziale. La lista SMTP è separata dagli endpoint Google Calendar dell’app. Squid ammette per l’add-on soltanto tunnel `CONNECT` verso gli host SMTP elencati sulle porte `465` e `587`; indirizzi IP e destinazioni private o riservate restano bloccati. Non è ammesso SMTP in chiaro sulla porta 25. I certificati SMTP sono verificati rispetto alle CA di sistema e al nome del fornitore; il trasporto richiede TLS.

Dopo aver predisposto configurazione, secret e allowlist, dalla radice del progetto ricreare i due servizi affinché rileggano ambiente e file montati:

```sh
docker compose --env-file .env.docker up -d --no-deps --force-recreate --wait proxy-egress mail
```

Non serve riavviare l’app per rilevare lo stato SMTP aggiornato. Quando SMTP diventa configurato, l’app può elaborare anche gli alert già accodati: verificare destinatari e contenuto dell’outbox prima di abilitare un fornitore reale.

L’add-on ha le sole interfacce `mail_api` e `app_egress`, entrambe interne. L’API ascolta solo sull’indirizzo `mail_api`; non pubblica porte sull’host e non è collegata a `public`, `private_app` o `backend_app`. L’uscita SMTP passa attraverso `MAIL_PROXY_URL=http://squid:3128`; soltanto Squid dispone della rete `proxy_wan`. L’add-on non riceve credenziali MySQL o MongoDB.

Per esecuzioni indipendenti, le altre impostazioni del programma sono `MAIL_BIND`, `MAIL_PORT`, `MAIL_ALLOWED_CLIENT`, `MAIL_TLS_CERT_FILE`, `MAIL_TLS_KEY_FILE`, `MAIL_LEDGER_PATH`, `MAIL_API_TOKEN_FILE`, `MAIL_SMTP_PASSWORD_FILE`, `MAIL_PROXY_URL`, `MAIL_SMTP_TIMEOUT` (1–60 secondi, predefinito 20) e l’eventuale `MAIL_SMTP_CA_FILE`. Quest’ultimo permette una CA SMTP dedicata, ma richiede un mount e una configurazione Compose aggiuntivi; non è impostato nello stack corrente. Una configurazione SMTP parziale, file TLS mancanti o chiave e certificato API non corrispondenti impediscono l’avvio.
