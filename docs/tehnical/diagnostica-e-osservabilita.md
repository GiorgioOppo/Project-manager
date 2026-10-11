# Diagnostica e osservabilità

Manuale riferito al codice del **10 ottobre 2026**. Fornisce controlli di lettura per individuare il componente coinvolto in un problema e stabilire il passo successivo. Non installa un sistema di monitoraggio e non introduce procedure automatiche di riparazione. Per la topologia consultare [architettura](architettura.md); per un intervento operativo usare [backup e ripristino](operazioni-backup-ripristino.md).

Fonti: [Compose](../../compose.yaml), [server HTTP](../../src/server.c), [health SQL](../../src/storage.c), [monitor del gruppo](../../docker/db-proxy/health.py), [health MongoDB](../../docker/mongodb/health.js), [health Squid](../../docker/squid/proxy-health), [client email](../../src/mail_client.c), [client Calendar](../../src/calendar_client.c).

## Strumenti presenti e limiti

Lo stack dispone di health check Docker, stato Group Replication, log dei processi, API di stato degli addon, audit applicativo e code persistenti. Non sono implementati endpoint Prometheus, dashboard Grafana, tracing distribuito, un archivio centralizzato dei log, soglie infrastrutturali con avvisi automatici o una misura automatica degli SLO.

Gli alert configurabili nella UI controllano ore mancanti e tentativi su commesse inesistenti. Non controllano spazio disco, scadenza certificati, salute del cluster o ritardi Calendar. Sono controlli di prodotto eseguiti dal processo app; se l'app è ferma non possono avvisare del proprio arresto.

| Segnale presente | Lettura corretta |
| --- | --- |
| Health check Docker | Ultimo esito di una prova limitata sul singolo servizio |
| `manage_cluster.py status` | Stato dei membri SQL e impostazioni rilevanti del gruppo |
| HTTP `/healthz` | Possibilità dell'app di eseguire `SELECT 1` sul backend configurato |
| Audit applicativo | Modifiche business confermate con attore e contenuto prima/dopo |
| Notifiche e outbox | Esiti del processo di alert e richieste email |
| API stato Calendar | Collegamento e avanzamento per l'utente autenticato |
| Log Squid | Sorgente, metodo, dominio e risultato del tunnel |

Un controllo riuscito è una misura puntuale, non una garanzia di disponibilità continuativa. L'assenza di un errore nei log non dimostra che una scrittura remota non sia avvenuta.

## Triage iniziale in cinque passaggi

1. Annotare ora, origine pubblica, funzione, profilo utente e codice HTTP; descrivere se il problema riguarda un utente o tutti. Non copiare password, cookie, token, query OAuth o contenuti documentali nella segnalazione.
2. Controllare se l'ingresso risponde e se il problema riguarda la pagina, il login oppure una sola API.
3. Leggere lo stato dei container e del gruppo SQL, senza riavviare i servizi per cancellare il sintomo.
4. Raccogliere una porzione limitata dei log dei soli componenti coinvolti e confrontare gli orari.
5. Prima di una mutazione ambigua, rileggere la risorsa o la coda. Le API business non hanno una chiave di idempotenza universale; gli addon hanno regole proprie.

Dalla radice del progetto, con dominio e porte predefiniti:

```sh
docker compose --env-file .env.docker ps
python3 scripts/manage_cluster.py --env-file .env.docker status
curl --silent --show-error --max-time 5 --output /dev/null --write-out '%{http_code}\n' http://localhost/healthz
docker compose --env-file .env.docker logs --since=15m --tail=100 app db-proxy
```

Il comando `status` è di lettura. `init`, `recover`, `configure_network.py apply`, `up --force-recreate` e l'esecuzione manuale di un alert sono invece interventi, non strumenti diagnostici equivalenti. `prepare` può creare materiali mancanti e non deve essere usato come se fosse una verifica totalmente priva di scritture.

## Interpretazione degli health check

| Servizio | Prova configurata | Cosa resta fuori |
| --- | --- | --- |
| App | HTTP sul bind privato e query `SELECT 1`; `200` con `database:mysql` o `sqlite`, altrimenti `503` | PDF, autorizzazioni business, completezza dati, SMTP e Google |
| `db` | Query SQL locale con account applicativo | Quorum e appartenenza Group Replication |
| `db2`, `db3` | Query SQL locale amministrativa | Quorum e scrivibilità applicativa |
| `db-proxy` | Almeno due endpoint monitor pronti; ognuno controlla `ONLINE`, `PRIMARY`, assenza di sola lettura e almeno due membri online | Validità di tutte le transazioni future o disponibilità di MongoDB |
| `documents` | Ping autenticato con account documentale e TLS | Correttezza di ogni PDF e corrispondenza con il manifesto SQL |
| Squid | Richiesta HTTP locale volutamente vietata che deve ricevere `403` | Connessione effettiva agli endpoint Google o SMTP |
| Addon email / Calendar | HTTPS con CA e nome/IP del servizio verificati, `/healthz` | Validità delle credenziali provider e risultato di un'operazione remota |
| NPM | `/usr/bin/check-health` dell'immagine | Funzionamento completo di tutti gli host, del login e dei flussi applicativi |

Un container può essere `running` e `unhealthy`. Dopo il primo avvio sono previsti periodi di attesa; valutare `start_period`, intervallo e numero di tentativi in Compose prima di interpretare una singola transizione.

Per separare ingresso NPM e processo app è possibile richiamare lo stesso health check dall'interno dell'app:

```sh
docker compose --env-file .env.docker exec -T app curl --noproxy '*' --fail --silent --show-error --max-time 3 --header 'Host: localhost:8080' http://172.30.20.10:8080/healthz
```

Se questo funziona ma l'URL pubblico fallisce, concentrare la diagnosi su NPM, hostname, DNS, certificato pubblico e origini ammesse. Il comando non richiede cookie o token. Non esporre la porta 8080 sull'host per aggirare il proxy.

## Triage per sintomo

### L'app non si apre o NPM restituisce 502

Verificare lo stato di NPM e app, il health check interno sopra e i log recenti dei due servizi:

```sh
docker compose --env-file .env.docker logs --since=15m --tail=100 nginx-proxy-manager app
```

La destinazione prevista del proxy è `http://app:8080` su `private_app`. L'app deve ascoltare su `172.30.20.10`; non risponde sulle altre sue interfacce. Un listener spostato su `0.0.0.0` o una pubblicazione diretta non è una correzione equivalente.

Se NPM non parte dopo una personalizzazione, controllare anche il filtro di privacy della callback. La build e l'avvio rifiutano configurazioni incompatibili con la protezione OAuth. Non rimuovere la patch o l'ACL come tentativo generico di ripristino. La guida [certificati e segreti](gestione-certificati-e-segreti.md) distingue i problemi del certificato pubblico da quelli della CA privata.

### `/healthz` app restituisce 503

Il problema riguarda la query del backend SQL utilizzato dall'app. Controllare nell'ordine gruppo, monitor HAProxy, autenticazione e TLS SQL:

```sh
python3 scripts/manage_cluster.py --env-file .env.docker status
docker compose --env-file .env.docker logs --since=15m --tail=100 db-proxy db db2 db3 app
```

Tre processi MySQL raggiungibili possono avere Group Replication fermo. Con un solo membro disponibile il blocco delle scritture è atteso: non impostare `super_read_only=OFF` o forzare un nuovo bootstrap. Se tutti i membri si sono arrestati, la procedura pertinente è `recover`, dopo avere verificato il contesto e seguendo [operazioni](operazioni-backup-ripristino.md).

Un errore del monitor impedisce a HAProxy di considerare i membri utilizzabili. L'entrypoint del proxy arresta il container se il processo monitor termina, permettendo alla normale politica di restart di intervenire. Non sostituire i controlli del monitor con un semplice ping TCP.

### Solo upload o download PDF fallisce

Un `/healthz` app positivo non esclude questo problema. SQL conserva il manifesto; MongoDB conserva i byte. Controllare `documents`, TLS e log applicativi. Un upload non autorizzato produce `403`; un contratto inesistente `404`; formato o firma non validi `400`; archivio indisponibile può produrre `503`.

Un download controlla dimensione e SHA-256 prima di restituire il PDF. Una mancata corrispondenza non si risolve aggiornando l'hash per adattarlo a un blob inatteso: verificare copia originale, manifesto e backup coerente. Non esiste una riconciliazione automatica degli oggetti non referenziati.

Non stampare i documenti GridFS o il contenuto dei contratti durante il triage. Se occorre verificare un file, usare un documento di prova o una procedura autorizzata e conservare solo ID, dimensione e impronta nel verbale.

### Login rifiutato o accesso perso

`401` indica credenziali non valide, account inattivo oppure sessione assente/scaduta secondo l'endpoint. `429` sul login deriva dal contatore globale del processo: dopo otto tentativi falliti nella finestra di 60 secondi, anche account diversi possono essere temporaneamente bloccati. Attendere la finestra; non usare ripetizioni rapide.

Il riavvio dell'app perde le sessioni in memoria. Cambiare `PUNTO_PASSWORD` nel file env non cambia un account esistente: quella variabile serve al primo utente su database vuoto. `403` può invece indicare Host/Origin errato o permesso insufficiente. Distinguere l'errore di autenticazione dall'autorizzazione a una commessa o a una funzione del gestore.

Le modifiche ai profili e allo stato degli utenti sono verificate lato server. L'API sessione può essere letta con un cookie jar privato per controllare il profilo attuale, senza diffondere il cookie. Vedere [API app](api-app.md).

### Ore rifiutate o report apparentemente incompleto

Verificare proprietario della registrazione, data, minuti e validità dell'abilitazione sia oggi sia nel giorno lavorato. Un gestore legge il registro giornaliero personale con la stessa API dell'operatore; i totali aziendali sono nei report. Non assumere che `jobs.total_minutes` sia un totale di tutti gli utenti.

Controllare il range del report, i filtri utente/commessa, la distinzione fra tariffa zero e tariffa assente e il limite di 1000 righe. La vista valore contratti seleziona contratti associati alle ore filtrate e conta il loro valore integrale una sola volta. Non è una fattura né un valore ripartito per data. Le regole sono nelle [API](api-app.md) e nel [modello dati](modello-dati.md).

### Alert non arrivato all'ora prevista

Verificare che l'app fosse avviata, la regola sia abilitata, il proprietario sia ancora un gestore attivo e i destinatari siano validi. Il ciclo controlla circa ogni 30 secondi, nello stesso processo delle richieste HTTP: non garantisce un'esecuzione al secondo esatto.

Per le ore mancanti contano lunedì–venerdì, obiettivo 480 minuti e tolleranza di 48 ore; i giorni vengono tracciati per regola e soggetto. Ferie e festività non vengono sottratte automaticamente. Distinguere i soggetti controllati dai destinatari. Per gli operatori, la lettura delle notifiche è ristretta a quelle che li riguardano.

Leggere ultimo run, prossimo run e notifiche attraverso le API autorizzate. Il comando `POST .../run` esegue realmente la regola e può generare notifiche o accodare email: non è una prova di sola lettura. Non usarlo come primo controllo su dati operativi.

### Email in attesa nonostante addon sano

Nel deployment SMTP è ancora da configurare. `pending_configuration` è quindi atteso. Il flag `smtp_configured` nell'API app deriva dall'ultima interrogazione al servizio email: può risultare falso anche se la chiamata privata fallisce o restituisce una risposta non valida. Non basta quel flag per distinguere assenza di configurazione e indisponibilità dell'addon.

Controllare health e log mail-addon, CA, alias `mail-api`, token e policy Squid. Il vero endpoint privato `GET /v1/status` richiede il token e una sorgente autorizzata; non esiste un pannello SMTP pubblico aggiuntivo. Consultarlo solo con strumenti che mantengano il bearer fuori da URL, shell history e log.

L'app ha un timeout di 3 secondi sulla chiamata privata email; l'addon può attendere fino a 20 secondi il provider con il valore Compose. Una richiesta può quindi avere un esito locale ambiguo mentre il servizio sta ancora lavorando. La ripetizione deve conservare la stessa chiave e contenuto: il ledger gestisce l'idempotenza. `delivery_unknown` e `delivery_failed` richiedono verifica del fornitore, non cancellazione della coda. `sent` attesta accettazione SMTP, non recapito nella casella.

### Calendar non sincronizza

Se `oauth_configured=false`, la configurazione Google è assente o rinviata; non eseguire collegamenti di prova a un account reale per diagnosticare il container. Se `connected=false`, non esiste un consenso attivo per l'utente. L'endpoint pubblico usa sempre l'identità della sessione, anche per un gestore.

Con un cookie jar già autenticato, conservato fuori dal repository, si possono leggere:

```sh
APP_ORIGIN='https://progetti.example.com'
COOKIE_JAR='/percorso/privato/session.cookies'
curl --fail-with-body --cookie "$COOKIE_JAR" "$APP_ORIGIN/api/calendar/status"
curl --fail-with-body --cookie "$COOKIE_JAR" "$APP_ORIGIN/api/calendar/changes"
```

Queste risposte possono contenere identificativi personali: non pubblicarle integralmente. I campi utili sono `last_sync`, `last_error`, `reauth_required`, `sync_requested`, `sync_running`, stato e numero di tentativi delle operazioni. `/changes` ha limiti di righe e byte e può indicare `truncated`; non è un export completo della coda.

| Condizione | Interpretazione / passo successivo |
| --- | --- |
| `reauth_required` | Occorre un nuovo consenso individuale; il solo restart non rinnova credenziali revocate |
| `queued` | Richiesta conservata, da elaborare dal worker |
| `unknown` | Esito remoto da riconciliare; non creare una nuova chiave per forzare un duplicato |
| `conflict` | ETag remoto diverso; recuperare la versione aggiornata e decidere come procedere |
| `failed` | Esito terminale da analizzare prima di una nuova operazione |
| `applied` | Operazione confermata dal servizio; verificare anche l'aggiornamento della cache |
| `sync_running=true` | Worker impegnato; osservare se `last_sync` avanza, considerando errori e timeout |
| `cache_integrity_failed` nell’API privata | Dato cifrato non decifrabile/verificabile; il bridge pubblico può riportare un errore generico. Preservare volume e chiave e indagare il problema |

Il refresh completo o incrementale è atomico rispetto alla cache locale: un download fallito conserva la precedente. Eventi ricorrenti sono serie ed eccezioni, non occorrenze già espanse. Una serie fuori dall'intervallo apparente non prova quindi un filtro difettoso. La descrizione completa è nelle [API addon](api-addon.md).

### Squid restituisce 403 o la nuova lista sembra ignorata

Un `403` può provenire da sorgente, metodo, host, porta o risposta DNS non consentiti. Leggere i log del proxy e il file non segreto montato:

```sh
docker compose --env-file .env.docker logs --since=15m --tail=100 squid
docker compose --env-file .env.docker exec -T squid cat /etc/squid/allowed-domains.txt
docker compose --env-file .env.docker exec -T squid squid -k parse -f /etc/squid/squid.conf
```

La verifica sintattica non ricarica la policy. Un editor può aver sostituito il file sul filesystem lasciando il bind mount legato al contenuto precedente: una ricreazione mirata, dopo build e verifica, è diversa da una diagnosi di lettura. La policy non può distinguere percorsi HTTPS dentro il tunnel; non aggiungere wildcard per aggirare un rifiuto non compreso.

## Letture SQL limitate alla configurazione

Quando serve conoscere la versione dello schema, un amministratore può leggere solo il metadato, evitando dump di tabelle utenti o contratti:

```sh
docker compose --env-file .env.docker exec -T db sh -c 'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql --user=root --database="$MYSQL_DATABASE" --batch --skip-column-names --execute="SELECT version FROM schema_metadata WHERE id=1;"'
```

Il comando usa la credenziale già presente nell'ambiente del container senza inserirne il valore nella riga di comando. Non abilita il debug della shell e non stampa l'ambiente. La versione attesa dal codice corrente è 7. Per una verifica del gruppo usare prima il controller, che evita di stampare query e valori sensibili in caso di errore.

## Log, privacy e raccolta di evidenze

I log sono diagnostici, mentre l'audit è storico business. Non usare l'audit come tracing HTTP completo e non cercare nei log un elenco esaustivo di ogni cambiamento Calendar o messaggio accettato da SMTP.

Squid e addon hanno rotazione `json-file` di tre file da 10 MB ciascuno. Questa impostazione non è applicata uniformemente a tutti i servizi Compose. NPM conserva anche file sotto la sua directory `data`; le query OAuth non devono essere recuperate aggirando la protezione del callback. Il periodo di conservazione centralizzato e il trasferimento sicuro dei log non sono configurati nel repository.

Una segnalazione utile contiene: versione del codice o immagine, ora e fuso, servizio, endpoint senza query sensibili, status, stato dei container interessati, stato del gruppo se pertinente, messaggio d'errore sanitizzato e ultimo cambiamento noto. Non comprende cookie, token API/OAuth, password, chiavi private, file env, configurazione Compose risolta o PDF.

## Miglioramenti proposti, ancora da implementare

Un futuro sistema di osservabilità potrebbe raccogliere latenza/errori HTTP, spazio disco, stato del quorum, età dell'ultimo backup verificato, scadenze TLS, età delle code e ritardo di sincronizzazione. Per ciascun segnale serviranno proprietario, soglia, deduplicazione, conservazione e destinatario dell'avviso.

Questa è una proposta operativa: non esistono oggi exporter o automazioni che implementino tali misure. Prima di aggiungerli occorre definire quali metadati possono uscire dal servizio e come evitare che identificativi, query o contenuti business finiscano nelle etichette delle metriche.
