# Domande frequenti

[Indice prodotto](README.md) · [Guida utente](guida-utente.md) · [Guida gestore](guida-gestore.md)

Le risposte descrivono il prodotto attuale al 10 ottobre 2026. Per le regole complete vedere [Processi e regole](processi-e-regole.md).

## Accesso e commesse

### Perché vedo una commessa ma non posso inserirvi ore?

Il ruolo sulla commessa può consentire soltanto la consultazione. Chiedi al gestore di verificare ruolo e date dell’abilitazione. Per un operatore, una stessa abilitazione alla registrazione deve essere valida sia oggi sia nella data del lavoro; due abilitazioni diverse, ciascuna valida soltanto in una delle due date, non bastano.

### Il gestore ha revocato un’abilitazione, ma vedo ancora la commessa. È un errore?

Può esserci un’altra abilitazione diretta, l’accesso tramite un gruppo o la casella Commessa comune attiva. Il gestore deve verificare tutti i percorsi applicabili. Una revoca singola rimuove solo quel collegamento.

### Posso digitare una nuova commessa quando registro le ore?

Il registro usa il catalogo delle commesse esistenti e utilizzabili dall’utente. Per aggiungere una nuova commessa occorre l’intervento di un gestore. Una commessa inesistente viene rifiutata anche se il suo identificativo è inviato direttamente alle API.

### Una commessa “Completata” o “Annullata” impedisce altre ore?

Lo stato descrive la commessa, ma da solo non blocca le registrazioni. Il controllo attuale riguarda esistenza e abilitazione. Per limitare gli operatori, il gestore deve gestire ruolo, scadenza o revoca degli accessi, considerando anche gruppi e stato di commessa comune. I gestori conservano l’accesso globale alle commesse.

### Generale, Formazione, Riunioni e Attività interne compaiono su ogni nuova installazione?

Sono il catalogo comune adottato per il progetto, configurabile dai gestori. Non sono un seed automatico obbligatorio del software: un’installazione nuova deve creare le commesse desiderate e abilitarne la casella Commessa comune. Gli account attivi creati in seguito potranno utilizzare le commesse comuni esistenti.

## Registrazioni e analisi

### Come inserisco mezz’ora o un’ora e mezza?

Nel campo delle ore puoi usare `0:30` o `1:30`, oppure il formato decimale corrispondente. `1,30` in formato decimale significa 1,3 ore, cioè 78 minuti: non significa un’ora e trenta. Il salvataggio deve risultare in un numero intero di minuti.

### Posso correggere le ore già inserite?

Un gestore può modificare o eliminare le proprie registrazioni. L’operatore può inserirle e consultarle; il flusso di richiesta e approvazione delle rettifiche è pianificato. Il gestore non dispone di una modifica ordinaria delle righe intestate a un altro utente. Per anomalie operative rivolgersi al gestore, senza duplicare una registrazione per tentare di correggerla.

### Il gestore può vedere le mie ore?

Può consultare gli aggregati nelle analisi per periodo, commessa e utente. Il Registro ore dettagliato rimane personale e le analisi non mostrano le note delle attività. Il gestore ha inoltre accesso allo Storico delle modifiche: gli eventi di audit possono contenere i dettagli prima/dopo delle operazioni. I permessi delle analisi e quelli dell’audit sono quindi distinti.

### Se perdo l’accesso a una commessa, perdo lo storico personale?

La revoca dell’abilitazione conserva le ore precedenti e la loro consultazione personale. Una cancellazione definitiva della commessa elimina invece le ore collegate di tutti gli utenti. Prima di un’eliminazione, il gestore deve considerare l’effetto sui dati storici; l’app mostra una conferma dedicata.

### È possibile annullare una cancellazione definitiva?

Non è presente un cestino. L’eventuale recupero richiede un backup e un intervento tecnico coordinato. Lo Storico conserva l’evento di eliminazione, ma non offre un pulsante che ripristina tutte le righe rimosse.

### Perché Valore contratti e Costo delle ore danno numeri diversi?

Misurano elementi differenti. Valore contratti mostra l’intero valore dei contratti collegati al lavoro selezionato, contando ogni contratto una volta. Costo delle ore applica le tariffe alle singole registrazioni. Il primo valore non viene distribuito fra giorni, commesse o persone e non rappresenta il ricavo maturato nel periodo.

### Perché alcune ore risultano senza costo?

Manca una tariffa valida per l’utente e la data della registrazione. Le ore rimangono indicate come non valorizzate; una tariffa esplicita di zero è un caso diverso. Modificare una tariffa può cambiare anche le stime relative al lavoro passato, perché non sono snapshot contabili congelati.

### Il collegamento a un’analisi concede accesso ai dati?

L’URL conserva vista e filtri. Chi lo apre deve comunque accedere e possedere i permessi necessari. Un operatore resta limitato alle proprie registrazioni; le viste economiche sono riservate ai gestori.

## Alert, documenti e integrazioni

### Quando compare un alert per una giornata con meno di otto ore?

La giornata deve essere da lunedì a venerdì e devono essere trascorse 48 ore effettive dalla sua fine nel fuso Europe/Rome. L’anomalia viene rilevata al successivo controllo giornaliero della regola, oppure con Esegui ora quando la tolleranza è già scaduta. Per esempio, il lunedì termina martedì alle 00:00 e diventa verificabile giovedì alle 00:00.

### Il controllo considera ferie, festività e part time?

Il criterio corrente è fisso: otto ore per ogni giornata lun–ven. Non è ancora presente un calendario di festività, assenze o orari individuali. Le relative eccezioni sono previste nella roadmap e non vanno presunte durante la lettura degli avvisi.

### “Commessa inesistente” significa che sono state salvate ore senza commessa?

Il controllo segnala tentativi respinti su identificativi inesistenti o eliminati. La registrazione non valida non viene salvata. Anche la cancellazione ordinaria di una commessa rimuove le relative ore nella stessa transazione, evitando di lasciare righe orfane.

### Perché una notifica è presente nell’app ma l’email non arriva?

I due canali hanno esiti separati. SMTP è da configurare: gli avvisi destinati alla posta possono restare in attesa di configurazione. Quando il canale sarà attivo, l’accettazione da parte del server SMTP non garantirà da sola la consegna nella casella. Un invio dall’esito incerto richiede verifica e non viene ripetuto automaticamente.

### Perché la mia regola non produce avvisi precedenti alla sua creazione?

Il monitoraggio parte dalla creazione della regola e dall’ingresso dell’utente nel relativo monitoraggio. Non è un controllo retroattivo indiscriminato di tutto lo storico. Le regole disattivate non vengono eseguite automaticamente; Esegui ora consente un controllo manuale. Le anomalie già notificate vengono deduplicate.

### Se disattivo una regola, si fermano anche le email già preparate?

La disattivazione sospende i controlli automatici, ma non cancella la coda email esistente. Quando SMTP sarà attivo, gli invii pendenti potranno essere elaborati se il proprietario è ancora un gestore attivo e l’indirizzo rimane tra i destinatari. Una consegna già iniziata non viene annullata. Prima di attivare il provider, il referente tecnico deve quindi verificare anche la coda preesistente.

### Perché un operatore destinatario non riceve gli avvisi di altre persone?

Le notifiche interne rispettano l’ambito del destinatario. Un operatore riceve soltanto anomalie riferite al proprio lavoro; un gestore destinatario può ricevere quelle degli utenti monitorati. Selezionare qualcuno come destinatario non gli attribuisce il profilo Gestore.

### Il caricamento di un PDF sostituisce quello precedente?

I documenti contrattuali sono conservati come originali distinti. Un nuovo caricamento aggiunge un documento; non costituisce ancora un sistema di versioni contrattuali approvate. Il controllo di firma `%PDF-`, dimensione e impronta verifica alcuni requisiti tecnici, ma non esegue OCR, antivirus o una validazione legale del contratto.

### Dove collego il calendario Google e trasformo un evento in ore?

Il servizio e le API di sincronizzazione sono implementati. Google OAuth deve essere configurato e la schermata Calendario deve ancora essere realizzata. Anche l’associazione evento–commessa e la conversione dell’evento in ore sono nella roadmap; gli appuntamenti non generano automaticamente registrazioni di lavoro.

### Posso già emettere fatture o gestire più società e valute?

Il prodotto attuale offre contratti e stime economiche in EUR. Fatturazione, incassi, pagamenti, contesto societario e multivaluta sono descritti nel [piano finanziario](piano-modulo-finanziario.md) e restano da implementare.

## Informazioni utili per il supporto

Quando segnali un problema indica la sezione, l’azione, la data del lavoro, il messaggio mostrato e, se necessario, il codice della commessa. Evita di includere password, codici OAuth, cookie di sessione o contenuti riservati dei contratti. Per problemi di disponibilità o configurazione, il gestore può coinvolgere chi amministra il sistema usando la [guida operativa](../tehnical/operazioni-backup-ripristino.md).
