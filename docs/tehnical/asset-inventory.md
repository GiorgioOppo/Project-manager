# Asset Inventory: servizio, API e operazioni

**Italiano** | [English](../en/tehnical/asset-inventory.md)

[Indice tecnico](README.md) · [Guida prodotto](../prod/asset-inventory.md) · [Manuale addon](../../inventory-addon/README.md)

L'inventario è un terzo addon C indipendente. Il browser usa sessione e API dell'app principale; `src/inventory_client.c` inoltra le richieste al servizio `inventory` via HTTPS, con token di servizio e verifica della CA e del nome del certificato. Il servizio usa SQLite nel proprio volume, con schema inventory versione 5. Lo schema applicativo MySQL/SQLite è alla versione 10, con i tipi alert per restituzioni e verifiche e il relativo intervallo.

## Contratto pubblico e autorizzazioni

| Metodo e percorso | Permessi e risultato |
| --- | --- |
| `GET /api/inventory/status` | Utente autenticato: stato del modulo e del servizio. |
| `GET /api/inventory/assets/export` | Utente autenticato: pagina CSV con `csv`, `exported`, `next_after_id`; stessi filtri del catalogo. |
| `POST /api/inventory/import/preview` | Gestore: valida `csv` e `mode=create_only|upsert`, restituisce anteprima e ID monouso. |
| `POST /api/inventory/import/commit` | Gestore: conferma `preview_id` e applica atomicamente il lotto. |
| `GET /api/inventory/due` | Utente autenticato: scadenze paginate, con `today`, `days_before`, `limit`, `after_id`. |
| `GET /api/inventory/dashboard` | Utente autenticato: conteggi e distribuzioni con gli stessi filtri del catalogo, senza cursori. |
| `GET /api/inventory/operational-due` | Utente autenticato: eventi paginati con `kind=return|verification`, `verification_days`, `today`, `limit`, `after_id`. |
| `GET /api/inventory/assets` | Utente autenticato: pagina del catalogo con `items`, `next_after_id`, `summary`. |
| `GET /api/inventory/assets/<id>` | Utente autenticato: scheda in `asset`. |
| `GET /api/inventory/assets/<id>/history` | Utente autenticato: eventi e snapshot del bene, paginati. |
| `POST /api/inventory/assets` | Gestore: crea una scheda, risposta `201`. |
| `POST /api/inventory/assets/<id>` | Gestore: modifica la scheda alla `version` corrente, risposta `200`. |
| `GET /api/inventory/assets/<id>/relations` | Utente autenticato: pagina delle relazioni in ingresso e in uscita. |
| `POST /api/inventory/assets/<id>/relations` | Gestore: crea una relazione dalla sorgente, restituisce relazione e scheda aggiornata. |
| `POST /api/inventory/assets/<id>/relations/<relation_id>/remove` | Gestore: disattiva una relazione in uscita e aggiorna la versione sorgente. |
| `GET /api/inventory/users` | Gestore: directory degli utenti attivi dell'app con `id`, `name`, `email`. |
| `GET /api/inventory/places` | Utente autenticato: anagrafiche di sedi, reparti e ubicazioni. |
| `POST /api/inventory/places` | Gestore: crea un'anagrafica. |
| `POST /api/inventory/places/<id>` | Gestore: aggiorna o disattiva un'anagrafica alla versione corrente. |
| `POST /api/inventory/assets/<id>/context` | Gestore: sostituisce assegnazione strutturata e collocazione del bene. |
| `POST /api/inventory/assets/<id>/checkout` | Gestore: apre una consegna di un bene disponibile o già assegnato strutturalmente allo stesso utente. |
| `POST /api/inventory/assets/<id>/checkin` | Gestore: chiude la consegna e rende il bene disponibile o in manutenzione. |
| `POST /api/inventory/assets/<id>/verify` | Gestore: registra una verifica fisica. |
| `GET /api/inventory/assets/<id>/custody` | Utente autenticato: storico paginato delle consegne. |
| `GET /api/inventory/assets/<id>/verifications` | Utente autenticato: storico paginato delle verifiche. |

Le scritture usano `application/x-www-form-urlencoded` e i controlli di origine dell'app. Il bridge accetta soltanto i parametri previsti e aggiunge `actor_id` dalla sessione autenticata. Non permette al browser di selezionare l'autore né di inviare percorsi privati arbitrari. L'inventario è globale: la visibilità non dipende dalle abilitazioni alle commesse.

Con il modulo disabilitato, `GET /api/inventory/status` restituisce `200` con `ok: true` e `configured: false`; le altre operazioni rispondono `503`. Un servizio configurato ma non raggiungibile produce un errore esplicito.

Il catalogo accetta `q` (massimo 200 byte UTF-8, inclusi i dettagli della tipologia), `status`, `asset_type`, `mine=1`, `limit` da 1 a 100 (predefinito 30) e `after_id`; storico e relazioni accettano `limit` e `after_id`. `mine=1` filtra il catalogo e l'esportazione per l'utente autenticato: il bridge ricava l'identità dalla sessione e non ammette un ID utente arbitrario dal browser. È un filtro di comodità, senza restringere la visibilità del catalogo globale. L'ordine è per ID decrescente, proseguendo con ID inferiori al cursore. `next_after_id` è nullo a fine elenco. Le pagine possono contenere meno elementi del limite richiesto per mantenere le risposte entro 128 KiB; il cursore conserva la continuità della lettura. `summary` contiene `total`, `available`, `assigned`, `maintenance`, `retired` e `lost` dell'intero catalogo, indipendentemente dai filtri.

Catalogo, esportazione e cruscotto condividono anche `site_id`, `department_id` (ID positivi), `attention=open_custody|overdue_returns|never_verified|verification_due|location_mismatch`, `verification_days` (1–365, predefinito 90) e `today` (data valida facoltativa, altrimenti giorno aziendale). La ricerca comprende i nomi strutturati di sede, reparto e assegnatario. I filtri di collocazione usano i riferimenti strutturati; i vecchi testi liberi non diventano automaticamente anagrafiche.

`dashboard` restituisce `today`, `verification_days`, `counts: {total,open_custody,overdue_returns,never_verified,verification_due,location_mismatch}`, `by_status`, `by_type`, `by_site`, `by_department`, `by_user` e `groups_truncated`. I conteggi si riferiscono all'intera popolazione filtrata, non alla prima pagina. Ogni distribuzione contiene `{id,name,count}`, ordinata per numero decrescente, con al massimo 20 gruppi e il gruppo senza riferimento mantenuto quando presente; `id` è nullo per quest'ultimo. I conteggi operativi considerano soltanto beni materiali non dismessi/smarriti. Le restituzioni richiedono una consegna aperta con `due_on < today`; le verifiche da ripetere richiedono `ultima verified_on + verification_days <= today`. I mai verificati sono contati separatamente e la differenza di ubicazione corrisponde all'ultima verifica cronologica con `location_matches=false`. Le query usano la verifica con data più recente, poi ID maggiore in caso di parità.

## Persone, identità e beni collegati

Lo schema inventory 5 aggiunge `inventory_people`, `inventory_identities`, `inventory_identity_people`, `inventory_entity_history` e `inventory_asset_subjects`. Persone reali e utenze censite sono indipendenti dagli utenti di login dell'app; la relazione persona-identità è molti a molti senza vincoli di cardinalità per tipo. Nessuna credenziale viene accettata tra i campi API. Le anagrafiche sono condivise con gli stessi permessi del catalogo: lettura autenticata, scrittura Gestore, autore dalla sessione.

Il bridge espone sotto `/api/inventory` i percorsi `/people`, `/identities`, i dettagli `/<id>` e storici `/<id>/history`; GET e POST sugli elenchi significano consultazione e creazione, POST sul dettaglio aggiornamento completo con versione. `/people/<id>/identities` e `/identities/<id>/people` elencano i collegamenti attivi. POST sul secondo accetta `version` dell'identità, `person_id`, `role=owner|user`, `notes`; POST su `/identities/<id>/people/<link_id>/remove` li disattiva. Versione e storico di entrambi i soggetti vengono aggiornati atomicamente.

GET/POST `/assets/<id>/subjects` consulta/crea collegamenti diretti a una persona o identità. POST richiede `version` del bene, `subject_kind=person|identity`, `subject_id`, `role=owner|user|administrator`, `notes`; POST su `/assets/<id>/subjects/<link_id>/remove` rimuove logicamente con la versione del bene. Le scritture aggiornano il solo bene, con snapshot immutabile del soggetto e evento nello storico. GET `/people/<id>/assets` e `/identities/<id>/assets` restituisce i collegamenti inversi, senza propagare le appartenenze alle utenze condivise. Non cambia custodia, stato o diritti di accesso.

Directory, collegamenti e storico sono paginati `{ok,items,next_after_id}` per ID decrescente, `limit=1..100` e budget 128 KiB; i singoli dettagli restituiscono `person` o `identity`. Directory: `q`, `active`; identità anche `kind` e `person_id`. Catalogo, CSV e cruscotto accettano `person_id` e `identity_id` positivi combinati in AND sui collegamenti diretti attivi. Formati e limiti dei campi sono nel [manuale addon](../../inventory-addon/README.md#persone-e-identità). Disattivare non cancella riferimenti o storia; nuovi collegamenti richiedono soggetti attivi, rimozioni restano possibili. `409` segnala duplicati o versioni superate.

## Assegnazioni, consegne e verifiche

Le anagrafiche `places` accettano `kind=site|department|location`, `name`, `parent_id`, `active` e, in modifica, `version`. Una sede non ha padre; un reparto può indicare una sede, mentre un'ubicazione deve indicarla. `kind` non è modificabile e non è prevista cancellazione. Cambiare `parent_id` di un luogo referenziato nei contesti correnti restituisce `409`, evitando di invalidare l'associazione tra sede e ubicazione/reparto; i soli riferimenti storici non impediscono il cambio. Il contesto della scheda contiene `assignee_user_id`, `assignee_user_name`, `site_id`, `site_name`, `department_id`, `department_name`, `location_id`, `location_name`; reparto e ubicazione devono appartenere alla sede selezionata. I nomi sono snapshot risolti dal servizio e dal bridge, non campi scrivibili liberamente dal browser. La directory `/api/inventory/users` interroga il database principale e restituisce soltanto utenti attivi ai Gestori.

`POST /assets/<id>/context` richiede `version` e sostituisce tutti i riferimenti di contesto: `assignee_user_id`, `site_id`, `department_id`, `location_id`. È previsto per dispositivi IT, apparati di rete e beni fisici. L'assegnazione di un utente imposta `assigned`; rimuoverlo riporta ad `available` un bene precedentemente assegnato. Gli utenti devono essere attivi al momento della nuova assegnazione; la loro successiva disattivazione non cancella i nomi già registrati. Le assegnazioni testuali delle relazioni di licenza restano compatibili con il contratto precedente.

La consegna richiede `version`, `user_id`, `checkout_on`, `due_on`, `condition_out=good|fair|damaged` e note facoltative; è ammessa su un bene disponibile oppure già assegnato strutturalmente allo stesso utente, sempre senza consegna aperta. La restituzione richiede `version`, `returned_on`, `condition_in`, note facoltative e `status=available|maintenance`; chiude la consegna, rimuove l'utente e conserva i luoghi. Le date effettive non possono superare il giorno corrente nel fuso aziendale; la scadenza prevista non può precedere la consegna. Una consegna aperta oltre `due_on` risulta in ritardo senza cambiare automaticamente lo stato.

La verifica richiede `version`, `verified_on`, `condition=good|fair|damaged`, note facoltative e `observed_location_id` facoltativo. Registra contesto atteso e osservato, senza trasferire il bene all'ubicazione osservata; `location_matches` è nullo se manca una delle due ubicazioni. Le risposte della scheda includono `context`, la consegna attiva in `custody` e `last_verification`, nulli dove non ancora registrati. I percorsi `custody` e `verifications` espongono lo storico in ordine ID decrescente con `limit` da 1 a 100 e `after_id`. Una consegna conserva `notes`, `return_notes`, `checkout_on`, `due_on`, `returned_on`, `actor_id`, `returned_actor_id` e `active`. Gli snapshot storici usano `change.operation=context_updated|checked_out|checked_in|verified`. Tutte queste operazioni incrementano la versione e salvano il relativo evento nella stessa transazione della scheda. Una consegna aperta non può essere aggirata attraverso l'editor comune o l'importazione CSV.

## QR locali e collegamenti alle schede

Il formato è un URL assoluto dell'installazione, `/app#inventory?asset_id=ID`. Dopo l'autenticazione l'interfaccia apre la scheda indicata; il frammento non introduce un percorso API anonimo o un'autorizzazione. L'URL contiene soltanto origine, pagina e ID. Le etichette dipendono dall'origine usata durante la generazione: se dominio o porta cambiano vanno ristampate.

`public/qrcodegen.js` è la libreria ufficiale [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library), compilata da TypeScript e distribuita localmente con licenza MIT conservata in testa al file e in `public/qrcodegen.LICENSE.txt`. Il file è vendorizzato senza modifiche e senza dipendenze remote. `public/inventory-qr.js` costruisce l'SVG con API DOM, testo sicuro, correzione d'errore almeno media e quattro moduli di bordo bianco. Espone `InventoryQr.assetUrl`, `createLabel`, `downloadLabel`, `printLabel`.

Il download conserva l'etichetta vettoriale 90 × 50 mm. La stampa usa un contenitore DOM temporaneo e regole `@media print` che nascondono il resto dell'app, con pulizia dopo la stampa. Non richiede iframe o eccezioni alla Content Security Policy e non invia dati a servizi esterni. `tests/test_inventory_qr.py` verifica URL, testo non interpretato come HTML, area di stampa, download e pulizia; su macOS con Swift la decodifica indipendente Vision verifica il QR effettivamente generato.

I campi, i limiti e i percorsi privati `/v1/*` sono documentati nel [manuale dell'addon](../../inventory-addon/README.md). La modifica sostituisce i campi comuni della scheda completa. L'omissione di `asset_type` e `details` conserva i valori correnti per i client precedenti; l'invio di `details` sostituisce i dettagli specifici. `asset_tag` usa un vincolo univoco SQLite `NOCASE`; tale confronto non offre una normalizzazione Unicode generale. `version` controlla la concorrenza e aumenta a ogni modifica della scheda o delle sue relazioni in uscita. `409` segnala etichetta duplicata, versione superata o conflitti con relazioni/capacità, `404` una risorsa inesistente, `400` parametri non validi. Gli errori leggibili dall'utente sono in italiano.

## CSV e controlli sulle scadenze

I percorsi privati corrispondono a `/v1/assets/export`, `/v1/import/preview`, `/v1/import/commit` e `/v1/due`. Il POST di anteprima ammette un form codificato fino a 1 MiB solo sul percorso pubblico esatto; il CSV decodificato resta limitato a 256 KiB e 200 righe. Tutti gli altri form inventory mantengono il limite di 32 KiB. Le risposte restano entro 128 KiB.

L’anteprima restituisce `preview_id` (nullo se non confermabile), `can_commit`, `summary: {total,create,update,errors}` e `items` con riga, codice, nome, tipologia, azione ed errori. Il piano normalizzato è persistente, legato all’autore, valido 15 minuti e monouso; sono ammesse al massimo 64 anteprime aperte per autore. Il commit rivalida le versioni e conferma schede e snapshot in un’unica transazione; un errore annulla tutto il lotto e consuma l’anteprima. Il formato e la protezione dalle formule sono nel [manuale addon](../../inventory-addon/README.md#csv).

`GET /v1/due` usa il giorno locale del servizio se `today=YYYY-MM-DD` è assente e `days_before=30` per impostazione iniziale (1–365). Include date <= oggi + anticipo, senza limite inferiore, per `warranty_until`, `maintenance_due`, `expires_on`, `renewal_on`; esclude `retired` e `lost`. Ogni elemento contiene `asset_id`, `asset_tag`, `name`, `asset_type`, `due_kind`, `due_on`, `days_remaining`. L’ordine è per ID asset decrescente; `limit` conta asset con scadenze e tutte le scadenze dello stesso asset rimangono nella stessa pagina.

La regola applicativa `inventory_due` salva `days_before` e usa il pianificatore esistente in Europe/Rome. Il client acquisisce tutte le pagine prima della transazione SQL, entro limiti di 100 pagine, 10000 eventi e 8 secondi; dati incompleti, non validi o un servizio non disponibile impediscono di consumare l’esecuzione. I vincoli univoci per regola/destinatario deduplicano `inventory_due:<asset_id>:<due_kind>:<due_on>:<days_before>`. Le notifiche espongono `event_kind`, `asset_id`, `due_kind`, `due_on`, `days_before`; `work_date` e `recorded_minutes` sono nulli. Per questa regola tutti gli utenti selezionati ricevono gli avvisi dell’inventario globale.

`GET /v1/operational-due` espone gli eventi `return_due` oppure `verification_due` in base a `kind=return|verification`. Oltre ai campi comuni delle scadenze, ogni evento contiene `source_id`: ID della consegna attiva per una restituzione, ID dell'ultima verifica per una verifica da ripetere, oppure zero per un bene mai verificato. In quest'ultimo caso `due_on` è la data di creazione del bene. La paginazione per asset, i limiti e l'acquisizione completa prima della transazione seguono le stesse garanzie del client delle scadenze.

Le regole `inventory_return` e `inventory_verification` usano il pianificatore giornaliero e il catalogo globale; tutti i destinatari selezionati ricevono gli avvisi. La prima segnala soltanto restituzioni scadute; la seconda include anche i beni mai verificati e salva `verification_days` da 1 a 365, predefinito 90. Le chiavi di deduplicazione sono `inventory_return:<asset_id>:<source_id>:<due_on>` e `inventory_verification:<asset_id>:<source_id>:<due_on>:<verification_days>`. Una nuova consegna o verifica può quindi generare un nuovo avviso, mentre la ripetizione dello stesso controllo non lo duplica. Le notifiche espongono `event_kind`, `asset_id`, `due_kind`, `due_on`, `source_id` e, per la verifica, `verification_days`. La migrazione applicativa 9 → 10 preserva regole, destinatari e notifiche esistenti; non crea regole automaticamente e non modifica il significato di `inventory_due`.

## Tipologie e relazioni

`asset_type` ammette `it_device` (predefinito per le creazioni legacy), `network_device`, `physical`, `software`, `license`. Il form trasporta `details` come stringa contenente un oggetto JSON; le risposte lo espongono come oggetto. La validazione dipende dalla tipologia e rifiuta campi sconosciuti, chiavi duplicate, tipi JSON errati e valori oltre i limiti. IP, MAC, VLAN, quantità e date sono validati dal servizio, anche per richieste private senza interfaccia. Una licenza richiede un riferimento positivo a una scheda `software` e `seats_total` da 1 a 1000000. `license_reference` conserva un riferimento amministrativo, non una chiave di attivazione.

`installed_on` collega software a dispositivi IT o apparati di rete, con quantità 1 e versione installata facoltativa. `connected_to` collega un dispositivo IT o apparato di rete a un apparato di rete diverso dalla sorgente, con quantità 1 e porte facoltative. `licensed_to` consuma posti di una licenza: `per_device` richiede un dispositivo, `per_user` un assegnatario testuale, `concurrent` e `site` esattamente uno dei due. Le relazioni sono dichiarazioni manuali; non vengono interrogati dispositivi, directory o license server.

Le licenze espongono `license_usage: {allocated, available, expired}`. `allocated` somma le quantità delle assegnazioni attive e `available` sottrae tale totale dai posti acquistati. Le assegnazioni oltre capacità o su licenze scadute sono rifiutate; rimuovere una relazione libera i posti. Il servizio rifiuta relazioni duplicate attive e nuovi collegamenti con sorgenti o destinazioni `retired`/`lost`. Non consente di ridurre la capacità sotto le assegnazioni, cambiare tipologia con relazioni attive o cambiare software/modello di una licenza assegnata. Un software referenziato da una scheda licenza deve restare software.

La scadenza è inclusiva: una licenza con `expires_on` resta valida per l'intera data indicata e risulta `expired` dal giorno successivo, calcolato nel fuso `INVENTORY_BUSINESS_TIMEZONE` (predefinito `Europe/Rome`). Una scadenza vuota non impone un limite temporale. La valutazione avviene alla lettura o alla richiesta di una nuova assegnazione, senza processi pianificati; le assegnazioni già registrate restano nel conteggio. `renewal_on` e `renewal_mode` sono informazioni amministrative e non attivano rinnovi automatici. I timestamp degli eventi e delle schede restano in UTC.

Aggiunta e rimozione richiedono la `version` corrente della sorgente. La risposta contiene la relazione e l'asset sorgente alla revisione successiva; la destinazione mantiene la propria revisione. L'elenco include `direction`, `source` e `target` sintetici per rendere consultabili entrambe le direzioni. La rimozione è una disattivazione: lo storico conserva l'identità della relazione.

## Persistenza e storia

Il database separato `inventory.sqlite3` conserva schede, relazioni e `asset_history`. Ogni bene conserva campi comuni, tipologia e dettagli JSON, autore dell'ultima scrittura, versione e date di creazione/aggiornamento. Ogni evento conserva `action` (`created`, `updated`), `actor_id`, versione, data e snapshot JSON completo dopo la scrittura. Gli eventi delle relazioni aggiungono `change`, con `operation` (`relation_added`, `relation_removed`) e una copia immutabile della relazione. Bene, relazione, incremento di versione e storico sono salvati nella stessa transazione: se lo storico non può essere registrato, l'intera modifica viene annullata. Non esiste cancellazione della scheda; la dismissione usa lo stato `retired`.

La migrazione automatica da schema inventory 1 a 2 avviene all'avvio in una transazione, conservando identificativi e dati. Le vecchie schede ricevono tipologia `it_device` e dettagli vuoti; i vecchi snapshot rimangono invariati, quindi i lettori dello storico devono accettare snapshot privi di `asset_type`, `details`, `context` e `change`. Un archivio con versione superiore a quella supportata impedisce l'avvio. La migrazione 2 → 3 aggiunge le anteprime CSV persistenti. La migrazione 3 → 4 aggiunge contesti strutturati, luoghi, consegne e verifiche senza reinterpretare assegnatari e ubicazioni testuali preesistenti. La migrazione 4 → 5 aggiunge persone, identità e collegamenti molti a molti senza modificare le assegnazioni esistenti. Le immagini precedenti non possono aprire lo schema 5.

L'identità autore è un ID dell'app registrato nel database indipendente; non è una foreign key MySQL. I nuovi utenti assegnatari vengono convalidati nel database principale e trasferiti come identità fidate con uno snapshot del nome; gli assegnatari e le posizioni testuali precedenti restano conservati. I dati dell'inventario e gli snapshot sono in chiaro nel volume; il TLS protegge il trasporto.

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

Prima dell’aggiornamento allo schema 5 creare un backup coerente dell’archivio precedente. Un rollback richiede la vecchia immagine e il ripristino di un backup compatibile: non modificare manualmente `PRAGMA user_version` per simulare un downgrade. Schede, relazioni, anteprime, consegne, verifiche, persone e identità aggiunte dopo la migrazione richiedono una copia aggiornata dello schema 5 per essere recuperate.

Per attivare il file ripristinato arrestare l'addon, conservare il vecchio archivio e gli eventuali WAL/SHM, collocare il nuovo database nel percorso configurato senza i vecchi file laterali e verificare proprietà UID/GID `10005:10005` e permessi prima del riavvio. Copiare solo `inventory.sqlite3` durante le scritture può perdere dati. Controllare poi healthcheck, catalogo e storico tramite l'app. Una singola istanza SQLite non eredita quorum, replica o failover di MySQL.

La build nativa è `make -C inventory-addon`. `python3 -m unittest discover -s tests -p 'test_inventory*.py' -v` verifica il contratto HTTPS con certificati e archivi temporanei, dettagli per tipologia, migrazione da uno schema 1 reale, compatibilità legacy, relazioni, allocazioni, autorizzazioni e rollback transazionale. I test del controller verificano la conservazione di CA e token. La suite opzionale `PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v` controlla TLS, autenticazione di servizio, hostname, segmentazione, assenza di egress e credenziali estranee. Queste indicazioni descrivono prove disponibili, non certificano uno stack live non collaudato.

Fonti: `inventory-addon/src/inventory.c`, `inventory-addon/Dockerfile`, `src/inventory_client.c`, `src/server.c`, `compose.yaml`, `scripts/configure_network.py`, `tests/test_network_security.py`.

### Verifica del pacchetto CSV, scadenze e backup

Verificato in modalità nativa con archivi e certificati temporanei: importazione/aggiornamento da CSV, duplicati, versione superata, esportazione filtrata, regola di scadenza, seconda esecuzione senza duplicati e apertura della scheda dalla notifica. Le build Docker di app (backend MySQL) e addon sono riuscite. La suite completa ha eseguito 419 test: 385 superati, 33 opzionali saltati e un reset TCP nel test di rifiuto del login sovradimensionato; quel singolo test è passato alla riesecuzione isolata. Le prove dedicate coprono importazione atomica, pagine complete delle scadenze, indisponibilità del servizio, copie WAL, ripristino, conservazione dei backup e migrazioni. Dopo l’ultima correzione dei riferimenti software nelle licenze CSV, 91 prove inventory sono passate in una nuova esecuzione dedicata; è passata anche l’ulteriore prova di rollback di un’anteprima precedente priva del riferimento verificato. Non è stato aggiornato lo stack attivo né eseguita la migrazione sul cluster MySQL.

### Verifica del cruscotto e degli alert operativi

La suite inventory finale comprende 146 test superati: conteggi sull'intero insieme filtrato, filtri condivisi con CSV, distribuzioni limitate, date e verifiche cronologiche, nuove regole, deduplicazione per verbale, rollback e migrazioni applicative 8/9 → 10. Un cursore diverso dall'ultimo asset della pagina viene rifiutato prima di creare notifiche, per impedire salti silenziosi nel set acquisito. Sono riuscite le build native e Docker dell'app MySQL e dell'addon C; tre controlli statici i18n sono passati. Nel browser, con cinque beni fittizi e archivi temporanei, sono stati verificati filtri sede/reparto, selezione delle card, intervallo di verifica, distribuzioni, creazione/esecuzione delle regole, notifiche e apertura della scheda. Verificati anche layout mobile a 390 px senza overflow orizzontale e nuove voci inglesi; nessun errore o warning in console. Lo stack attivo e il cluster MySQL non sono stati aggiornati.

### Verifica del pacchetto persone e identità

La suite inventory aggiornata supera 175 test, inclusi 29 nuovi casi per anagrafiche separate, cardinalità molti a molti, disattivazione, duplicati, versioni, storico atomico, migrazione 4 → 5 e rollback migratorio, permessi e filtri diretti. Pagine oltre il budget 128 KiB sono attraversate senza perdere o duplicare collegamenti. Build native con `-Werror` e immagini Docker app MySQL/addon riuscite; tre controlli i18n statici superati, prova browser i18n automatica opzionale non eseguita.

Nel browser, su archivi temporanei e dati fittizi, verificati una persona con due identità, un'identità condivisa da due persone, collegamenti diretti al bene, navigazione inversa, filtro del catalogo/cruscotto, storico e recupero dopo codice duplicato. Verificato layout a 390 px senza overflow orizzontale e console senza errori o warning. Nessun rilascio sullo stack attivo e nessuna migrazione dei dati di produzione.
