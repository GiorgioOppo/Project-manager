# Piano di implementazione delle 20 funzionalità di punto

> **Documento di pianificazione.** La prima stesura assumeva lo schema 3; il codice documentato al 10 ottobre 2026 usa lo schema 7. Le entità, le API e i rilasci proposti qui non attestano funzionalità già disponibili. Per il confronto aggiornato consulta [stato e roadmap](roadmap.md) e [panoramica del prodotto](panoramica.md). Analisi, tariffe, alert e addon sono stati aggiunti dopo la base iniziale di questo piano.

Le 20 aggiunte saranno realizzate in sette rilasci, preceduti da una base tecnica comune. Il primo rilascio semplifica la navigazione e rende comprensibili gli accessi; i successivi migliorano ore, censimento e deleghe, quindi introducono versioni contrattuali, bozze offline e lettura assistita dei PDF.

Ogni rilascio comprende backend, interfaccia, migrazioni, test e documentazione. I numeri delle funzionalità corrispondono alla proposta discussa. Le scelte descritte sono il progetto di implementazione; la base iniziale del piano era la versione 3; lo stato implementato è riepilogato nella roadmap aggiornata.

Il [piano del modulo finanziario](piano-modulo-finanziario.md) estende questa roadmap con più società, paesi e valute, includendo fatturazione e incassi nel primo rilascio finanziario. Le funzioni operative condividono il contesto societario e i controlli di accesso; i permessi economici sono separati da quelli delle commesse.

Dopo F1.0 i riferimenti al gestore globale indicano il gestore della singola società, entro le capacità assegnate. Nessun ruolo operativo mantiene un bypass tra società; le deleghe rimangono interne al medesimo ambito societario.

## Base iniziale del piano e vincoli

Il punto di partenza è il commit `2b16fe3`: server C, interfaccia JavaScript senza framework, MySQL 8.4 con tre nodi Group Replication, HAProxy, MongoDB GridFS e Nginx Proxy Manager. La modalità nativa usa SQLite e archivio documentale locale. MySQL conserva 14 tabelle applicative e di servizio nello schema 3.

Le abilitazioni delle commesse derivano da utenti e gruppi gerarchici, con ruoli Lettore, Collaboratore e personalizzati. Una stessa abilitazione deve permettere la registrazione e coprire sia oggi sia la giornata lavorata. I ruoli operativi sulle commesse sono distinti dai poteri amministrativi.

Le ore appartengono sempre a un utente stabile e sono oggi consultabili solo dal loro autore, anche per i gestori. Gli operatori possono aggiungerle; i gestori possono modificare o eliminare le proprie. La futura approvazione delle rettifiche richiede quindi un permesso specifico.

Le durate rimangono minuti interi; gli importi EUR esistenti restano centesimi. Il nuovo modulo finanziario usa unità monetarie minime ed esponente per valuta. Le giornate e gli intervalli restano date ISO; gli istanti tecnici e di audit sono UTC. Si rende esplicito il fuso aziendale, inizialmente `Europe/Rome` per la società legacy e configurabile per le altre, per determinare oggi e le settimane, evitando che nodi o browser prendano decisioni su giornate diverse.

L'app calcola una sola data aziendale per operazione, dopo aver acquisito la protezione della transazione di scrittura, e la passa a tutti i controlli. Data e fuso vengono restituiti alla UI; la decisione non dipende dall'orologio del browser o da `CURRENT_DATE` sui singoli nodi.

Gli originali PDF restano immutabili in GridFS o nell'archivio locale. Lo schema relazionale conserva riferimenti, metadati e audit. La segmentazione mantiene NPM come unico servizio pubblico; le nuove funzioni non richiedono uscita Internet dall'app. Google Calendar e la roadmap precedente restano sviluppi separati, integrabili attraverso API e scadenze condivise.

Gli eventuali connettori finanziari automatici hanno un componente dedicato con uscita limitata alle destinazioni configurate. Lo scambio manuale di file mantiene l'app isolata da Internet. Società, anagrafiche, ore, gruppi, ricerca e documenti richiedono isolamento coerente prima di attivare una seconda società.

## Sequenza dei rilasci

| Rilascio | Funzionalità | Risultato verificabile | Dipendenze |
| --- | --- | --- | --- |
| R0 | Base tecnica comune | Scritture riutilizzabili, revisioni, idempotenza e controlli di accesso condivisi | Schema 3 |
| R1 | 3, 6, 8, 9, 14 | Preferite, viste, comandi rapidi, interfaccia accessibile e spiegazione degli accessi | R0 |
| R2 | 1, 2, 4, 5 | Compilazione settimanale, copie controllate, rettifiche e avvisi sulle ore | R0 e componenti UI di R1 |
| R3 | 7, 11, 12, 13, 16 | Ricerca unica, importazione, operazioni multiple e creazione guidata | R0, R1; duplicati prima dell'importazione |
| R4 | 15 | Responsabili di area con amministrazione circoscritta | Matrice 14 e verifiche per risorsa |
| R5 | 17, 19, 20 | Revisioni contrattuali, obblighi e qualificazione fornitori | R3; autorizzazioni coerenti con R4 |
| R6 | 10 | App installabile e bozze offline da confermare online | R1, R2, idempotenza e politica dei dati locali |
| R7 | 18 | Estrazione locale del testo e proposte di compilazione dai PDF | R5 e coda persistente di elaborazione |

R6 e R7 sono indipendenti tra loro: il lavoro sulle bozze può procedere mentre si prepara l'elaborazione dei documenti. R5 non è un prerequisito delle bozze ore. In R1 i comandi rapidi cercano azioni e sezioni; R3 aggiunge la ricerca dei record alla stessa barra.

Il percorso finanziario parte da R0 e dalla separazione societaria di F1.0. Riutilizza R1 e integra progressivamente R2, R3, R4 e R5, senza aspettare tutti e sette i rilasci per fornire fatture e incassi. Anticipa in F1 revisioni, cancellazione logica e protezione delle ore valorizzate o fatturate, anche se le richieste di rettifica arrivano in R2. R6 mantiene inizialmente le bozze offline limitate alle ore.

La complessità è relativa: **bassa** per una modifica locale con dati già disponibili, **media** per un nuovo flusso con API e persistenza, **alta** per autorizzazioni circoscritte, concorrenza o nuovi processi di elaborazione. La stima in giorni si farà per il singolo rilascio dopo averne definito i dettagli e la capacità del team.

## R0 Base tecnica comune

1. Estrarre gradualmente dai gestori HTTP le operazioni di dominio e i validatori di commesse, ore, assegnazioni e contratti. Form, importazioni, wizard e batch chiameranno le stesse funzioni. Mantenere compatibili le API esistenti.
2. Introdurre un contesto di autorizzazione per attore, azione e risorsa. Riutilizzare `organization_job_access` e `organization_access_list`; aggiungere alle loro decisioni la spiegazione dei grant, evitando un secondo motore di permessi per ricerca e matrice.
3. Aggiungere revisioni numeriche ai record modificabili interessati da anteprime e correzioni. Ogni aggiornamento controlla la revisione attesa nella stessa transazione; un conflitto restituisce 409 e conserva la bozza sul client.
4. Introdurre `mutation_receipts`, con attore, UUID dell'operazione, impronta del contenuto ed esito minimo. Scrittura, ricevuta e audit sono atomici. Lo stesso UUID con lo stesso contenuto non crea un secondo effetto; con contenuto diverso restituisce 409. La durata delle ricevute deve coprire quella delle bozze e dei tentativi ammessi. Dopo la pulizia degli esiti dettagliati resta una traccia durevole delle chiavi consumate, così una vecchia bozza non viene applicata nuovamente. Il recupero dell'esito minimo di una scrittura già avvenuta è distinto da una nuova modifica, che richiede sempre autorizzazioni attuali.
5. Aggiungere corpi JSON strutturati alle nuove API multiple, con parser condiviso e limiti di dimensione, profondità, righe e campi. Mantenere i form attuali; convalidare UTF-8, numeri interi e campi sconosciuti. Le nuove rotte mantengono sessione e controlli di origine.
6. Dividere progressivamente `public/app.js` in moduli per API, azioni, preferenze, ore e organizzazione. Introdurre token CSS per colori, spaziatura, focus e densità senza una riscrittura completa dell'interfaccia.
7. Usare paginazione e filtri server nelle nuove ricerche. Conservare limiti espliciti: nessun elenco troncato silenziosamente e nessun incremento implicito dei limiti attuali.
8. Preparare il contesto societario condiviso dal piano finanziario. Preferenze, viste, autorizzazioni, importazioni e code portano la società quando trattano dati aziendali; il profilo operativo non diventa un bypass degli altri ambiti societari.

Le transazioni MySQL mantengono la protezione `cluster_write_guard`. Le migrazioni saranno eseguite una sola volta, con scritture applicative sospese e connessione a un nodo scelto, aspettando l'allineamento del gruppo prima della ripresa. In modalità multi-primary MySQL richiede particolare attenzione a DDL e DML concorrenti; le nuove relazioni continueranno a evitare operazioni `CASCADE`. [Documentazione MySQL 8.4](https://dev.mysql.com/doc/refman/8.4/en/group-replication-multi-primary-mode.html)

**Completamento R0:** aggiornamento da una copia dello schema 3 senza perdita di dati; un invio ripetuto produce un solo evento di dominio; un aggiornamento obsoleto viene rifiutato; gli endpoint esistenti conservano il comportamento precedente.

## R1 Navigazione e comprensione degli accessi

### Funzione 3 Commesse preferite e recenti

**Complessità bassa.** Aggiungere `user_job_preferences(user_id,job_id,is_favorite,last_used_at)`, con chiave composta. Le API `/api/me/job-preferences` operano sempre sull'utente autenticato. Aggiornare l'uso recente solo dopo un salvataggio ore riuscito; ordinare preferite e recenti prima delle altre commesse.

**Accettazione:** preferenze separate tra utenti; una commessa revocata scompare dalle scelte anche se preferita; un recente non concede accessi e un errore di salvataggio non lo aggiorna.

### Funzione 6 Viste personali salvate

**Complessità media.** Aggiungere `saved_views`, con proprietario, sezione, nome, configurazione versionata, vista predefinita e revisione. `/api/me/views` salva filtri, ordinamento e colonne tra un insieme di valori ammessi. Prima consegna con viste private per commesse, contratti e fornitori nelle sezioni accessibili.

**Accettazione:** la stessa vista si ritrova su un altro dispositivo; nessun accesso alle viste altrui; filtri incompatibili sono segnalati; i dati vengono sempre nuovamente filtrati dal server secondo i permessi attuali.

### Funzione 8 Comandi rapidi da tastiera

**Complessità bassa.** Creare un catalogo unico di azioni usato dai pulsanti e dalla barra `Ctrl/Cmd + K`. R1 include navigazione, nuova registrazione e apertura delle schermate consentite; R3 aggiunge risultati dalla ricerca globale. Le azioni rivalutano la disponibilità quando vengono eseguite.

**Accettazione:** apertura, selezione e chiusura complete da tastiera; Escape restituisce il focus; nessuna interferenza con scrittura o composizione del testo; una revoca intervenuta a barra aperta viene rispettata.

### Funzione 9 Interfaccia personalizzabile e accessibile

**Complessità media.** Aggiungere `user_preferences` e `/api/me/preferences` per tema chiaro, scuro o di sistema e densità normale o compatta. Completare i token CSS, gli stati di caricamento, gli errori collegati ai campi e gli annunci per tecnologie assistive. Preferenze estetiche possono avere un fallback locale senza dati aziendali.

**Accettazione:** percorsi principali utilizzabili da tastiera e smartphone; focus visibile, contrasto e zoom verificati; stato e ruolo riconoscibili anche senza colore; dialog e messaggi coerenti. Il riferimento di progetto è WCAG 2.2 livello AA, da verificare sui flussi reali. [WCAG 2.2](https://www.w3.org/TR/WCAG22/)

### Funzione 14 Matrice degli accessi effettivi

**Complessità media.** Esporre `/api/jobs/{id}/access-matrix` e `/api/users/{id}/effective-access`, inizialmente solo ai gestori. Mostrare accesso effettivo, ruolo, grant diretti e di gruppo, percorso ereditato e date. La consultazione è paginata; la data simulata serve solo all'anteprima, mentre il diritto dell'attore a consultarla è sempre verificato nel presente.

**Accettazione:** la matrice concorda con le API operative per ruoli alternativi, date inclusive e appartenenze multiple. La revoca di un grant evidenzia eventuali accessi rimasti. Una simulazione non modifica dati o autorizzazioni.

## R2 Registro ore più veloce e controllabile

### Funzione 1 Registro ore settimanale a griglia

**Complessità alta.** Estendere la lettura delle proprie ore con un intervallo di sette giorni, paginazione delle righe e totali. Ogni cella mostra il totale e permette di aprire le singole registrazioni con note e ID; l'inserimento in griglia aggiunge righe senza sostituire quelle già presenti. La prima consegna supporta lettura e inserimenti, mantenendo le modifiche nei flussi già consentiti.

La nuova API batch salva al massimo 100 nuove registrazioni per operazione, tutte nella stessa transazione. Controlla il totale finale di ogni giornata, il limite di registrazioni e il grant valido oggi e nella data lavorata.

**Accettazione:** totali uguali al registro giornaliero, più note preservate nella stessa cella, storico revocato leggibile ma non utilizzabile per nuove ore. Una riga non valida annulla tutto il batch; il doppio invio non duplica le ore.

### Funzione 2 Copia giornata o settimana precedente

**Complessità media.** Riutilizzare lettura settimanale e batch per preparare una bozza con destinazione esplicita. Offrire copia delle durate oppure anche delle note. Mostrare righe escluse o non valide, consentendone la rimozione o modifica prima della conferma.

**Accettazione:** origine invariata; controlli corretti a fine mese, cambio anno e anno bisestile; nessun salvataggio automatico; revoche o variazioni di ruolo tra anteprima e invio ricontrollate dal server.

### Funzione 4 Richiesta di correzione delle ore

**Complessità alta.** Aggiungere `time_entry_correction_requests` con autore, registrazione, revisione di partenza, valori proposti, motivazione, stato e revisore. Una sola richiesta pendente per registrazione; stati pendente, approvata, respinta, annullata e in conflitto. Le API dedicate consentono richiesta propria, elenco proprio, coda del revisore e decisione motivata.

Le eliminazioni delle ore diventano logiche tramite `deleted_at` e incremento della revisione: la riga rimane disponibile per richieste e audit, ma è esclusa dal registro corrente e dai totali. Si evita così che una richiesta storica impedisca una cancellazione o che questa lasci riferimenti non validi.

Introdurre la capacità specifica di approvare rettifiche: prima ai gestori globali, poi ai responsabili autorizzati in R4. La coda mostra solo i dati necessari alla decisione e non estende genericamente le API delle ore altrui. Impedire l'autoapprovazione; un gestore conserva il proprio flusso di modifica già esistente.

La correzione approvata controlla revisione, limiti giornalieri e permessi del revisore nella stessa transazione di ore, richiesta e audit. Dopo una revoca sono proponibili riduzione o cancellazione storica; aumenti e spostamenti richiedono un'abilitazione attuale valida per la destinazione. Il nome dell'autore della registrazione non cambia.

**Accettazione:** nessuna modifica prima dell'approvazione; seconda decisione senza ulteriori effetti; revisione modificata o registrazione eliminata produce conflitto; respingimento motivato; nessuna visualizzazione di richieste fuori ambito.

### Funzione 5 Controlli sulle anomalie del registro

**Complessità media.** Partire da avvisi su possibili duplicati e totali incoerenti. Aggiungere orari attesi per utente, periodi di validità ed eccezioni di calendario per rendere affidabile il rilevamento delle giornate incomplete. Tabelle proposte: `work_schedules`, `work_schedule_assignments`, `work_schedule_exceptions` e `time_policy_rules`.

Gli avvisi sono inizialmente informativi; rimane il limite duro di 24 ore al giorno. Lo schema conserva durata e giornata, quindi non si ricavano sovrapposizioni di fasce orarie. L'operatore vede i propri avvisi; il revisore vede quelli pertinenti alla sua coda e, dopo R4, all'ambito delegato.

**Accettazione:** part time, festività ed eccezioni non generano falsi blocchi; assenza di orario configurato segnalata; modifica dei dati aggiorna l'avviso; nessuna esposizione delle ore di altri utenti tramite conteggi o messaggi.

## R3 Ricerca e censimento con meno passaggi

### Funzione 16 Rilevamento dei duplicati

**Complessità media.** Realizzarla prima dell'importazione: riutilizzare codici univoci e SHA-256 dei PDF, indicizzare gli hash e aggiungere paese e partita IVA normalizzata ai fornitori. Mostrare candidati accessibili e la ragione della corrispondenza. Inizialmente partita IVA e hash uguali producono avvisi, mentre i codici univoci continuano a essere vincoli obbligatori.

**Accettazione:** PDF rinominato riconosciuto tramite hash; stesso nome con contenuto diverso resta distinto; nessuna fusione o eliminazione automatica; eventuale prosecuzione motivata è tracciata; controllo ripetuto al salvataggio.

### Funzione 7 Ricerca globale

**Complessità media.** Aggiungere `/api/search` per codici e testi di commesse, fornitori, contratti e metadati documentali. Applicare i permessi prima di risultati, conteggi e anteprime. Prima consegna con SQL parametrizzato e risultati paginati; l'indicizzazione del testo PDF arriva con R7. La ricerca degli utenti rimane una funzione amministrativa circoscritta.

**Accettazione:** l'operatore trova solo le commesse autorizzate; contratti e fornitori rispettano i loro permessi; Unicode e caratteri jolly sono gestiti; una ricerca senza risultati non rivela oggetti vietati.

### Funzione 11 Importazione guidata da CSV

**Complessità alta.** Implementare caricamento, associazione delle colonne, validazione, anteprima e conferma. Usare `operation_batches` e `operation_rows` per autore, tipo, contenuto, scadenza, errori e risultato, riutilizzandole per le operazioni multiple. API proposte `/api/imports`, `/{id}/preview` e `/{id}/commit`.

Primo incremento per fornitori e commesse; massimo 1000 righe in anteprima e 100 per conferma atomica. File più grandi richiedono partizioni esplicite con esito per ciascuna parte. Secondo incremento per utenti: importare anagrafica e profilo Operatore, senza password nel CSV. Aggiungere un'attivazione con token monouso, scadenza e impronta salvata in `account_enrollments`; il link può essere consegnato dal gestore senza introdurre invii email automatici. I privilegi di Gestore restano assegnati dal flusso amministrativo dedicato.

L'anagrafica rimane nell'enrollment fino all'attivazione, senza creare account autenticabili con credenziali provvisorie. La conferma valida un enrollment non scaduto o annullato, crea l'account con la password scelta e derivata secondo il metodo esistente e consuma il token nella stessa transazione. Un'email già registrata produce conflitto e non riattiva o sostituisce un account esistente. Il token completo viene mostrato una sola volta al gestore; non compare in CSV, audit o log.

**Accettazione:** virgolette, separatori, newline e UTF-8 validati; errori per riga e campo; conferma ripetuta senza duplicati; dati e permessi ricontrollati; materiali temporanei scaduti rimossi; credenziali escluse dai file e dall'audit.

### Funzione 12 Operazioni su più elementi

**Complessità alta.** Aggiungere selezione di commesse e anteprima per cambio stato, assegnazione e revoca di utenti o gruppi. `/api/jobs/bulk/preview` produce un'operazione persistente con revisioni attese; `/api/jobs/bulk/commit` verifica ambito ed eventuali modifiche intervenute. Primo limite: 100 commesse per transazione.

**Accettazione:** risultati tutti applicati oppure nessuno; anteprima obsoleta produce 409; audit per ogni oggetto con identificativo comune dell'operazione; nessuna revoca dichiarata completa se rimangono altri grant validi.

### Funzione 13 Creazione guidata e modelli di commessa

**Complessità media.** Creare un wizard contratto, dati commessa, abilitazioni e riepilogo. `job_templates` e `job_template_grants` conservano solo configurazioni esplicite e versionate. Il gestore può preparare un modello per tipologia di servizio, poi scegliere un codice nuovo e confermare i dati proposti.

**Accettazione:** creazione di commessa e grant atomica; contratto, fornitore, gruppi e ruoli ricontrollati alla conferma; riferimenti disattivati segnalati; ore e documenti non vengono copiati dal modello; nessun accesso individuale implicito.

## R4 Responsabilità amministrative per area

### Funzione 15 Responsabili di area con deleghe limitate

**Complessità alta.** Aggiungere `area_delegations`, con destinatario, gruppo radice, capacità, intervallo e ruoli assegnabili, e `job_management_scopes` per associare esplicitamente le commesse a un ambito amministrativo. Appartenenza a un gruppo, accesso operativo alla commessa e facoltà di amministrarla rimangono concetti distinti.

Prima consegna: il responsabile gestisce abilitazioni delle commesse nel proprio ambito e appartenenze interne al ramo; può approvare rettifiche solo con capacità esplicita. I destinatari delle nuove abilitazioni devono appartenere al ramo gestito; l'inserimento iniziale di persone esterne al ramo rimane al gestore globale. Un utente presente in più rami conserva intatte le appartenenze e le abilitazioni esterne.

Creazione o promozione di gestori, modifiche al catalogo ruoli, deleghe, spostamenti dei gruppi e subdeleghe rimangono al gestore globale. La delega non concede accesso all'archivio contratti. La UI riceve capacità per singola risorsa: la presenza di una delega non trasforma `manage_jobs` in un permesso globale.

**Accettazione:** nessuna operazione fuori ramo o sulle commesse soltanto visibili; nessuna autoestensione dei poteri; revoca valida alla richiesta successiva; spostamento del ramo ricalcola gli ambiti; modifiche e decisioni sempre in audit. Ricerca, matrice e operazioni multiple mostrano soltanto l'ambito autorizzato.

## R5 Contratti e fornitori strutturati

### Funzione 17 Versioni e integrazioni contrattuali

**Complessità alta.** Aggiungere `contract_revisions`, `contract_revision_documents` e `jobs.contract_revision_id`. Una revisione in bozza può essere preparata e poi pubblicata come snapshot immutabile di dati e riferimenti documentali. Contratto e revisioni hanno responsabilità distinte; le modifiche sostanziali di un contratto pubblicato generano una nuova revisione.

La migrazione crea una revisione iniziale dalla situazione corrente e collega le commesse già associate; questa fotografia non ricostruisce retroattivamente versioni mai registrate. I documenti originali restano riutilizzabili tra revisioni senza modificarne i metadati protetti.

**Accettazione:** una nuova revisione non cambia il riferimento delle commesse esistenti; cambio versione di una commessa esplicito e tracciato; pubblicazione concorrente controllata; codici, valori, date e PDF preesistenti conservati.

### Funzione 19 Registro degli obblighi contrattuali

**Complessità media.** Aggiungere `contract_obligations` e `obligation_evidence`, con revisione, tipo, descrizione, responsabile autorizzato, scadenza, anticipo avviso, stato ed evidenze. Prima consegna con agenda interna e indicatore delle scadenze nella UI; promemoria esterni e Google Calendar si collegheranno al modulo di integrazione previsto separatamente.

**Accettazione:** responsabilità e completamento tracciati; rinnovi e termini di preavviso inseriti o confermati dal gestore; evidenze collegate alla revisione corretta; scadenza superata non rinnova né chiude automaticamente il contratto. Un incarico su un obbligo non concede automaticamente accesso a tutto l'archivio.

### Funzione 20 Qualificazione dei fornitori

**Complessità alta.** Aggiungere checklist versionate con requisiti, obbligatorietà, validità, verifica ed evidenze. Tabelle proposte: `qualification_templates`, `qualification_requirements`, `supplier_qualifications` e `supplier_requirement_checks`. Introdurre metadati `supplier_documents` e un bucket dedicato, mantenendo compatibile quello dei contratti.

La prima consegna calcola uno stato informativo: incompleto, qualificato o scaduto. Un eventuale blocco dei nuovi incarichi sarà un secondo incremento configurabile, con deroga motivata. I criteri vengono definiti dall'organizzazione e non dedotti automaticamente da obblighi normativi.

**Accettazione:** documentazione scaduta aggiorna lo stato; revisione della checklist non riscrive verifiche storiche; evidenze leggibili solo agli autorizzati; contratti, commesse e ore già presenti vengono conservati.

## R6 App installabile e bozze offline

### Funzione 10 App installabile con bozze offline

**Complessità alta.** Introdurre manifest, service worker e cache versionata dei soli file statici dell'interfaccia. La PWA richiede HTTPS nell'ambiente pubblico; `localhost` consente lo sviluppo locale. [Service worker su MDN](https://developer.mozilla.org/en-US/docs/Web/API/Service_Worker_API/Using_Service_Workers)

Le bozze personali in IndexedDB contengono identificativo del proprietario, UUID, data, commessa, minuti, nota e stato. La conservazione locale è facoltativa per dispositivo, con scadenza e comando di cancellazione. Password, token di sessione, API autenticate e PDF non entrano nella cache. La progettazione deve chiarire quali contenuti delle note possono essere conservati e come proteggerli sul dispositivo prima di abilitare la funzione.

Al ritorno online si richiedono sessione valida, riepilogo e conferma. Il server applica i permessi attuali e restituisce conferma, rifiuto o conflitto per l'operazione. Un cambio account non può inviare bozze del precedente; logout e cambio account cancellano i dati locali dopo aver segnalato eventuali bozze non inviate. Un dispositivo disconnesso può conservare dati già scaricati fino alla successiva connessione: la revoca sul server impedisce nuove scritture, ma non cancella a distanza una copia offline.

Prevedere sincronizzazione dall'app aperta e pulsante manuale; Background Sync è solo un miglioramento facoltativo, poiché non ha supporto uniforme nei browser. [Background Synchronization API su MDN](https://developer.mozilla.org/en-US/docs/Web/API/Background_Synchronization_API)

**Accettazione:** perdita di rete dopo il commit senza duplicazioni; permesso revocato durante l'offline rispettato al rientro; nessun passaggio di bozze tra account; spazio esaurito e sessione scaduta gestiti; aggiornamento del service worker senza perdita delle bozze.

## R7 Lettura assistita dei documenti

### Funzione 18 Lettura e compilazione assistita dai PDF

**Complessità alta.** Aggiungere `document_processing_jobs`, con stato, documento, hash, versione dell'elaboratore, tentativi e scadenza del lavoro, e risultati versionati con pagina, estratto, valore proposto e conferma del gestore. Prima estrarre il testo già presente; per pagine scansite convertire il PDF in immagini e usare OCR locale. Tesseract richiede questa conversione perché non legge direttamente i PDF. [Formati di ingresso Tesseract](https://tesseract-ocr.github.io/tessdoc/InputFormats.html)

L'app C continua a gestire autenticazione, coda e persistenza. Un worker locale separato esegue estrazione e OCR con limiti di CPU, memoria, pagine, file e durata. La soluzione iniziale prevede `network_mode: none`, nessuna credenziale database e un volume di scambio dedicato: l'app esporta un PDF verificato e raccoglie un risultato validato. Il worker non entra nelle tre reti applicative. Nomi interni generati, nessun comando costruito con percorsi forniti dall'utente e pulizia dei temporanei fanno parte del flusso.

Ogni tentativo ha un identificativo e una scadenza di presa in carico. Il risultato deve corrispondere a lavoro, tentativo corrente, hash del PDF e versione dell'elaboratore; l'app rifiuta quelli tardivi appartenenti a un tentativo sostituito dopo un riavvio.

Originali in GridFS; testo derivato e risultati estesi in un bucket separato; stato, proposte e riferimenti in SQL. La prima ricerca del testo usa un indice controllato e ricostruibile, interrogato solo dopo il filtro sui documenti autorizzati. Non esiste una transazione distribuita SQL–MongoDB: usare stati di lavorazione, compensazione e riconciliazione per risultati incompleti o orfani.

**Accettazione:** PDF testuale e scansione elaborati senza cambiare l'originale; PDF protetto, malformato o oltre i limiti produce un esito esplicito; riavvio del worker recupera i lavori senza doppie applicazioni; importi e date sono proposte da confermare, con pagina di provenienza; nessun testo contrattuale nei log o nei risultati di utenti non autorizzati.

## Evoluzione dello schema e delle API

Le versioni definitive saranno assegnate alle migrazioni quando ciascun rilascio viene implementato. La seguente suddivisione descrive il contenuto, senza assumere che tutto debba essere applicato in un unico aggiornamento.

| Incremento | Dati da aggiungere o estendere |
| --- | --- |
| R0 | Revisioni sui record mutabili interessati; `mutation_receipts`; fuso aziendale esplicito |
| R1 | `user_preferences`, `user_job_preferences`, `saved_views`; matrice calcolata, senza copie permanenti dei permessi |
| R2 | `time_entry_correction_requests`, `time_entries.deleted_at` (anticipato in F1 per le fonti economiche); orari, assegnazioni, eccezioni e regole degli avvisi |
| R3 | `operation_batches`, `operation_rows`, `account_enrollments`, `job_templates`, `job_template_grants`; campi normalizzati e indici duplicati |
| R4 | `area_delegations`, capacità associate e `job_management_scopes` |
| R5 | Revisioni e riferimenti documentali; obblighi ed evidenze; checklist e documenti fornitori |
| R6 | Schema IndexedDB versionato; riuso delle ricevute server, senza duplicare il registro ore nel browser |
| R7 | Coda, metadati dei risultati, proposte e indice ricostruibile; bucket separato per testo derivato |

Tutte le nuove tabelle SQL avranno chiavi primarie, relazioni esplicite e migrazione equivalente SQLite/MySQL. Gli ID e i riferimenti esistenti saranno conservati. Le nuove risposte useranno campi aggiuntivi o endpoint dedicati; gli errori distingueranno validazione, divieto, anteprima scaduta e conflitto.

## Verifica e rilascio

Per ogni incremento applicare questi criteri alle parti effettivamente cambiate:

- Test di dominio e HTTP per il flusso principale, gli errori e il comportamento delle API precedenti.
- Prove dei permessi con gestore, operatore, lettore e, da R4, responsabile di area; revoche con sessione già aperta e tra anteprima e conferma.
- Migrazione da una copia dello schema 3 e dalle versioni intermedie, confrontando dati storici, relazioni e hash dei documenti.
- Verifica live MySQL per transazioni, audit e concorrenza. Per idempotenza e code, simulare interruzioni di rete e commit dall'esito incerto in ambiente di prova isolato.
- Verifica browser dei percorsi cambiati, tastiera, mobile ed errori recuperabili. Per R6 includere browser con e senza Background Sync; per R7 PDF testuali, scansiti e non elaborabili.
- Backup coordinato SQL e documenti prima delle migrazioni; verifica della concordanza dei tre nodi e dei servizi interessati dopo il rilascio. Il piano di ripristino specifica se basta tornare al codice precedente o se serve recuperare il backup; nessuna migrazione inversa distruttiva automatica.
- Commit distinti per incrementi coerenti, con migrazioni, test e istruzioni incluse. Un modulo è completato quando il flusso è utilizzabile dall'interfaccia e verificato sul backend previsto.

## Primo incremento da sviluppare

Avviare R0 con estrazione dei validatori, revisioni e protocollo di idempotenza, quindi R1 nell'ordine **tema e componenti accessibili → preferite → viste salvate → matrice degli accessi → comandi rapidi**. Le preferite costituiscono la prima consegna funzionale piccola; la matrice prepara le future deleghe.

Orario aziendale, contenuti conservabili offline, requisiti delle checklist e confini amministrativi sono impostazioni da definire quando si affronta il relativo rilascio. I valori iniziali del piano consentono di avanzare sui primi incrementi senza bloccarli su decisioni delle fasi successive.
