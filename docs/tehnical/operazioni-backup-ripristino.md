# Operazioni, backup e ripristino

**Italiano** | [English](../en/tehnical/operazioni-backup-ripristino.md)

Questa guida descrive le procedure disponibili nel repository al **10 ottobre 2026**. Non esegue né promette un ripristino automatico completo: gli script presenti coprono inizializzazione, stato, dump e recupero del gruppo SQL. Consultare [installazione](installazione-configurazione.md), [architettura](architettura.md) e [README principale](../../README.md) per il contesto.

## Controlli ordinari

Dalla radice del progetto:

```sh
python3 scripts/manage_cluster.py --env-file .env.docker status
docker compose --env-file .env.docker ps
curl --fail http://localhost/healthz
docker compose --env-file .env.docker logs --tail=100 app proxy-db mysql-1 mysql-2 mysql-3
```

Adattare dominio e porta se diversi. `status` riporta nodo, UUID, stato, ruolo, sola lettura e impostazioni degli ID. Lo stato normale del gruppo è tre membri `ONLINE` / `PRIMARY`; tutti i servizi Compose devono raggiungere il proprio health check.

| Controllo | Cosa dimostra | Cosa non dimostra |
| --- | --- | --- |
| `/healthz` app | Server HTTP e query SQL disponibili, `database: mysql` o `sqlite` | Disponibilità dei PDF o dei fornitori esterni |
| Health MySQL | Processo SQL raggiungibile | Appartenenza al quorum e scrivibilità multi-primary |
| Health `proxy-db` | Almeno due endpoint di monitor pronti | Assenza di errori futuri o integrità completa dei dati |
| Health MongoDB | TLS, account applicativo e servizio documentale disponibili | Coerenza di ogni riferimento SQL/GridFS |
| Health Squid | Listener e risposta locale prevista | Google o SMTP raggiungibili |
| Health addon | Processo HTTPS locale disponibile | Credenziali Google/SMTP valide e consegna esterna |
| Stato addon | Configurazione locale presente e stato della coda | Consegna nella casella email o completamento di tutte le operazioni remote |

I log degli addon e di Squid usano rotazione Docker a tre file da 10 MB. La protezione dei log NPM esclude le informazioni OAuth sensibili. Non allegare dump, file env o output completo di configurazioni risolte durante la diagnosi.

## Arresto e riavvio

Per interrompere lo stack conservando i volumi:

```sh
docker compose --env-file .env.docker down
```

Questo comando rimuove container e reti del progetto, ma conserva volumi nominati e bind mount. L'opzione `--volumes`/`-v` eliminerebbe i volumi: non fa parte di un normale riavvio o aggiornamento.

Dopo l'arresto di **tutti e tre** i membri SQL, usare:

```sh
python3 scripts/manage_cluster.py --env-file .env.docker recover
python3 scripts/manage_cluster.py --env-file .env.docker status
docker compose --env-file .env.docker ps
```

Se esiste già un gruppo attivo, `recover` fa rientrare i nodi offline senza creare un gruppo concorrente. Se nessun nodo è online, arresta i canali del gruppo, confronta GTID eseguiti e certificati e sceglie un membro il cui insieme contiene quello degli altri. Non sceglie automaticamente il primo nodo (`mysql-1`, identità interna `db`). Se gli insiemi divergono, si ferma senza forzare quorum o cancellare dati.

`recover` recupera **i volumi esistenti**. Non legge un dump SQL, non ricrea i PDF da backup, non ripristina secret e non risolve la perdita totale del disco. `data/cluster-state.json` deve corrispondere al cluster esistente; non va ricreato manualmente inventando UUID o marcatori.

## Guasto di un singolo membro

Con due membri online il cluster può continuare a servire l'app. Un nodo riavviato tenta il rientro e il recupero distribuito. Per un rientro ordinario di un servizio fermo:

```sh
docker compose --env-file .env.docker start mysql-2
python3 scripts/manage_cluster.py --env-file .env.docker status
```

Controllare il suo ritorno `ONLINE`, gli altri membri e la disponibilità dell'app. Non cancellare il volume del nodo come primo tentativo. Se il rientro non riesce, verificare TLS, configurazione, account di replica e GTID prima di progettare un nuovo seed.

Con due nodi indisponibili, l'indisponibilità delle scritture è il comportamento di protezione atteso. Non forzare la scrivibilità del membro isolato: ripristinare il quorum oppure seguire una procedura di recupero verificata. Le prove di guasto della suite HA sono descritte in [sviluppo e test](sviluppo-test.md) e causano interruzioni intenzionali.

## Inventario necessario al backup

| Elemento | Contenuto | Dipendenze di ripristino |
| --- | --- | --- |
| Dump MySQL | Database applicativo, schema, righe, routine, trigger ed eventi inclusi dal dump | Versione applicativa/schema e credenziali ricostruite o conservate |
| Archivio MongoDB | Database documentale, collezioni GridFS e metadati | Metadati documentali SQL dello stesso punto logico |
| `calendar_addon_data` | SQLite Calendar e relativi journal/WAL | Chiave `calendar-token-key`, configurazione OAuth e identità degli utenti SQL |
| `mail_addon_data` | SQLite con storico di idempotenza SMTP | Outbox SQL e `MAIL_API_NAMESPACE` stabile |
| `inventory_addon_data` | SQLite con catalogo e revisioni dei beni | Copia consistente del database inventory e relativi WAL |
| Directory NPM `data` e `letsencrypt` | Configurazione proxy, database amministrativo, certificati pubblici | Stessa versione/configurazione del proxy e relativi file |
| `data/network-tls` | CA, chiave CA, certificati e chiavi server | SAN, nomi, IP e mount della distribuzione |
| `data/addon-secrets` | Token API, chiave Calendar e segreti dei provider | Database addon e configurazione corrispondenti |
| `.env.docker` | Account, password e configurazione dell'ambiente | Identità effettive degli account persistiti |
| `data/cluster-state.json` | Marcatori locali e UUID del cluster | Utile per recuperare gli stessi volumi; non è un seed generico |
| Codice/configurazione e manifest | Versione, schema, immagini, nomi volumi, orario e hash archivi | Riproducibilità e verifica della copia |

La replica MySQL conserva tre copie dello stesso stato, compresi errori o cancellazioni; non è una cronologia recuperabile. I dump SQL e gli archivi gzip non sono cifrati dagli script. Custodire copie esterne al singolo host, proteggere accessi e trasferimenti e separare la custodia delle chiavi dai dati cifrati. La politica di frequenza, conservazione, RPO e RTO non è automatizzata nel repository.

## Dump disponibili

```sh
python3 scripts/manage_cluster.py --env-file .env.docker backup
python3 scripts/backup_documents.py --env-file .env.docker
```

Il primo comando seleziona un membro online quando disponibile e scrive `backups/mysql-<UTC>-<suffisso>.sql`, con permessi `0600`. Usa `mysqldump --single-transaction --routines --triggers --events --hex-blob --set-gtid-purged=OFF --databases ...`. Un file incompleto viene rimosso. Il dump include il database applicativo, non gli account globali MySQL, la configurazione completa del cluster o un checkpoint pronto per il recupero GTID.

Il secondo usa `mongodump` autenticato e TLS sul solo database documentale, scrivendo `backups/documents-<UTC>-<suffisso>.archive.gz`, con directory `0700` e file `0600`. Non passa la password in URI o argomenti visibili del comando; usa un file temporaneo nel container. Verifica l'intestazione gzip e rimuove la copia incompleta. Non costituisce, da solo, una prova di ripristinabilità di tutti i PDF.

Nessuno dei due comandi arresta automaticamente l'app per un backup coordinato; il backup SQL singolo può essere consistente internamente grazie alla transazione, ma non è atomico con MongoDB o con gli addon.

## Finestra di backup coerente dell'intero stack

In assenza di altri client che scrivano direttamente nei database:

1. Annunciare una finestra di manutenzione e impedire nuove modifiche o invii.
2. Fermare l'app, lasciando terminare il lavoro in corso; fermare poi gli addon e NPM. Attendere la loro effettiva uscita. Una richiesta già inviata a Google o SMTP può avere un esito remoto incerto anche dopo l'arresto.
3. Con SQL e MongoDB ancora disponibili, creare entrambi i dump.
4. Con addon fermi, archiviare l'intero contenuto dei loro volumi, includendo gli eventuali `-wal` e `-shm`; con NPM fermo, archiviare entrambe le sue directory. Non copiare soltanto il file `.sqlite3` mentre il servizio scrive.
5. Salvare i materiali operativi e le chiavi con protezione separata, registrando quali archivi appartengono alla stessa finestra.
6. Verificare dimensioni, hash, leggibilità, permessi e l'esito dei comandi; conservare il manifest senza password o token.
7. Riavviare prima addon e app, poi NPM, e verificare salute e stato delle code.

I comandi di arresto e dump previsti sono:

```sh
docker compose --env-file .env.docker stop app
docker compose --env-file .env.docker stop mail calendar inventory proxy-web
python3 scripts/manage_cluster.py --env-file .env.docker backup
python3 scripts/backup_documents.py --env-file .env.docker
```

**Prima del riavvio completare le copie dei volumi, delle directory e delle chiavi dei punti 4–6.** Il repository non include un esportatore unico per questi elementi. Usare un sistema di backup del daemon/host che monti i volumi in sola lettura, preservi proprietà e permessi e non alteri gli originali. In alternativa, per SQLite è valida un'operazione tramite la SQLite Backup API, se realizzata e verificata con gli strumenti dell'ambiente. Una copia live del solo database non è equivalente.

Dopo il completamento verificato delle copie:

```sh
docker compose --env-file .env.docker up -d --no-deps --wait mail calendar inventory app
docker compose --env-file .env.docker up -d --no-deps --wait proxy-web
```

Se una copia fallisce, conservare i dati originali e distinguere chiaramente una copia parziale da un punto di ripristino completo. Un errore non autorizza a eliminare né rigenerare volumi o chiavi. Lo spazio di backup deve essere sufficiente anche per gli archivi temporanei.

## Ripristino da backup: procedura controllata

Non esiste uno script `restore` nel progetto. Il ripristino va prima provato in un ambiente separato, con volumi nuovi e con invii SMTP e operazioni Google reali disabilitati. Non importare direttamente tre volte un dump nel gruppo multi-primary attivo.

1. Identificare una serie coerente di archivi, relativa versione del codice/schema, chiavi e configurazione. Verificare gli hash registrati; conservare una copia intatta dell'ambiente danneggiato prima di ulteriori interventi.
2. Preparare un'istanza SQL isolata e ripristinare il dump applicativo. Gli account globali e le credenziali vanno ricostruiti in modo coerente; il dump applicativo non li contiene. Validare vincoli, conteggi e dati prima di collegare l'app.
3. Progettare il seed degli altri membri con il checkpoint GTID corretto. `init` rifiuta sorgenti con storico GTID non vuoto e marcatori incompatibili: non aggirare questa protezione cancellando GTID o `cluster-state.json`. Un ripristino logico e una ricostruzione del gruppo richiedono una procedura SQL esplicita e verificata per quella situazione.
4. Ripristinare l'archivio documentale nel database MongoDB isolato corrispondente. Verificare collezioni, documenti di più chunk e hash SHA-256 rispetto ai metadati SQL. Un oggetto non referenziato va valutato, non eliminato automaticamente.
5. Ripristinare l'intera directory Calendar con la **stessa chiave AES**, i secret appropriati e gli ID utenti SQL corrispondenti. Non far partire una sincronizzazione reale durante la validazione della copia. Una chiave errata deve impedire l'apertura, non condurre alla cancellazione del database.
6. Ripristinare insieme ledger email, outbox SQL e namespace. Esaminare gli stati `unknown`, `failed` e gli invii già accettati dal provider. Ripristinare un backup può riportare indietro il database locale ma non annulla un'email già inviata.
7. Ripristinare NPM e TLS, verificando nomi, SAN, scadenze, permessi, destinazioni del proxy e protezione della callback OAuth. I materiali non devono entrare nel repository.
8. Eseguire verifiche di login, ruoli, commesse, ore, report, download PDF, code e stato del gruppo. Solo dopo la validazione, predisporre il passaggio dell'ingresso all'ambiente ripristinato e riabilitare consapevolmente le integrazioni esterne.

Una prova di restore riuscita deve registrare tempi effettivi, limiti incontrati e dati verificati. I risultati storici della suite non dimostrano da soli che una nuova copia di backup sia ripristinabile.

## Manutenzione di code e integrazioni

`sent` per email significa accettazione SMTP, non consegna nella casella. `delivery_unknown` e gli esiti terminali richiedono verifica del provider. Non cambiare namespace né cancellare il ledger per forzare il reinvio: potrebbe duplicare messaggi.

Calendar conserva operazioni `queued`, `unknown`, `applied`, `conflict` e `failed`. Un conflitto ETag va risolto acquisendo la versione remota e prendendo una decisione esplicita sul contenuto. Una disconnessione cancella i dati locali dell'utente e tenta la revoca del consenso; non annulla eventi già creati o una richiesta remota in corso. I dettagli sono nel [manuale Calendar](../../calendar-addon/README.it.md).

Le API e i controlli di stato sono documentati in [API addon](api-addon.md). Non ripulire gli archivi manualmente per eliminare errori di sincronizzazione prima di averne compreso l'esito remoto.

## Diagnosi di errori frequenti

| Sintomo | Verifiche iniziali |
| --- | --- |
| App `503`, nodi SQL avviati | Stato Group Replication, quorum, sola lettura, health monitor HAProxy; il processo SQL sano non basta |
| Errore TLS interno | Ora del sistema, scadenza, CA montata, SAN, nome destinazione e corrispondenza chiave/certificato |
| PDF non disponibile | Health MongoDB, account documentale, metadato SQL, oggetto GridFS e hash; non rigenerare byte mancanti |
| Addon sano ma integrazione non attiva | `smtp_configured` / `oauth_configured`; la configurazione fornitori è ancora rinviata |
| Squid `403` | IP sorgente, metodo CONNECT, host esatto, porta e risposta DNS pubblica |
| Nuova allowlist non letta | Un editor può aver sostituito l'inode del bind mount; controllare il file nel container e ricreare Squid dopo una build valida |
| NPM non accetta una configurazione personalizzata | Controllare le restrizioni del filtro privacy OAuth e le location/ACL; non disabilitare il filtro |
| Login perso dopo un update | Sessioni in memoria: autenticarsi nuovamente |
| `recover` rifiuta GTID divergenti | Conservare i volumi e analizzare gli insiemi; non forzare bootstrap o quorum |
| Calendar rifiuta la chiave | Ripristinare il secret corretto per quel volume; non cancellare il controllo crittografico |

Il rinnovo dei leaf TLS è manuale e va pianificato prima della scadenza. `prepare` verifica il bundle ma non effettua una rotazione automatica: la procedura è descritta in [installazione e configurazione](installazione-configurazione.md).
