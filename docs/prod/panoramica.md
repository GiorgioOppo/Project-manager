# Panoramica di prodotto

**Italiano** | [English](../en/prod/panoramica.md)

Stato documentato: **11 ottobre 2026**, codice applicativo con schema dati **13**. Questa pagina descrive le funzioni presenti; la [roadmap](roadmap.md) distingue le estensioni ancora da realizzare.


Le dieci migliorie operative sono descritte nei [flussi migliorati](flussi-operativi.md): settimana, rettifiche, ore sulle attività, vista personale, orari, accessi, riepilogo, notifiche, avvio guidato e archivio.
## Scopo

**punto** organizza commesse, persone e tempo lavorato in un unico spazio. Il gestore definisce quali commesse possono essere utilizzate e controlla il lavoro della propria squadra. Commerciali gestisce fornitori e contratti; Finance consulta valori e costi aggregati. L’operatore sceglie una commessa autorizzata e registra il lavoro della giornata, senza dover creare codici o anagrafiche.

Il percorso principale è:

```mermaid
flowchart LR
  F[Fornitore] --> C[Contratto e PDF]
  C --> J[Commessa]
  A[Utenti, gruppi e ruoli] --> J
  J --> K[Attività e sottoattività]
  K --> B[Kanban e registro blocchi]
  J --> O[Ore personali]
  O --> R[Analisi del lavoro]
  T[Tariffe orarie] --> R
  O --> N[Controlli giornalieri e notifiche]
```

Il collegamento della commessa a un contratto è facoltativo. Le attività interne possono essere registrate su commesse comuni, senza un contratto. Il valore del contratto e il costo stimato delle ore sono informazioni gestionali distinte.

## Persone e responsabilità

| Profilo | Obiettivo | Funzioni principali |
| --- | --- | --- |
| Operatore | Svolgere e rendicontare il lavoro autorizzato | Commesse, bacheca, stato delle attività assegnate, propri blocchi, proprie ore, analisi e notifiche |
| Gestore | Organizzare il lavoro e controllarne l’andamento | Commesse; utenti e accessi; attività e blocchi; ore dettagliate e analisi della propria squadra; alert e storico autorizzato |
| Commerciali | Gestire gli accordi e i documenti | Fornitori, contratti e PDF, in consultazione e modifica |
| Finance | Consultare la dimensione economica | Fornitori, contratti e PDF in sola lettura; valori e costi aggregati per commessa; lettura delle tariffe |
| Referente tecnico dell’installazione | Mantenere disponibile e configurare il servizio | Container, database, backup, rete e configurazione degli addon, descritti nella documentazione tecnica |

Ogni account attivo ha **Operatore** implicito; **Gestore** deriva dal ruolo dell’identità. **Commerciali** e **Finance** si assegnano separatamente in **Utenti → Profili**, anche agli account Keycloak. Nessun gestore li riceve automaticamente. Per modificare tariffe servono Gestore + Finance; per cambiare il contratto della commessa servono Gestore + Commerciali. **Lettore**, **Collaboratore** e gli eventuali ruoli personalizzati sono invece ruoli su una singola commessa. Un Collaboratore può registrare le proprie ore, aggiornare lo stato delle attività assegnate e gestire i blocchi di cui è autore o responsabile; non diventa per questo amministratore degli utenti o dei contratti.

L’applicazione attuale ha un unico spazio operativo. Le società separate e i permessi finanziari per società appartengono al [piano finanziario](piano-modulo-finanziario.md).

## Moduli disponibili

| Sezione o servizio | Stato | Cosa offre oggi |
| --- | --- | --- |
| Accesso | Implementato | Login con email e password, sessione personale, opzione «Ricordami per 30 giorni», logout e revoca degli account disattivati |
| Commesse | Implementato | Dati e contratto, creazione guidata e modelli, abilitazioni con spiegazione, riepilogo, chiusura, archivio e cancellazione definitiva |
| Commesse comuni | Implementato | Una casella rende la commessa utilizzabile da tutti gli utenti attivi, compresi quelli creati successivamente |
| Attività e blocchi | Implementato | Attività e sottoattività, responsabile, priorità, scadenza, Kanban a cinque stati ed elenco gerarchico; blocchi con impatto, prossima azione e stato |
| Registro ore | Implementato | Registro giornaliero e settimanale, copia della settimana precedente, inserimenti multipli atomici, attività facoltativa e rettifiche approvate |
| La mia giornata | Implementato | Attività assegnate, blocchi e ore di oggi con collegamenti contestuali |
| Orari e assenze | Implementato | Orari individuali con decorrenza, eccezioni personali e festività per il controllo delle ore attese |
| Analisi | Implementato | Viste Per periodo, Per commessa e Per utente, con date e filtri; ambito personale per gli operatori e della propria squadra per i gestori |
| Valore contratti | Implementato | Valore EUR dei contratti distinti collegati alle ore filtrate, valori mancanti e ore prive di contratto |
| Costo delle ore | Implementato | Aggregati Finance per commessa, basati su tariffe e date, con evidenza delle ore non valorizzate |
| Fornitori | Implementato | Anagrafica e contatti: modifica Commerciali, consultazione Commerciali o Finance |
| Contratti e PDF | Implementato | Contratti con fornitore obbligatorio, valore e date facoltativi, caricamento e download degli originali |
| Utenti | Implementato | Identità locale o aziendale, stato di accesso e profili funzionali separati |
| Gruppi e ruoli | Implementato | Albero dei gruppi, appartenenze multiple, ereditarietà e ruoli operativi personalizzati |
| Alert e trigger | Implementato | Regole del singolo gestore, orario quotidiano, destinatari e due controlli predefiniti |
| Notifiche | Implementato | Avvisi personali, contatore e marcatura come letti |
| Storico | Implementato | Eventi delle modifiche con autore, data e dettagli precedenti/successivi, riservati ai gestori |
| Invio email | Parziale nell’uso operativo | Addon e coda implementati; SMTP ancora da configurare, quindi nessuna spedizione reale |
| Google Calendar | Richiede configurazione del provider | Addon C, interfaccia personale, sincronizzazione e proposta ore con conferma implementati; OAuth e consenso individuale necessari |
| Controllo economico EUR | Implementato | Finance consulta budget, previsione e costi; approva fotografie immutabili dei periodi e rettifiche motivate |
| Modulo finanziario completo | Pianificato | Più società e valute, fatture, incassi, pagamenti, partite e reporting contabile |

«Implementato» descrive una funzione presente nel codice e nell’interfaccia, dove prevista. «Parziale» indica che una parte tecnica è disponibile ma il percorso operativo richiede ancora configurazione o interfaccia. «Pianificato» indica un progetto, non una funzione già utilizzabile.

## Esperienza dell’operatore

Tra le sezioni dell’operatore ci sono **La mia giornata**, **Commesse**, **Attività e blocchi**, **Registro ore**, **Orari e assenze**, **Analisi** e **Notifiche**. Il selettore delle ore contiene le commesse su cui è consentita la registrazione; quelle in sola lettura rimangono consultabili nell’elenco ma non possono ricevere nuove ore da quell’utente.

Una revoca impedisce nuove registrazioni sulla commessa, ma conserva la consultazione delle proprie ore già salvate. Le ore appartengono sempre a chi ha effettuato l’accesso: non è possibile attribuirle a un collega dal modulo di registrazione. L’operatore può aggiungere ore e richiedere rettifiche motivate; un gestore diverso dall’autore decide le richieste.

La bacheca è visibile a chi può consultare la commessa. Con accesso di Collaboratore l’operatore aggiorna lo stato delle attività assegnate a lui e può segnalare impedimenti. Assegnazione e autorizzazione restano distinte: il nome del responsabile non sostituisce l’abilitazione.

Per iniziare: [guida utente](guida-utente.md) e [Attività, Kanban e blocchi](attivita-kanban-e-blocchi.md).

## Esperienza del gestore

Il gestore dispone delle stesse funzioni personali e delle sezioni **Alert e trigger**, **Utenti**, **Gruppi e ruoli** e **Storico**. Amministra tutte le commesse dello spazio. Le ore personali altrui sono limitate alla sua squadra: sé stesso, membri dei propri gruppi organizzativi e relativi sottogruppi; senza gruppi, o con gruppi aziendali indisponibili, vede soltanto sé stesso. Fornitori, Contratti e analisi economiche dipendono dai profili Commerciali e Finance, non dal solo ruolo Gestore.

Il Registro ore parte dai dati personali e permette al gestore di scegliere una persona della squadra per la consultazione dettagliata in sola lettura. Modifica e cancellazione diretta riguardano le proprie ore. Storico, alert e coda rettifiche rispettano la stessa squadra e la revoca corrente. Le richieste di rettifica sono decise da un gestore della squadra diverso dall’autore, con revisione e storico della decisione. La cancellazione di una commessa è un’operazione distinta e rimuove anche attività, sottoattività, blocchi e tutte le ore collegate di tutti gli utenti.

Per impostare lo spazio: [guida del gestore](guida-gestore.md).

## Commesse per il lavoro comune

Il catalogo di riferimento per le attività trasversali comprende:

| Commessa | Utilizzo suggerito |
| --- | --- |
| Generale | Lavoro generale non attribuito a una commessa specifica |
| Formazione | Corsi, studio e aggiornamento professionale |
| Riunioni | Riunioni interne non imputate a un progetto specifico |
| Attività interne | Organizzazione e altre attività interne |

Il meccanismo è la casella **Commessa comune**. I nomi non sono riservati dal software e una nuova installazione non li genera automaticamente: il gestore verifica o crea le quattro anagrafiche, sceglie codici univoci e le rende comuni. Le regole aziendali devono chiarire quando usare questi contenitori e quando imputare il lavoro direttamente a una commessa di progetto.

## Confini attuali del prodotto

Le cinque etichette di stato della commessa descrivono l’avanzamento ma non costituiscono un workflow con approvazioni o blocchi automatici. Una commessa Completata o Annullata può ricevere ore da un utente autorizzato finché non viene esplicitamente chiusa o archiviata. Chiusura e archivio bloccano nuove ore anche ai gestori e conservano i dati; la riapertura richiede un motivo. Le date di un contratto non ne cambiano automaticamente lo stato.

Attività e sottoattività hanno stati indipendenti. Non sono ancora disponibili dipendenze, Gantt, pianificazione del carico o checklist separate. Le ore possono collegare una singola attività. I blocchi descrivono impedimenti presenti; il registro dei rischi futuri rimane da sviluppare. Assegnazioni e scadenze alimentano un feed operativo personale con controlli sugli accessi.

Il controllo delle ore mancanti applica orari individuali, festività e assenze configurati, trascorse 48 ore dalla fine della giornata. In assenza di configurazione usa 8 ore nei feriali. Una notifica documenta l’anomalia rilevata; il pulsante di lettura non modifica le ore.

I report economici attuali usano solo EUR e vengono ricalcolati dai dati correnti. Non producono fatture, documenti fiscali, incassi o valori contabili congelati. Il campo Cliente della commessa è un testo descrittivo, non un’anagrafica completa di fatturazione.

L’addon Calendar cifra token e contenuti nel proprio archivio, mentre l’applicazione non dispone ancora di un vault che cifri tutti i campi o i PDF contrattuali. L’elaborazione autorizzata usa dati in chiaro in memoria. I requisiti di cifratura più estesi non devono essere considerati già soddisfatti.

## Documenti di riferimento

- [Processi e regole](processi-e-regole.md): permessi, limiti, date, cancellazioni e significato dei report.
- [Attività, Kanban e blocchi](attivita-kanban-e-blocchi.md): organizzazione, permessi e gestione degli impedimenti.
- [Roadmap](roadmap.md): stato dei miglioramenti operativi, del modulo attività e del percorso finanziario.
- [Glossario](glossario.md): termini del prodotto.
- [Architettura tecnica](../tehnical/architettura.md): componenti e responsabilità.
- [Verifica UI del 10 ottobre 2026](../tehnical/verifica-ui-2026-10-10.md): evidenza storica riferita alla versione esaminata; non certifica tutte le aggiunte successive.
