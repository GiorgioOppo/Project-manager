# Punto Ticket — gestione ticket in C

**Italiano** | [English](README.md)

L’interfaccia supporta italiano, inglese, cinese semplificato, spagnolo, francese e tedesco. Il selettore Lingua è disponibile prima e dopo l’accesso; la scelta resta nel browser e segue i collegamenti verso Punto. I cataloghi condivisi si aggiornano con `python3 scripts/sync_locales.py` dalla radice del progetto.

Il servizio Compose si chiama `tickets` e la sua immagine locale è `punto/tickets:${PUNTO_IMAGE_TAG:-local}`. Per aggiornare un’installazione con i nomi precedenti seguire la [guida nomenclatura Docker](../docs/tehnical/nomi-servizi-e-immagini.md); directory, volumi e alias di rete conservano i nomi già previsti.

App web autonoma in italiano, affiancata a Punto. Server HTTP in C11, archivio SQLite dedicato, interfaccia HTML/CSS/JavaScript senza npm. Account, sessioni e ticket hanno un archivio dedicato. Il collegamento facoltativo a Punto permette di associare commesse e asset ai ticket, rispettando i permessi della sessione Punto collegata. Il login può usare Keycloak in comune con le altre app, associando esplicitamente l’identità aziendale a un account Ticket locale attivo.

## Funzioni

- Apertura ticket con descrizione, richiedente, priorità, assegnatario e scadenza.
- Stati **Aperto**, **In lavorazione**, **In attesa**, **Risolto**, **Chiuso**, con possibilità di riapertura.
- Priorità **Bassa**, **Normale**, **Alta**, **Urgente**.
- Bacheca, elenco e agenda settimanale degli interventi, con ricerca e filtri combinabili.
- Programmazione con inizio, fine e fuso orario, distinta dalla scadenza del ticket.
- Sincronizzazione esplicita degli interventi con il calendario Google personale collegato a Punto, con coda persistente e controllo dei conflitti.
- Code rapide **Da lavorare**, **I miei**, **Non assegnati**, **Scaduti** e **In scadenza** nei prossimi sette giorni.
- Ordinamento per ultimo aggiornamento, priorità o scadenza; date e avvisi leggibili anche su mobile.
- Preferenze di vista e filtri conservate per account nel browser, senza salvare il testo delle ricerche.
- Collegamenti diretti ai ticket e azione **Prendi in carico** per assegnarsi una richiesta e avviarne la lavorazione.
- Commenti e storico delle modifiche, conservati insieme ai ticket.
- Riferimenti a commesse e asset reali, filtri dedicati e collegamenti in entrambe le direzioni.
- Gestori e operatori con account personali. Il gestore gestisce gli account Ticket; i ticket collegati seguono le abilitazioni della sessione Punto, anche per l’amministratore Ticket.
- Controllo della revisione: una modifica concorrente restituisce un conflitto e richiede di ricaricare il ticket.
- Ricezione IMAPS in un processo C separato, con caselle e mittenti autorizzati configurabili dal gestore.
- Casella **Messaggi RAW**, originali `.eml`, anteprima testuale, allegati e quarantena.
- Presa in carico, scarto motivato, conversione manuale o collegamento a un ticket esistente, con controllo dei duplicati e storico.

Il richiedente è un campo descrittivo, non un account esterno. La coda interna comprende ingresso RAW, conversazione cliente, note interne, SLA e ore effettive collegate a Punto. Il portale clienti resta esterno a questa app. I ticket si chiudono senza cancellare lo storico. Il collegamento ai cataloghi usa una delega di lettura della sessione Punto; l'accesso aziendale OIDC resta una configurazione separata.

La bacheca iniziale mostra il lavoro aperto nelle colonne **Aperto**, **In corso** e **In attesa**. La coda **Tutti** include anche risolti e chiusi. Ogni colonna carica venti ticket e permette di mostrarne altri; i conteggi includono tutti i risultati dei filtri, anche quelli ancora da caricare. Aprire una scheda per modificare stato, priorità, assegnatario e scadenza. I contatori delle code rappresentano tutti i ticket accessibili alla sessione, mentre il totale della vista e quelli delle colonne seguono i filtri selezionati.

Le code operative escludono risolti e chiusi. **Scaduti** contiene le scadenze precedenti a oggi; **In scadenza** comprende oggi e i sette giorni successivi. L’interfaccia usa la data di **Europe/Rome**. I link ai ticket richiedono comunque un account autenticato.

## Operatività dell’operatore (punti 10–20)

La vista **Operatore** affianca elenco e dettaglio e consente di passare alla richiesta successiva. Le bozze sono conservate per operatore sul server, con controllo della revisione; la presenza dei colleghi è temporanea. Le viste condivise memorizzano filtri e ordinamento, comprese le code **Da rispondere**, **In attesa cliente**, **SLA oggi**, **Interventi da programmare** e **Ricontatti scaduti**, mentre l’accesso ai risultati viene sempre ricalcolato sulla sessione Punto corrente.

Impatto e urgenza propongono la priorità: alto/alto dà urgente, almeno un valore alto dà alta, basso/basso dà bassa; gli altri casi danno normale. Una priorità diversa richiede una motivazione. Categoria e gruppo competente sono modificabili e le regole propongono anche la commessa da casella, mittente e testo: l’operatore conferma la classificazione e la conversione RAW.

Gli accordi SLA possono essere generali o specifici per commessa e priorità, con giorni lavorativi, intervallo giornaliero e fuso IANA. La configurazione iniziale è lunedì–venerdì 09:00–18:00 Europe/Rome. Le scadenze escludono il tempo fuori servizio, rispettano il cambio dell’ora e includono il tempo trascorso in RAW prima della conversione. La prima risposta si ferma soltanto dopo una risposta cliente inviata; le note interne non la soddisfano. Le attese indicano motivo e ricontatto e sospendono il tempo di risoluzione. Non sono precaricati calendari di festività o accordi contrattuali reali.

Le azioni multiple hanno una **anteprima** con richieste interessate ed escluse. La conferma controlla nuovamente permessi e revisioni e registra lo storico; un’anteprima scaduta o cambiata va rigenerata. Il **Cruscotto operativo** mostra arretrati RAW, richieste non assegnate, SLA, ricontatti, carico per gruppo, tempi e riaperture, sempre limitati alle richieste consultabili.

### Conversazione cliente e invio email

Nella scheda, **Risposta al cliente** conserva destinatario, oggetto, testo e stato; le **note interne** hanno un flusso distinto. I messaggi RAW collegati restano nella conversazione con l’originale disponibile. I modelli di risposta sono modificabili; una soluzione ricavata da un ticket risolto rimane associata al ticket e segue i suoi permessi.

**La casella reale resta da configurare.** Le bozze funzionano anche senza SMTP e l’app mostra l’invio non disponibile. L’invio richiede `MAIL_API_URL`, `MAIL_API_TOKEN_FILE`, `MAIL_API_CA_FILE` e `MAIL_API_NAMESPACE`, con TLS verificato verso l’addon C già usato da Punto. Nell’installazione privata, il file facoltativo [compose.tickets-mail.yaml](../compose.tickets-mail.yaml), aggiunto dopo `compose.yaml` e `compose.tickets.yaml`, collega Ticket alla sola API email privata; non abilita da sé una casella o SMTP. Usare un namespace stabile dedicato a Ticket. Cambi di servizio/namespace richiedono una nuova verifica delle bozze, non il reinvio di consegne dubbie.

Ogni risposta confermata ha chiave di consegna e Message-ID stabili, con In-Reply-To e References quando disponibili. Il registro dell’addon impedisce duplicati anche dopo un riavvio. Un invio interrotto o di esito ambiguo resta **da verificare** e non viene ritentato automaticamente. Prima della futura attivazione ricompilare sia Ticket sia l’addon email per il supporto alle intestazioni della conversazione.

### Ore effettive sulla commessa

Il pulsante di collegamento ore chiede un consenso Punto dedicato. Il destinatario delle ore è l’account Punto collegato, mostrato nel modulo; non si selezionano identità arbitrarie. Data, minuti e descrizione devono essere confermati dall’operatore: un intervento pianificato propone il contesto e non crea ore automaticamente.

La registrazione usa il flusso canonico delle ore di Punto: permessi della commessa alla data lavorata, chiusure, periodi bloccati, limite giornaliero e storico. Una chiave persistente permette di recuperare l’esito dopo un’interruzione senza duplicare le ore. Senza collegamento o consenso ore, il salvataggio sulla commessa è indisponibile.

## Avvio nativo

Dipendenze: compilatore C11, `make`, `pkg-config`, SQLite, OpenSSL, json-c, libcurl e GMime 3. Su macOS possono essere installate con `brew install pkg-config openssl@3 json-c sqlite curl gmime`; su Debian con `apt install build-essential pkg-config libssl-dev libsqlite3-dev libjson-c-dev libcurl4-openssl-dev libgmime-3.0-dev`. La compilazione produce l'app HTTP e il ricevitore `build/punto-ticket-receiver`.

Dalla radice del repository:

```sh
make tickets
cd ticket-app
export TICKETS_EMAIL='gestore@example.test'
export TICKETS_NAME='Gestore'
read -rs 'TICKETS_PASSWORD?Password iniziale (almeno 12 caratteri): '
export TICKETS_PASSWORD
./build/punto-tickets
```

Il comando `read` mostrato è per zsh, la shell predefinita di macOS. In bash usare `read -rs -p 'Password iniziale: ' TICKETS_PASSWORD` e poi `export TICKETS_PASSWORD`.

Aprire [http://localhost:8083](http://localhost:8083) e accedere con le credenziali scelte. Il server ascolta sul loopback; `Ctrl+C` lo arresta. Il database predefinito è `ticket-app/data/tickets.db` quando si avvia dalla cartella `ticket-app`. Per il primo avvio servono email e password di almeno 12 caratteri; su un database già inizializzato queste variabili non cambiano gli account. Le password si aggiornano dalla gestione utenti.

Per avviare nuovamente dalla radice:

```sh
make run-tickets
```

## Docker indipendente

```sh
cp ticket-app/.env.example ticket-app/.env
```

Compilare `TICKETS_EMAIL` e `TICKETS_PASSWORD` nel file locale, quindi:

```sh
docker compose --env-file ticket-app/.env -f ticket-app/compose.yaml up --build -d
docker compose --env-file ticket-app/.env -f ticket-app/compose.yaml ps
```

Aprire [http://localhost:8083](http://localhost:8083). La porta è pubblicata soltanto su `127.0.0.1`. Il volume `tickets_data` conserva SQLite; l'app gira con UID/GID `10006`, filesystem in sola lettura e nessuna dipendenza dai database di Punto. La modalità autonoma usa una rete bridge standard per consentire la pubblicazione della porta locale; il container può raggiungere la rete esterna. L'overlay affiancato a Punto usa invece la rete privata interna dello stack.

`TICKETS_HOST_PORT` cambia la porta host: aggiornare anche `TICKETS_PUBLIC_ORIGIN`, per esempio `http://localhost:8183`. L'origine è esatta, senza slash finale. Quando è impostata, usare nel browser il nome configurato.

## Affiancamento allo stack Punto e Nginx Proxy Manager

L'overlay facoltativo [compose.tickets.yaml](../compose.tickets.yaml) aggiunge il servizio `tickets` alla rete privata già usata da NPM, all'indirizzo `172.30.20.11:8083`, senza pubblicare porte host e senza collegarlo alle reti dei database. Lo stack Punto deve essere già configurato e avviato secondo il [README principale](../README.it.md).

1. Preparare `ticket-app/.env` come sopra e impostare `TICKETS_PUBLIC_ORIGIN` all'origine pubblica dedicata, ad esempio `https://ticket.tuaazienda.it`.
2. Avviare soltanto il nuovo servizio dalla radice del repository:

   ```sh
   docker compose --env-file .env.docker --env-file ticket-app/.env \
     -f compose.yaml -f compose.tickets.yaml up --build -d tickets
   ```

3. In Nginx Proxy Manager creare un **Proxy Host** per il nome scelto: schema `http`, destinazione `ticket-app` (alias del servizio `tickets`), porta `8083`. Conservare l'header Host del browser, come nella configurazione standard di NPM. Configurare il certificato per il dominio e l'inoltro HTTPS coerente con l'origine scelta.
4. Aprire l'origine configurata. Per consultare lo stato usare gli stessi file Compose con `ps tickets`.

Per configurare più indirizzi, sessioni separate e regole NPM ripetibili, vedere
la [guida alla pubblicazione di Ticket](../docs/tehnical/ticket-reverse-proxy.md).
`TICKETS_ADDITIONAL_ORIGINS` ammette fino a tre origini aggiuntive esatte, separate
da virgole senza spazi. Ogni Host deve avere un solo schema; gli ingressi HTTP
di un sito HTTPS vengono reindirizzati da Nginx. Il login aziendale rimane
disponibile soltanto sull'origine della redirect URI OIDC configurata.

I due file Compose descrivono modalità alternative. Quella indipendente e quella affiancata usano nomi di progetto e volumi effettivi potenzialmente diversi: non passare dall'una all'altra aspettandosi un'importazione automatica. Per spostare dati, arrestare l'app sorgente ed eseguire un backup e ripristino esplicito dell'archivio.

## Collegamento a commesse e asset

Configurare su Punto `TICKETS_APP_ORIGIN` con l’origine pubblica di Ticket e su Ticket `TICKETS_PUNTO_ORIGIN` con quella di Punto, senza slash finale. In produzione entrambe devono essere HTTPS. L’overlay `compose.tickets.yaml` passa la prima variabile a Punto e le variabili del connettore a Ticket. Con NPM sulla rete privata impostare anche `TICKETS_PUNTO_RESOLVE_IP=172.30.20.2`: il client raggiunge NPM direttamente mantenendo Host, SNI e verifica del certificato del dominio pubblico. Le CA pubbliche sono già installate nell’immagine. Per una CA privata montare il PEM in sola lettura con un override Compose e impostare `TICKETS_PUNTO_CA_FILE` nel servizio Ticket.

Dopo la modifica ricompilare e ricreare **entrambi** i servizi, usando gli stessi file e segreti dello stack:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  -f compose.yaml -f compose.tickets.yaml up --build -d app tickets
```

Per una prova nativa: Punto su `http://127.0.0.1:8080` con `TICKETS_APP_ORIGIN=http://127.0.0.1:8083`; Ticket con `TICKETS_PUNTO_ORIGIN=http://127.0.0.1:8080`. HTTP è ammesso soltanto per origini loopback. Non usare `127.0.0.1` per collegare due container.

1. Accedere a Ticket e premere **Collega Punto**. Il browser apre Punto e, se necessario, richiede l’accesso.
2. Tornando a Ticket, il riquadro mostra il nome dell’account Punto collegato. Il modulo ticket permette di cercare e scegliere una commessa e/o un asset. Non viene creata una relazione di proprietà fra commessa e asset.
3. **Ticket** e **+ Ticket** nella riga commessa, oppure **Ticket collegati** e **Nuovo ticket** nella scheda asset, aprono la coda filtrata o un modulo già contestualizzato. Nel dettaglio del ticket i riferimenti riportano alla commessa o all’asset.

Senza una sessione Punto collegata sono visibili soltanto i ticket senza riferimenti. Con il collegamento attivo sono visibili i ticket degli asset e delle commesse consultabili dall’account Punto. I permessi vengono verificati sul server a ogni richiesta, anche per dettagli, commenti, modifiche e contatori. Non si associano account per email o ID. La delega dei cataloghi è di sola lettura; calendario e registrazione ore richiedono ciascuno un consenso esplicito separato. La delega dura al massimo otto ore, resta in memoria ed è revocata da logout/scadenza della sessione Punto o dal riavvio di uno dei servizi; ripetere **Collega Punto**. **Scollega** la rimuove dalla sessione Ticket corrente.

Il server risolve ID e nomi dai cataloghi di Punto, attraverso le sue API di lettura; non accede ai database principali e non riceve il token privato dell’inventario. Codice e nome vengono conservati nel ticket come riferimenti storici. Modificare altri campi non sostituisce questi riferimenti. In caso di indisponibilità di Punto la lettura dei ticket collegati fallisce senza ampliare l’accesso; i ticket non collegati restano utilizzabili. Un asset non disponibile non impedisce di conservare un collegamento già salvato.

La migrazione SQLite dalla versione 1 alla 2 aggiunge riferimenti nullable e indici in una transazione; i ticket esistenti restano senza collegamenti. Effettuare il backup descritto sotto prima dell’aggiornamento. Il ripristino di una versione precedente dell’eseguibile richiede anche il ripristino del relativo database.

## Agenda, commesse e Google Calendar

Nel dettaglio del ticket impostare **Inizio intervento** e **Fine intervento** e salvare. La scadenza resta una data distinta: indica entro quando risolvere la richiesta, mentre l’intervento occupa un intervallo in agenda. Gli orari sono salvati come istanti UTC e visualizzati nel fuso indicato dal modulo; controllare il fuso prima di confermare.

La vista **Agenda** mostra la settimana e permette di navigare con **Oggi**, precedente e successiva. Mantiene i filtri di stato, priorità, assegnatario, commessa e asset e applica gli stessi permessi della bacheca. Carica automaticamente tutte le pagine disponibili, anche nelle settimane molto affollate. Gli interventi senza Google restano utilizzabili. Dalla riga commessa di Punto, **Agenda** apre direttamente gli interventi di quella commessa; dalla sezione **Calendario** si può scegliere la commessa e aprire l’agenda Ticket.

Per Google:

1. L’amministratore configura il servizio Calendar e il client OAuth secondo il [manuale Calendar](../calendar-addon/README.it.md). La configurazione di esempio lascia Google disabilitato; non sono necessari segreti Google nell’app Ticket.
2. In Punto aprire **Calendario**, scegliere **Collega Google Calendar** e completare il consenso nel proprio account Google. Selezionare un calendario modificabile con **Usa questo calendario**. **Sincronizza ora** richiede un aggiornamento; la schermata mostra l’ultimo completamento e le operazioni pendenti.
3. Nel ticket scegliere **Collega calendario** e autorizzare in Punto la delega calendario per la sessione corrente. Il normale **Collega Punto** mantiene la sola lettura dei cataloghi.
4. Salvare l’intervento e scegliere l’azione di sincronizzazione Google nel ticket. Il server compone l’evento con il riferimento al ticket e alla commessa. **In attesa** indica che l’operazione è accodata, non che Google l’ha già applicata. Aggiornare lo stato per consultarne l’esito.

Il calendario è personale dell’account Punto collegato, anche quando il ticket è assegnato a un’altra persona. Gli altri operatori possono consultare l’intervento interno se abilitati alla commessa; il ruolo di gestore non concede l’accesso al calendario Google di un altro utente. Il collegamento conserva proprietario, calendario, evento e operazione: cambiare account o calendario non sposta automaticamente un evento esistente.

Le modifiche locali richiedono una nuova sincronizzazione esplicita. Le modifiche fatte direttamente in Google non sovrascrivono automaticamente l’intervento interno: aggiornare lo stato e risolvere eventuali differenze nel ticket. Un conflitto ETag impedisce di sovrascrivere un evento modificato altrove. In caso di timeout, il server conserva la stessa operazione per verificarne l’esito senza inventare una nuova creazione. La rimozione da Google conserva il collegamento finché la cancellazione non è confermata; la programmazione interna è gestita separatamente.

Punto elenca gli eventi Google personali, con paginazione e serie ricorrenti separate; non espande automaticamente tutte le occorrenze. L’integrazione non genera ore lavorate, inviti o notifiche ai partecipanti. Prima di scollegare Google risolvere le operazioni pendenti: la disconnessione elimina cache e coda locali, mentre gli eventi già creati possono restare in Google.

Lo schema SQLite corrente è la versione 5 e conserva ticket, riferimenti, programmazione e metadati di sincronizzazione, oltre a RAW, operazioni e comunicazioni. La migrazione storica alla versione 3 ha introdotto i dati di programmazione e calendario; gli aggiornamenti successivi li preservano. Fare un backup prima dell’aggiornamento. Le deleghe di sessione restano temporanee e non entrano nel backup; al riavvio occorre ricollegare Punto. Il volume Calendar e la sua chiave di cifratura richiedono un backup separato dall’archivio Ticket.

## Ricezione email e messaggi RAW

Il percorso è **casella IMAP dedicata → verifica mittente → Messaggi RAW → valutazione → nuovo ticket o collegamento a un ticket esistente**. Ricevere un'email non crea automaticamente un ticket e non invia risposte. La ricezione reale resta disabilitata finché non si configurano e abilitano una casella e il processo ricevitore.

In **Messaggi RAW**, il gestore apre **Caselle e mittenti autorizzati** e definisce indirizzo destinatario, etichetta, host e porta IMAPS, utente, cartella, metodo `password` oppure `xoauth2` e nome del file credenziali. È possibile salvare una casella inattiva e completarla in seguito. Non inserire password o token nei campi dell'app: il nome identifica soltanto un file dentro `TICKETS_IMAP_SECRET_DIR`. Il ricevitore legge quel file sul server; password e token non vengono restituiti dalle API, conservati nel database o registrati nei log. I token OAuth2 richiedono rinnovo esterno; il ricevitore non esegue un nuovo consenso OAuth né rinnova autonomamente i token.

Per ogni casella aggiungere gli indirizzi esatti autorizzati, con eventuale scadenza e commessa. Sono esclusi wildcard e autorizzazioni implicite dell'intero dominio. Casella o mittente vincolati a una commessa limitano l'accesso e la conversione a quella commessa; l'operatore deve collegare una sessione Punto autorizzata. Un indirizzo autorizzato all'ingresso non riceve un account Ticket.

Configurare l'identificatore esatto del server che aggiunge `Authentication-Results` e confermare la fiducia soltanto se il provider elimina gli esiti contraffatti provenienti dall'esterno e aggiunge il proprio risultato. L'elenco mittenti da solo non impedisce la falsificazione del campo `From`. Il ricevitore richiede un mittente singolo autorizzato e un esito DMARC valido e coerente con il dominio; i controlli di dominio non attestano da soli l'identità della singola persona. Esiti assenti, ambigui, non fidati o negativi e mittenti non autorizzati producono quarantena, consultabile soltanto dai gestori e non convertibile. La configurazione del provider deve rispettare il confine di fiducia descritto da [RFC 8601](https://www.rfc-editor.org/rfc/rfc8601.html).

Il ricevitore è un programma separato: non apre porte HTTP, non introduce un server SMTP e non blocca le richieste degli operatori. Usa IMAPS con verifica TLS del certificato e del nome server; non cancella i messaggi remoti. Stato di ricezione, cursore UID e identificativo della generazione IMAP sono persistenti. Ripetere un download o riavviare il processo conserva la stessa origine RAW; il solo `Message-ID` non viene usato come prova di unicità.

### Avvio del ricevitore

Preparare una directory protetta e i file credenziali con i nomi configurati nelle caselle. In Docker devono essere leggibili dall'UID `10006`; evitare accesso ad altri utenti e non includerli nel repository. Impostare `TICKETS_IMAP_SECRETS_PATH` al percorso assoluto della directory. L'overlay monta la directory in sola lettura e riutilizza il volume `tickets_data` della stessa installazione.

Per Ticket indipendente, dalla radice del repository:

```sh
docker compose --env-file ticket-app/.env \
  -f ticket-app/compose.yaml -f ticket-app/compose.inbound.yaml \
  up --build -d tickets tickets-receiver
```

Nello stack Punto privato, sostituire prima il segnaposto in `docker/squid/allowed-imap-domains.txt` con il solo hostname pubblico IMAP autorizzato. Il proxy consente al ricevitore `172.30.40.13` soltanto tunnel verso tale lista sulla porta `993`; conserva il blocco di destinazioni private, indirizzi IP e altre porte. Applicare la configurazione del proxy quando si abilita la ricezione:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.tickets-inbound.yaml \
  up --build -d proxy-egress tickets tickets-receiver
```

In nativo avviare prima l'app, che migra l'archivio, e poi, da `ticket-app`, il ricevitore con lo stesso `TICKETS_DB_PATH` e `TICKETS_IMAP_SECRET_DIR`. `TICKETS_IMAP_POLL_SECONDS` regola l'intervallo, predefinito 30 secondi; `TICKETS_IMAP_CA_FILE` permette una CA privata verificata. L'eventuale `TICKETS_IMAP_PROXY_URL` deve indicare il proxy HTTP esplicito per i tunnel TLS. `--once` esegue un ciclo; un'importazione locale con `--import-file` conserva un messaggio non verificato in quarantena e non aggira l'autenticazione del provider.

### Lavorazione dell'operatore

La casella mostra **Nuovo**, **In revisione**, **Convertito**, **Collegato**, **Scartato** e, per i gestori, **Quarantena**. Aprire un messaggio per leggerne anteprima, controlli, allegati e storico; **Prendi in carico** lo riserva all'operatore. **Rilascia** lo rende nuovamente disponibile. Lo scarto richiede una motivazione e conserva l'originale.

**Converti in ticket** precompila titolo, descrizione e richiedente, lasciando all'operatore scelta di priorità, assegnatario, scadenza, commessa, asset e allegati da mantenere. **Collega a ticket** cerca una richiesta esistente e richiede conferma. Creazione o collegamento, allegati selezionati e storico vengono registrati nella stessa transazione. Ritentativi e conversioni concorrenti non creano un secondo ticket. Le autorizzazioni vengono controllate nuovamente alla conferma; un mittente revocato o scaduto non viene autorizzato dal precedente download.

L'originale è immutabile e scaricabile come `.eml`; l'app mostra testo e non esegue l'HTML ricevuto né carica immagini esterne. Limiti: originale 10 MiB, massimo 20 allegati, singolo allegato 5 MiB e totale decodificato 10 MiB. Il download è autenticato e forzato come allegato. Non è incluso un antivirus: la disponibilità di un file non certifica che sia innocuo. Il parser limita inoltre la struttura a 128 parti e 16 livelli e il corpo testuale a 64 KiB; errori e superamenti vanno in quarantena conservando l'originale entro il limite. Se l'originale supera 10 MiB, il ricevitore interrompe il download e segnala l'errore sulla casella senza avanzare oltre quell'UID: occorre spostare o ridurre il messaggio presso il provider per riprendere la ricezione della casella. Le altre caselle continuano a essere elaborate.

I messaggi non vincolati sono condivisi con il team interno; quelli associati a commesse seguono i permessi Punto. Dopo conversione o collegamento, anche il RAW richiede l'accesso al ticket di destinazione. I file trasferiti sono disponibili dal ticket; quelli non selezionati restano nell'originale. In questa versione gli originali e lo storico sono conservati senza scadenza automatica: dimensionare e proteggere il volume e definire la conservazione operativa prima dell'uso reale.

La corrispondenza cliente richiede l'accesso corrente a tutte le sorgenti RAW collegate, perché una risposta può citarne il contenuto. Se anche una sorgente diventa privata o l'accesso viene revocato, conversazione, bozze e invio restituiscono 404. Il ticket locale e le note interne condivise restano disponibili; i ticket senza sorgenti RAW seguono i propri permessi.

L'aggiornamento porta SQLite alla versione 5 senza eliminare ticket e collegamenti esistenti. App e ricevitore devono usare lo stesso archivio e la stessa versione dello schema. Per un backup a servizio fermo arrestare anche `tickets-receiver`, se attivo; la copia contiene originali email e allegati, mentre i file credenziali richiedono custodia separata.

## Configurazione

| Variabile | Predefinito nativo | Uso |
| --- | --- | --- |
| `TICKETS_BIND` | `127.0.0.1` | Indirizzo IP numerico di ascolto |
| `TICKETS_PORT` | `8083` | Porta HTTP interna |
| `TICKETS_PUBLIC_DIR` | `public` | Cartella dell'interfaccia, relativa alla directory di avvio |
| `TICKETS_DB_PATH` | `data/tickets.db` | Database dedicato; la directory viene creata se necessario |
| `TICKETS_PUBLIC_ORIGIN` | Origini loopback sulla porta configurata | Origine HTTP/HTTPS esatta autorizzata, senza percorso |
| `TICKETS_EMAIL` | Nessuno | Email del primo gestore |
| `TICKETS_PASSWORD` | Nessuno | Password del primo gestore, almeno 12 caratteri |
| `TICKETS_NAME` | `Gestore` | Nome del primo gestore |
| `TICKETS_PUNTO_ORIGIN` | Disabilitato | Origine HTTPS di Punto; HTTP ammesso solo sul loopback |
| `TICKETS_PUNTO_RESOLVE_IP` | Risoluzione DNS | IPv4 fissa facoltativa per raggiungere l’origine HTTPS, mantenendo la verifica TLS |
| `TICKETS_PUNTO_CA_FILE` | CA di sistema | CA PEM privata facoltativa, leggibile dal processo |
| `AUTH_MODE` | `local` | `local`, `hybrid` (locale + Keycloak), `oidc` (solo Keycloak) |
| `OIDC_ISSUER_URL` | Nessuno | Issuer pubblico esatto del realm Keycloak, ad esempio `https://auth.example.it/realms/punto` |
| `OIDC_BACKCHANNEL_URL` | Issuer pubblico | URL del realm raggiungibile dal server per token e chiavi; deve corrispondere allo stesso realm |
| `OIDC_CLIENT_ID` | Nessuno | Client OIDC dedicato a Ticket |
| `OIDC_CLIENT_SECRET_FILE` | Nessuno | File del secret del client, leggibile dal processo Ticket |
| `OIDC_REDIRECT_URI` | Nessuno | URL pubblico esatto di `/auth/callback` su Ticket |
| `OIDC_BINDINGS_FILE` | Nessuno | File JSON che associa i subject Keycloak a email locali Ticket |
| `OIDC_CA_FILE` | CA di sistema | CA PEM per le connessioni HTTPS al provider |
| `OIDC_ALLOW_HTTP` | `0` | Abilita HTTP esplicitamente solo per prove locali |

Le password sono conservate come derivazioni PBKDF2 con sale casuale. Le sessioni risiedono in memoria e scadono dopo otto ore; quelle create tramite Keycloak hanno anche il limite di durata del token verificato. Un riavvio richiede un nuovo accesso. Il browser riceve cookie HttpOnly e SameSite=Strict; con origine HTTPS viene aggiunto Secure. Le scritture richiedono il token CSRF restituito dalla sessione. Il server controlla Host e Origin, usa query preparate e registra ticket e storico nella stessa transazione.

In `hybrid` e `oidc` il browser apre il login Keycloak e torna al callback con un codice monouso; token e secret restano sul server. La transazione usa state, nonce, PKCE e un cookie temporaneo HttpOnly SameSite=Lax. Il modulo condiviso verifica il token e risolve il subject tramite il file di associazioni; Ticket richiede poi che l’account locale sia attivo. I ruoli `admin`/`agent` rimangono quelli dell’archivio Ticket. L’email dichiarata nel token non crea o collega account automaticamente. Una configurazione incompleta impedisce l’avvio prima dell’apertura del database.

Il file `OIDC_BINDINGS_FILE` contiene un oggetto JSON, per esempio `{"subject-keycloak-del-gestore":"gestore@example.test"}`. Usare il valore `sub` stabile dell’utente nel realm configurato, non il nome utente o l’email Keycloak. Il file è specifico dell’app: la stessa identità può essere associata a un account diverso in Punto. Conservare e montare in sola lettura sia questo file sia il secret del client; una modifica viene applicata riavviando Ticket.

Per Docker multi app usare gli overlay Keycloak e la configurazione `KEYCLOAK_*` generata da `scripts/configure_keycloak.py`. L’overlay Ticket usa il backchannel TLS `https://keycloak:8443/realms/punto` e monta `/run/punto-auth/client-secret`, `/run/punto-auth/bindings.json` e `/run/punto-auth/ca.crt`. Le variabili della tabella sono quelle lette dal processo; per un avvio nativo esportarle con percorsi assoluti dell’host. Il Compose Ticket indipendente mantiene il login locale.

Il primo account e gli altri utenti si preparano come prima, senza cambiare gli ID o importare credenziali da Punto. In `oidc` il form locale è nascosto e `POST /api/login` restituisce `403`; le password locali conservate nell’archivio non sono utilizzabili finché questa modalità resta attiva. Il pulsante **Esci** chiude soltanto la sessione Ticket: la sessione Keycloak e le altre app restano attive, quindi un nuovo accesso aziendale può essere immediato.

Il server nativo parla HTTP. Il TLS pubblico viene terminato dal reverse proxy. Per l'esposizione remota configurare un'origine HTTPS dedicata e il relativo proxy.

## API

Gli endpoint operativi aggiunti usano JSON, sessione e CSRF come gli altri endpoint Ticket:

| Metodo e percorso | Funzione |
| --- | --- |
| `POST /api/operations/suggest`, `GET /api/raw/messages/:id/suggestions` | Suggerimenti di classificazione, senza conversione automatica |
| `GET/POST /api/operations/rules`, `PUT /api/operations/rules/:id` | Regole revisionate; scrittura amministratore |
| `GET/POST /api/operations/sla-policies`, `PUT /api/operations/sla-policies/:id` | Accordi SLA e calendario lavorativo |
| `GET/POST /api/operations/views`, `PUT /api/operations/views/:id` | Viste condivise, modifica proprietario o amministratore |
| `GET/POST /api/tickets/:id/presence`, `PUT /api/tickets/:id/draft`, `POST /api/tickets/:id/draft/discard` | Presenza temporanea e bozze personali revisionate |
| `POST /api/tickets/bulk/preview`, `POST /api/tickets/bulk/apply` | Anteprima temporanea legata alla sessione, conferma e replay |
| `GET /api/reports/operations` | Cruscotto filtrato dai permessi correnti |
| `GET /api/tickets/:id/conversation`, `POST /api/tickets/:id/messages`, `PUT /api/tickets/:id/messages/:message` | Conversazione e bozze cliente |
| `POST /api/tickets/:id/messages/:message/send` | Invio esplicito della revisione confermata; richiede SMTP configurato |
| `GET/POST /api/tickets/:id/time` | Proposta e registrazioni ore effettive con chiave persistente e conferma |
| `GET/POST /api/response-templates`, `PUT /api/response-templates/:id` | Modelli revisionati |
| `GET /api/solutions`, `POST /api/tickets/:id/solution` | Soluzioni derivate da ticket risolti e soggette ai relativi permessi |

Il bridge Punto aggiunge `time=1` al consenso di delega e `scope.time_access`; gli endpoint bearer `GET /api/integrations/tickets/time/status`, `GET .../time/entries?job_id=N&request_key=K` e `POST .../time/create` sono separati dal calendario. La creazione accetta form `request_key` ed `entries` JSON con una sola riga (`job_id,date,minutes,note`); rifiuta identità selezionate dal chiamante e controlla l’ACL anche sui replay. La chiave viene isolata con prefisso `ticket-` nel registro canonico delle ore.

La cronologia ore nel ticket è personale dell’account Punto attualmente collegato: richiede il consenso ore ancora valido, la stessa origine Punto e l’accesso corrente alla commessa di ogni registrazione. Leggere un ticket non concede accesso alle ore di altri account. Un tentativo in attesa o dall’esito incerto può essere ripreso con la chiave originale soltanto dall’autore Ticket, per lo stesso account Punto e la stessa commessa; richiede una nuova conferma e non genera automaticamente una nuova registrazione.


Le API restituiscono JSON. Gli errori hanno forma `{"ok":false,"error":"messaggio"}`. Login e creazione usano `POST`; gli aggiornamenti `PUT`. Per le mutazioni autenticate inviare `Content-Type: application/json`, il cookie di sessione e `X-CSRF-Token`.

| Metodo e percorso | Contenuto / risultato |
| --- | --- |
| `GET /healthz` | Salute del servizio, senza autenticazione |
| `GET /api/auth/config` | `{sso,local}`; modalità di accesso pubbliche, senza secret |
| `GET /auth/login` | Avvia l’accesso aziendale con redirect HTTP `303` e cookie temporaneo |
| `GET /auth/callback` | Verifica il codice OIDC, crea una sessione Ticket e redirige a `/` |
| `POST /api/login` | `{email,password}` → `{ok,user,csrf_token}` e cookie; `403` in modalità `oidc` |
| `GET /api/session` | Utente e token CSRF; `401` senza sessione |
| `POST /api/logout` | Termina la sessione corrente |
| `GET /api/users` | `{items}`; gli operatori vedono gli account attivi |
| `POST /api/users` | Solo gestori: `{name,email,password,role}` → `{user}` |
| `PUT /api/users/:id` | Solo gestori: campi `name`, `role`, `active`, `password` opzionali |
| `GET /api/integrations/punto/status` | `{configured,connected,user_name,expires_at,origin}` della sessione corrente |
| `POST /api/integrations/punto/start` | `{}` → `{state,url}` per aprire Punto |
| `POST /api/integrations/punto/connect` | `{state,grant}` → stato; richiede lo state pendente della stessa sessione |
| `POST /api/integrations/punto/disconnect` | `{}` → stato scollegato |
| `GET /api/integrations/punto/jobs` | Catalogo commesse abilitate; `search`, `limit`, `offset` |
| `GET /api/integrations/punto/assets` | Catalogo asset; `q`, `limit`, `after_id` |
| `GET /api/integrations/punto/validate` | `job_id` e/o `asset_id` → `{job,asset}` canonici |
| `GET /api/tickets` | `{items,total,stats,queues,reference_date}`; parametri `search`, `status`, `priority`, `assignee_id`, `job_id`, `asset_id`, `queue`, `sort`, `today`, `limit`, `offset` |
| `POST /api/tickets` | `{title,description,requester,priority,assignee_id,due_date,job_id,asset_id}` → `{ticket}` |
| `GET /api/tickets/:id` | `{ticket,comments,history,raw_sources,attachments}` |
| `PUT /api/tickets/:id` | Campi del ticket, `status` opzionale e `revision` obbligatoria → `{ticket}` |
| `POST /api/tickets/:id/comments` | `{body}` → `{comment}` |
| `GET /api/raw/messages` | Coda `{items,total,stats,mailboxes}`; filtri `state`, `search`, `mailbox_id`, `limit`, `offset` |
| `GET /api/raw/messages/:id` | `{raw,attachments,history,eml_url}` con verifica dei permessi |
| `GET /api/raw/messages/:id/eml` | Originale immutabile, download autenticato |
| `GET /api/raw/messages/:id/attachments/:aid` | Allegato RAW, download autenticato |
| `POST /api/raw/messages/:id/claim` | `{revision}` → presa in carico |
| `POST /api/raw/messages/:id/release` | `{revision}` → ritorno alla coda |
| `POST /api/raw/messages/:id/discard` | `{revision,reason}` → scarto motivato |
| `POST /api/raw/messages/:id/convert` | `{revision,ticket,attachment_ids}` → `{ticket,raw,replayed}` |
| `POST /api/raw/messages/:id/link` | `{revision,ticket_id,attachment_ids}` → `{ticket,raw,replayed}` |
| `GET /api/tickets/:id/attachments/:aid` | Solo allegati selezionati, con permessi sul ticket e RAW |
| `GET /api/raw/mailboxes`, `GET /api/raw/senders` | Solo gestori; elenco configurazioni; mittenti filtrabili per `mailbox_id` |
| `POST /api/raw/mailboxes`, `POST /api/raw/senders` | Solo gestori; creazione configurazione senza segreti |
| `PUT /api/raw/mailboxes/:id`, `PUT /api/raw/senders/:id` | Solo gestori; aggiornamento con `revision` obbligatoria |

Ruoli: `admin`, `agent`. Stati: `open`, `in_progress`, `waiting`, `resolved`, `closed`. Priorità: `low`, `normal`, `high`, `urgent`. La scadenza è una data valida `YYYY-MM-DD` oppure `null`; l'assegnatario è l'ID di un account attivo oppure `null`. Il filtro `assignee_id=unassigned` seleziona i ticket non assegnati. I contatori `stats` e `queues` si riferiscono a tutti i ticket visibili all’account, indipendentemente dai filtri. La revisione protegge gli aggiornamenti del ticket; un commento aggiorna la data dell’ultima attività senza modificare la revisione.

`job_id` e `asset_id` sono ID interi positivi oppure `null` nelle scritture; nei filtri sono ID positivi. I nomi non sono accettati dal browser: il server aggiunge `job_code`, `job_name`, `asset_tag`, `asset_name` dai cataloghi. La ricerca comprende questi riferimenti. Un aggiornamento che omette gli ID conserva i riferimenti esistenti; `null` li rimuove. Le risposte dei cataloghi contengono soltanto ID, codice/tag, nome, stato e tipologia dell’asset; nessuna credenziale dell’inventario.

Punto espone `POST /api/integrations/tickets/grant` alla sua sessione autenticata, con form `state` di 64 caratteri esadecimali minuscoli e verifica di origine. Le letture `/api/integrations/tickets/{scope,jobs,assets,validate}` richiedono la delega opaca Bearer, mai un ID utente scelto dal client. Nessuna di queste API modifica commesse o asset.

`queue` accetta `all` (predefinito), `active`, `mine`, `unassigned`, `overdue` e `due_soon`. Tutte le code eccetto `all` includono solo gli stati attivi. `mine` usa l’utente della sessione. I filtri si combinano con AND. `today` è una data valida `YYYY-MM-DD`: il browser invia quella italiana; se omessa, l’API usa il giorno UTC e lo restituisce in `reference_date`. `due_soon` include gli estremi da `today` a `today + 7 giorni`.

`sort` accetta `updated` (predefinito, più recenti prima), `priority` (urgente, alta, normale, bassa, poi scadenza) e `due` (scadenze più vicine, poi priorità). Le scadenze assenti vengono dopo quelle presenti; aggiornamento e ID decrescenti risolvono le parità. `limit` va da 1 a 100, `offset` da 0 a 1.000.000. Parametri sconosciuti, duplicati o non validi restituiscono `400`.

Il filtro aggiuntivo `operations_queue` accetta `all`, `needs_reply`, `waiting_customer`, `sla_today`, `unscheduled` e `follow_up_due`. Si combina con gli altri filtri e viene conservato nelle viste condivise. Le code operative comprendono solo ticket attivi; **SLA oggi** usa il fuso dell’accordo del ticket. Le note interne e le bozze non contano come risposte al cliente.

## Verifica e backup

```sh
make test-tickets
```

I test compilano i server e usano database temporanei, credenziali sintetiche e porte loopback, senza scrivere nei dati di Punto. Coprono workflow, permessi, commenti, storico, revisioni, persistenza, code operative, scadenze, ordinamento, controlli HTTP, migrazione dello schema e collegamenti ai cataloghi con revoca dei permessi e indisponibilità delle sorgenti. Le prove RAW coprono presa in carico, conversione concorrente, collegamento, selezione degli allegati e ACL correnti; quelle del ricevitore usano veri server IMAPS TLS sintetici, incluse interruzioni, UID, MIME, autenticazione, password/XOAUTH2 e tunnel CONNECT. Non richiedono una casella reale.

La suite `tests/test_tickets_oidc.py` usa un provider OIDC locale con vere firme RSA; non richiede un Keycloak esterno. Verifica collegamento esplicito agli account, ruoli locali, CSRF, durata e revoca della sessione, riuso dei callback, modalità `oidc`, account disattivati, subject sconosciuti e firme non valide.

Le suite operative verificano matrice di priorità, motivazioni, attese e ricontatti, ore lavorative e cambio dell’ora, snapshot SLA, regole soggette ai permessi, bozze e presenza, azioni multiple con anteprima e revisioni, dashboard, modelli e soluzioni. Le comunicazioni usano un servizio HTTPS sintetico per verificare threading, revisioni, esiti incerti e assenza di duplicati. Il bridge ore viene provato anche contro Punto reale con database temporaneo, revoca dei permessi, consenso distinto dal calendario e recupero idempotente; nessuna prova richiede posta reale. Per verificare anche le intestazioni SMTP dell’addon eseguire `python3 -m unittest discover -s tests -p 'test_mail_addon.py' -v`.

Per il backup più semplice arrestare `tickets` e `tickets-receiver`, se attivo, copiare l'intera directory dati o il volume dedicato, quindi riavviare i servizi. Per backup a caldo usare l'API SQLite Backup o il comando `.backup` di SQLite: copiare il solo file `.db` mentre il processo scrive può perdere dati presenti nel WAL. Conservare il backup in una posizione protetta perché contiene ticket, commenti, account, originali email e allegati.
