# Gestione di certificati e segreti

**Italiano** | [English](../en/tehnical/gestione-certificati-e-segreti.md)

Inventario e procedure riferiti al codice del **10 ottobre 2026**. Questo manuale descrive ciò che viene generato o letto oggi e separa i percorsi manuali di manutenzione dalle rotazioni che richiedono ancora strumenti dedicati. Non contiene valori reali né ha letto o modificato i materiali dell'installazione.

Fonti: [Compose](../../compose.yaml), [preparazione TLS e secret](../../scripts/configure_network.py), [account del cluster](../../scripts/manage_cluster.py), [client email](../../src/mail_client.c), [client Calendar](../../src/calendar_client.c), [servizio email](../../addon/src/mailer.c), [servizio Calendar](../../calendar-addon/src/calendar.c). Per i confini della protezione leggere [rete e sicurezza](rete-sicurezza.md).

## Tre categorie da gestire separatamente

1. **Certificati e chiavi TLS**: autenticano il servizio e proteggono il trasporto; rinnovare un leaf non ricifra i database.
2. **Password e token di autenticazione**: consentono l'accesso a database, API private o provider; devono corrispondere fra chiamante e destinatario.
3. **Chiave AES Calendar**: decifra dati già persistiti; cambiarla senza migrare i ciphertext rende l'archivio illeggibile.

Le password degli utenti finali sono derivate con PBKDF2 e salt, non cifrate con una chiave da custodire. Non è presente un vault di chiavi per utente/campo e non esiste una chiave centrale che permetta di recuperare le password di login.

## Inventario TLS privato

Il percorso host comune è `data/network-tls/`, escluso da Git e dal contesto delle immagini. La tabella elenca nomi di file, non link a materiali dell'installazione. I nomi storici dei file e dei SAN restano validi attraverso gli alias Compose; vedere [nomenclatura Docker](nomi-servizi-e-immagini.md).

| File host | Consumatore e mount | Funzione |
| --- | --- | --- |
| `ca.crt` | App, proxy-db, mysql-1/mysql-2/mysql-3, mongodb, mail, calendar, inventory; percorsi TLS specifici dei servizi | Radice pubblica di fiducia interna |
| `ca.key` | Solo operatore/script sull'host; nessun mount nei container | Firma dei certificati interni |
| `db.crt`, `db.key` | `mysql-1` → `/etc/mysql/tls/server.crt` e `server.key` | Identità del primo nodo |
| `db2.crt`, `db2.key` | `mysql-2`, stessi percorsi interni | Identità del secondo nodo |
| `db3.crt`, `db3.key` | `mysql-3`, stessi percorsi interni | Identità del terzo nodo |
| `documents.crt`, `documents.key` | Materiali originali sull'host | Certificato e chiave documentale usati per creare il PEM |
| `documents.pem` | `mongodb` → `/etc/mongo/tls/server.pem` | Chiave e certificato concatenati per MongoDB |
| `mail-addon.crt`, `mail-addon.key` | `mail` → `/etc/punto/tls/mail-addon.crt` e `.key` | HTTPS `mail-api:8090` |
| `calendar-addon.crt`, `calendar-addon.key` | `calendar` → `/etc/punto/tls/calendar-addon.crt` e `.key` | HTTPS `calendar-api:8091` |
| `inventory-addon.crt`, `inventory-addon.key` | `inventory` → `/etc/punto/tls/inventory-addon.crt` e `.key` | HTTPS `inventory-api:8092` |

App e proxy-db ricevono `ca.crt` su `/etc/punto/tls/ca.crt`; i nodi SQL su `/etc/mysql/tls/ca.crt`; MongoDB su `/etc/mongo/tls/ca.crt`. Squid usa le CA di sistema per le proprie esigenze e non riceve la chiave CA privata. La cifratura dei tunnel verso Google/SMTP è terminata dai client, non da Squid.

### Identità nei certificati

| Leaf | DNS richiesti dalla configurazione | IP previsti |
| --- | --- | --- |
| db | `db`, `db-proxy` | `172.30.30.21` |
| db2 | `db2`, `db-proxy` | `172.30.30.22` |
| db3 | `db3`, `db-proxy` | `172.30.30.23` |
| documents | `documents`, `localhost` | `127.0.0.1`, `172.30.30.30` |
| mail-addon | `mail-api`, `mail-addon`, `localhost` | `127.0.0.1`, `172.30.60.2` |
| calendar-addon | `calendar-api`, `calendar-addon`, `localhost` | `127.0.0.1`, `172.30.70.2` |
| inventory-addon | `inventory-api`, `inventory-addon`, `localhost` | `127.0.0.1`, `172.30.80.2` |

`db-proxy` compare nei certificati SQL perché HAProxy inoltra TCP senza terminare TLS: il client continua a verificare la destinazione configurata. Gli IP loopback negli addon/documenti servono alle verifiche previste, non pubblicano nuove porte.

La CA è RSA 3072/SHA-256, valida 3650 giorni dalla generazione; i leaf sono RSA 3072/SHA-256, validi 365 giorni. Queste sono durate di emissione, non la scadenza dell'installazione concreta: per quella leggere i certificati pubblici con i comandi sotto.

## Secret addon e credenziali provider

Il percorso host è `data/addon-secrets/`, protetto dalla directory padre. Sono secret Compose basati su file locali, non secret gestiti da un KMS o da Docker Swarm.

| File host | Nome/mount Compose | Chi lo riceve | Uso |
| --- | --- | --- | --- |
| `mail-api-token` | `mail_api_token` → `/run/secrets/mail_api_token` | App e mail | Bearer condiviso dell'API privata email |
| `smtp-password` | `mail_smtp_password` → `/run/secrets/mail_smtp_password` | Solo mail | Password del provider SMTP quando configurato |
| `calendar-api-token` | `calendar_api_token` → `/run/secrets/calendar_api_token` | App e calendar | Bearer condiviso dell'API privata Calendar |
| `calendar-token-key` | `calendar_token_key` → `/run/secrets/calendar_token_key` | Solo calendar | Chiave AES-256-GCM, 32 byte rappresentati da 64 cifre esadecimali |
| `google-client-secret` | `google_client_secret` → `/run/secrets/google_client_secret` | Solo calendar | Credenziale del client OAuth Google |

`prepare` genera i tre file esadecimali con valori casuali distinti e crea inizialmente vuoti i due file provider. Conserva i file esistenti, rifiuta symlink/file non regolari e verifica il formato dei tre valori esadecimali attesi. Non stampa i valori e non esegue una rotazione automatica.

I token API prodotti hanno 64 caratteri esadecimali. I client C accettano token di 32–128 caratteri ASCII nel proprio insieme consentito, ma mantenere il formato generato preserva la compatibilità anche con la verifica di `prepare`. Il token dell'API non va usato come chiave AES o password provider.

Il bearer viene caricato in memoria all'avvio dei processi. Cambiare il file sul disco non è un hot reload del token. Inoltre una sostituzione atomica del file può lasciare un container esistente collegato all'inode precedente: una ricreazione coordinata è il modo previsto per rileggere file e ambiente.

## Credenziali in ambiente

| Variabile locale | Consumatore effettivo | Nota operativa |
| --- | --- | --- |
| `PUNTO_EMAIL`, `PUNTO_PASSWORD` | App | Bootstrap del primo gestore su database vuoto; non aggiornano l'utente già salvato |
| `MYSQL_USER`, `MYSQL_PASSWORD` | Inizializzazione nodi e app tramite `DB_USER`/`DB_PASSWORD` | L'app riceve il proprio account SQL, non root |
| `MYSQL_ROOT_PASSWORD` | Nodi e operazioni amministrative del controller nei container | Non fornita all'app o agli addon |
| `CLUSTER_REPLICATION_PASSWORD` | Controller, che configura account e canale nei nodi | Account `punto_recovery`, massimo 32 byte |
| `CLUSTER_MONITOR_PASSWORD` | Controller e proxy-db | Account `punto_monitor` limitato alle letture di stato previste |
| `DOC_DB_PASSWORD` | App e MongoDB | Account documentale con ruolo limitato al database documenti |
| `DOC_DB_ROOT_PASSWORD` | MongoDB come password amministrativa | Non fornita all'app |
| `NPM_ADMIN_EMAIL`, `NPM_ADMIN_PASSWORD` | Script `configure_proxy.py` | Credenziali del pannello, non variabili passate al container app |
| `MAIL_SMTP_USER` | mail | Nome di accesso provider; password separata nel secret |
| `GOOGLE_CLIENT_ID`, `GOOGLE_REDIRECT_URI` | calendar | Identificatore e URI OAuth; client secret separato |

Il controller genera le quattro password interne mancanti di replica, monitor e documenti durante `init`. Non interpreta una successiva modifica del file env come richiesta di aggiornamento degli account già creati. `CREATE USER IF NOT EXISTS` non è una procedura di rotazione delle password esistenti.

Le variabili d'ambiente e i mount sono leggibili dall'amministratore del daemon/host. La separazione fra container riduce l'esposizione ai singoli processi ma non costituisce una difesa contro chi amministra l'infrastruttura.

## Permessi, copie e materiali pubblici

La directory TLS e quella dei secret hanno permessi `0700` sull'host. La chiave CA ha `0600`. I certificati pubblici hanno `0644`; le chiavi server e i file secret montati sono impostati a `0444`, perché Compose conserva proprietà e permessi del file host e i servizi usano UID diversi. La protezione locale dipende dalla directory padre, oltre che dai controlli dell'host; i mount nei container sono in sola lettura.

Non rendere i file scrivibili da gruppo o altri per risolvere un problema di accesso: i client rifiutano file sensibili non regolari o con permessi di scrittura troppo aperti. Verificare invece utente del processo, directory, mount e nome del file.

Certificati pubblici, date e impronte possono essere mostrati durante il controllo. Chiavi private, password, bearer, refresh token e il contenuto dei file provider non devono finire in console condivise, log, ticket o Git. Le copie di backup devono conservare il materiale necessario senza collocare la chiave AES insieme all'unica copia dei ciphertext in un archivio non protetto.

## Verifiche TLS di sola lettura

I seguenti comandi leggono solo certificati pubblici locali. Non contattano provider e non rinnovano alcunché:

```sh
openssl x509 -in data/network-tls/ca.crt -noout -subject -issuer -dates -fingerprint -sha256
openssl x509 -in data/network-tls/db.crt -noout -subject -issuer -dates -fingerprint -sha256
openssl x509 -in data/network-tls/calendar-addon.crt -noout -ext subjectAltName
openssl x509 -in data/network-tls/calendar-addon.crt -noout -checkend 2592000
openssl verify -CAfile data/network-tls/ca.crt -verify_hostname db-proxy data/network-tls/db.crt
openssl verify -CAfile data/network-tls/ca.crt -verify_hostname mail-api data/network-tls/mail-addon.crt
openssl verify -CAfile data/network-tls/ca.crt -verify_ip 172.30.70.2 data/network-tls/calendar-addon.crt
```

`-checkend 2592000` controlla una finestra operativa di 30 giorni proposta per il rinnovo. `prepare` usa invece 86400 secondi come soglia tecnica di rifiuto. Ripetere le verifiche sui leaf coinvolti, non soltanto su quello di esempio. Un esito non zero deve essere analizzato: scadenza, CA, identità o file possono essere errati.

Questi controlli non provano che il processo stia effettivamente usando il nuovo certificato. Per quello serve una connessione TLS reale dalla rete autorizzata, per esempio il health check addon:

```sh
docker compose --env-file .env.docker exec -T app curl --noproxy '*' --fail --silent --show-error --max-time 3 --cacert /etc/punto/tls/ca.crt https://calendar-api:8091/healthz
```

La chiamata non invia il bearer e non esegue una sincronizzazione. L'analoga verifica mail usa `https://mail-api:8090/healthz`. Non usare `-k` o disabilitare il controllo del nome per considerare riuscito il test. Un certificato valido non verifica l'autenticazione del token API.

Per corrispondenza certificato/chiave, `prepare` confronta le chiavi pubbliche derivate senza stamparne i materiali privati. Poiché `prepare` può anche creare elementi mancanti, non è un comando di sola lettura universale: eseguirlo solo quando è appropriato preparare l'ambiente.

## Rinnovo manuale di un leaf sotto la stessa CA

**Disponibile come manutenzione manuale; non automatizzato dallo script.** Prima del rinnovo identificare il servizio, tutti i nomi/IP richiesti, data di scadenza e procedura di ritorno. Conservare il leaf precedente e il suo abbinamento alla chiave in una copia protetta.

1. Generare il nuovo materiale in una directory di staging protetta, senza sovrascrivere il bundle attivo. Usare la CA corretta, estensioni e SAN coerenti con l'inventario.
2. Verificare date, catena, tutti i nomi/IP richiesti e corrispondenza fra leaf e chiave. Per MongoDB aggiornare insieme `.crt`, `.key` e `.pem`; il PEM deve essere la concatenazione prevista.
3. Pianificare la sostituzione coerente di certificato e chiave durante la manutenzione. Non avviare il servizio con metà del nuovo materiale.
4. Ricreare il servizio interessato per rileggere i bind mount e il contesto TLS; verificare l'identità via connessione reale.
5. Ripetere health e controlli del flusso applicativo; conservare il verbale con impronta pubblica e nuova scadenza, senza chiavi.

Per i nodi SQL con la stessa CA, una procedura può rinnovare un membro alla volta mantenendo due membri sani e attendendo ogni rientro `ONLINE` prima di proseguire. Va prima collaudata sul laboratorio e non equivale a un comando rolling automatico presente nel repository. Se il quorum si riduce, fermare la sequenza e analizzare lo stato.

App e addon sono singole istanze: il loro riavvio ha una breve interruzione. Un rinnovo della stessa CA non richiede di cambiare la chiave AES Calendar, i bearer, il namespace email o i dati business.

## Rotazione manuale di un token API addon

**Manutenzione coordinata, senza supporto simultaneo di due token.** App e addon caricano un singolo bearer ciascuno. Cambiarlo solo da un lato causa errori di autenticazione.

1. Definire la finestra, salvare i materiali necessari e interrompere nuove richieste all'addon fermando l'app.
2. Lasciare concludere le operazioni in corso e fermare l'addon coinvolto. Considerare eventuali esiti remoti incerti come descritto nella guida operativa.
3. Generare un nuovo token casuale nel formato del progetto, senza stamparlo. Installarlo nel solo file `mail-api-token` oppure `calendar-api-token`, mantenendo directory, proprietà e permessi previsti.
4. Ricreare addon e app affinché leggano lo stesso file nuovo. Un semplice aggiornamento del file non ricarica i token in memoria.
5. Verificare sia TLS sia un'API autenticata autorizzata, senza esporre il bearer. Controllare la ripresa delle code con le stesse chiavi di idempotenza.

Non cambiare `MAIL_API_NAMESPACE`, non cancellare ledger o volume Calendar e non toccare `calendar-token-key`. Sono identità e materiali diversi. Se la rotazione risponde a una compromissione, il piano deve valutare anche revoca del vecchio valore e accessi possibili nel periodo, senza usare il vecchio token come ritorno automatico.

## Rotazione di password database e provider

**Da pianificare esplicitamente; nessun comando unico implementato.** Una rotazione di password SQL richiede aggiornamento dell'account effettivo e dei suoi client. Per l'account di replica comprende anche i canali Group Replication su tutti i membri; per il monitor comprende HAProxy. La password di recupero mantiene il limite di 32 byte.

Il file `.env.docker` non è la fonte che riscrive ogni volta gli account persistiti. Aggiornarlo senza modificare l'account lascia i nuovi processi incapaci di autenticarsi. La procedura deve avere un ordine che conservi quorum, accesso amministrativo e possibilità di verifica, collaudato per quella versione SQL.

Per SMTP, coordinare credenziale sul provider, file `smtp-password` e riavvio dell'addon; verificare prima gli invii pendenti, perché un servizio nuovamente autenticato può riprendere l'elaborazione. Per Google, cambiare il client secret può richiedere la gestione dei consensi e dei token già emessi: mantenere separati segreto del client, token OAuth utenti e chiave AES locale. Il repository non automatizza la revoca o la rotazione nel progetto Google.

Per NPM, la modifica di `NPM_ADMIN_PASSWORD` nel file locale non cambia da sola la password del pannello già inizializzato. Per gli utenti Punto, non esiste attualmente un endpoint pubblico di reset password. Questi aggiornamenti richiedono un flusso dedicato, non una nuova inizializzazione dei database.

## Sostituzione della CA interna

**Procedura futura da progettare e collaudare; non una funzione di `prepare`.** La CA è condivisa come radice di fiducia da database, monitor, app e addon. Sostituire soltanto `ca.crt` o `ca.key` interrompe la catena dei leaf esistenti.

Il piano deve inventariare tutti i consumer, produrre nuovi leaf con le identità corrette, preservare l'accesso ai dati, distribuire la fiducia e prevedere il rientro dei nodi SQL. Il progetto non implementa una transizione verificata a doppia CA né una rotazione online del bundle. Un eventuale periodo di fiducia doppia va validato sui driver e sullo script, che attualmente gestisce un bundle locale coerente con una CA.

Una manutenzione con fermo coordinato può essere più semplice, ma richiede comunque backup, copie dei materiali precedenti, test del recupero Group Replication e aggiornamento di ogni mount. Non cancellare la directory attuale per ottenere una nuova CA casuale con `prepare`: quel gesto perde il riferimento che serve a validare il materiale esistente.

La sostituzione della CA privata non rinnova i certificati pubblici NPM. Questi rimangono sotto `nginx-proxy-manager/letsencrypt` e sono amministrati dal proxy.

## Rotazione della chiave AES Calendar

**Non implementata. Non eseguire sostituendo il file.** Il database conserva un controllo cifrato che rifiuta la chiave sbagliata; gli eventi, le richieste, i token OAuth e i verificatori PKCE sono legati tramite AAD all'utente e all'identità della riga.

Una futura procedura deve almeno:

1. Fermare API e worker e ottenere una copia consistente dell'intero archivio con la chiave precedente.
2. Validare tutti i ciphertext con la chiave vecchia senza esportarne i plaintext in log o file non protetti.
3. Ricifrare tutte le categorie coperte e il controllo di chiave con nonce nuovi e identici dati autenticati, preservando ID e relazioni.
4. Garantire una transizione atomica fra archivio ricifrato e nuovo secret, con una strategia per interruzioni a metà.
5. Verificare lettura, OAuth, coda, idempotenza e isolamento utente su una copia; conservare la chiave precedente per i backup storici che la richiedono.

Non esiste oggi un key ID per riga né una modalità che accetti automaticamente più chiavi. La perdita della chiave impedisce il recupero dei ciphertext; la sola presenza del database o del token API non basta. Un KMS/HSM potrebbe migliorare la custodia, ma richiederebbe un'integrazione che non è presente.

## Registro di manutenzione e controllo finale

Registrare tipo di intervento, servizi, impronte pubbliche, scadenze, versione del codice, esito delle verifiche e riferimento alle copie protette. Non registrare il valore dei segreti. Per il post-intervento controllare cluster, salute, login, API addon e code secondo [diagnostica](diagnostica-e-osservabilita.md).

Le copie dei dati e le chiavi devono avere una politica coerente: eliminare una vecchia chiave prima dei backup che la usano può rendere quei backup inutilizzabili. Frequenza dei controlli, avvisi di scadenza e rotazioni periodiche non sono schedulati dal progetto; vanno definiti operativamente e, se desiderato, implementati in un successivo sistema di gestione.
