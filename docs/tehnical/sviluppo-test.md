# Sviluppo e test

**Italiano** | [English](../en/tehnical/sviluppo-test.md)

Questa guida descrive strumenti e test presenti l’**11 ottobre 2026**. I comandi sono da eseguire dalla radice del repository. I risultati del nuovo modulo sono nel [verbale attività dell'11 ottobre](verifica-attivita-2026-10-11.md); le istruzioni qui sotto non attestano da sole l'esecuzione di una prova. Il [README principale](../../README.md) contiene il contesto del progetto; [architettura](architettura.md) descrive i componenti.

## Toolchain e backend

Il [Makefile principale](../../Makefile) compila C11 con `-Wall -Wextra -Wpedantic -Werror`, oltre a `-O2` salvo override. Le segnalazioni del compilatore sono trattate come errori; non disabilitarle per far passare una modifica.

| Target | Dipendenze principali | Risultato |
| --- | --- | --- |
| App SQLite | Compilatore C11, make, `pkg-config`, SQLite, libcurl, json-c; Security/CommonCrypto su macOS oppure OpenSSL su Linux | `build/sqlite/punto`, copiato atomicamente in `build/punto` |
| App MySQL | Dipendenze C comuni, MariaDB Connector/C (`libmariadb`), MongoDB C Driver (`libmongoc-1.0`), OpenSSL | `build/mysql/punto`, copiato in `build/punto` |
| Addon email | OpenSSL, libcurl, SQLite, compilatore C11 | `addon/build/punto-mailer` |
| Addon Calendar | OpenSSL, libcurl, SQLite, json-c, `pkg-config`, pthread | `calendar-addon/build/punto-calendar` |
| Suite Python | Python 3, binari e librerie di compilazione, OpenSSL disponibile nel PATH, socket locali | Fixture e database temporanei |
| Privacy NPM | Node.js; Docker soltanto per il caso runtime opt-in | Verifica della trasformazione delle configurazioni Nginx |

Il frontend viene servito direttamente da `public/` e non richiede un'installazione npm. Node serve per gli strumenti NPM e per i relativi test, non per compilare l'interfaccia dell'app C.

```sh
make
make run
make DB_BACKEND=mysql
make -C addon
make -C calendar-addon
```

`make run` eredita le variabili esportate nella shell e non carica `.env` automaticamente. Non usare le credenziali del deployment nei database di fixture. La compilazione MySQL nativa non avvia MySQL o MongoDB; l'ambiente va configurato separatamente. La procedura Docker usa il [Dockerfile principale](../../Dockerfile), che compila il backend MySQL in uno stadio builder e copia il binario nello stadio runtime.

`make clean` elimina la directory `build/` dell'app. Gli addon hanno ciascuno un target `clean` nella propria directory. I database, i PDF e i volumi Docker non sono output di compilazione e non vanno rimossi per risolvere un errore del compilatore.

## Suite ordinaria

```sh
make test
```

Il target compila l'app e avvia `python3 -m unittest discover -s tests -v`. Le suite locali usano porte libere, database e directory temporanei. I test SMTP e Calendar usano provider HTTPS/SMTP e proxy CONNECT simulati su loopback: non inviano email né modificano calendari reali.

I test opt-in sul deployment vengono saltati se le rispettive variabili non sono presenti. La dicitura `skipped` indica una prova non eseguita e non equivale a un risultato positivo. Per leggere un risultato correttamente conservare sempre conteggio dei test eseguiti, superati, saltati e falliti.

Per eseguire una famiglia specifica:

```sh
python3 -m unittest discover -s tests -p 'test_reports.py' -v
python3 -m unittest discover -s tests -p 'test_work_board.py' -v
python3 -m unittest discover -s tests -p 'test_calendar_addon.py' -v
python3 -m unittest discover -s tests -p 'test_calendar_client.py' -v
```

Le due famiglie Calendar possono compilare binari e generare certificati di fixture; un ambiente che blocca i socket locali impedisce questi test anche se la logica applicativa è corretta. Non sostituire una fixture TLS con un fornitore reale per superare la limitazione.

## Mappa delle verifiche

| Famiglia | File principali | Copertura |
| --- | --- | --- |
| HTTP e autenticazione | [test_server.py](../../tests/test_server.py), [test_roles.py](../../tests/test_roles.py), [test_public_port.py](../../tests/test_public_port.py), [test_reverse_proxy.py](../../tests/test_reverse_proxy.py) | Parsing, sessioni, profili, Host/Origin, porte pubbliche e comportamento dietro proxy |
| Commesse e ore | [test_workspace.py](../../tests/test_workspace.py), [test_assignments.py](../../tests/test_assignments.py), [test_job_defaults.py](../../tests/test_job_defaults.py) | Stati, dati, permessi, assegnazioni, commesse comuni e storico |
| Attività, Kanban e blocchi | [test_work_board.py](../../tests/test_work_board.py) | Permessi diretti/gruppi/comuni, revoche, assegnatari, gerarchie, revisioni obsolete, validazione Unicode e limite HTTP dedicato, audit atomico, cancellazioni e migrazione 7→8 |
| Organizzazione | [test_groups.py](../../tests/test_groups.py) | Albero, appartenenze, ruoli, abilitazioni, cicli, intervalli e migrazione |
| Contratti e PDF | [test_procurement.py](../../tests/test_procurement.py), [test_documents_storage.py](../../tests/test_documents_storage.py), [test_documents_backup.py](../../tests/test_documents_backup.py) | Fornitori, contratti, relazioni, upload/download, storage e comportamento del backup |
| Report e tariffe | [test_reports.py](../../tests/test_reports.py), [test_labor_rates.py](../../tests/test_labor_rates.py) | Filtri, aggregati, visibilità, costi, arrotondamenti e tariffe temporali |
| Alert | [test_alerts.py](../../tests/test_alerts.py) | Regole, soglie, 48 ore, giorni lavorativi, destinatari, orari, ripartenza e cambio d'ora |
| Email | [test_mail_addon.py](../../tests/test_mail_addon.py), [test_mail_client.py](../../tests/test_mail_client.py) | SMTP TLS, proxy, API, ledger, esiti incerti, revoche, outbox e migrazione |
| Calendar | [test_calendar_addon.py](../../tests/test_calendar_addon.py), [test_calendar_client.py](../../tests/test_calendar_client.py) | OAuth, isolamento utente, cifratura, paging, sincronizzazione, ETag, idempotenza, timeout e riconnessione |
| Distribuzione | [test_deployment.py](../../tests/test_deployment.py), [test_cluster_manager.py](../../tests/test_cluster_manager.py) | Configurazioni, procedure del controller e salvaguardie |
| Privacy proxy | [test_proxy_privacy.py](../../tests/test_proxy_privacy.py) | Conservazione di ACL/header e rimozione dei segreti OAuth dai log |

La suite attività/blocchi usa esclusivamente un server HTTP e database SQLite temporanei. Il Kanban è una presentazione degli stati delle attività: i test HTTP coprono i valori e i permessi, mentre elenco, colonne, dialog e tastiera richiedono anche il collaudo della UI. La presenza dei test non certifica un deployment MySQL aggiornato.

Il C di supporto in `tests/` verifica anche comportamento documentale, pianificatore alert e TLS del driver. I test di integrazione con database reali completano, senza sostituire, le prove SQLite.

## Integrazione su stack MySQL/GridFS

Questi test scrivono dati sintetici su un ambiente già avviato. Preparare un deployment di collaudo e un gestore dedicato. Le credenziali arrivano dall'ambiente `PUNTO_MYSQL_TEST_EMAIL` / `PUNTO_MYSQL_TEST_PASSWORD`, con fallback su `PUNTO_EMAIL` / `PUNTO_PASSWORD`; le suite non caricano automaticamente `.env.docker` e non assumono un account dimostrativo.

Dopo aver fornito le credenziali con un metodo protetto:

```sh
PUNTO_MYSQL_TEST_URL=http://localhost python3 tests/test_mysql.py -v
PUNTO_PROCUREMENT_TEST_URL=http://localhost python3 tests/test_procurement_mysql.py -v
PUNTO_GROUPS_TEST_URL=http://localhost python3 tests/test_groups_mysql.py -v
```

Gli URL devono essere origini HTTP(S) senza credenziali. Le fixture hanno nomi univoci, spesso con prefisso `codex-test-`. Queste suite non svuotano tabelle o rimuovono account preesistenti. Le prove SQL e approvvigionamento conservano dati di verifica; gli operatori creati da prove pertinenti vengono disattivati nella pulizia. Eseguirle su dati operativi lascia quindi tracce e audit: non sono equivalenti a una lettura di salute.

`PUNTO_MYSQL_TEST_MANIFEST` e `PUNTO_PROCUREMENT_TEST_MANIFEST` consentono di salvare ID e hash delle fixture senza password, per verifiche successive di persistenza. Un manifest non è un backup.

## Rete e isolamento

Su uno stack locale attivo e con materiali TLS corretti:

```sh
PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v
```

La suite controlla le sette reti, IP/interfacce, porte pubblicate, accesso all'app tramite NPM, listener privati, flussi vietati verso database e addon, impossibilità di uscita diretta, policy Squid, TLS e separazione dei secret. Una prova esegue anche HTTPS verso un host Google autorizzato; non crea un evento e non richiede credenziali OAuth. La raggiungibilità della rete esterna può influenzarla.

Il codice consente `PUNTO_NETWORK_TEST_ALLOWED_HOST` per scegliere un host autorizzato e `PUNTO_NETWORK_TEST_EGRESS_IP` per la prova di uscita diretta. Usare valori coerenti con il laboratorio, senza trasformare il test in una modifica delle policy di produzione. Le opzioni del contesto Docker sono lette dagli helper di cluster; controllare il contesto prima di eseguire prove sul daemon.

Per verificare la specifica ACL Squid sulle risposte DNS private senza cambiare lo stack:

```sh
PUNTO_SQUID_POLICY_TEST=1 python3 tests/test_squid_policy.py -v
```

Questa prova usa l'immagine Squid in un container temporaneo `--network none`, con filesystem in sola lettura e senza credenziali. Adatta il listener al loopback e costruisce una risposta DNS locale controllata. È un test della policy, non una connessione a un provider.

## TLS del driver MySQL

```sh
PUNTO_MYSQL_TLS_TEST=1 python3 tests/test_mysql_tls.py -v
```

Il test compila ed esegue un programma di verifica C nativo sul loopback, insieme al driver e a MariaDB Connector/C. È indipendente da Docker e dai database reali e non usa credenziali d’ambiente o CA di produzione. Verifica otto casi di rifiuto TLS, inclusi errori di configurazione e tentativi di downgrade. Come alternativa manuale, il programma può essere compilato ed eseguito in un container con `--network none`, come indicato nella docstring del test. Non collegarlo a un database operativo per simulare certificati errati. Un fallimento di identità va corretto nel certificato o nella destinazione, non impostando una modalità TLS più debole.

## Privacy di Nginx Proxy Manager

La parte Node del test verifica le trasformazioni delle configurazioni generate. Se `node` non è nel PATH, `PUNTO_NODE_BINARY` può indicare un eseguibile Node disponibile nell'ambiente. In assenza di Node i casi pertinenti vengono saltati.

Per abilitare anche la prova runtime Nginx isolata:

```sh
PUNTO_DOCKER_TESTS=1 python3 tests/test_proxy_privacy.py -v
```

`PUNTO_PROXY_IMAGE` permette di scegliere l'immagine NPM compilata da questo repository; il nome predefinito del test presuppone il prefisso Compose locale previsto. Quando il nome del progetto è diverso, impostare il nome effettivo dell'immagine. La prova avvia un container temporaneo, verifica richieste sintetiche con query sensibili e controlla i log; non modifica NPM in esecuzione.

La patch dipende dalla versione NPM fissata. Prima di aggiornare quell'immagine, verificare sia la trasformazione statica sia il test runtime, inclusi ACL, autenticazione e configurazioni personalizzate rifiutate.

## Prove di alta disponibilità

La suite seguente è **distruttiva rispetto alla disponibilità temporanea**, pur prevedendo il ripristino nel blocco di pulizia. Va usata soltanto su un cluster di sviluppo, dopo backup e con autorizzazione alla finestra di interruzione:

```sh
PUNTO_CLUSTER_HA_TEST=1 python3 tests/test_cluster_ha.py -v
```

Legge il file di configurazione locale, arresta un membro, sospende due membri e verifica recupero e riavvio completo. `PUNTO_CLUSTER_ENV_FILE` può indicare un altro file. Durante la perdita di quorum l'app deve rifiutare scritture e può restituire `503`. La procedura finale tenta di ripristinare i servizi, ma dopo una prova interrotta occorre comunque verificarne lo stato.

Il risultato corretto comprende scritture possibili con due membri, blocco con un membro isolato, rientro dei nodi e conservazione dei dati. `make test` senza la variabile di abilitazione non esegue queste prove. Non forzare la scrivibilità di un membro per ottenere un esito positivo.

## Collaudo dell'interfaccia

Per modifiche al frontend verificare almeno accesso, navigazione, messaggi di errore, layout mobile e desktop, tastiera e il flusso completo interessato con profilo gestore e operatore. Le risposte API vanno controllate anche quando l'interfaccia nasconde un'azione: il test dei permessi è lato server.

Usare dati sintetici in un ambiente dedicato per operazioni che generano notifiche, cancellano commesse/ore o modificano abilitazioni. Conservare le evidenze senza credenziali, dati personali o token OAuth. Il [verbale storico UI del 10 ottobre 2026](verifica-ui-2026-10-10.md) documenta un collaudo precedente; non sostituisce una verifica successiva quando cambia il codice.

Calendar attualmente espone API e non una nuova pagina calendario; non dichiarare il flusso grafico evento–commessa verificato. SMTP e Google reali restano da configurare: i test con provider simulati non attestano il consenso o l'invio reale presso un account esterno.

## Procedura per una modifica

1. Identificare il modulo e il contratto interessati, consultando [modello dati](modello-dati.md), [API app](api-app.md) e [API addon](api-addon.md).
2. Preservare la separazione fra SQLite nativo e MySQL Docker. Se cambia lo schema, predisporre migrazione e verifiche di conservazione dei dati; non modificare retroattivamente un database già distribuito senza un percorso di aggiornamento.
3. Aggiornare controlli server, errori e frontend in modo coerente. Non fidarsi di ID utente provenienti dal browser per decidere accessi personali.
4. Eseguire la compilazione con warning come errori e i test che verificano il comportamento cambiato. Ampliare la verifica quando il cambiamento attraversa più moduli o introduce una migrazione.
5. Se cambiano immagini, TLS o reti, compilare le immagini interessate e ripetere le prove specifiche in un laboratorio; distinguere queste prove dai test unitari.
6. Aggiornare la documentazione del contratto, della configurazione e delle operazioni. Riportare cosa è passato e cosa è stato saltato o resta da configurare.
7. Prima di un deploy, salvare il backup appropriato e ricreare i soli servizi necessari seguendo [installazione](installazione-configurazione.md).

Non inserire nei commit `.env.docker`, volumi, dump, file in `data/`, certificati, chiavi, token, log o manifest contenenti dati sensibili. I `.gitignore` e `.dockerignore` riducono l'esposizione accidentale; la verifica della modifica rimane necessaria.

## Risultati storici disponibili

Il [README principale](../../README.md) registra un collaudo del **10 ottobre 2026**, dopo l'aggiunta Calendar:

- Suite completa: **270 test**, dei quali **241 superati e 29 facoltativi saltati**.
- Dopo le ultime correzioni: **23 test addon Calendar**, **15 test bridge Calendar** e **8 test privacy NPM** superati.
- Prova Squid isolata superata e **20/20 controlli live di rete** superati.
- Dieci servizi sani; tre membri SQL `ONLINE` / `PRIMARY`, schema 7; impronte delle otto tabelle protette invariate nel collaudo.

Questi sono risultati registrati in precedenza, non una nuova esecuzione effettuata durante la scrittura dei manuali. Le prove Calendar impiegavano un provider simulato e OAuth Google non era configurato. Un conteggio storico va aggiornato soltanto dopo una nuova esecuzione osservata e documentata.
