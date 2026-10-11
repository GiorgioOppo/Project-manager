# Rilascio e compatibilità

Guida al rilascio locale dello stack presente l’**11 ottobre 2026**. Distingue compilazione, aggiornamento dei processi, migrazione dei dati e ripristino. È una procedura operativa da eseguire quando si distribuisce una modifica; la scrittura del manuale non ha ricompilato né riavviato servizi.

Fonti: [Makefile](../../Makefile), [Dockerfile app](../../Dockerfile), [Compose](../../compose.yaml), [migrazioni](../../src/storage.c), [schema MySQL](../../src/mysql_schema.c), [controller cluster](../../scripts/manage_cluster.py), [controller rete](../../scripts/configure_network.py), [addon email](../../addon/README.md) e [addon Calendar](../../calendar-addon/README.md).

## Unità che possono cambiare

Un rilascio deve dichiarare quali elementi modifica. L'immagine applicativa comprende sia il binario C sia i file `public/`; un aggiornamento frontend richiede quindi una nuova immagine nel deployment Docker. Configurazioni Squid e certificati sono anche bind mount: ricompilare l'immagine non garantisce da solo che ogni file effettivamente montato abbia il contenuto atteso.

| Elemento | Versione/configurazione corrente | Relazione di compatibilità |
| --- | --- | --- |
| App C | Compilazione C11 con warning come errori | Schema applicativo massimo 9; API e frontend distribuiti insieme |
| Backend MySQL | `mysql:8.4` | Tre membri con configurazione Group Replication coerente |
| Schema applicativo SQL | 9 | Stessa versione logica per MySQL e SQLite, con DDL e migrazioni specifici |
| Proxy SQL | `haproxy:3.2-alpine` e monitor Python | Controlla modalità multi-primary, quorum e TLS dei tre nodi |
| MongoDB | `mongo:8.0`, GridFS `contract_pdfs` | Dati PDF collegati al manifesto SQL, senza transazione distribuita |
| NPM | Base `jc21/nginx-proxy-manager:2.16.0` con patch locali | Entry point amministrativo e generazione configurazioni sono verificati dalla build |
| Squid | Pacchetto Debian Trixie nell'immagine del progetto | ACL, IP e allowlist devono coincidere con le reti Compose |
| Addon email | Binario C separato; ledger SQLite `deliveries` | Contratto HTTPS `/v1`, stati outbox introdotti dallo schema 7, conservati nello schema 9, e namespace stabile |
| Addon Calendar | Binario C separato; SQLite `user_version=1` | Contratto HTTPS `/v1`, chiave AES e identificativi utenti persistenti |
| Addon inventory | Binario C separato; archivio SQLite versione 3 | Contratto HTTPS `/v1`, schede e revisioni persistenti |
| Reti | Otto bridge, IP e interfacce espliciti | Docker Engine ≥28, Compose ≥2.36.0 e SAN coerenti |
| TLS privato | CA locale e leaf per servizio | Nomi/IP/CA verificati; file attesi dai mount |

Le immagini locali seguono `punto/<servizio>:${PUNTO_IMAGE_TAG:-local}` e `pull_policy: build`; non vengono prelevate dal namespace omonimo di un registry. La [guida nomenclatura](nomi-servizi-e-immagini.md) descrive il cambio di identità Compose, separato dalla migrazione SQL.

I tag MySQL, MongoDB, HAProxy e Debian non sono tutti fissati a un digest immutabile. Una ricompilazione in un momento successivo può usare pacchetti o immagini diversi. Il progetto non contiene un manifesto automatico completo delle versioni effettivamente scaricate: registrarne gli identificativi prima e dopo un rilascio quando serve riproducibilità.

Non è definita una matrice generale che prometta compatibilità con qualunque minor release futura. In particolare, cambiare NPM richiede il riesame delle patch; cambiare MySQL richiede verifiche di driver, schema, replica e recupero; cambiare libcurl/OpenSSL interessa entrambi gli addon.

## Scheda minima di rilascio

Preparare un record privo di segreti con:

- riferimento Git e presenza di modifiche non committate;
- elenco di servizi, file di configurazione e contratti API modificati;
- versione schema iniziale e attesa;
- ID delle immagini di partenza e di arrivo, quando disponibili;
- test realmente eseguiti, test saltati e relativi motivi;
- punto di backup coerente e materiali necessari a recuperarlo;
- finestra di indisponibilità prevista e impatto sulle sessioni;
- criterio di successo, criterio di arresto e percorso di ritorno possibile.

Comandi di lettura utili per preparare la scheda:

```sh
git status --short
git rev-parse HEAD
git diff --check
docker compose --env-file .env.docker config --quiet
docker compose --env-file .env.docker images
docker compose --env-file .env.docker ps
python3 scripts/manage_cluster.py --env-file .env.docker status
```

`config --quiet` verifica la configurazione senza stamparne i valori. Non allegare l'output completo di `config`, `inspect` o `env` se può includere password. `git diff --check` controlla la formattazione dei diff tracciati; non revisiona automaticamente file nuovi non aggiunti, contenuti sensibili o correttezza applicativa.

## Preparazione e prove prima del deploy

La [guida sviluppo e test](sviluppo-test.md) spiega i prerequisiti e gli effetti delle suite. Il rilascio deve scegliere verifiche pertinenti alla modifica.

| Modifica | Verifiche da considerare |
| --- | --- |
| Regola business o API | Compilazione, suite del modulo, ruoli, errori e conservazione dei dati |
| Migrazione SQL | Upgrade da versione precedente su copia, schema e audit finali, suite SQLite e integrazione MySQL pertinenti |
| Frontend | Flusso reale, profilo gestore/operatore, mobile, desktop, tastiera ed errori API |
| Email | Fixture SMTP TLS, ledger, outbox, esiti incerti e revoche |
| Calendar | Fixture OAuth/Google, isolamento, cifratura, ETag, idempotenza e worker |
| NPM | Test della trasformazione privacy e prova Nginx isolata |
| Squid/reti/TLS | Build/configurazione, test policy isolati e controlli live della matrice |
| Group Replication | Controller, driver e verifiche HA in un ambiente con interruzioni autorizzate |

Le prove con provider simulati non richiedono l'attivazione dei fornitori reali. Nel deployment attuale SMTP e Google sono rinviati: un rilascio del codice non deve abilitarli implicitamente solo perché i servizi diventano sani.

Per una modifica C ordinaria, una sequenza di compilazione e test è:

```sh
make
make test
docker compose --env-file .env.docker build app
```

Il target Docker verifica anche il backend MySQL e le dipendenze dell'immagine. La suite nativa non sostituisce la compilazione dell'immagine, e la build dell'immagine non esegue automaticamente tutte le prove live sullo stack.

Per gli addon compilare i componenti effettivamente cambiati e le relative suite. Una modifica solo documentale non richiede di avviare provider, fare migrazioni o simulare guasti dei container.

## Backup prima della distribuzione

Un dump SQL è sufficiente solo per un intervento il cui piano di ritorno non coinvolga gli altri archivi. Se cambiano documenti, addon, configurazioni NPM o chiavi, predisporre un punto coerente secondo [backup e ripristino](operazioni-backup-ripristino.md).

Salvare ledger email e outbox insieme, mantenendo `MAIL_API_NAMESPACE`. Salvare volume Calendar e relativa chiave AES senza sostituirla. I database SQL e MongoDB non condividono uno snapshot: sospendere le scritture durante una copia coordinata. Non considerare i tre volumi MySQL come tre backup di versioni diverse.

Conservare inoltre l'immagine precedente o un riferimento riproducibile: `docker compose build` aggiorna il tag locale della build e non costituisce una strategia automatica di rollback. Documentare dove si trova la versione da ripristinare prima di sovrascrivere l'unico riferimento utile.

## Distribuzione mirata dell'app

Su uno stack già avviato e con dipendenze sane, dopo le verifiche e il backup appropriato:

```sh
docker compose --env-file .env.docker up -d --no-deps --force-recreate --wait app
```

Il comando usa l'immagine già compilata. Non ricrea SQL o MongoDB; interrompe le connessioni dell'app e perde le sessioni in memoria. Le migrazioni necessarie vengono eseguite dal nuovo processo durante l'avvio, prima che il normale servizio sia disponibile.

Se cambia anche NPM, compilarlo prima e ricrearlo in modo esplicito dopo l'app. Per un insieme più ampio di servizi usare soltanto quelli realmente modificati:

```sh
docker compose --env-file .env.docker build proxy-egress mail calendar inventory app proxy-web
docker compose --env-file .env.docker up -d --no-deps --force-recreate --wait proxy-egress mail calendar inventory app proxy-web
```

Questa seconda sequenza è per un rilascio congiunto dei componenti elencati; non è obbligatoria per ogni modifica. `--no-deps` richiede che le dipendenze escluse siano già disponibili. Le chiavi e i certificati mancanti vanno predisposti prima dei container, con la procedura documentata, non creando file vuoti per soddisfare i mount.

## Migrazioni: percorso applicato e limiti

L'app rifiuta database che dichiarino una versione superiore a quella supportata. Per SQLite legge `PRAGMA user_version`; per MySQL controlla `schema_metadata`. Il codice corrente porta i dati alla versione 9; la versione effettivamente distribuita va letta nell’ambiente di destinazione. Calendar gestisce separatamente la propria versione 1 e rifiuta versioni superiori; il ledger email non ha una numerazione `user_version` gestita come quella Calendar.

Il rilascio inventory aggiunge lo schema applicativo 9 (`inventory_due`, `days_before`), senza nuove tabelle dell’app, e lo schema indipendente inventory 3 (anteprime CSV). Richiede app, servizio inventory e interfaccia aggiornati insieme. Conservare backup di entrambi gli archivi; la verifica nativa non certifica la migrazione del cluster MySQL.

Il rilascio attività/blocchi passa da schema 7 a 8, aggiungendo due tabelle senza trasformare le commesse o le ore esistenti. Richiede il binario nuovo insieme a `app.html`, `app.js` e `work-board.css`; gli addon mantengono i propri formati. Prima della distribuzione verificare aggiornamento su copia, accessi Gestore/Collaboratore/Lettore, revisioni obsolete, gerarchie e cancellazioni. La verifica SQLite non prova da sola il DDL o le transazioni sul cluster MySQL. Vedere [specifica del modulo](attivita-kanban-e-blocchi.md).

Le migrazioni applicative comprendono nuove tabelle, colonne, vincoli, trigger, dati iniziali e audit. Non basta copiare il DDL della documentazione per ottenere lo stesso aggiornamento. Le operazioni DDL MySQL e i passaggi del controller non sono una transazione unica che possa annullare automaticamente qualsiasi aggiornamento parziale.

In caso di avvio interrotto:

1. Conservare log sanitizzati, schema osservato e volumi.
2. Leggere lo stato reale e la versione persistita, distinguendo errore di connessione, versione incompatibile ed errore durante la migrazione.
3. Stabilire se il codice corrente può completare in sicurezza il passaggio previsto; non ritentare automaticamente cancellando tabelle o metadati.
4. Se serve recuperare una copia precedente, seguire il piano di ripristino e validare l'insieme degli archivi coinvolti.

`manage_cluster.py init` prepara o recupera il gruppo, ma non è un comando per tornare a uno schema precedente. `recover` ricostituisce il gruppo sui volumi esistenti; non importa il backup del rilascio.

## Verifiche dopo il rilascio

```sh
docker compose --env-file .env.docker ps
python3 scripts/manage_cluster.py --env-file .env.docker status
curl --fail --max-time 5 http://localhost/healthz
docker compose --env-file .env.docker logs --since=10m --tail=100 app proxy-db
```

Completare poi le verifiche specifiche: nuovo login, funzione modificata, isolamento dei ruoli, dati precedenti leggibili, eventuali PDF scaricabili con integrità verificata, stato addon e code. Le API di stato possono essere controllate senza avviare invii o modifiche di calendari reali.

Criteri di successo del deployment locale: servizi interessati sani, tre membri SQL `ONLINE`/`PRIMARY` quando tutti devono essere disponibili, schema atteso, percorso NPM→app funzionante e nessuna regressione osservata nei controlli scelti. Documentare eventuali limiti, come fornitori non configurati o test live non eseguiti.

`--wait` non è un collaudo end-to-end del prodotto. Anche un health check positivo dell'app non verifica MongoDB, SMTP o Google. Usare [diagnostica](diagnostica-e-osservabilita.md) se un controllo fallisce.

## Ritorno alla versione precedente

| Situazione | Possibile percorso | Condizione essenziale |
| --- | --- | --- |
| Solo codice/frontend, schema e formati invariati | Ridistribuire un'immagine precedente compatibile | Verifica preventiva che legga i dati e le risposte prodotti dalla versione nuova |
| Nuovo schema non supportato dal vecchio binario | Completare/correggere il rilascio oppure ripristinare una copia coerente | Non abbassare manualmente il numero versione per eludere il controllo |
| Cambiamento di formato addon | Ripristino coordinato di binario, archivio e secret necessari | Conservare ledger/idempotenza e chiavi corrette |
| Modifica a reti o CA | Ripristino della configurazione e della fiducia di tutti i peer | IP, bind, SAN, allowlist e mount devono tornare coerenti |
| Perdita dei volumi | Ripristino da backup verificato | `recover` da solo non può ricreare i dati perduti |

Un rollback del codice non annulla dati già modificati, notifiche emesse, email accettate da SMTP o eventi creati in Google. Il ripristino di un database vecchio può far riapparire operazioni già completate: ledger, namespace e gestione degli esiti incerti devono essere valutati prima di riabilitare i provider.

Non sono implementate down-migration automatiche, sostituzione blue/green dell'app, rollout su più host o rollback automatico in caso di health check fallito. Questi sono eventuali sviluppi futuri, non proprietà dell'attuale Compose.

## Cambi di infrastruttura più ampi

Aggiornare una major version SQL, spostare i membri su host differenti, cambiare subnet o sostituire la CA richiede un piano specifico. Il controller locale assume nomi, indirizzi, gruppo e marcatori del progetto; non va riutilizzato come orchestratore generico di un nuovo ambiente senza revisione.

Anche con tre membri SQL, app, proxy e archivi addon rimangono singole istanze. Una manutenzione dell'host o un aggiornamento del daemon può interrompere tutto lo stack. Non dichiarare un rilascio senza interruzioni basandosi soltanto sulla modalità multi-primary.

## Evidenze storiche e nuove verifiche

La [guida test](sviluppo-test.md) riporta risultati registrati il 10 ottobre 2026: suite completa 270 casi, 241 superati e 29 facoltativi saltati; controlli specifici Calendar, privacy NPM e 20 prove di rete superati in quel collaudo. Sono evidenze della versione allora verificata, non una certificazione delle modifiche successive.

Ogni nuovo rilascio deve riportare i propri esiti osservati e distinguere test eseguiti da controlli proposti o non disponibili. Non aggiornare i numeri storici sulla base del solo numero di metodi presenti nei file di test.
