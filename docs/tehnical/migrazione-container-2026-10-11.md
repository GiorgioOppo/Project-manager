# Migrazione dei container — 11 ottobre 2026

[Indice tecnico](README.md) · [Nomi dei servizi e delle immagini](nomi-servizi-e-immagini.md) · [Backup e ripristino](operazioni-backup-ripristino.md)

**Stato: migrazione e collaudo completati.** Gli undici servizi con i nomi nuovi sono `running` e `healthy`; ciascuno dei tre membri MySQL vede tre membri `ONLINE` / `PRIMARY` e usa schema 8. Conservati i dati preesistenti, i volumi originali e le identità del cluster. Superati i controlli API, PDF, rete e interfaccia browser.

Questo rapporto riguarda l’esecuzione sul progetto Docker **`projectmanament`**. Le prove di preparazione e compilazione descritte nella [guida nomenclatura](nomi-servizi-e-immagini.md#verifiche-di-preparazione--11-ottobre-2026) precedono questa manutenzione e non equivalgono al collaudo del deployment.

## Perimetro e situazione iniziale

L’inventario iniziale conteneva dieci container legacy del progetto, tutti già arrestati (`exited`): `app`, `nginx-proxy-manager`, `squid`, `db-proxy`, `db`, `db2`, `db3`, `documents`, `mail-addon` e `calendar-addon`. Lo stato health memorizzato era `unhealthy`; per container fermi questo non costituisce una nuova prova di malfunzionamento dei processi.

Ticket non era installato e non viene aggiunto dalla migrazione. `inventory` è una dipendenza nuova del codice applicativo corrente ed è stato avviato con il nuovo volume `projectmanament_inventory_addon_data`; non sostituisce un archivio inventario preesistente. Il totale è quindi di sette volumi originali conservati più uno nuovo.

Il database operativo iniziale usava **schema 7 e 22 tabelle MySQL**. Il codice distribuito richiede schema 8 e ha aggiunto le tabelle `job_tasks` e `job_blockers`. Il cambio dei nomi Compose e la migrazione applicativa sono quindi due passaggi distinti della stessa manutenzione.

| Dati iniziali | Conteggio |
| --- | ---: |
| Utenti | 15 |
| Commesse | 4 |
| Registrazioni ore | 0 |
| Fornitori / contratti | 2 / 2 |
| Metadati PDF SQL | 4 |
| Gruppi / appartenenze | 5 / 2 |
| Eventi audit | 205 |
| GridFS: file / chunk / chiavi PDF | 4 / 12 / 4 |

Le altre tabelle sono inventariate con conteggi e impronte nel manifest privato. Le tabelle operative degli alert, assegnazioni e tariffe erano vuote. Gli archivi SQLite mail e Calendar avevano integrità `ok`; il registro invii, gli account Google, gli eventi, le operazioni e gli state OAuth erano vuoti. Calendar conservava una riga di metadati.

## Copie di sicurezza

La finestra di backup è identificata da `backups/container-names-20261011T054307Z/`. Sono stati preparati dump logici e **otto archivi a freddo**, per **21.022.888 byte** complessivi, circa 21,0 MB. Gli otto archivi comprendono i sette volumi persistenti esistenti e la configurazione operativa. Il manifest registra dimensione, impronta SHA-256 e numero di file regolari di ciascun archivio.

| Volume originale preservato | Scopo |
| --- | --- |
| `projectmanament_mysql_data` | Primo membro MySQL |
| `projectmanament_mysql_data_2` | Secondo membro MySQL |
| `projectmanament_mysql_data_3` | Terzo membro MySQL |
| `projectmanament_documents_data` | Dati MongoDB |
| `projectmanament_documents_config` | Configurazione persistente MongoDB |
| `projectmanament_mail_addon_data` | Registro idempotenza email |
| `projectmanament_calendar_addon_data` | Archivio Calendar |

La directory backup è privata (`0700`), i file hanno permessi `0600`. Dump, archivi operativi, credenziali e configurazioni reali non sono riprodotti nella documentazione. La presenza degli archivi e dei loro hash non equivale a una prova completa di ripristino in un ambiente distinto.

## Operazioni già completate

- Fermati e rimossi i dieci container legacy selezionati dal progetto, **senza `-v`**. Conservati i sette volumi originali e i bind mount persistenti.
- Ricreati i membri con i servizi `mysql-1`, `mysql-2`, `mysql-3`, mantenendo le identità interne `db`, `db2`, `db3`.
- Recuperato il gruppo MySQL: tutti e tre i membri risultano `ONLINE` / `PRIMARY`. Il confronto conferma gli UUID originali e la conservazione dei GTID su ciascun membro.
- Avviato il servizio `mongodb` sugli archivi documentali esistenti. Le tre collezioni confrontate sono identiche rispetto alla baseline.
- Conservati credenziali, certificati e stato del cluster originari. I nomi nuovi non hanno comportato la rigenerazione delle identità TLS.
- Conservate le immagini vendor effettive: MySQL e MongoDB usano gli stessi ID dell’installazione precedente. Le immagini effettivamente avviate dei servizi corrispondono agli ID registrati prima del cambio dei container.
- Confermata l’assenza di container legacy residui; superato il controllo `require_service_migration_complete` del controller.

| Immagine vendor | ID effettivo conservato |
| --- | --- |
| `mysql:8.4` | `sha256:6ea90827b1100f8f2ae306a539f86d2c264a26ed435a2a9f75551dd5c3aeb242` |
| `mongo:8.0` | `sha256:d0d926f94df099bff534b7ee5b5986458131a22489dfff8664509af0c1e2ca9c` |

MySQL riporta versione **8.4.11** nella baseline. Prima dell’avvio della nuova app, tutte le 22 tabelle dello schema 7 erano identiche alla baseline su ciascuno dei tre membri. Questo controllo precede la creazione delle due tabelle applicative nuove e il relativo audit.

## Verifiche dopo l’avvio applicativo

La migrazione ha portato i tre membri allo **schema 8, con 24 tabelle**. Il confronto con lo stato iniziale distingue gli aggiornamenti tecnici attesi dai dati business:

- **19 tabelle originali identiche**, comprese utenti, commesse, ore, contratti, fornitori, metadati PDF, gruppi e abilitazioni.
- Tutte le **205 righe audit originali conservate**; una riga nuova porta il totale a 206 al controllo immediatamente successivo alla migrazione.
- `schema_metadata` passa da versione 7 a 8 e il contatore di `cluster_write_guard` cambia durante le transazioni. Queste sono le altre due tabelle originali con differenze attese.
- Mail e Calendar: tutte le tabelle originali identiche, controllo SQLite di integrità `ok`.
- Nginx Proxy Manager: tutte le tabelle originali identiche, controllo SQLite di integrità `ok`.

Il totale audit si riferisce al confronto dopo l’avvio: i successivi accessi di collaudo possono aggiungere eventi legittimi e non cambiano la verifica di conservazione dello storico originale.

| Controllo | Esito |
| --- | --- |
| Servizi con i nomi nuovi | 11 su 11 `running` / `healthy` |
| Inventory | Avviato, API configurata, nuovo archivio dedicato |
| Schema MySQL | Versione 8 su tutti e tre i membri |
| Dati prima dell’app | Tutte le 22 tabelle originali identiche su ciascun nodo; tre collezioni MongoDB identiche |
| Dati dopo l’app | Dati business conservati; sole differenze tecniche descritte sopra |
| API applicative | 34 controlli superati, compresi login/logout, sezioni gestionali, report, attività e stato addon |
| PDF | Quattro download verificati tramite hash |
| Rete e sicurezza | 23 test opt-in superati in 83,552 secondi, nessun test saltato |
| Interfaccia browser | 19 verifiche aggregate superate; nessun errore JavaScript o HTTP 5xx |

La suite `tests/test_network_security.py` è stata eseguita sullo stack: verifica isolamento, trasporto TLS, autenticazione e collegamenti privati degli addon, limitazioni dell’uscita e policy proxy. Il risultato riguarda la configurazione realmente avviata, oltre alle prove statiche di preparazione.

Il collaudo browser copre login del gestore, nove schede gestionali, cinque viste report, attività con cinque colonne Kanban e lista, inventario e logout. Le viste vuote previste rispondono correttamente. Il controllo responsive dell’inventario non rileva scorrimento orizzontale della pagina a larghezze **1440, 390 e 320 pixel**. Non sono state tentate mutazioni dei dati business durante questa verifica.

Le **19 verifiche UI** sono quelle aggregate in `ui-smoke.json`: larghezze e sottoazioni non sono test aggiuntivi da sommare. Anche i quattro download PDF sono inclusi nei 34 controlli API. Questi risultati non vengono sommati in un unico totale eterogeneo. La verifica browser si è conclusa alle **05:54:47 UTC** dell’11 ottobre 2026, corrispondenti alle 07:54:47 in Europe/Rome.

SMTP e OAuth Google rimangono da configurare; non si considerano verificati invii reali o sincronizzazioni presso i fornitori. Non sono dichiarate nuove prove di guasto HA o un ripristino completo da backup.

## Evidenze consultate

Questo rapporto usa soltanto i metadati non sensibili della finestra di backup e gli esiti operativi comunicati durante la manutenzione: `inventory-before.json`, `cold-backup-manifest.json`, `data-before.json`, `mongodb-before.json`, `sqlite-before.json`, `image-ids-before.json`, `data-after-before-app.json`, `data-after-app.json`, `api-smoke.json`, `ui-smoke.json` e `network-verification.json`. I file originali restano nel percorso privato di backup; il rapporto espone risultati aggregati e identità tecniche utili alla verifica, senza contenuti applicativi o segreti.
