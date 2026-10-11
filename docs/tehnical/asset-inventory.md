# Asset Inventory: servizio, API e operazioni

[Indice tecnico](README.md) · [Guida prodotto](../prod/asset-inventory.md) · [Manuale addon](../../inventory-addon/README.md)

L'inventario è un terzo addon C indipendente. Il browser usa sessione e API dell'app principale; `src/inventory_client.c` inoltra le richieste al servizio `inventory` via HTTPS, con token di servizio e verifica della CA e del nome del certificato. Il servizio usa SQLite nel proprio volume, con schema inventory versione 3. Gli alert sulle scadenze richiedono lo schema applicativo MySQL/SQLite 9.

## Contratto pubblico e autorizzazioni

| Metodo e percorso | Permessi e risultato |
| --- | --- |
| `GET /api/inventory/status` | Utente autenticato: stato del modulo e del servizio. |
| `GET /api/inventory/assets/export` | Utente autenticato: pagina CSV con `csv`, `exported`, `next_after_id`; stessi filtri del catalogo. |
| `POST /api/inventory/import/preview` | Gestore: valida `csv` e `mode=create_only|upsert`, restituisce anteprima e ID monouso. |
| `POST /api/inventory/import/commit` | Gestore: conferma `preview_id` e applica atomicamente il lotto. |
| `GET /api/inventory/due` | Utente autenticato: scadenze paginate, con `today`, `days_before`, `limit`, `after_id`. |
| `GET /api/inventory/assets` | Utente autenticato: pagina del catalogo con `items`, `next_after_id`, `summary`. |
| `GET /api/inventory/assets/<id>` | Utente autenticato: scheda in `asset`. |
| `GET /api/inventory/assets/<id>/history` | Utente autenticato: eventi e snapshot del bene, paginati. |
| `POST /api/inventory/assets` | Gestore: crea una scheda, risposta `201`. |
| `POST /api/inventory/assets/<id>` | Gestore: modifica la scheda alla `version` corrente, risposta `200`. |
| `GET /api/inventory/assets/<id>/relations` | Utente autenticato: pagina delle relazioni in ingresso e in uscita. |
| `POST /api/inventory/assets/<id>/relations` | Gestore: crea una relazione dalla sorgente, restituisce relazione e scheda aggiornata. |
| `POST /api/inventory/assets/<id>/relations/<relation_id>/remove` | Gestore: disattiva una relazione in uscita e aggiorna la versione sorgente. |

Le scritture usano `application/x-www-form-urlencoded` e i controlli di origine dell'app. Il bridge accetta soltanto i parametri previsti e aggiunge `actor_id` dalla sessione autenticata. Non permette al browser di selezionare l'autore né di inviare percorsi privati arbitrari. L'inventario è globale: la visibilità non dipende dalle abilitazioni alle commesse.

Con il modulo disabilitato, `GET /api/inventory/status` restituisce `200` con `ok: true` e `configured: false`; le altre operazioni rispondono `503`. Un servizio configurato ma non raggiungibile produce un errore esplicito.

Il catalogo accetta `q` (massimo 200 byte UTF-8, inclusi i dettagli della tipologia), `status`, `asset_type`, `limit` da 1 a 100 (predefinito 30) e `after_id`; storico e relazioni accettano `limit` e `after_id`. L'ordine è per ID decrescente, proseguendo con ID inferiori al cursore. `next_after_id` è nullo a fine elenco. Le pagine possono contenere meno elementi del limite richiesto per mantenere le risposte entro 128 KiB; il cursore conserva la continuità della lettura. `summary` contiene `total`, `available`, `assigned`, `maintenance`, `retired` e `lost` dell'intero catalogo, indipendentemente dai filtri.

I campi, i limiti e i percorsi privati `/v1/*` sono documentati nel [manuale dell'addon](../../inventory-addon/README.md). La modifica sostituisce i campi comuni della scheda completa. L'omissione di `asset_type` e `details` conserva i valori correnti per i client precedenti; l'invio di `details` sostituisce i dettagli specifici. `asset_tag` usa un vincolo univoco SQLite `NOCASE`; tale confronto non offre una normalizzazione Unicode generale. `version` controlla la concorrenza e aumenta a ogni modifica della scheda o delle sue relazioni in uscita. `409` segnala etichetta duplicata, versione superata o conflitti con relazioni/capacità, `404` una risorsa inesistente, `400` parametri non validi. Gli errori leggibili dall'utente sono in italiano.

## CSV e controlli sulle scadenze

I percorsi privati corrispondono a `/v1/assets/export`, `/v1/import/preview`, `/v1/import/commit` e `/v1/due`. Il POST di anteprima ammette un form codificato fino a 1 MiB solo sul percorso pubblico esatto; il CSV decodificato resta limitato a 256 KiB e 200 righe. Tutti gli altri form inventory mantengono il limite di 32 KiB. Le risposte restano entro 128 KiB.

L’anteprima restituisce `preview_id` (nullo se non confermabile), `can_commit`, `summary: {total,create,update,errors}` e `items` con riga, codice, nome, tipologia, azione ed errori. Il piano normalizzato è persistente, legato all’autore, valido 15 minuti e monouso; sono ammesse al massimo 64 anteprime aperte per autore. Il commit rivalida le versioni e conferma schede e snapshot in un’unica transazione; un errore annulla tutto il lotto e consuma l’anteprima. Il formato e la protezione dalle formule sono nel [manuale addon](../../inventory-addon/README.md#csv).

`GET /v1/due` usa il giorno locale del servizio se `today=YYYY-MM-DD` è assente e `days_before=30` per impostazione iniziale (1–365). Include date <= oggi + anticipo, senza limite inferiore, per `warranty_until`, `maintenance_due`, `expires_on`, `renewal_on`; esclude `retired` e `lost`. Ogni elemento contiene `asset_id`, `asset_tag`, `name`, `asset_type`, `due_kind`, `due_on`, `days_remaining`. L’ordine è per ID asset decrescente; `limit` conta asset con scadenze e tutte le scadenze dello stesso asset rimangono nella stessa pagina.

La regola applicativa `inventory_due` salva `days_before` e usa il pianificatore esistente in Europe/Rome. Il client acquisisce tutte le pagine prima della transazione SQL, entro limiti di 100 pagine, 10000 eventi e 8 secondi; dati incompleti, non validi o un servizio non disponibile impediscono di consumare l’esecuzione. I vincoli univoci per regola/destinatario deduplicano `inventory_due:<asset_id>:<due_kind>:<due_on>:<days_before>`. Le notifiche espongono `event_kind`, `asset_id`, `due_kind`, `due_on`, `days_before`; `work_date` e `recorded_minutes` sono nulli. Per questa regola tutti gli utenti selezionati ricevono gli avvisi dell’inventario globale.

## Tipologie e relazioni

`asset_type` ammette `it_device` (predefinito per le creazioni legacy), `network_device`, `physical`, `software`, `license`. Il form trasporta `details` come stringa contenente un oggetto JSON; le risposte lo espongono come oggetto. La validazione dipende dalla tipologia e rifiuta campi sconosciuti, chiavi duplicate, tipi JSON errati e valori oltre i limiti. IP, MAC, VLAN, quantità e date sono validati dal servizio, anche per richieste private senza interfaccia. Una licenza richiede un riferimento positivo a una scheda `software` e `seats_total` da 1 a 1000000. `license_reference` conserva un riferimento amministrativo, non una chiave di attivazione.

`installed_on` collega software a dispositivi IT o apparati di rete, con quantità 1 e versione installata facoltativa. `connected_to` collega un dispositivo IT o apparato di rete a un apparato di rete diverso dalla sorgente, con quantità 1 e porte facoltative. `licensed_to` consuma posti di una licenza: `per_device` richiede un dispositivo, `per_user` un assegnatario testuale, `concurrent` e `site` esattamente uno dei due. Le relazioni sono dichiarazioni manuali; non vengono interrogati dispositivi, directory o license server.

Le licenze espongono `license_usage: {allocated, available, expired}`. `allocated` somma le quantità delle assegnazioni attive e `available` sottrae tale totale dai posti acquistati. Le assegnazioni oltre capacità o su licenze scadute sono rifiutate; rimuovere una relazione libera i posti. Il servizio rifiuta relazioni duplicate attive e nuovi collegamenti con sorgenti o destinazioni `retired`/`lost`. Non consente di ridurre la capacità sotto le assegnazioni, cambiare tipologia con relazioni attive o cambiare software/modello di una licenza assegnata. Un software referenziato da una scheda licenza deve restare software.

La scadenza è inclusiva: una licenza con `expires_on` resta valida per l'intera data indicata e risulta `expired` dal giorno successivo, calcolato nel fuso `INVENTORY_BUSINESS_TIMEZONE` (predefinito `Europe/Rome`). Una scadenza vuota non impone un limite temporale. La valutazione avviene alla lettura o alla richiesta di una nuova assegnazione, senza processi pianificati; le assegnazioni già registrate restano nel conteggio. `renewal_on` e `renewal_mode` sono informazioni amministrative e non attivano rinnovi automatici. I timestamp degli eventi e delle schede restano in UTC.

Aggiunta e rimozione richiedono la `version` corrente della sorgente. La risposta contiene la relazione e l'asset sorgente alla revisione successiva; la destinazione mantiene la propria revisione. L'elenco include `direction`, `source` e `target` sintetici per rendere consultabili entrambe le direzioni. La rimozione è una disattivazione: lo storico conserva l'identità della relazione.

## Persistenza e storia

Il database separato `inventory.sqlite3` conserva schede, relazioni e `asset_history`. Ogni bene conserva campi comuni, tipologia e dettagli JSON, autore dell'ultima scrittura, versione e date di creazione/aggiornamento. Ogni evento conserva `action` (`created`, `updated`), `actor_id`, versione, data e snapshot JSON completo dopo la scrittura. Gli eventi delle relazioni aggiungono `change`, con `operation` (`relation_added`, `relation_removed`) e una copia immutabile della relazione. Bene, relazione, incremento di versione e storico sono salvati nella stessa transazione: se lo storico non può essere registrato, l'intera modifica viene annullata. Non esiste cancellazione della scheda; la dismissione usa lo stato `retired`.

La migrazione automatica da schema inventory 1 a 2 avviene all'avvio in una transazione, conservando identificativi e dati. Le vecchie schede ricevono tipologia `it_device` e dettagli vuoti; i vecchi snapshot rimangono invariati, quindi i lettori dello storico devono accettare snapshot privi di `asset_type`, `details` e `change`. Un archivio con versione superiore a quella supportata impedisce l'avvio. La compatibilità dei vecchi client riguarda i form senza i nuovi campi; le immagini precedenti non possono aprire lo schema 3. La migrazione 2 → 3 aggiunge le anteprime CSV persistenti, senza riscrivere schede e storico.

L'identità autore è un ID dell'app registrato nel database indipendente; non è una foreign key MySQL e non risolve automaticamente i nomi degli utenti. Assegnatario e posizione sono testi liberi. I dati dell'inventario e gli snapshot sono in chiaro nel volume; il TLS protegge il trasporto.

## Deployment e configurazione

| Elemento | Configurazione |
| --- | --- |
| Rete | `inventory_api`, `172.30.80.0/24`, `internal: true`, gateway IPv4 `isolated`. |
| App | `172.30.80.10`, nuova interfaccia `inventory_api`; nessun listener HTTP su questa interfaccia. |
| Addon | `172.30.80.2:8092`, alias DNS `inventory-api`, client ammesso `172.30.80.10`. |
| Token | `data/addon-secrets/inventory-api-token`, montato come `/run/secrets/inventory-api-token` solo in app e addon. |
| TLS addon | Certificato e chiave `data/network-tls/inventory-addon.crt` e `.key`; CA interna condivisa. |
| Archivio | Volume `inventory_addon_data`, file `/var/lib/punto-inventory/inventory.sqlite3`. |
| Processo | UID/GID `10005:10005`, radice readonly, `/tmp` temporaneo, `cap_drop: ALL`, `no-new-privileges`. |
| Giorno di scadenza | `INVENTORY_BUSINESS_TIMEZONE`, predefinito `Europe/Rome`; il runtime Docker include `tzdata` per i fusi IANA. |

L'addon è collegato alla sola rete inventory, non a `app_egress` o `backend_app`; non ha porta host, accesso a Squid, credenziali database o necessità di uscire verso Internet. Le otto reti Compose mantengono gli altri addon nei rispettivi segmenti. Bind, IP ammesso, subnet e SAN devono cambiare insieme se gli indirizzi vengono modificati; `configure_network.py` include questa subnet nel controllo delle sovrapposizioni.

Per un'installazione esistente già configurata, dalla radice del repository:

```sh
python3 scripts/configure_network.py --env-file .env.docker prepare
docker compose --env-file .env.docker build inventory app
docker compose --env-file .env.docker up -d inventory app
```

`prepare` genera soltanto i materiali mancanti, verifica quelli presenti e conserva CA, certificati SQL e token esistenti. Non stampare né versionare i segreti. Compose attende l'healthcheck HTTPS dell'addon prima dell'avvio dell'app. L'aggiornamento ricrea app e addon; conservare i volumi. Per un cluster non ancora inizializzato seguire la procedura generale nel [README](../../README.md).

Nell'app: `INVENTORY_API_URL`, `INVENTORY_API_TOKEN_FILE`, `INVENTORY_API_CA_FILE`. In modalità nativa tutte assenti disabilitano il modulo; una configurazione parziale è rifiutata. Se abilitate, HTTPS, token e CA restano obbligatori. Il client ammette `https://inventory-api:8092`, `https://inventory-addon:8092` e URL HTTPS sul loopback con porta esplicita; il proxy è disabilitato per queste chiamate private. Il listener nativo dell'addon richiede `INVENTORY_BIND`, `INVENTORY_ALLOWED_CLIENT`, `INVENTORY_PORT`, `INVENTORY_TLS_CERT_FILE`, `INVENTORY_TLS_KEY_FILE`, `INVENTORY_API_TOKEN_FILE` e `INVENTORY_DB_PATH` coerenti con l'ambiente.

## Backup, ripristino e verifiche

Il backup del cluster SQL e quello MongoDB non includono l'inventario. Compose abilita il backup automatico dell'addon tramite `INVENTORY_BACKUP_DIR=/var/backups/punto-inventory`, sul volume separato `inventory_addon_backups` di UID/GID `10005:10005`. In modalità nativa il backup resta disabilitato senza una directory esplicita, assoluta, reale e non scrivibile da altri. `INVENTORY_BACKUP_INTERVAL_HOURS` vale 24 (range 1–168); `INVENTORY_BACKUP_KEEP` vale 14 (range 1–365).

La prima copia viene creata all'avvio, le successive all'intervallo previsto. L'API SQLite Backup acquisisce anche le scritture confermate nel WAL; ogni copia temporanea privata `0600` supera `PRAGMA integrity_check` prima di `fsync` e rinomina atomica. I tentativi occupati hanno un limite temporale; gli errori non arrestano l'addon e introducono una pausa di almeno cinque minuti prima del nuovo tentativo. All'avvio si cercano copie integre e si recupera lo stato persistente `.inventory-backup-status.json`: il riavvio non raddoppia la copia ancora valida. La conservazione elimina soltanto copie regolari appartenenti al servizio, con nome previsto e integrità verificata; i link simbolici e i file estranei non vengono toccati.

`GET /v1/status` espone `backup.enabled`, `interval_hours`, `keep`, `last_success`, `last_attempt`, `next_due`, `latest_file` ed eventuale `last_error`. Gli orari sono UTC, i valori mancanti `null` e il file è un nome senza percorso host. Salvare le copie anche fuori dall'host; conservare token/TLS separatamente e non ruotarli per un normale aggiornamento.

Il programma `scripts/inventory_backup.py` offre `create SORGENTE NUOVO_FILE`, `verify BACKUP` e `restore BACKUP NUOVO_FILE`. Le copie usano SQLite Backup e verifica di integrità; la pubblicazione rifiuta qualsiasi destinazione già presente, anche un link simbolico. Un esempio:

```sh
python3 scripts/inventory_backup.py verify /backup/inventory-copia.sqlite3
python3 scripts/inventory_backup.py restore /backup/inventory-copia.sqlite3 /ripristino/inventory.sqlite3
```

Prima dell'aggiornamento allo schema 2 creare un backup coerente dello schema 1. Un eventuale rollback richiede la vecchia immagine e il ripristino di tale backup: non modificare manualmente `PRAGMA user_version` per simulare un downgrade. Le assegnazioni e le relazioni aggiunte dopo la migrazione richiedono un backup aggiornato dello schema 2 per essere recuperate.

Per attivare il file ripristinato arrestare l'addon, conservare il vecchio archivio e gli eventuali WAL/SHM, collocare il nuovo database nel percorso configurato senza i vecchi file laterali e verificare proprietà UID/GID `10005:10005` e permessi prima del riavvio. Copiare solo `inventory.sqlite3` durante le scritture può perdere dati. Controllare poi healthcheck, catalogo e storico tramite l'app. Una singola istanza SQLite non eredita quorum, replica o failover di MySQL.

La build nativa è `make -C inventory-addon`. `python3 -m unittest discover -s tests -p 'test_inventory*.py' -v` verifica il contratto HTTPS con certificati e archivi temporanei, dettagli per tipologia, migrazione da uno schema 1 reale, compatibilità legacy, relazioni, allocazioni, autorizzazioni e rollback transazionale. I test del controller verificano la conservazione di CA e token. La suite opzionale `PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v` controlla TLS, autenticazione di servizio, hostname, segmentazione, assenza di egress e credenziali estranee. Queste indicazioni descrivono prove disponibili, non certificano uno stack live non collaudato.

Fonti: `inventory-addon/src/inventory.c`, `inventory-addon/Dockerfile`, `src/inventory_client.c`, `src/server.c`, `compose.yaml`, `scripts/configure_network.py`, `tests/test_network_security.py`.
