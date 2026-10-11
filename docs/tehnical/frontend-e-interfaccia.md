# Frontend: struttura, stato e interazione

**Italiano** | [English](../en/tehnical/frontend-e-interfaccia.md)

Riferimento: codice dell’**11 ottobre 2026**. Questa guida è destinata a chi modifica l’interfaccia. Le istruzioni per usare il prodotto sono nelle guide [utente](../prod/guida-utente.md) e [gestore](../prod/guida-gestore.md); il contratto delle richieste è in [API app](api-app.md).

## File e dipendenze

| File | Responsabilità |
| --- | --- |
| [index.html](../../public/index.html) | Struttura del login, campi, messaggi e contenuti introduttivi |
| [login.js](../../public/login.js) | Validazione del login, invio, ricordo email e redirect |
| [app.html](../../public/app.html) | Struttura dell’area autenticata, pannelli, moduli e dialog nativi |
| [app.js](../../public/app.js) | Stato in memoria, caricamento dati, navigazione, rendering e salvataggi |
| [styles.css](../../public/styles.css) | Variabili visive, regole generali e pagina di accesso |
| [workspace.css](../../public/workspace.css) | Area di lavoro, tabelle, dialog, organizzazione, contratti e notifiche |
| [reports.css](../../public/reports.css) | Filtri, schede, tabelle e tariffe delle analisi |
| [work-board.css](../../public/work-board.css) | Colonne Kanban, elenco gerarchico, filtri, schede dei blocchi e layout responsive della nuova sezione |
| [i18n.js](../../public/i18n.js), [i18n.css](../../public/i18n.css) | Scelta lingua, cataloghi locali, testi statici marcati e selettore accessibile |
| `public/locales/{en,zh,es,fr,de}.json` | Traduzioni complete dell’interfaccia e degli errori riconosciuti |
| [favicon.svg](../../public/favicon.svg) | Identità grafica del browser |

Non ci sono un framework UI, un bundler o un processo npm per generare le pagine. Gli script sono IIFE in modalità strict; funzioni e stato non sono pubblicati come API globali. Gli ID HTML costituiscono un contratto diretto con `getElementById`: rinominare un campo richiede di aggiornare il JavaScript che lo usa.

L’interfaccia richiede JavaScript e API browser moderne, fra cui `fetch`, `AbortController`, `URLSearchParams`, `BigInt`, `dialog.showModal()` e `matchMedia`. Il repository non definisce una matrice certificata di versioni browser né un insieme di polyfill. Una nuova dipendenza va quindi valutata e collaudata sui browser scelti per la distribuzione.

## Localizzazione

`i18n.js` viene caricato prima degli script applicativi. `PuntoI18n.ready` attende il catalogo della lingua selezionata prima di inizializzare etichette, formati `Intl` e dati; l’italiano usa direttamente i messaggi sorgente. La selezione segue `lang` valido nell’URL, preferenza `punto.language` del browser, lingue supportate di `navigator.languages`, quindi italiano. Il caricamento fallito rende esplicito il ripiego sull’italiano e conserva utilizzabile l’accesso. Storage indisponibile e parametri sconosciuti non bloccano la pagina.

Nell’HTML `data-i18n` traduce soltanto i nodi di testo diretti dell’elemento marcato; `data-i18n-attrs` elenca gli attributi traducibili (`placeholder`, `aria-label`, `title`, descrizione meta). Nel JavaScript `t("Messaggio", [valore0, valore1])` sostituisce i placeholder `{0}`, `{1}` tramite callback, senza interpretare HTML o sequenze di sostituzione dei valori. Conservare gli spazi esterni dei frammenti concatenati. Non passare a `t` nomi o note utente, classi CSS, percorsi API, enum, filtri, identificativi o formati CSV. I nomi dei ruoli predefiniti possono essere tradotti; quelli dei ruoli personalizzati restano originali.

Gli errori API sono localizzati al confine del client, conservando `sourceMessage` quando una regola preesistente deve ancora distinguere una risposta tramite il testo originale. La preferenza della lingua non modifica autorizzazioni o payload. `PuntoI18n.url` aggiunge la lingua ai collegamenti tra app; i QR mantengono un URL canonico indipendente dalla lingua. `Intl` presenta date e importi nella lingua attiva, conservando valuta EUR, date ISO e fusi orari delle regole applicative.

Punto Ticket distribuisce una copia autonoma del runtime e dei cataloghi. Dopo le modifiche eseguire `python3 scripts/sync_locales.py`; `python3 scripts/sync_locales.py --check` e `python3 -m unittest discover -s tests -p test_i18n.py -v` verificano cataloghi, placeholder, copertura dei testi marcati e copie. Il test browser facoltativo documentato nel README verifica le sei lingue su server temporanei, accesso locale/SSO, moduli, dati utente, cambio lingua, ripiego e layout mobile.

## Avvio e durata dello stato

```mermaid
sequenceDiagram
    participant B as Browser
    participant UI as app.js
    participant A as App C
    B->>UI: Caricamento /app
    UI->>A: GET /api/session
    A-->>UI: Identità e permessi correnti
    alt Sessione valida
        UI->>UI: applySession e selezione rotta
        UI->>A: Commesse, ore, notifiche e anagrafiche consentite
        A-->>UI: Risposte JSON
        UI->>UI: Aggiornamento state e rendering
    else Sessione assente
        UI-->>B: Redirect al login
    end
```

`initialize()` attende la sessione prima di mostrare `workspace`; interpreta poi l’hash e richiama `refreshAll()`. Questo carica commesse, ore e notifiche e, se consentito, utenti, fornitori e contratti. Attività/blocchi, organizzazione, storico, alert e analisi hanno anche caricamenti associati al pannello attivo. Non tutte le letture sono effettuate esclusivamente alla prima apertura della scheda.

Lo stato dei record vive nell’oggetto `state`, nella memoria della pagina. Un ricaricamento ricostruisce le liste dal server. Non esiste un archivio offline dei record, delle password o dei token Google nel frontend. Il login usa `localStorage` soltanto per l’email ricordata, sotto `punto.rememberedEmail`; errori o indisponibilità di questo storage non devono bloccare l’accesso.

La casella «ricordami» ha due effetti distinti: il login invia `remember=on` al server per la durata della sessione e conserva l’email nel browser dopo il successo. La password non viene scritta in `localStorage`. Il pulsante dimostrativo compila i valori dimostrativi del sorgente: non scopre né modifica le credenziali del deployment.

## Navigazione e URL

| Hash / area | Disponibilità |
| --- | --- |
| `#jobs`, `#work-board`, `#hours`, `#notifications` | Utenti autenticati, con dati nel proprio ambito |
| `#suppliers`, `#contracts`, `#users`, `#organization`, `#audit`, `#alerts` | Gestori, secondo i permessi e i controlli specifici |
| `#analisi/periodo`, `#analisi/commesse`, `#analisi/utenti` | Analisi globale per gestori, personale per operatori |
| `#analisi/costi`, `#analisi/costo-ore` | Gestori |

Le viste Analisi scrivono nell’hash `from`, `to` e, quando presenti, `job_id` e `user_id`. La vista `costi` corrisponde a `view=cost`; `costo-ore` a `view=labor_cost`. Il frontend traduce questi nomi prima della richiesta HTTP.

Esempio sintetico:

```text
/app#analisi/commesse?from=2026-10-01&to=2026-10-31&job_id=123
```

Il frammento consente di ritrovare i filtri, non concede l’accesso ai dati e non è una vista salvata nell’account. Il server riceve i filtri nella successiva richiesta API e verifica l’autorizzazione. Browser history, condivisione dello schermo e copia dell’indirizzo possono rendere visibili i filtri e gli identificativi presenti nell’URL.

`hashchange` e `popstate` ricostruiscono il pannello. Una scheda non disponibile ricade su Commesse; una vista analitica non consentita ricade su Per periodo. Il filtro utente viene rimosso per l’operatore. Dopo login o scadenza viene mantenuto solo un hash Analisi riconosciuto lungo al massimo 2048 caratteri, non una destinazione arbitraria.

## Client HTTP ed errori

`api()` centralizza le chiamate con cookie della stessa origine, `cache: no-store`, parsing JSON e timeout di 15 secondi. `post()` usa `URLSearchParams` con form URL-encoded; i PDF sono inviati come corpo binario `application/pdf`, con timeout di 60 secondi.

| Condizione | Comportamento del frontend |
| --- | --- |
| `401` | Redirect al login e messaggio di sessione scaduta |
| `403` | Avviso e aggiornamento della sessione, salvo ricorsione sulla richiesta sessione |
| JSON non valido | Errore esplicito, senza usare la risposta come dati |
| Risposta HTTP fallita o `ok:false` | Messaggio business restituito dal server o testo di ripiego |
| Timeout | Interruzione dell’attesa browser e messaggio di connessione/tempo |
| Errore di rete | Messaggio di servizio non raggiungibile |

L’interruzione della richiesta browser non prova l’annullamento di una transazione sul server. Il frontend non implementa una chiave generale di idempotenza per commesse, ore e anagrafiche: dopo un esito incerto verificare l’elenco prima di ripetere un inserimento. Il pulsante disabilitato evita doppi clic ordinari, ma non garantisce unicità fra due schede, retry o dispositivi.

Le mutazioni riuscite sulle risorse gestionali invalidano i report. Le funzioni specifiche ricaricano poi liste, statistiche o audit pertinenti. Non applicare un aggiornamento ottimistico permanente prima di conoscere l’esito del backend.

## Richieste concorrenti e revoche

Lo stato contiene contatori come `sessionRequest`, `entriesRequest`, `reportRequest`, `documentsRequest`, `organizationRequest` e `workRequest`. Ogni caricamento rilevante conserva la propria generazione e confronta il contatore prima di applicare il risultato. Per le analisi verifica anche vista, periodo e pannello ancora attivi.

Esempio: l’utente richiede ottobre e subito novembre; se ottobre arriva per ultimo, non deve sostituire il risultato di novembre. Questo controllo scarta risposte obsolete nella UI; non annulla necessariamente il lavoro HTTP già in corso.

`applySession()` rileva cambi di email, ruolo e mappa permessi. Invalida generazioni, pulisce dati sensibili, chiude dialog non più ammessi e aggiorna i comandi disponibili. I moduli verificano nuovamente i permessi anche quando ricevono una risposta asincrona. Il backend rimane l’autorità: una revoca in un’altra sessione può essere riconosciuta prima dal server che dalla pagina già aperta.

Non è implementato un canale push generale delle modifiche o dei permessi. Il timer periodico aggiorna le notifiche ogni 60 secondi solo quando il documento è visibile e la sessione risulta autenticata; non aggiorna tutte le anagrafiche ogni minuto. La risposta `403` e i caricamenti espliciti fanno parte del riallineamento della pagina.

## Attività, Kanban e blocchi

La sezione `#work-board` seleziona una commessa dall’elenco autorizzato già caricato; il comando «Attività» presente su ogni commessa apre direttamente quel contesto. La selezione della commessa, i filtri e la modalità Kanban/elenco restano in memoria: non sono serializzati nell’hash né salvati nell’account.

`refreshWorkBoard()` interroga `GET /api/work-board?job_id=...` e controlla ID della commessa, generazione `workRequest` e contesto `workEpoch` prima di applicare la risposta. Durante un caricamento svuota dati, permessi e schede precedenti. Cambi di identità, permessi o commessa invalidano il contesto; una risposta arrivata dopo il cambio non ripristina dati obsoleti. Le mutazioni verificano lo stesso contesto prima di aggiornare la pagina.

Il Kanban presenta cinque colonne fisse. Il cambio di stato usa un select e un pulsante «Applica», accessibili anche da tastiera; non richiede trascinamento e non implementa ordinamento manuale delle schede. L’elenco alternativo rende la gerarchia padre/figli. Ricerca testuale, stato e priorità filtrano le attività già ricevute; il registro blocchi ha un filtro distinto per aperti/in gestione, risolti o tutti. Le schede mostrano assegnatario, scadenza, descrizione e numero di blocchi aperti collegati. Non ricavano avanzamento dalle ore.

I comandi seguono `permissions.manage_tasks`, `permissions.report_blockers`, `can_edit` e `can_update_status` restituiti dal server. Gli operatori assegnati inviano soltanto `status` e `revision` per le attività; i dialog del gestore inviano i campi completi. Gli assegnatari già presenti ma non più eleggibili restano visibili come «accesso da verificare», evitando riassegnazioni involontarie.

`workBusy` disabilita filtri, cambio commessa e azioni durante il salvataggio. Ogni update/delete invia la revisione letta. Un `409` nel dialog di modifica conserva i dati immessi, disabilita il salvataggio e offre il ricaricamento; questo chiude il dialog e scarta la bozza prima di leggere la versione corrente. Nei cambi di stato e nelle cancellazioni il conflitto porta alla rilettura della bacheca. La UI non ripete automaticamente la mutazione e non fonde versioni concorrenti.

I testi vengono inseriti con `textContent`; le descrizioni usano espansori `<details>`. Le colonne hanno scorrimento orizzontale su viewport stretti; le schede blocco passano a una colonna e i dialog a campi sovrapposti. La [specifica tecnica del modulo](attivita-kanban-e-blocchi.md) definisce limiti e autorizzazioni lato server.

## Rendering e formati

Le funzioni `node()` e i renderer costruiscono elementi DOM e usano `textContent` per i valori applicativi. Le tabelle sono ricostruite con `replaceChildren()`. Conservare questo approccio per nomi, note, descrizioni e messaggi: un contenuto memorizzato nel database non diventa HTML fidato.

| Tipo | Conversione / presentazione |
| --- | --- |
| Ore | `hoursToMinutes()` converte durate con virgola, punto o `H:MM` in minuti interi; usa `BigInt` per rifiutare frazioni di minuto |
| Importi | `valueToCents()` converte il testo del modulo in centesimi; la visualizzazione EUR segue la lingua attiva dell’interfaccia |
| Giornata | Stringa `YYYY-MM-DD`; validazione calendario esplicita, senza affidarsi alla normalizzazione permissiva di `Date` |
| Report | Periodo inclusivo massimo 366 giorni; gli ID dei filtri devono essere interi JavaScript sicuri |
| Date alert | Formattazione in `Europe/Rome` |
| Audit | Formattazione nel fuso del browser |

`1,30` ore significa 78 minuti; `1:30` significa 90. Non sostituire il parser con `parseFloat(value) * 60` senza preservare il requisito dei minuti interi. Le validazioni HTML e JS aiutano la compilazione, ma possono avere limiti diversi da quelli del server: per esempio l’upload UI scarta file vuoti mentre il backend richiede anche firma `%PDF-` e almeno cinque byte.

Il giorno iniziale del Registro ore deriva dal browser. I periodi rapidi di Analisi diventano disponibili con `today` restituito dal server; la prima richiesta può essere ripetuta per allineare il periodo predefinito. Nessuna conversione automatica trasforma una data di lavoro in una fascia oraria.

## Dialog, tastiera e feedback

I moduli usano `<dialog>`, `showModal()`, etichette associate ai campi, messaggi di validazione e comandi espliciti di chiusura. `busyDialogs` tiene traccia dei salvataggi: disabilita i controlli e impedisce chiusure con Escape o click esterno durante l’attesa. Alla fine il blocco viene rimosso anche nel percorso di errore.

La navigazione principale aggiorna `aria-selected`, `tabindex`, pannelli `hidden` e titolo pagina. Frecce, Home ed End attraversano solo le schede consentite. Le schede Analisi hanno gestione tastiera separata. Sotto 760 px l’orientamento ARIA della navigazione diventa orizzontale.

I messaggi ordinari hanno ruolo `status` e si chiudono dopo sette secondi; gli errori usano ruolo `alert` e rimangono fino a un’azione successiva o chiusura. Le aree in caricamento espongono `aria-busy`; stati vuoti ed errori sono distinti. Non sostituire uno stato fallito con «nessun dato».

I CSS includono focus visibile, regole responsive e riduzione delle animazioni con `prefers-reduced-motion`. Questo documenta controlli presenti, non certifica una conformità completa di accessibilità: servono verifiche dei flussi con tastiera, lettore di schermo, ingrandimento e contrasto.

## Percorso di modifica consigliato

Questi sono criteri di sviluppo, non nuove funzioni già implementate:

1. Individuare pannello e dialog in HTML, stato e caricamento in JS, autorizzazione nell’API.
2. Definire caricamento, vuoto, errore, successo e assenza di permesso prima di aggiungere il renderer.
3. Inserire i nuovi dati come testo, validare URL e ID secondo la risorsa e conservare i controlli server.
4. Usare un contatore di richiesta per dati sostituibili e invalidarlo su cambio identità o contesto.
5. Gestire navigazione indietro, riapertura dialog, doppio clic, timeout e risposta arrivata dopo una revoca.
6. Aggiornare le dipendenze di invalidazione quando la modifica cambia report o liste correlate.
7. Collaudare i ruoli coinvolti, mobile e desktop con dati sintetici; consultare [collaudo di accettazione](../prod/collaudo-accettazione.md).

Il file `app.js` è attualmente monolitico. Una futura suddivisione in moduli dovrà preservare queste interazioni; non esiste oggi una libreria di componenti con API autonoma. Calendario grafico, tema personalizzabile, offline e comandi globali da tastiera restano nella [roadmap](../prod/roadmap.md).
