# Addon Asset Inventory

Il servizio Compose si chiama `inventory` e la sua immagine locale è `punto/inventory:${PUNTO_IMAGE_TAG:-local}`. Per aggiornare un’installazione con i nomi precedenti seguire la [guida nomenclatura Docker](../docs/tehnical/nomi-servizi-e-immagini.md); directory, volumi e identità TLS conservano i nomi già previsti.

`punto-inventory` è un'applicazione C indipendente che conserva il catalogo dei beni, i dettagli per tipologia, le relazioni e lo storico delle modifiche in SQLite. Gestisce dispositivi IT, apparati di rete, beni fisici, software e licenze. L'app principale autentica gli utenti, consente la lettura a tutti gli utenti autenticati e riserva creazioni e modifiche ai Gestori. Censimento, installazioni, collegamenti e assegnazioni delle licenze sono manuali: il servizio non esegue scansioni né rilevamento automatico.

## API privata

Il listener HTTPS è `https://inventory-api:8092`, sulla sola rete interna `inventory_api`. La sorgente ammessa è l'app `172.30.80.10`; l'addon ascolta su `172.30.80.2`. Le richieste `/v1/*` richiedono `Authorization: Bearer <token dedicato>`. `GET /healthz` verifica il servizio senza token, restando limitato alle sorgenti ammesse o al container stesso. Il browser usa il bridge `/api/inventory/*` dell'app.

| Metodo e percorso privato | Uso |
| --- | --- |
| `GET /healthz` | Controllo di salute HTTPS. |
| `GET /v1/status` | Stato del servizio (`ok`, `configured`) e `summary` del catalogo. |
| `GET /v1/dashboard` | Conteggi e distribuzioni del catalogo filtrato. |
| `GET /v1/operational-due` | Eventi paginati su restituzioni scadute e verifiche da ripetere. |
| `GET /v1/assets` | Catalogo con ricerca, stato, tipologia e paginazione. |
| `GET /v1/assets/<id>` | Dettaglio di un bene. |
| `GET /v1/assets/<id>/history` | Storico del bene con snapshot. |
| `POST /v1/assets` | Crea un bene. |
| `POST /v1/assets/<id>` | Aggiorna un bene alla versione indicata. |
| `GET /v1/assets/<id>/relations` | Relazioni in ingresso e in uscita, paginate. |
| `POST /v1/assets/<id>/relations` | Aggiunge una relazione dalla scheda indicata. |
| `POST /v1/assets/<id>/relations/<relation_id>/remove` | Disattiva una relazione in uscita senza cancellarne lo storico. |
| `GET /v1/places` | Elenca sedi, reparti e ubicazioni. |
| `POST /v1/places` | Crea un'anagrafica di luogo. |
| `POST /v1/places/<id>` | Aggiorna o disattiva un'anagrafica alla versione indicata. |
| `POST /v1/assets/<id>/context` | Sostituisce assegnazione strutturata e collocazione. |
| `POST /v1/assets/<id>/checkout` | Registra una consegna a un utente. |
| `POST /v1/assets/<id>/checkin` | Registra la restituzione della consegna aperta. |
| `POST /v1/assets/<id>/verify` | Registra una verifica fisica. |
| `GET /v1/assets/<id>/custody` | Elenca lo storico delle consegne, paginato. |
| `GET /v1/assets/<id>/verifications` | Elenca lo storico delle verifiche, paginato. |

I POST usano `application/x-www-form-urlencoded`. Parametri sconosciuti, ripetuti o non validi sono rifiutati. L'app aggiunge `actor_id` ricavandolo dalla sessione: il browser non può scegliere l'autore. Questa identità è una dichiarazione fidata dell'app, non un account autenticato separatamente dall'addon.

| Campo | Regola |
| --- | --- |
| `asset_tag` | Obbligatorio, massimo 64 byte UTF-8, unico con confronto SQLite `NOCASE`. |
| `name` | Obbligatorio, massimo 160 byte. |
| `category` | Massimo 80 byte, predefinito `hardware`. |
| `serial_number`, `manufacturer`, `model` | Facoltativi, massimo 120 byte ciascuno. |
| `status` | `available`, `assigned`, `maintenance`, `retired` o `lost`; predefinito `available`. |
| `location` | Testo libero, massimo 160 byte. |
| `assignee` | Testo libero, massimo 160 byte; obbligatorio con stato `assigned`. |
| `purchase_date`, `warranty_until` | Facoltativi, date valide `YYYY-MM-DD`; la garanzia non può precedere l'acquisto. |
| `notes` | Facoltative, massimo 4000 byte. |
| `asset_type` | `it_device`, `network_device`, `physical`, `software` o `license`; in creazione predefinito `it_device`. |
| `details` | Oggetto JSON serializzato nel form, massimo 4096 byte, con soli campi della tipologia scelta; non accetta chiavi sconosciute o ripetute. |
| `version` | Richiesta in modifica, intero positivo restituito dal dettaglio corrente. |
| `actor_id` | ID positivo dell'utente, aggiunto dal bridge nelle scritture. |

Una modifica invia i campi comuni della scheda completa, inclusi quelli da svuotare; non è una PATCH. Per compatibilità con i client precedenti, `asset_type` e `details` omessi in modifica conservano i valori correnti. Se `details` viene inviato, sostituisce i dettagli della tipologia. Un `asset_tag` già presente o una versione superata restituiscono `409`. I messaggi di errore sono in italiano. Per dismettere un bene si usa `status=retired`: non è prevista la cancellazione.

### Dettagli per tipologia

I limiti testuali sono in byte UTF-8. I campi sono facoltativi salvo quelli indicati per le licenze; le date usano `YYYY-MM-DD`. VLAN, numero di porte, riferimenti e quantità sono numeri JSON interi, non stringhe o booleani.

| Tipologia | Campi di `details` |
| --- | --- |
| `it_device` | `hostname` (253), `ip_address` (IPv4 o IPv6 valido), `mac_address` (sei coppie esadecimali separate da `:`), `operating_system` (120). |
| `network_device` | Gli stessi campi dei dispositivi IT, più `network_role` (80), `vlan` (1–4094), `port_count` (0–65535). |
| `physical` | `asset_kind` (80), `department` (120), `maintenance_due` (data). |
| `software` | `publisher` (120), `software_version` (80), `edition` (80), `software_kind` (80). |
| `license` | `software_id` obbligatorio (ID di una scheda software), `seats_total` obbligatorio (1–1000000), `license_model` (`per_device`, `per_user`, `concurrent`, `site`; predefinito `per_device`), `expires_on` e `renewal_on` (date), `renewal_mode` (`manual`, `automatic`, `none`; predefinito `manual`), `license_reference` (120). |

`license_reference` è un riferimento amministrativo, per esempio un ordine o un contratto; non è un campo per memorizzare chiavi di attivazione. Il modello e il rinnovo sono informazioni dichiarate: il servizio non rinnova abbonamenti e non consulta sistemi di licenza. Le schede licenza espongono `license_usage` con `allocated`, `available` ed `expired`; la disponibilità è calcolata sulle assegnazioni attive registrate, non sull'utilizzo rilevato dei programmi.

La licenza rimane valida per tutto il giorno indicato in `expires_on`: `expired` diventa vero quando tale data precede il giorno corrente nel fuso `INVENTORY_BUSINESS_TIMEZONE`, predefinito `Europe/Rome`. Senza scadenza, `expired` è falso. Gli eventi e le date di creazione/modifica continuano a usare timestamp UTC. `renewal_on` e `renewal_mode` descrivono il rinnovo, senza eseguirlo.

`GET /v1/assets` accetta `q` (massimo 200 byte UTF-8, cerca anche nei dettagli), `status`, `asset_type`, `limit` da 1 a 100 (predefinito 30) e `after_id`. L'elenco è ordinato per ID decrescente; `after_id` continua con ID inferiori al cursore. La risposta contiene `items`, `next_after_id` e `summary`, con totale e conteggi per stato dell'intero catalogo, indipendenti dai filtri. Le pagine possono contenere meno elementi del limite richiesto per rispettare il budget di risposta di 128 KiB; il cursore permette comunque di proseguire. Il dettaglio restituisce `asset`. Lo storico accetta `limit` (predefinito 30, massimo 100) e `after_id`, usa lo stesso ordine decrescente e restituisce `items` e `next_after_id`: ogni evento contiene `id`, `action` (`created` o `updated`), `actor_id`, `version`, `created_at` e `snapshot` della scheda dopo la scrittura.

### Filtri e cruscotto operativo

Catalogo ed esportazione accettano anche `assignee_user_id`, `site_id`, `department_id` come ID positivi, `attention=open_custody|overdue_returns|never_verified|verification_due|location_mismatch`, `verification_days` da 1 a 365 (predefinito 90) e `today=YYYY-MM-DD` facoltativo. Il bridge pubblico ammette soltanto `mine=1` per l'assegnatario e ricava l'ID dell'utente dalla sessione. I riferimenti di sede e reparto filtrano i contesti strutturati; la ricerca testuale comprende anche i loro nomi e il nome dell'assegnatario.

`GET /v1/dashboard` usa gli stessi filtri, senza `limit` e `after_id`, e restituisce `{ok,today,verification_days,counts,by_status,by_type,by_site,by_department,by_user,groups_truncated}`. `counts` contiene `total`, `open_custody`, `overdue_returns`, `never_verified`, `verification_due`, `location_mismatch`. I totali riguardano tutta la popolazione filtrata. Le tre distribuzioni contengono `{id,name,count}`, ordinate per numero decrescente, al massimo 20 gruppi ciascuna; il gruppo senza riferimento mantiene `id=null` ed è incluso quando presente, anche sostituendo l'ultimo gruppo. `groups_truncated` segnala distribuzioni limitate. Il precedente `summary` del catalogo conserva i conteggi globali indipendenti dai filtri.

I conteggi operativi considerano soltanto `it_device`, `network_device`, `physical` e scartano `retired`/`lost`. Una consegna aperta è in ritardo quando `due_on < today`; una verifica è da ripetere quando la sua data più l'intervallo è `<= today`. I beni privi di verifiche hanno un conteggio separato. L'ultima verifica è quella con `verified_on` maggiore, poi ID maggiore a parità di data; una sua differenza di ubicazione alimenta `location_mismatch`. Queste letture non aggiornano stato, versione o storico dei beni.

### Relazioni e assegnazioni delle licenze

La creazione di una relazione richiede `relation_type`, `version` corrente della scheda sorgente e `actor_id`. Accetta inoltre `target_id`, `assignee` (160 byte), `quantity` (intero 1–1000000, predefinito 1), `installed_version` (80), `source_port` e `target_port` (80 ciascuno), `notes` (2000), secondo le regole della relazione.

| Relazione | Sorgente e destinazione |
| --- | --- |
| `installed_on` | Software → dispositivo IT o apparato di rete. Quantità 1, nessun assegnatario o porta; può indicare la versione installata. |
| `connected_to` | Dispositivo IT o apparato di rete → apparato di rete diverso dalla sorgente. Quantità 1; ammette le due porte. |
| `licensed_to` | Licenza → dispositivo IT o apparato di rete per `per_device`; solo assegnatario testuale per `per_user`; esattamente un dispositivo oppure un assegnatario per `concurrent` e `site`. La quantità consuma i posti disponibili. |

Le relazioni duplicate attive sono rifiutate. Non si possono creare collegamenti con schede dismesse o smarrite, né nuove assegnazioni su una licenza scaduta. Il totale assegnato non può superare `seats_total`, e la capacità non può essere ridotta sotto il già assegnato. Una tipologia non può cambiare finché la scheda ha relazioni attive, e un software referenziato da licenze deve restare software. Modello e software di una licenza non possono cambiare mentre sono presenti assegnazioni attive.

La rimozione richiede `version` corrente della sorgente e `actor_id`, disattiva la relazione e libera i posti della licenza. Aggiunta e rimozione restituiscono `asset` aggiornato e `relation`, incrementando la versione della sola sorgente. Lo storico mantiene `action=updated` e aggiunge `change` con `operation=relation_added` o `relation_removed` e una copia immutabile della relazione. I vecchi eventi non vengono riscritti.

L'elenco contiene le relazioni attive e accetta `limit` e `after_id`, con ordine ID decrescente e budget 128 KiB come gli altri elenchi. Ogni elemento indica `direction` (`incoming` o `outgoing`) e le schede sintetiche `source` e `target`; una destinazione utente usa il testo `assignee`. Le relazioni rimosse restano consultabili tramite lo storico.

Il salvataggio del bene, dell'eventuale relazione e del suo evento storico avviene nella stessa transazione SQLite. Una scrittura non viene confermata senza il relativo snapshot. Le versioni evitano di sovrascrivere un aggiornamento concorrente; dopo un conflitto il chiamante deve rileggere i dati.

## Persone e identità

Schema inventory 5 distingue `inventory_people` (persone reali), `inventory_identities` (utenze censite) e gli utenti autenticati di Punto. Non crea account applicativi e non conserva password, token o chiavi. `inventory_identity_people` rappresenta una relazione molti a molti: una persona può usare più identità e un'identità può essere condivisa da più persone, anche se classificata `personal`. Il tipo è descrittivo.

| Percorsi `/v1` | Operazioni |
| --- | --- |
| `/people`, `/identities` | GET elenco, POST creazione. |
| `/people/<id>`, `/identities/<id>` | GET dettaglio, POST aggiornamento completo con `version`. |
| `/people/<id>/history`, `/identities/<id>/history` | GET storico immutabile con snapshot. |
| `/people/<id>/identities`, `/identities/<id>/people` | GET collegamenti attivi; POST sul secondo collega una persona. |
| `/identities/<id>/people/<link_id>/remove` | POST rimuove logicamente un collegamento. |
| `/assets/<id>/subjects` | GET collegamenti diretti, POST collega persona o identità al bene. |
| `/assets/<id>/subjects/<link_id>/remove` | POST rimuove logicamente il collegamento diretto. |
| `/people/<id>/assets`, `/identities/<id>/assets` | GET beni collegati direttamente, paginati per ID del collegamento. |

Una persona richiede `person_code` (64 byte UTF-8, unico NOCASE) e `display_name` (160); accetta `email` (254), `organization` (160), `job_title` (120), `notes` (2000), `active=0|1`. Un'identità richiede `realm` e `login` (160 ciascuno, coppia unica NOCASE); accetta `display_name` (160), `kind=personal|shared|service`, `notes` (2000), `active=0|1`. Gli aggiornamenti sostituiscono i campi, richiedono `version` e non eliminano la storia. Tutte le scritture richiedono autore fidato.

Il collegamento persona-identità accetta `person_id`, `role=owner|user`, `notes` (2000) e la `version` corrente dell'identità; la rimozione richiede la stessa versione. Entrambe le operazioni incrementano versione e storico di persona e identità nella stessa transazione. Duplicati attivi e versioni superate producono `409`. Disattivare conserva i collegamenti; nuovi collegamenti richiedono entrambi i soggetti attivi, mentre le rimozioni sono consentite anche dopo la disattivazione.

Il collegamento al bene accetta `subject_kind=person|identity`, `subject_id`, `role=owner|user|administrator`, `notes` (2000) e la `version` del bene. Aggiunta e rimozione restituiscono `{ok,asset,link}`, aggiornano soltanto versione e storico del bene, con `change.operation=subject_added|subject_removed` e copia del collegamento. `subject_snapshot` conserva il soggetto al momento del collegamento; `subject` mostra l'anagrafica corrente. Non modifica stato, consegne, posti licenza o diritti di accesso. Non propaga i beni di un'identità alle persone associate. Nuovi collegamenti richiedono soggetto attivo e bene non dismesso/smarrito; la rimozione resta possibile.

Gli elenchi restituiscono `{ok,items,next_after_id}`, con `limit=1..100` (30 predefinito), `after_id`, ordine decrescente e budget 128 KiB. Le directory accettano `q` (200 byte) e `active=0|1`; le identità anche `kind` e `person_id`. Persone espongono `identity_count`, identità `people_count`. Catalogo, CSV e cruscotto accettano `person_id` e `identity_id` positivi, combinabili in AND sui soli collegamenti diretti attivi. Il CSV mantiene il formato delle schede e non importa/esporta le anagrafiche o i collegamenti.

## Assegnazioni strutturate, consegne e verifiche

Lo schema 4 aggiunge anagrafiche `places`, contesto delle schede, consegne e verifiche. Le anagrafiche usano `kind=site|department|location`, `name`, `parent_id`, `active`, `version` e autore fidato. Una sede non ha padre; un reparto può indicare una sede, mentre un'ubicazione deve indicarla. Il tipo non è modificabile. Un cambio di padre su un luogo usato nei contesti correnti restituisce `409`; i riferimenti conservati soltanto nello storico non lo impediscono. Non è prevista eliminazione: disattivare un luogo conserva i riferimenti e i nomi memorizzati nello storico.

Le schede restituiscono `context` con `assignee_user_id`, `assignee_user_name`, `site_id`, `site_name`, `department_id`, `department_name`, `location_id`, `location_name`, oltre alla consegna attiva in `custody` e a `last_verification`; questi campi sono nulli se non ancora registrati. Il contesto è previsto per `it_device`, `network_device` e `physical`; software e licenze conservano i propri dettagli e relazioni. `POST /assets/<id>/context` richiede `version` e sostituisce l'insieme dei riferimenti utente/luoghi. Reparto e ubicazione devono appartenere alla sede indicata. Impostare l'utente porta a `assigned`; rimuoverlo riporta ad `available` un bene prima assegnato. La collocazione rimane indipendente dall'utente.

La consegna usa `version`, `user_id`, `checkout_on`, `due_on`, `condition_out=good|fair|damaged`, note facoltative e autore. Richiede un bene disponibile oppure già assegnato strutturalmente allo stesso utente, sempre senza consegna aperta; assegna il bene all'utente. La restituzione usa `version`, `returned_on`, `condition_in`, note facoltative e `status=available|maintenance`: chiude la consegna, rimuove l'utente e conserva i luoghi. Una scadenza precedente al giorno corrente rende in ritardo una consegna ancora aperta. Le date effettive non possono essere future e `due_on` non può precedere `checkout_on`.

La verifica usa `version`, `verified_on`, `condition=good|fair|damaged`, `observed_location_id` facoltativo, note facoltative e autore. Conserva contesto atteso e osservato; una differenza non sposta automaticamente il bene e `location_matches` è nullo se manca una delle due ubicazioni. Gli elenchi di consegne e verifiche usano ID decrescenti, `limit` da 1 a 100 e `after_id`. Una consegna mantiene note iniziali in `notes`, note finali in `return_notes` e gli autori in `actor_id`/`returned_actor_id`, oltre alle date e al flag `active`. Lo storico delle schede registra `change.operation=context_updated|checked_out|checked_in|verified`. Ogni operazione controlla la revisione e salva scheda, evento e storico nella stessa transazione.

Il servizio non interroga il database principale: l'app convalida gli utenti attivi, risolve i nomi e trasmette questa identità fidata, come già avviene per l'autore. Il browser non può fornire nomi utente o autore arbitrari. Una successiva disattivazione dell'utente non riscrive gli snapshot. Il bridge espone ai soli Gestori `GET /api/inventory/users` con `id`, `name`, `email`. Il filtro pubblico `mine=1` per catalogo ed esportazione diventa il riferimento dell'utente autenticato; il catalogo rimane globale. Le vecchie stringhe `assignee` e `location` non vengono convertite automaticamente e CSV/editor generale non possono aggirare il contesto o una consegna aperta.

### Etichette QR

L'app genera un SVG 90 × 50 mm con codice, nome e URL assoluto `/app#inventory?asset_id=ID`. La scansione richiede l'autenticazione prima di aprire la scheda. L'URL incorpora dominio e porta correnti: generare le etichette dall'indirizzo definitivo raggiungibile dai dispositivi e ristamparle se l'origine cambia.

Il generatore [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library) è vendorizzato in `public/qrcodegen.js` con licenza MIT e provenienza in `public/qrcodegen.LICENSE.txt`. `public/inventory-qr.js` costruisce SVG tramite DOM senza HTML derivato da dati, usa correzione d'errore almeno media e mantiene quattro moduli bianchi di bordo. Download e stampa operano localmente senza CDN o servizio esterno; la stampa isola l'etichetta con CSS senza modificare la CSP. Conservare contrasto e bordo bianco durante stampa o ridimensionamento.

## CSV

`GET /v1/assets/export` usa gli stessi filtri e cursori del catalogo e restituisce `{ok,csv,exported,next_after_id}`. Ogni pagina include l’intestazione; concatenando le pagine mantenerla soltanto dalla prima. Le 16 colonne sono `id`, `asset_tag`, `name`, `category`, `serial_number`, `manufacturer`, `model`, `status`, `location`, `assignee`, `purchase_date`, `warranty_until`, `notes`, `asset_type`, `details`, `version`. Tutti i campi sono quotati; `details` è JSON. Non esporta né importa relazioni.

`POST /v1/import/preview` accetta `csv`, `mode=create_only|upsert` (predefinito create_only) e autore fidato. CSV UTF-8, BOM facoltativo, separatore virgola, LF/CRLF, campi multilinea quotati e virgolette raddoppiate secondo RFC4180; massimo 256 KiB e 200 righe. Sono obbligatorie le colonne `asset_tag` e `name`. Intestazioni sconosciute/duplicate, tag ripetuti, campi invalidi e conflitti vengono segnalati per riga. `upsert` risolve per tag e richiede la versione corrente per aggiornare; un `id` appartenente a un altro tag è rifiutato. Le colonne omesse conservano i valori esistenti; celle vuote esplicite svuotano i campi facoltativi. `details` richiede un oggetto JSON (`{}` per svuotare i dettagli facoltativi), mentre tipologia, stato e versione di aggiornamento devono restare validi. I riferimenti `software_id` delle licenze devono appartenere a schede software già persistenti all’inizio dell’anteprima: non possono usare ID temporanei di software creati nello stesso lotto. Gli ID fanno riferimento all’archivio locale, quindi verificare le licenze importate da un inventario diverso.

L’anteprima esegue la stessa validazione del salvataggio senza persistere schede o storico. Restituisce `summary`, `items`, `can_commit` e `preview_id` di 32 cifre esadecimali, legato all’autore, monouso, valido 15 minuti (massimo 64 piani aperti per autore). `POST /v1/import/commit` accetta l’ID e l’autore fidato: rivalida le versioni e salva tutto il lotto insieme con uno snapshot per riga. Anche un conflitto consuma l’anteprima e non lascia aggiornamenti parziali.

Per proteggere l’apertura nei fogli di calcolo, l’esportazione antepone un apostrofo ai valori che iniziano con un apostrofo o il cui primo carattere non spazio ASCII/tab/CR/LF è `=`, `+`, `-`, `@`. L’importazione elimina esattamente un apostrofo se il resto soddisfa questa condizione. La convenzione consente il roundtrip delle esportazioni; in file manuali raddoppiare l’apostrofo letterale davanti a tali prefissi.

## Scadenze

`GET /v1/due?today=YYYY-MM-DD&days_before=30&limit=100&after_id=...` restituisce `{ok,items,next_after_id}`. Oggi è facoltativo e segue il fuso aziendale; l’anticipo va da 1 a 365 giorni. Include garanzia, manutenzione, scadenza e rinnovo fino alla soglia, comprese date già superate; esclude schede dismesse o smarrite. I campi sono `asset_id`, `asset_tag`, `name`, `asset_type`, `due_kind` (nome del campo), `due_on`, `days_remaining`. Il limite conta asset e tutte le scadenze di un asset restano nella stessa pagina. Il servizio espone i dati; il pianificatore e la deduplicazione degli avvisi sono nell’app principale.

`GET /v1/operational-due?kind=return|verification&verification_days=90&today=YYYY-MM-DD&limit=100&after_id=...` restituisce lo stesso involucro paginato. `kind` è obbligatorio; data, intervallo e paginazione sono facoltativi, con gli stessi limiti del catalogo. Gli elementi includono i campi delle scadenze e `source_id`. Per `return`, `due_kind=return_due`, `source_id` identifica la consegna aperta e `due_on` è la restituzione prevista, strettamente precedente al giorno richiesto. Per `verification`, `due_kind=verification_due`, `source_id` identifica l'ultima verifica e `due_on` è la sua data più l'intervallo; per beni mai verificati `source_id=0` e `due_on` è la data di creazione. Quest'ultimo controllo include entrambi i casi. Beni non materiali, dismessi e smarriti sono esclusi. Il servizio non crea regole: l'app principale configura `inventory_return` e `inventory_verification`, acquisisce tutte le pagine e deduplica gli avvisi includendo la consegna o verifica di origine.

## Avvio e configurazione

Dalla radice del progetto, per aggiungere il servizio a un'installazione Docker già configurata:

```sh
python3 scripts/configure_network.py --env-file .env.docker prepare
docker compose --env-file .env.docker build inventory app
docker compose --env-file .env.docker up -d inventory app
```

Per una nuova installazione seguire prima l'avvio del cluster nel [README principale](../README.md). `prepare` estende la CA esistente con il certificato inventory mancante e crea `data/addon-secrets/inventory-api-token` una sola volta. Non sostituisce CA, certificati o token preesistenti; un materiale incompleto o non valido richiede il ripristino dell'originale.

| Variabile addon | Valore Compose |
| --- | --- |
| `INVENTORY_BIND` | `172.30.80.2` |
| `INVENTORY_PORT` | `8092` |
| `INVENTORY_ALLOWED_CLIENT` | `172.30.80.10` |
| `INVENTORY_API_TOKEN_FILE` | `/run/secrets/inventory-api-token` |
| `INVENTORY_TLS_CERT_FILE` | `/etc/punto/tls/inventory-addon.crt` |
| `INVENTORY_TLS_KEY_FILE` | `/etc/punto/tls/inventory-addon.key` |
| `INVENTORY_DB_PATH` | `/var/lib/punto-inventory/inventory.sqlite3` |
| `INVENTORY_BUSINESS_TIMEZONE` | `Europe/Rome` (predefinito del servizio; fuso per il giorno di scadenza delle licenze). |
| `INVENTORY_BACKUP_DIR` | `/var/backups/punto-inventory`; abilita le copie automatiche. Assente/vuota in modalità nativa: backup disabilitato. |
| `INVENTORY_BACKUP_INTERVAL_HOURS` | `24`, configurabile da 1 a 168 ore. |
| `INVENTORY_BACKUP_KEEP` | `14`, configurabile da 1 a 365 copie verificate. |

L'app usa `INVENTORY_API_URL=https://inventory-api:8092`, lo stesso token e `INVENTORY_API_CA_FILE=/etc/punto/tls/ca.crt`. In modalità nativa il modulo è facoltativo: lasciare tutte e tre le variabili dell'app vuote per disabilitarlo. Per abilitarlo configurarle insieme, con HTTPS e CA verificata; sono ammessi i nomi interni sulla porta 8092 oppure `localhost`/`127.0.0.1` con porta esplicita. Per avviare l'addon nativamente impostare bind e client ammesso sul loopback, percorsi TLS/token esistenti e un percorso SQLite scrivibile. Non è prevista una modalità HTTP senza TLS.

## Persistenza e isolamento

Il volume dedicato `inventory_addon_data` contiene `inventory.sqlite3` e gli eventuali file SQLite WAL/SHM. L'addon usa UID/GID `10005:10005`, filesystem radice in sola lettura, capacità Linux rimosse e nessuna porta pubblicata sull'host. È collegato soltanto a `inventory_api` (`172.30.80.0/24`, rete `internal` con gateway `isolated`), senza `app_egress`, accesso a Squid o reti e credenziali MySQL/MongoDB. Solo app e addon ricevono il token inventory.

I dati e gli snapshot sono memorizzati in chiaro nel volume; TLS protegge il trasporto. Il backup MySQL e il backup documentale non includono l'inventario. L'addon è una singola istanza e non eredita l'HA del cluster MySQL. Non cancellare il volume per aggiornare l'immagine.

### Backup automatici e ripristino verificato

Compose abilita una copia SQLite coerente all'avvio iniziale, poi ogni 24 ore, nel volume separato `inventory_addon_backups`, conservando le ultime 14 copie verificate. Un riavvio recupera l'ultima copia integra e la sua scadenza, senza produrre un duplicato a ogni avvio. La directory appartiene a UID/GID `10005:10005`; i file sono creati con permessi `0600`. La copia usa l'API SQLite Backup, comprende le scritture confermate nel WAL, supera `PRAGMA integrity_check` ed è pubblicata con rinomina atomica. La rotazione interessa soltanto file regolari verificati con il nome del servizio; non segue collegamenti simbolici e non rimuove file estranei o copie corrotte.

`GET /v1/status` include `backup` con abilitazione, intervallo, conservazione, ultimo tentativo, ultima riuscita, nome della copia, prossima scadenza ed eventuale errore. Gli orari sono UTC e il percorso host non viene esposto. Un errore di backup mantiene disponibile l'inventario, viene registrato nei log e rinvia il tentativo per almeno cinque minuti. Le copie locali devono essere trasferite anche su un supporto esterno per proteggersi dalla perdita dell'host; conservare token e materiali TLS separatamente.

Il programma di supporto richiede Python con SQLite e opera su file accessibili all'amministratore:

```sh
python3 scripts/inventory_backup.py create /percorso/inventory.sqlite3 /percorso/backup-manuale.sqlite3
python3 scripts/inventory_backup.py verify /percorso/backup-manuale.sqlite3
python3 scripts/inventory_backup.py restore /percorso/backup-manuale.sqlite3 /percorso/inventory-ripristinato.sqlite3
```

`create` è una copia online coerente; `restore` produce un nuovo database verificato e non sovrascrive mai un file esistente. Per attivare un ripristino, arrestare l'addon, conservare l'archivio precedente e gli eventuali WAL/SHM, collocare il nuovo file come `inventory.sqlite3` in un archivio senza i vecchi WAL/SHM, quindi ripristinare proprietà UID/GID `10005:10005` prima del riavvio. Verificare catalogo e storico. Non copiare soltanto il database principale mentre il servizio scrive.

Lo schema inventory è alla versione 5. All'avvio l'addon migra automaticamente un archivio versione 1, conservando ID, schede, autore e storico: le schede esistenti diventano `it_device` con dettagli vuoti. Gli snapshot precedenti restano nel formato originale; i nuovi contengono tipologia e dettagli. Eseguire un backup coerente prima dell'aggiornamento. La migrazione è transazionale; un database con versione futura viene rifiutato. La migrazione 2 → 3 aggiunge le anteprime CSV persistenti e la migrazione 3 → 4 aggiunge anagrafiche, contesti, consegne e verifiche senza reinterpretare i testi precedenti. La migrazione 4 → 5 aggiunge persone, identità, legami molti a molti, storico delle anagrafiche e collegamenti diretti ai beni. Per tornare a un’immagine precedente occorre ripristinare un backup compatibile: non basta cambiare `PRAGMA user_version`.

## Compilazione e verifiche

```sh
make -C inventory-addon
python3 -m unittest discover -s tests -p 'test_inventory*.py' -v
python3 -m unittest discover -s tests -p 'test_cluster_manager.py' -v
```

Dipendenze native: compilatore C11, pkg-config, OpenSSL, SQLite, json-c e un database dei fusi orari aggiornato. L'immagine Docker usa Debian, include `tzdata` per i fusi IANA e avvia il servizio con un utente non privilegiato. I test funzionali usano archivi e certificati temporanei. La suite di rete è separata e facoltativa, contro uno stack già avviato:

```sh
PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v
```

Ulteriori riferimenti: [guida operativa](../docs/prod/asset-inventory.md), [architettura e contratto pubblico](../docs/tehnical/asset-inventory.md).
