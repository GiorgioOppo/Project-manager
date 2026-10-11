# Autenticazione multi app con Keycloak

**Italiano** | [English](../en/tehnical/keycloak.md)

Keycloak fornisce un'identità e una sessione SSO comuni a Punto e Punto Tickets. Ciascuna app mantiene il proprio archivio, le autorizzazioni e i cookie di sessione. Il realm `punto` contiene due client OIDC confidenziali distinti, `punto` e `punto-tickets`, con Authorization Code, PKCE S256 e redirect esatti `/auth/callback`. Non sono abilitate registrazione libera, grant password, implicit flow o utenze dimostrative.

L'integrazione è facoltativa. Senza gli overlay resta l'autenticazione locale. Le app supportano `AUTH_MODE=local`, `hybrid` e `oidc`: `hybrid` mantiene l'accesso locale mentre si verificano le associazioni; `oidc` richiede il login tramite Keycloak. Le variabili di Compose hanno prefisso `KEYCLOAK_` per evitare collisioni con la configurazione standalone delle app.

## Componenti e rete

L'overlay `compose.keycloak.yaml` aggiunge Keycloak **26.8.0**, PostgreSQL **17.11** dedicato e il servizio iniziale `auth-config-init`. Le versioni sono fissate: Keycloak viene eseguito con `start --import-realm` in modalità produzione. PostgreSQL conserva i dati nel volume `keycloak_database`; le applicazioni non ricevono accesso al database delle identità.

| Componente | Rete e indirizzo | Uso |
| --- | --- | --- |
| Keycloak | `private_app`, `172.30.20.12:8443` | HTTPS da Nginx Proxy Manager |
| Keycloak | `auth_api`, `172.30.90.2:8443` | Backchannel HTTPS da Punto e Tickets |
| Punto | `auth_api`, `172.30.90.10` | Token e chiavi del provider |
| Tickets | `auth_api`, `172.30.90.11` | Token e chiavi del provider |
| Keycloak | `auth_database`, `172.30.91.2` | Connessione al database dedicato |
| PostgreSQL | `auth_database`, `172.30.91.3:5432` | Archivio identità |

`auth_api` e `auth_database` sono reti Docker interne. Nessuna porta di Keycloak o PostgreSQL viene pubblicata sull'host. La porta di management `9000` ascolta esclusivamente su loopback nel container e serve il controllo `/health/ready`. L'ingresso esterno passa da NPM; l'app Punto continua ad ascoltare soltanto su `172.30.20.10`.

Le richieste del browser usano l'issuer pubblico, per esempio `https://auth.example.test/realms/punto`. Token e JWKS viaggiano direttamente su `https://keycloak:8443/realms/punto`; la CA privata viene verificata da entrambe le app. `KC_HOSTNAME` resta l'origine pubblica completa e i token devono riportare l'issuer pubblico. Gli header del proxy sono accettati soltanto da NPM (`172.30.20.2/32`).

## Preparazione senza avvio

Prima configura HTTPS e nomi DNS pubblici per l'identità e ciascuna app. Usa origini diverse, senza slash finale o sottopercorsi; configura anche `PUBLIC_ORIGINS` di Punto e `TICKETS_PUBLIC_ORIGIN` con le rispettive origini HTTPS. Il certificato pubblico del dominio di autenticazione è gestito da NPM.

Punto verifica all'avvio che l'origine di `OIDC_REDIRECT_URI` sia configurata in `PUBLIC_ORIGINS` (per lo sviluppo HTTP è ammesso anche l'indirizzo loopback sulla porta locale/pubblica). Se si avvia l'accesso aziendale da un alias, per esempio `http://localhost`, `/auth/login` risponde con un redirect `303` all'origine della callback, prima di creare lo stato OIDC e il cookie. In questo modo il cookie rimane sullo stesso host della callback. La destinazione proviene soltanto dalla configurazione; parametri URL e header `Forwarded` non possono cambiarla. Per l'origine HTTPS il reverse proxy deve sempre imporre **Force SSL**, anche quando l'upstream applicativo usa HTTP. Dopo un vecchio errore di callback, riparti dalla pagina di accesso: un codice OIDC già consumato non può essere riutilizzato.

```sh
python3 scripts/configure_keycloak.py init \
  --issuer https://auth.example.test/realms/punto \
  --punto-origin https://punto.example.test \
  --tickets-origin https://tickets.example.test
```

Se Tickets non viene usato, ometti `--tickets-origin`: il client corrispondente è disabilitato e non ha redirect consentiti. Lo script prepara soltanto file locali e non avvia container, modifica database o migra password.

Il materiale viene creato in `data/keycloak`, escluso da Git:

| Percorso | Contenuto |
| --- | --- |
| `compose.env` | URL e modalità di autenticazione; nessuna password |
| `config.json` | Parametri della prima inizializzazione |
| `secrets/bootstrap-password` | Password dell'amministratore bootstrap |
| `secrets/database-password` | Password dedicata di PostgreSQL |
| `secrets/punto-client-secret`, `secrets/tickets-client-secret` | Segreti distinti dei due client |
| `realm/punto-realm.json` | Realm da importare, con i segreti espansi |
| `bindings/punto.json`, `bindings/tickets.json` | Associazioni esplicite tra subject Keycloak ed email locale |
| `tls/` | CA privata e certificato HTTPS interno di Keycloak |

Directory e file sensibili hanno permessi `0700` e `0600`. I certificati pubblici `.crt` sono `0644` dentro directory protette. `auth-config-init`, senza rete, copia i soli file necessari in volumi separati e assegna permessi `0400` ai rispettivi UID: Keycloak `1000`, PostgreSQL `999`, Punto `10001`, Tickets `10006`. I container applicativi ricevono il proprio segreto, mapping e CA, montati in sola lettura. La chiave privata della CA resta sull'host. Il wrapper di Keycloak legge bootstrap e database da file senza mettere le password negli argomenti o nel modello Compose.

`init` è idempotente: conserva segreti, certificati, mapping e `compose.env`, anche quando l'operatore modifica la modalità. Rifiuta URL diversi da quelli iniziali, un realm incoerente, link simbolici e bundle TLS incompleti. Non rigenera automaticamente un certificato scaduto e non ruota segreti. La CA dura dieci anni; il certificato interno un anno. Pianifica il rinnovo prima della scadenza verificando entrambi i percorsi interni.

## Ingresso Nginx Proxy Manager

Crea manualmente un Proxy Host per il dominio dell'issuer, con certificato pubblico valido e **Force SSL**:

| Campo NPM | Valore |
| --- | --- |
| Domain Names | `auth.example.test` |
| Scheme | `https` |
| Forward Hostname/IP | `keycloak` |
| Forward Port | `8443` |
| Cache Assets | Disabilitato |
| Websockets Support | Disabilitato |

Nella configurazione avanzata del Proxy Host abilita la verifica del certificato interno:

```nginx
proxy_ssl_server_name on;
proxy_ssl_name keycloak;
proxy_ssl_verify on;
proxy_ssl_trusted_certificate /etc/punto/keycloak-ca.crt;
proxy_ssl_verify_depth 2;
```

NPM deve sovrascrivere gli header `X-Forwarded-Host`, `X-Forwarded-Proto`, `X-Forwarded-Port` e `X-Forwarded-For` con i valori del collegamento esterno. Non inserire un prefisso `/auth` nel proxy o cambiare l'issuer in base all'header Host.

Per un'installazione locale sulla stessa macchina sono disponibili i nomi `punto.localhost`, `ticket.localhost` e `auth.localhost`. Inizializza Keycloak con queste tre origini HTTPS, poi prepara il certificato web firmato dalla CA locale:

```sh
python3 scripts/configure_local_sso_proxy.py --prepare-only
```

Quando Keycloak, app e NPM sono avviati con gli overlay corretti, applica i tre Proxy Host indicando esplicitamente l'ID di un amministratore NPM già presente e attivo:

```sh
python3 scripts/configure_local_sso_proxy.py \
  --container projectmanament-proxy-web-1 --admin-id 1
```

Il helper crea un backup SQLite consistente con `.backup` in `data/keycloak/npm-before-sso.sqlite`, mantiene la password NPM e apre una sessione proprietario di cinque minuti solo per le operazioni API. Importa un certificato per i tre nomi locali e configura gli upstream della tabella precedente; rifiuta host o certificati omonimi incompatibili. Non modifica direttamente il database NPM. Il certificato e la chiave restano in `data/keycloak/tls/local-web.crt` e `local-web.key`, con la chiave `0600`. Per il browser macOS, la procedura locale seguente considera attendibile il solo certificato web, limitato ai tre nomi, nel portachiavi dell'utente. La CA continua a essere usata per le verifiche dei collegamenti interni. Questi nomi e certificati locali non sostituiscono domini e certificati pubblici per un servizio aziendale condiviso.

Prima di rendere accessibile il Proxy Host, limita `/admin/` e `/realms/master/` agli indirizzi amministrativi della VPN, tramite Custom Locations e Access List basata su IP in NPM; usa lo stesso upstream HTTPS e la verifica del certificato. Gli endpoint pubblici del realm `punto` e `/resources/` devono restare disponibili per il login. Non pubblicare la porta `9000`. Accedi al pannello Keycloak dalla rete amministrativa, crea un amministratore nominativo con MFA e rimuovi l'account temporaneo `punto-bootstrap` dopo il bootstrap. La password iniziale è leggibile solo dal file segnalato da `init`.

## Verifica del modello e avvio esplicito

Per Punto senza Tickets:

```sh
docker compose --env-file .env.docker --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.keycloak.yaml config --quiet
```

Per Punto e Tickets:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml config --quiet
```

Mantieni l'ordine dei file: base, Tickets se usato, Keycloak, Tickets-Keycloak. Carica `data/keycloak/compose.env` dopo gli altri file di ambiente. Aggiungi l'overlay Tickets-Keycloak soltanto quando usi anche `compose.tickets.yaml`: l'overlay principale non crea implicitamente l'app Tickets.

Quando i domini, NPM e i binding sono pronti, lo stesso comando con `up --build -d` al posto di `config --quiet` avvia lo stack. Se lo stack è già in esecuzione, pianifica la ricreazione delle app per caricare la nuova configurazione. La prima partenza può impiegare più tempo per la preparazione di Keycloak. Le dipendenze attendono la copia dei segreti, la disponibilità di PostgreSQL e il controllo di salute di Keycloak.

## Avvio locale con HTTPS

Per l'installazione locale i tre nomi sono `punto.localhost`, `ticket.localhost` e `auth.localhost`, tutti in HTTPS sulla porta NPM `443`. I comandi seguenti partono dalla cartella del progetto e da un cluster MySQL già inizializzato. Se usi altre porte o un altro progetto Docker, adatta anche origini, redirect e nomi container prima della preparazione. Non cambiare gli URL di un bundle Keycloak già inizializzato: `init` li conserva e rifiuta una configurazione diversa.

1. Nel file `.env.docker` aggiungi `https://punto.localhost` a `PUBLIC_ORIGINS`, conservando le origini che vuoi ancora usare. In `ticket-app/.env` imposta `TICKETS_PUBLIC_ORIGIN=https://ticket.localhost`. Conserva le credenziali locali già presenti: servono anche per l'accesso durante la transizione `hybrid`.

2. Prepara il realm e il certificato web per i tre nomi:

   ```sh
   python3 scripts/configure_keycloak.py init \
     --issuer https://auth.localhost/realms/punto \
     --punto-origin https://punto.localhost \
     --tickets-origin https://ticket.localhost
   python3 scripts/configure_local_sso_proxy.py --prepare-only
   ```

   `data/keycloak/compose.env` mantiene inizialmente `KEYCLOAK_AUTH_MODE=hybrid` e `KEYCLOAK_TICKETS_AUTH_MODE=hybrid`.

3. Avvia lo stack con entrambi gli overlay, attraverso il recupero conservativo del controller. Poi configura i Proxy Host locali indicando l'ID verificato di un amministratore NPM già esistente; nell'esempio è `1`:

   ```sh
   python3 scripts/manage_cluster.py --docker-context desktop-linux \
     --keycloak --tickets start
   python3 scripts/configure_local_sso_proxy.py --context desktop-linux \
     --container projectmanament-proxy-web-1 --admin-id 1 --port 81
   ```

   `start` richiede un cluster inizializzato; per la prima installazione del cluster usa `init` con gli stessi flag. Il controller carica sempre anche `data/keycloak/compose.env` e `ticket-app/.env`, senza rigenerare credenziali Keycloak o modificare i binding. Il helper proxy conserva gli host preesistenti incompatibili e la password dell'amministratore NPM.

4. Su macOS considera attendibile il solo certificato web locale per l'uso SSL nel portachiavi dell'utente:

   ```sh
   security add-trusted-cert -r trustAsRoot -p ssl \
     -k "$HOME/Library/Keychains/login.keychain-db" \
     data/keycloak/tls/local-web.crt
   ```

   Questo certificato copre `punto.localhost`, `ticket.localhost` e `auth.localhost`. Dopo un rinnovo del certificato web, aggiorna anche questa voce di trust. Nel collaudo locale macOS, `curl` ha risolto automaticamente i nomi `*.localhost`: non è stato modificato il file hosts. Se Safari o un altro client usa un resolver che non li riconosce, aggiungi manualmente questa riga al file hosts del sistema:

   ```text
   127.0.0.1 punto.localhost ticket.localhost auth.localhost
   ```

   I browser che usano un archivio certificati separato richiedono la configurazione del trust nel proprio archivio. La porta amministrativa NPM resta [http://127.0.0.1:81](http://127.0.0.1:81).

5. Apri [la console Keycloak](https://auth.localhost/admin/master/console/). L'amministratore iniziale è `punto-bootstrap`; la sua password si trova nel file protetto `data/keycloak/secrets/bootstrap-password`. Crea un'identità e associala agli account locali esistenti, come descritto sotto. Per usare il helper CLI, sostituisci username, email e file password con quelli scelti per questa installazione:

   ```sh
   python3 scripts/manage_keycloak.py --context desktop-linux \
     --container projectmanament-keycloak-1 \
     create-user --username utente.sso --email utente@example.test \
     --name 'Utente SSO' --password-file /percorso/protetto/password-sso \
     --bind punto EMAIL_LOCALE_PUNTO --bind tickets EMAIL_LOCALE_TICKETS
   ```

   Il file password deve esistere già e avere permessi `0600`. Per un'identità preesistente priva del manifest locale, passa anche `--subject ID_VERIFICATO`; il helper non sceglie un utente sulla sola base dell'email.

6. Dopo la creazione dei binding, copiali nei volumi runtime e riavvia le due app:

   ```sh
   docker --context desktop-linux compose \
     --env-file .env.docker --env-file ticket-app/.env \
     --env-file data/keycloak/compose.env \
     -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
     -f compose.tickets-keycloak.yaml run --rm --no-deps auth-config-init
   docker --context desktop-linux compose \
     --env-file .env.docker --env-file ticket-app/.env \
     --env-file data/keycloak/compose.env \
     -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
     -f compose.tickets-keycloak.yaml restart app tickets
   ```

   Apri [Punto](https://punto.localhost/) e scegli **Accedi con l’account aziendale**. Dopo il login, apri [Tickets](https://ticket.localhost/) nello stesso browser e scegli lo stesso accesso: la sessione del provider consente il secondo ingresso senza reinserire la password. Entrambe le app mostrano i permessi degli account locali associati. Il login locale resta disponibile finché la modalità è `hybrid`.

## Gestione utenti esclusivamente in Keycloak

La modalità `AUTH_USER_MANAGEMENT=keycloak` centralizza creazione, modifica, credenziali, abilitazione e ruoli degli utenti. Negli overlay si attiva con `KEYCLOAK_USER_MANAGEMENT=keycloak` in `data/keycloak/compose.env`. È distinta da `AUTH_MODE`: lo stack locale usa `hybrid`, con accesso locale limitato al solo `KEYCLOAK_LOCAL_ADMIN_EMAIL=admin@punto.local`; `oidc` elimina anche questa possibilità. Senza la nuova opzione, la gestione locale precedente resta disponibile.

In questa modalità Punto e Tickets nascondono gli editor utenti e rifiutano le API locali di creazione/modifica. Il link **Gestione utenti in Keycloak** / **Utenti Keycloak** apre [la console del realm punto](https://auth.localhost/admin/punto/console/). Il ruolo amministrativo `realm-management → realm-admin` autorizza l'admin condiviso nella sola console di quel realm; i ruoli applicativi da soli non concedono accesso amministrativo al provider.

Per un nuovo utente, nella console crea il profilo con email, nome, cognome e credenziali, quindi assegna i ruoli nella scheda **Role mapping → Assign role → Filter by clients**:

| Client | Responsabile | Operatore |
| --- | --- | --- |
| `punto` | `manager` | `worker` |
| `punto-tickets` | `admin` | `agent` |

Assegna solo i client necessari. Nessun ruolo applicativo viene assegnato per default: il login a un'app senza uno dei suoi ruoli è rifiutato. Se entrambi i ruoli sono assegnati, prevale quello amministrativo. I client mantengono `fullScopeAllowed=false` e includono nel proprio ID token solo i rispettivi ruoli, tramite il claim firmato `app_roles`. Gli scope `profile` ed `email` devono essere abilitati.

Il primo SSO di un nuovo subject crea nell'app un profilo tecnico con password locale inutilizzabile. Nome, email, ruolo e abilitazione sono aggiornati alla callback SSO; le assegnazioni e lo storico usano l'ID locale stabile. La tabella `oidc_identities` conserva la coppia issuer/subject e impedisce che un secondo subject rivendichi lo stesso account. Un'email uguale a quella di un profilo preesistente non crea un'associazione: serve prima un binding amministrativo esplicito. Dopo la prima associazione, il subject persistente resta autorevole anche se l'email cambia.

Disabilitare l'utente o rimuovere i ruoli in Keycloak blocca i nuovi accessi. Le sessioni applicative OIDC già emesse scadono entro la validità del token (cinque minuti nel realm iniziale, limite massimo otto ore); non è implementata la revoca push. Il profilo locale può conservare l'ultima abilitazione nota per lo storico. L'admin di emergenza resta una credenziale locale indipendente: disabilitarlo nel provider non disabilita il fallback autorizzato da `AUTH_LOCAL_ADMIN_EMAIL`.

### Migrazione dello stack locale già avviato

Prima crea backup consistenti di MySQL, PostgreSQL e `data/keycloak`. Salva una fotografia JSON dagli endpoint autenticati `GET /api/users` di entrambe le app, nella forma `{"punto": [...], "tickets": [...]}`: ogni riga deve avere `id`, `email`, `name`, `role`, `active`. Il helper seguente riguarda lo stack con admin condiviso `admin@punto.local`, già associato esplicitamente a entrambe le app. Passa il suo subject verificato, senza sceglierlo tramite corrispondenza email:

```sh
python3 scripts/configure_keycloak_managed.py \
  --container projectmanament-keycloak-1 \
  --legacy-users-file data/keycloak/legacy-users-before-managed.json \
  --admin-subject SUBJECT-ADMIN-VERIFICATO
```

Il helper aggiunge ruoli, scope e mapper ai client esistenti mediante Admin API, migra esplicitamente ogni identità e registra i binding. Conserva gli ID locali e gli utenti già disabilitati; gli utenti del provider già esistenti richiedono manifest o binding esplicito. Un rerun non cambia password né ricrea identità precedentemente migrate ma mancanti. Gli account appena creati ricevono password casuali temporanee, salvate esclusivamente nel file privato `data/keycloak/credentials/migrated-users.json` (`0600`), da cambiare al primo accesso. Il manifest `managed-users-migration.json` registra subject e ID delle app senza password. Il JSON del realm viene aggiornato per le future installazioni; l'aggiornamento live passa dalle API, poiché `--import-realm` salta un realm già esistente.

Poi imposta in `data/keycloak/compose.env`:

```dotenv
KEYCLOAK_USER_MANAGEMENT=keycloak
KEYCLOAK_LOCAL_ADMIN_EMAIL=admin@punto.local
KEYCLOAK_USERS_ADMIN_URL=https://auth.localhost/admin/punto/console/
KEYCLOAK_AUTH_MODE=hybrid
KEYCLOAK_TICKETS_AUTH_MODE=hybrid
```

Mantieni l'admin scelto anche nelle variabili bootstrap locali `PUNTO_EMAIL/PUNTO_PASSWORD` e `TICKETS_EMAIL/TICKETS_PASSWORD`, usate soltanto su database vuoti. Copia i binding, compila e ricrea le app con tutti gli overlay:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml run --rm --no-deps auth-config-init
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml build app tickets
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml up -d --no-deps --no-build app tickets
```

Le sessioni delle app sono in memoria e vengono invalidate dalla ricreazione. Verifica SSO condiviso, ID locali preservati, editor assenti, API utenti in scrittura rifiutate, console amministrativa accessibile e fallback locale riservato all'admin. Per i successivi utenti nuovi usa direttamente la console; non serve rieseguire la migrazione né preparare profili nelle app.

## Gruppi e appartenenze esclusivamente in Keycloak

I gruppi operativi di Punto possono essere centralizzati separatamente dagli utenti. La modalità `AUTH_GROUP_MANAGEMENT=keycloak` richiede `AUTH_USER_MANAGEMENT=keycloak`; in Compose le opzioni corrispondono a `KEYCLOAK_GROUP_MANAGEMENT=keycloak` e `KEYCLOAK_USER_MANAGEMENT=keycloak` in `data/keycloak/compose.env`. Il valore predefinito senza attivazione è `local`. Aggiungi esplicitamente `compose.keycloak-groups.yaml` **per ultimo**, dopo gli overlay Keycloak e, se usati, Tickets/Tickets-Keycloak. Il solo file di esempio non attiva il servizio.

In questa modalità nomi, gerarchia e appartenenze si modificano soltanto nella [pagina Groups della console Keycloak](https://auth.localhost/admin/punto/console/#/punto/groups), disponibile all'admin del realm. La sezione **Gruppi e ruoli** di Punto resta consultabile, con albero e membri diretti, e mostra **Gestione gruppi in Keycloak**. Le API locali di creazione, modifica, eliminazione e aggiunta/rimozione membri rispondono `403`. Il catalogo dei ruoli sulle commesse e le abilitazioni di utenti e gruppi alle singole commesse continuano a essere gestiti in Punto.

L'overlay aggiunge `keycloak-groups-sync`, che legge la directory del realm ogni 30 secondi tramite HTTPS verificato e pubblica uno snapshot atomico in un volume condiviso. Punto legge quel volume in sola lettura; il servizio usa il client dedicato `punto-groups-sync`, con i soli ruoli di lettura `realm-management → query-groups` e `view-users`. Non riceve le credenziali bootstrap. Il segreto del client resta nel file protetto `data/keycloak/secrets/groups-client-secret`; il contenuto del realm non viene scritto nei log.

La coppia issuer/ID Keycloak identifica ogni gruppo, indipendentemente da nome e percorso. I binding espliciti della migrazione mantengono gli ID locali, le abilitazioni esistenti e lo storico. Un gruppo nuovo riceve un nuovo ID locale; eliminare un gruppo in Keycloak lo rende inattivo nella proiezione e rimuove le sue appartenenze operative, conservando i riferimenti storici. Ricreare un gruppo con lo stesso nome produce una nuova identità e non recupera le vecchie abilitazioni. Le appartenenze utenti usano i subject persistenti o i binding amministrativi già verificati; la corrispondenza email da sola non associa un account. Per un nuovo utente senza profilo locale, la proiezione dell'appartenenza viene completata dopo il primo SSO in Punto.

Le modifiche alla directory arrivano normalmente al ciclo successivo, circa 30 secondi. Gli utenti disabilitati in Keycloak sono esclusi dalle appartenenze autorizzative anche se il profilo locale conserva uno stato precedente. Uno snapshot mancante, non valido o vecchio di almeno 120 secondi disabilita l'accesso derivato dai gruppi fino alla successiva sincronizzazione valida; le assegnazioni dirette e i privilegi del gestore restano indipendenti. Sono supportati fino a 200 gruppi nel realm, 200 membri per gruppo, 16 livelli e nomi di massimo 120 caratteri. Il superamento di questi limiti interrompe la pubblicazione di nuovi snapshot.

Tickets non ha un catalogo locale di gruppi operativi. In Keycloak puoi assegnare a un gruppo i ruoli dei client `punto` e `punto-tickets`: i suoi membri li ereditano e le app li ricevono nel claim firmato `app_roles` al successivo SSO. Questi ruoli applicativi sono distinti dai ruoli e dalle abilitazioni sulle commesse di Punto.

### Migrazione e attivazione dei gruppi

Prima crea backup consistenti di MySQL, PostgreSQL e `data/keycloak`. Completa la migrazione utenti descritta sopra: `data/keycloak/managed-users-migration.json` deve contenere i subject espliciti di tutti i membri locali. Recupera i gruppi da `GET /api/groups` e, per ogni gruppo, i membri diretti da `GET /api/groups/ID/members`, usando una sessione gestore autenticata. Salva `data/keycloak/legacy-groups-before-managed.json` con permessi `0600`, trasformando `members` nella lista degli ID utenti locali:

```json
{"groups":[{"id":3,"name":"Area tecnica","parent_id":null,"members":[1,50]},{"id":4,"name":"Progettazione","parent_id":3,"members":[]}]}
```

Per un'installazione senza gruppi usa `{"groups":[]}`. Mantieni gli ID reali della tua installazione e includi anche i gruppi padre. Non sono ammessi cicli, membri duplicati o nomi uguali fra gruppi con lo stesso padre. La fotografia deve includere tutti i gruppi e solo le appartenenze dirette, senza espandere quelle ereditate.

```sh
python3 scripts/configure_keycloak_groups.py \
  --container projectmanament-keycloak-1 \
  --legacy-groups-file data/keycloak/legacy-groups-before-managed.json
```

Il helper crea i gruppi Keycloak e le appartenenze esplicite, registra `bindings/groups.json` e `managed-groups-migration.json`, prepara il client di lettura e salva il rapporto `managed-groups-report.json`. Usa `--users-manifest PERCORSO` se il manifest utenti si trova altrove e `--context NOME` per un altro contesto Docker. Non adotta gruppi preesistenti in base al nome e rifiuta un gruppo precedentemente migrato ma mancante. Un rerun non rinomina gruppi già completati né ripristina appartenenze successivamente rimosse nella console. Conserva manifest e binding insieme ai backup.

Imposta poi `KEYCLOAK_GROUP_MANAGEMENT=keycloak` in `data/keycloak/compose.env`, mantenendo `KEYCLOAK_USER_MANAGEMENT=keycloak`, e applica la configurazione con l'overlay finale:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml -f compose.keycloak-groups.yaml \
  run --rm --no-deps auth-config-init
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml -f compose.keycloak-groups.yaml \
  build app keycloak-groups-sync
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml -f compose.keycloak-groups.yaml \
  up -d --no-build keycloak-groups-sync app
```

Per Punto senza Tickets ometti `--env-file ticket-app/.env`, `compose.tickets.yaml` e `compose.tickets-keycloak.yaml`. La ricreazione di Punto invalida le sue sessioni in memoria. Verifica che il servizio di sincronizzazione sia healthy, gli ID dei gruppi e le abilitazioni siano conservati, gli editor locali siano assenti e le API in scrittura siano rifiutate; poi controlla dalla console una modifica di nome o appartenenza e la relativa proiezione in Punto.

Negli avvii e recuperi successivi conserva la selezione completa, per esempio `python3 scripts/manage_cluster.py --keycloak --keycloak-groups --tickets start` oppure `recover`. `--keycloak-groups` richiede `--keycloak`; il controller blocca un avvio che ometta l'overlay quando il servizio di sincronizzazione esiste già, anche arrestato. Per una migrazione reti usa `python3 scripts/configure_network.py --keycloak --keycloak-groups --tickets apply`. Anche nei comandi Compose manuali mantieni sempre `compose.keycloak-groups.yaml` per ultimo.

## Associazione esplicita con gestione locale

Questa sezione descrive `AUTH_USER_MANAGEMENT=local`, utile per migrazioni graduali in cui le app conservano la gestione dei ruoli.

Gli utenti già presenti in Punto o Tickets conservano i rispettivi ruoli e permessi. Crea l'identità in Keycloak, imposta le credenziali secondo la policy aziendale e recupera il suo ID utente, usato come claim `sub`. Associa quell'ID all'email di un **account locale esistente** in ciascuna app autorizzata:

```sh
python3 scripts/configure_keycloak.py bind \
  --app punto --subject ID-UTENTE-KEYCLOAK --email utente@example.test
python3 scripts/configure_keycloak.py bind \
  --app tickets --subject ID-UTENTE-KEYCLOAK --email utente@example.test
```

Il file prodotto ha formato `{"ID-UTENTE-KEYCLOAK": "utente@example.test"}`. Lo script prepara il mapping; l'app verifica l'esistenza e l'abilitazione dell'account locale al login. Il binding può anche essere fornito alla prima preparazione con `init ... --bind punto ID-UTENTE-KEYCLOAK utente@example.test`. Un subject già associato non può essere riassegnato a un'altra email senza rimozione esplicita con `unbind --app punto --subject ID-UTENTE-KEYCLOAK`.

L'email dichiarata da Keycloak non abilita alcun collegamento automatico. Il controllo dell'identità dipende dalla coppia issuer e subject verificata, dal mapping amministrativo e dalle autorizzazioni locali. Le password locali non vengono esportate o importate in Keycloak.

Per creare operativamente una nuova identità con un file password privato, puoi usare il helper amministrativo. Verifica prima gli account locali esistenti da associare e prepara una nuova password SSO in un file `0600`, senza inserirla negli argomenti del comando:

```sh
python3 scripts/manage_keycloak.py --container projectmanament-keycloak-1 \
  create-user --username mario.rossi --email mario.rossi@example.test \
  --name 'Mario Rossi' --password-file /percorso/protetto/password-sso \
  --bind punto mario.rossi@example.test \
  --bind tickets mario.ticket@example.test
```

Adatta il nome container al progetto Docker in esecuzione. Il helper usa l'amministratore bootstrap già configurato, verifica la CA interna e invia il payload delle credenziali via stdin. Password e token non entrano negli argomenti o nell'output. La sessione CLI amministrativa viene conservata in una directory temporanea nel container e rimossa alla fine. L'opzione `--temporary-password` richiede un cambio password al primo accesso.

Fornisci sempre nome e cognome, per esempio `--name 'Gestore Punto'`: il profilo predefinito del realm richiede entrambi. Il subject restituito viene registrato nel manifest protetto `data/keycloak/provisioned-users.json`, così una ripetizione non crea un secondo utente e non cambia la password dell'identità esistente. Un username preesistente privo di manifest richiede `--subject ID-VERIFICATO`; una semplice corrispondenza dell'email non basta. Il helper non modifica gli utenti già presenti, salva copie dei mapping precedenti in `data/keycloak/binding-backups/` e applica solo i binding forniti esplicitamente. Dopo la rimozione dell'amministratore bootstrap, il provisioning continua tramite un amministratore nominativo nel pannello del provider.

Dopo ogni modifica ai binding, copia il nuovo mapping nel volume runtime e riavvia l'app interessata, usando gli stessi file Compose e ambiente dello stack. Per Punto senza Tickets:

```sh
docker compose --env-file .env.docker --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.keycloak.yaml run --rm --no-deps auth-config-init
docker compose --env-file .env.docker --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.keycloak.yaml restart app
```

Per Tickets, includi anche i suoi file ambiente e overlay come nel comando di verifica precedente e riavvia `tickets`; se hai aggiornato entrambe le app, riavvia `app tickets`. Il solo riavvio dell'app non copia i file sorgente nei volumi. Le modifiche ai binding incidono sui login successivi; per revocare immediatamente un accesso, disabilita anche l'account nell'app, invalida le sue sessioni locali e termina la sessione Keycloak.

Verifica il login in entrambe le app, il secondo accesso tramite sessione SSO e il rifiuto di un subject privo di binding. Poi imposta `KEYCLOAK_AUTH_MODE=oidc` e, se usato, `KEYCLOAK_TICKETS_AUTH_MODE=oidc` in `data/keycloak/compose.env` e ricrea le app. `hybrid` resta disponibile per una transizione esplicita.

## Gestione successiva

Per riavviare o recuperare lo stack mantenendo l'autenticazione multi app usa il controller con la selezione esplicita degli overlay:

```sh
python3 scripts/manage_cluster.py --keycloak --tickets start
python3 scripts/manage_cluster.py --keycloak --tickets recover
python3 scripts/manage_cluster.py --keycloak --tickets status
python3 scripts/manage_cluster.py --keycloak --tickets backup
```

`start` segue lo stesso percorso di `recover`: verifica lo stato SQL, riunisce i nodi o esegue il bootstrap del nodo con GTID più avanzati soltanto quando la procedura esistente lo consente, poi avvia app, Tickets e reverse proxy. `status` legge lo stato dei tre nodi; `backup` salva MySQL, senza includere PostgreSQL Keycloak. Nessuna delle due operazioni avvia servizi.

Il controller compone i file nell'ordine base → Tickets → Keycloak → Tickets-Keycloak e carica gli ambienti base → Tickets → Keycloak. Usa `--keycloak` da solo quando non vuoi Tickets. I percorsi predefiniti sono `.env.docker`, `ticket-app/.env` e `data/keycloak/compose.env`; puoi sostituirli con `--env-file`, `--tickets-env-file` e `--keycloak-env-file`. Gli ultimi due richiedono anche il rispettivo flag di selezione. Su macOS il contesto predefinito è `desktop-linux`; per un altro daemon passa sempre `--docker-context NOME`.

La presenza di file Keycloak o di segreti non attiva automaticamente gli overlay. Ripeti i flag a ogni comando di avvio o recupero, anche dopo un `down`: se nel progetto esistono container Keycloak o Tickets, compresi quelli arrestati, il controller blocca un avvio che ometta i rispettivi overlay prima di modificare i container. Questo impedisce a un successivo `recover` della configurazione base di ricreare accidentalmente l'app con il solo login locale. Il controllo non ricostruisce una selezione precedente dopo la rimozione di tutti i container. Per altre operazioni Compose, compresi aggiornamenti delle immagini e ricreazioni delle app, conserva l'intero elenco di file e ambienti mostrato sopra.

Anche la migrazione esplicita delle reti inoltra la stessa selezione al controller che recupera e riavvia lo stack. Se devi applicare una modifica alle reti con SSO e Tickets installati, usa:

```sh
python3 scripts/configure_network.py --keycloak --tickets apply
```

Sono disponibili anche `--docker-context`, `--env-file`, `--keycloak-env-file` e `--tickets-env-file`, con lo stesso significato. `prepare` continua a preparare soltanto i file di rete esistenti e non attiva Keycloak.

L'importazione `--import-realm` riguarda la creazione iniziale: Keycloak salta un realm già presente nel database. Modificare il JSON sull'host o rieseguire `init` non aggiorna utenti, URL o segreti di un realm esistente. Per aggiungere Tickets dopo l'inizializzazione, abilita e configura manualmente il client `punto-tickets` in Keycloak con la sua origine e il suo redirect esatto, aggiorna le variabili `KEYCLOAK_TICKETS_*` e sincronizza il segreto del client con il file protetto; quindi riesegui `auth-config-init` e ricrea Tickets. Le rotazioni dei segreti richiedono lo stesso coordinamento tra provider e applicazione.

Il pulsante **Esci** termina la sessione di quell'app. La sessione SSO di Keycloak può consentire un successivo login senza chiedere nuovamente la password; per terminarla usa la gestione delle sessioni nel provider. Le sessioni locali delle app restano valide fino alla propria scadenza o revoca. Questa integrazione non implementa logout federato o backchannel logout. La modalità di gestione Keycloak sincronizza i ruoli alla callback SSO; la modalità di gestione locale usa invece i ruoli archiviati nelle app. La sessione locale creata tramite OIDC è limitata alla validità del token e a otto ore; con il realm iniziale la validità del token è di cinque minuti.

Esegui un backup consistente di PostgreSQL insieme a `data/keycloak`, proteggendo realm, CA, segreti e credenziali contenute nel dump. Non copiare i file del volume PostgreSQL mentre il database è attivo. Per un dump logico, usando gli stessi file di ambiente e overlay dello stack:

```sh
umask 077
mkdir -p backups/keycloak
docker compose --env-file .env.docker --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.keycloak.yaml exec -T keycloak-db \
  pg_dump --username=keycloak --dbname=keycloak --format=custom \
  > backups/keycloak/keycloak-YYYYMMDD.dump
tar -czf backups/keycloak/config-YYYYMMDD.tar.gz data/keycloak
```

Sostituisci `YYYYMMDD` con una data univoca e verifica l'esito dei comandi prima di considerare valido il backup. Conserva dump e archivio in una destinazione protetta, preferibilmente cifrata, e prova il ripristino in un ambiente separato. I backup MySQL e documentali già presenti nel progetto non includono automaticamente l'identità Keycloak. I volumi runtime contengono copie rigenerabili e non sostituiscono il backup delle sorgenti. L'autenticazione aggiunta è un'istanza singola: il cluster MySQL dell'app non rende Keycloak o PostgreSQL altamente disponibili.

Test offline disponibili:

```sh
python3 -m unittest discover -s tests -p 'test_keycloak_config.py' -v
```

Per verificare anche l'avvio in produzione, l'importazione dei due client e la disponibilità TLS contro Keycloak reale, abilita esplicitamente il test Docker:

```sh
PUNTO_KEYCLOAK_DOCKER_TESTS=1 python3 -m unittest discover \
  -s tests -p 'test_keycloak_runtime.py' -v
```

Il test crea un progetto Docker separato, con dati e credenziali temporanei, e rimuove container, reti e volumi del progetto alla fine. Può scaricare le immagini fissate e richiede il daemon Docker raggiungibile; lo stack e i dati applicativi reali non sono usati.

Riferimenti ufficiali verificati l'11 ottobre 2026: [Keycloak 26.8.0](https://www.keycloak.org/2026/10/keycloak-2680-released), [configurazione hostname e backchannel](https://www.keycloak.org/server/hostname), [reverse proxy](https://www.keycloak.org/server/reverseproxy), [interfaccia management](https://www.keycloak.org/server/management-interface), [bootstrap amministrativo](https://www.keycloak.org/server/bootstrap-admin-recovery) e [PostgreSQL 17.11](https://www.postgresql.org/docs/17/release-17-11.html).
