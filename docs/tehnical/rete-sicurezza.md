# Rete e sicurezza

**Italiano** | [English](../en/tehnical/rete-sicurezza.md)

La configurazione descritta è quella del repository al **11 ottobre 2026**. Le fonti principali sono [Compose](../../compose.yaml), [configurazione MySQL](../../docker/mysql/cluster.cnf), [Squid](../../docker/squid/squid.conf), [preparazione TLS](../../scripts/configure_network.py) e i client C. Per il quadro generale consultare [architettura](architettura.md) e [README principale](../../README.md).

## Otto reti e indirizzi

Le reti interne usano sia `internal: true` sia `com.docker.network.bridge.gateway_mode_ipv4: isolated`. NPM e Squid hanno ciascuno un bridge con uscita esterna distinto. I nomi delle interfacce nei container sono assegnati esplicitamente da Compose.

| Rete / interfaccia | Subnet | Membri e IP | Tipo |
| --- | --- | --- | --- |
| `public` | `172.30.10.0/24` | NPM `.2` | Bridge con gateway; ingresso pubblico |
| `private_app` | `172.30.20.0/24` | NPM `.2`, app `.10` | Interna, gateway isolato |
| `backend_app` | `172.30.30.0/24` | app `.10`, HAProxy `.20`, mysql-1 `.21`, mysql-2 `.22`, mysql-3 `.23`, mongodb `.30` | Interna, gateway isolato |
| `app_egress` | `172.30.40.0/24` | Squid `.2`, app `.10`, mail `.11`, calendar `.12` | Interna, gateway isolato |
| `proxy_wan` | `172.30.50.0/24` | Squid `.2` | Bridge con gateway; uscita del proxy |
| `mail_api` | `172.30.60.0/24` | mail `.2`, app `.10` | Interna, gateway isolato |
| `calendar_api` | `172.30.70.0/24` | calendar `.2`, app `.10` | Interna, gateway isolato |
| `inventory_api` | `172.30.80.0/24` | inventory `.2`, app `.10` | Interna, gateway isolato |

L'app ha sei interfacce. Gli alias `mail-api`, `calendar-api` e `inventory-api` sono definiti soltanto nelle rispettive reti API. La [nomenclatura Docker](nomi-servizi-e-immagini.md) mantiene gli alias storici dei servizi per DNS e TLS; i comandi Compose usano i nomi nuovi. Gli addon non appartengono a `backend_app` e non ricevono credenziali SQL o MongoDB. NPM non appartiene alle reti database o addon. Solo NPM pubblica porte sull'host. NPM dispone anche di uscita diretta dalla propria rete `public`, necessaria alla gestione del proxy e dei certificati: la policy Squid riguarda app e addon e non filtra il traffico di NPM.

Su Docker Desktop le reti bridge sono nella VM Linux; non rappresentano interfacce direttamente raggiungibili dal Mac. L'amministratore dell'host o del daemon Docker rimane in grado di leggere volumi, configurazioni e memoria o collegare nuovi container: la segmentazione non protegge da tale amministratore.

## Listener e flussi ammessi

| Sorgente | Destinazione | Protocollo / porta | Controllo |
| --- | --- | --- | --- |
| Browser | Host NPM | HTTP 80 o HTTPS 443 | Proxy host; TLS pubblico da configurare |
| Host locale | Pannello NPM | `127.0.0.1:81` predefinita | Autenticazione NPM |
| NPM | `172.30.20.10:8080` | HTTP privato | Bind dell'app, Host e Origin autorizzati |
| App | `db-proxy:3306` | MySQL con TLS | CA, identità server, account SQL |
| HAProxy | db/db2/db3 `3306` | Inoltro TCP | TLS end-to-end; selezione membri del gruppo |
| Monitor HAProxy | db/db2/db3 `3306` | MySQL TLS | Account monitor e verifica del certificato |
| mysql-1/mysql-2/mysql-3 | Peer `33061` | Group Replication XCom con TLS | Allowlist dei tre IP e verifica identità |
| mysql-1/mysql-2/mysql-3 | Peer `3306` | Recupero distribuito MySQL con TLS | Account recupero e verifica server |
| App | `documents:27017` | MongoDB TLS | CA, nome, autenticazione e ruolo documentale |
| App `.60.10` | `mail-api:8090` | HTTPS | IP sorgente e token bearer dedicato |
| App `.70.10` | `calendar-api:8091` | HTTPS | IP sorgente e token bearer dedicato |
| App `.80.10` | `inventory-api:8092` | HTTPS | IP sorgente e token bearer dedicato |
| App e addon | `172.30.40.2:3128` | Proxy esplicito CONNECT | Policy Squid distinta per IP sorgente |
| Squid | Host Google autorizzati `443` | Tunnel TLS | Nome esatto, porta e destinazione pubblica |
| Squid | Host SMTP autorizzati `465/587` | Tunnel SMTP TLS | Consentito solo per il servizio email; provider non ancora configurato |

L'app ascolta soltanto su `172.30.20.10:8080`, quindi le interfacce backend, egress, email, Calendar e inventory non espongono il server HTTP. I tre addon ascoltano solo sulle rispettive interfacce API; Squid solo su `172.30.40.2:3128`.

MongoDB ascolta su loopback e `172.30.30.30`. MySQL X Protocol è disattivato (`mysqlx=0`), quindi la porta `33060` non fa parte dei servizi previsti. Il monitor HAProxy espone i suoi tre endpoint HTTP solo su `127.0.0.1:9201–9203` nel container.

NPM espone l'API amministrativa Node solo su `127.0.0.1:3000` nel container. Il pannello Nginx ascolta su loopback e `172.30.10.2:81`, mentre la pubblicazione host di 81 è limitata a `127.0.0.1`. Non ascolta sull'interfaccia `private_app`, evitando che l'app raggiunga il pannello dalla propria rete.

La matrice descrive i flussi previsti, non un firewall per ogni coppia nella stessa rete. `backend_app` è una rete condivisa: l'app può tecnicamente aprire una connessione diretta a una porta MySQL di un membro. La rete non impone il passaggio obbligatorio da HAProxy e non applica ACL per porta fra tutti i suoi membri. Per imporre tale requisito servono policy aggiuntive sul daemon/host o una diversa topologia.

## Uscita attraverso Squid

Le tre sorgenti accettate sono gli IP dell'app, di `mail` e di `calendar` sulla rete egress. Sono vietate sorgenti diverse, richieste diverse da CONNECT, IP letterali e destinazioni private/riservate. Gli hostname non ammessi vengono rifiutati prima della risoluzione DNS; la risposta DNS di un nome ammesso viene comunque controllata contro [denied-destinations.txt](../../docker/squid/denied-destinations.txt).

| Client | Host consentiti | Porte |
| --- | --- | --- |
| App | `accounts.google.com`, `oauth2.googleapis.com`, `www.googleapis.com` | `443` |
| Calendar | Gli stessi tre host Google | `443` |
| Email | Voci esatte di `allowed-smtp-domains.txt` | `465`, `587` |

Attualmente la lista SMTP contiene solo `smtp-not-configured.invalid`. Non sono ammessi wildcard, punto iniziale, schema, porta o percorso nelle liste di domini. `www.googleapis.com.example.org` non corrisponde a `www.googleapis.com`.

Il proxy non intercetta né decifra TLS. Può autorizzare il nome e la porta del tunnel, ma non ispezionare URL HTTPS, corpo delle richieste o scope OAuth. `www.googleapis.com` non è un host esclusivo Calendar: il codice dell'addon limita a sua volta origini, percorsi e operazioni ammesse.

L'app riceve le variabili `HTTP_PROXY`/`HTTPS_PROXY` e le varianti minuscole. I servizi interni sono in `NO_PROXY`. I client API addon disabilitano esplicitamente il proxy per l'HTTPS privato; gli addon impongono Squid per le chiamate esterne, senza permettere a `NO_PROXY` di aggirarlo. Il controllo dell'uscita diretta è dato dalle reti interne, non dalla sola presenza di variabili d'ambiente.

Squid non usa cache e non è un router. `tcp_outgoing_address` imposta l'uscita su `172.30.50.2`; il forwarding IP dei container è disabilitato. I log riportano sorgente, metodo, dominio ed esito senza header, percorso o query. La configurazione è montata in sola lettura e i log Docker sono limitati a tre file da 10 MB.

## TLS interno e materiale crittografico

[configure_network.py](../../scripts/configure_network.py) genera una CA locale e leaf distinti con i nomi storici db, db2, db3, documents, mail-addon, calendar-addon e inventory-addon. La chiave CA rimane sull'host. L'app riceve solo la CA pubblica, mentre ciascun server riceve il proprio leaf e la propria chiave.

| Collegamento | Protezione configurata |
| --- | --- |
| App → SQL | `verify_identity`, CA locale obbligatoria e nome verificato |
| Replica SQL / recupero | TLS e verifica dei peer; account recupero `REQUIRE SSL` |
| App → MongoDB | `requireTLS`, CA e nome verificati; TLS 1.0/1.1 disabilitati sul server |
| App → addon | HTTPS con CA e hostname verificati; TLS minimo 1.2; redirect non seguiti |
| Addon → provider | TLS con verifica del nome e CA; CONNECT tramite Squid |
| NPM → app | HTTP in chiaro sulla rete `private_app` |

I certificati dei nodi SQL includono il rispettivo nome, l'IP e `db-proxy`, perché HAProxy non termina TLS. Le chiavi SQL e documentali non sono intercambiabili. I certificati addon includono l'alias della rete API oltre al nome storico e agli indirizzi previsti. I nuovi nomi Compose non vengono aggiunti automaticamente ai SAN: usare gli alias TLS già configurati.

I secret sono file locali separati dal database e montati in sola lettura. La directory host ha permessi `0700`; i file destinati ai container sono leggibili dai rispettivi utenti non privilegiati. Questa soluzione non è un KMS, non usa un HSM e non impedisce a un amministratore dell'host di leggere la chiave. Rinnovo TLS e rotazione delle chiavi richiedono procedure coordinate; vedere [configurazione](installazione-configurazione.md).

## Cifratura effettiva dei dati

| Dato | Protezione presente | Confine da conoscere |
| --- | --- | --- |
| Password degli utenti | PBKDF2-HMAC-SHA-256, 210.000 iterazioni, salt casuale 16 byte, derivato 32 byte | È una derivazione non reversibile, non una password cifrata recuperabile |
| Nomi, email, ruoli e dati gestionali SQL | Autenticazione, autorizzazione applicativa e TLS di rete | Nessuna cifratura applicativa generale delle celle |
| PDF originali e metadati contrattuali | Accesso gestore, TLS, verifica SHA-256 dei contenuti | PDF non cifrati individualmente dall'app; un hash non cifra |
| Token OAuth, verificatori PKCE, payload eventi e modifiche Calendar | AES-256-GCM con nonce casuali e AAD che lega utente e riga | Sono decifrati in memoria dal servizio autorizzato |
| Metadati Calendar | Controlli d'accesso al servizio | ID utente/account/calendario, ID evento, ETag, stati, sync token, chiavi idempotenza, impronte e date restano in chiaro |
| Ledger email | Hash SHA-256 del contenuto, chiave, stato e orari | Non conserva destinatario/oggetto/testo; tali contenuti sono nell'outbox SQL in chiaro |
| Dump SQL e archivio MongoDB gzip | Permessi locali `0600` | Dump e compressione non costituiscono cifratura |

Non sono configurati nel repository un vault generale di chiavi per colonna, un'autorizzazione crittografica per campo, cifratura omomorfica, enclave per elaborazione riservata o cifratura at-rest universale dei volumi. La cifratura del disco host e delle copie fuori sede è una responsabilità ulteriore dell'infrastruttura. I dati necessari alle operazioni vengono elaborati in chiaro nella memoria dei processi.

In Calendar, cambiare utente o identità della riga invalida l'autenticazione GCM del ciphertext. Un controllo cifrato all'apertura del database impedisce l'avvio con una chiave diversa. Il token bearer dell'API concede all'app fidata la possibilità di inoltrare richieste per gli utenti: l'isolamento dei browser dipende dal bridge dell'app, che deriva l'utente dalla sessione e non accetta un ID alternativo dal client.

## Sicurezza applicativa e OAuth

I profili globali sono gestore e operatore; gruppi e ruoli sulle commesse aggiungono abilitazioni puntuali. Le verifiche sono lato server. Nascondere un pulsante nel frontend non concede né revoca un'autorizzazione. Contratti, fornitori e costi sono riservati ai gestori; le registrazioni personali vengono sempre riferite alla sessione. I dettagli sono in [API applicative](api-app.md) e [modello dati](modello-dati.md).

Le sessioni casuali risiedono in memoria. I cookie sono `HttpOnly`, `SameSite=Lax` e ricevono `Secure` sulle richieste riconosciute come HTTPS. Durata breve: 8 ore; con ricordo accesso: 30 giorni. L'app controlla Host e Origin rispetto alle origini autorizzate e restituisce header di protezione, tra cui CSP e divieto di incorporamento in frame. I controlli dei moduli e delle API si aggiungono ai permessi di dominio.

OAuth usa state monouso con durata 600 secondi, legato all'utente e a un'impronta della sessione, oltre a PKCE S256. Il bridge non restituisce token Google al browser. Callback, permessi e dati per utente sono descritti nelle [API addon](api-addon.md).

L'immagine NPM derivata elimina query e referrer dagli access log. Per il percorso esatto `/api/calendar/oauth/callback` disabilita anche l'access log e i messaggi di errore che potrebbero riportare il codice OAuth. La trasformazione clona la location radice, mantenendo ACL, autenticazione e header; si applica alle configurazioni già presenti all'avvio e a quelle rigenerate da NPM. Configurazioni personalizzate incompatibili vengono rifiutate: non rimuovere questa protezione per fare accettare una location non verificata.

## Hardening dei container

Tutti i servizi hanno `no-new-privileges` e forwarding IPv4/IPv6 disabilitato. Non sono privilegiati, non usano la rete host e non montano il socket Docker.

App, addon, Squid e HAProxy hanno `cap_drop: ALL`, root filesystem in sola lettura e `/tmp` su tmpfs con `noexec,nosuid`. Gli addon hanno solo il volume dati necessario. NPM, MySQL e MongoDB rimuovono `NET_ADMIN` e `NET_RAW`, ma mantengono le altre capacità necessarie alle immagini e non hanno root filesystem interamente in sola lettura. Non va attribuito loro lo stesso profilo dei processi C.

I Dockerfile escludono dati locali, file env, backup, segreti, log e directory Git dal contesto grazie ai rispettivi `.dockerignore`. Il `.gitignore` esclude gli stessi materiali operativi dal versionamento. Queste esclusioni non sostituiscono la verifica di ciò che si aggiunge a Git.

## Verifica della matrice

La suite [test_network_security.py](../../tests/test_network_security.py) è opt-in e controlla le reti, gli endpoint, i tentativi vietati, il TLS e l'uscita. [test_squid_policy.py](../../tests/test_squid_policy.py) usa un container isolato per la policy DNS; [test_proxy_privacy.py](../../tests/test_proxy_privacy.py) verifica la protezione dei log OAuth; [test_mysql_tls.py](../../tests/test_mysql_tls.py) copre rifiuti e downgrade del driver.

Un health check positivo o un ping fallito non bastano a dimostrare l'isolamento. Ripetere le verifiche pertinenti dopo cambi a reti, IP, bind, immagini, allowlist, certificati o proxy. I comandi e gli effetti delle prove sono in [sviluppo e test](sviluppo-test.md).
