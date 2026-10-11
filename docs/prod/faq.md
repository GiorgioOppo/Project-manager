# Domande frequenti

**Italiano** | [English](../en/prod/faq.md)

[Indice prodotto](README.md) · [Guida utente](guida-utente.md) · [Guida gestore](guida-gestore.md)

Le risposte descrivono il prodotto attuale all’11 ottobre 2026. Per le regole complete vedere [Processi e regole](processi-e-regole.md).

## Accesso e commesse

### Perché vedo una commessa ma non posso inserirvi ore?

Il ruolo sulla commessa può consentire soltanto la consultazione. Chiedi al gestore di verificare ruolo e date dell’abilitazione. Per un operatore, una stessa abilitazione alla registrazione deve essere valida sia oggi sia nella data del lavoro; due abilitazioni diverse, ciascuna valida soltanto in una delle due date, non bastano.

### Il gestore ha revocato un’abilitazione, ma vedo ancora la commessa. È un errore?

Può esserci un’altra abilitazione diretta, l’accesso tramite un gruppo o la casella Commessa comune attiva. Il gestore deve verificare tutti i percorsi applicabili. Una revoca singola rimuove solo quel collegamento.

### Posso digitare una nuova commessa quando registro le ore?

Il registro usa il catalogo delle commesse esistenti e utilizzabili dall’utente. Per aggiungere una nuova commessa occorre l’intervento di un gestore. Una commessa inesistente viene rifiutata anche se il suo identificativo è inviato direttamente alle API.

### Una commessa “Completata” o “Annullata” impedisce altre ore?

Lo stato descrive la commessa, ma da solo non blocca le registrazioni. Per impedire nuove ore a tutti, anche ai gestori, usa la chiusura o l’archiviazione motivata: conserva dati e storico. La riapertura richiede un motivo. Ruoli, date e revoche continuano a regolare gli accessi individuali; la spiegazione degli accessi mostra anche percorsi alternativi diretti, di gruppo o comuni.

### Generale, Formazione, Riunioni e Attività interne compaiono su ogni nuova installazione?

Sono il catalogo comune adottato per il progetto, configurabile dai gestori. Non sono un seed automatico obbligatorio del software: un’installazione nuova deve creare le commesse desiderate e abilitarne la casella Commessa comune. Gli account attivi creati in seguito potranno utilizzare le commesse comuni esistenti.

### Sono Gestore: perché non vedo Contratti o Costo delle ore?

I profili sono separati. In **Utenti → Profili** un gestore assegna Commerciali per creare/modificare fornitori, contratti e caricare PDF; Finance per consultarli e leggere aggregati economici e tariffe. Finance è in sola lettura; modificare tariffe richiede anche Gestore. L’assegnazione vale anche per le identità Keycloak e non deriva dai gruppi organizzativi. Nessun gestore riceve automaticamente Commerciali o Finance.

## Attività e blocchi

### Essere responsabile di un’attività mi dà accesso alla commessa?

No. L’assegnazione indica chi segue il lavoro; l’abilitazione determina cosa può consultare o modificare. Per aggiornare lo stato devi essere responsabile dell’attività e avere un accesso Collaboratore valido oggi, oppure essere Gestore. La revoca impedisce gli aggiornamenti anche se il tuo nome resta associato.

### Perché vedo la bacheca ma non posso creare attività?

La creazione, l’organizzazione e la cancellazione delle attività spettano al Gestore. Il Collaboratore aggiorna lo stato delle proprie attività e segnala blocchi; il Lettore consulta soltanto. Sono le stesse abilitazioni della commessa, senza un nuovo ruolo globale.

### Se completo il genitore, si completano anche le sottoattività?

No. Ogni attività mantiene stato, responsabile e scadenza propri. Le sottoattività formano un albero fino a otto livelli; non sono ancora dipendenze temporali. Per eliminare il genitore occorre prima spostare o eliminare i figli.

### Risolvere un blocco rimette l’attività In corso?

No. Il registro degli impedimenti e lo stato Kanban sono indipendenti. Aggiorna entrambi quando necessario. Un blocco può anche riguardare tutta la commessa e può coesistere con altri impedimenti sulla stessa attività.

### Chi può modificare un blocco?

Il Gestore può gestirli tutti. L’operatore può aggiornarlo se ne è autore o responsabile e conserva un accesso Collaboratore. Il gestore può cambiare il responsabile senza cambiare l’autore. Solo il Gestore può eliminare il blocco o riassegnarlo.

### Dove sono finiti i blocchi risolti?

Il registro mostra inizialmente **Aperti e in gestione**. Nel filtro **Mostra blocchi**, scegli **Tutti** o **Risolti**. Gli elementi risolti continuano a contare nel limite di 500 blocchi per commessa; anche le attività completate contano nel rispettivo limite di 500.

### Perché il salvataggio segnala una versione cambiata?

Un’altra modifica ha aggiornato lo stesso elemento. L’app impedisce di sovrascriverla con una bozza basata su dati precedenti. Annota le tue variazioni, usa **Chiudi e ricarica la versione aggiornata** e confronta il contenuto prima di riprovare. Non è prevista una fusione automatica delle modifiche.

### Posso registrare ore su una specifica attività o ricevere avvisi sulla sua scadenza?

Le ore appartengono alla commessa e possono collegare facoltativamente una sua attività; il campo **Attività svolta / nota** rimane descrittivo. Assegnazioni e scadenze alimentano avvisi operativi personali con controllo degli accessi, senza invio automatico di email. Completare un’attività non genera ore. Dipendenze, Gantt e pianificazione del carico restano nella roadmap.

### Cosa accade ai blocchi se elimino un’attività?

I blocchi restano nel registro della commessa e perdono il collegamento all’attività eliminata; la loro revisione cambia. Eliminare l’intera commessa rimuove invece anche attività, blocchi e ore. La [guida del modulo](attivita-kanban-e-blocchi.md) riassume tutti i casi.

## Registrazioni e analisi

### Come inserisco mezz’ora o un’ora e mezza?

Nel campo delle ore puoi usare `0:30` o `1:30`, oppure il formato decimale corrispondente. `1,30` in formato decimale significa 1,3 ore, cioè 78 minuti: non significa un’ora e trenta. Il salvataggio deve risultare in un numero intero di minuti.

### Posso correggere le ore già inserite?

Un gestore può modificare o eliminare le proprie registrazioni. L’operatore può usare **Richiedi rettifica**, proponendo una modifica o cancellazione con un motivo. Le ore cambiano solo dopo l’approvazione di un gestore diverso dall’autore della richiesta; originale, proposta, decisione e conflitti rimangono consultabili. Il gestore non dispone di un editor generico delle ore altrui. Non duplicare una registrazione per tentare di correggerla.

### Il gestore può vedere le mie ore?

Soltanto se fai parte della sua squadra: il gestore stesso, membri dei suoi gruppi organizzativi e dei sottogruppi. Può leggere aggregati e dettagli giorno/settimana in sola lettura; non modifica direttamente le tue righe. Rettifiche, Storico e alert sulle ore rispettano lo stesso ambito corrente. Un gestore senza gruppi, o con gruppi aziendali indisponibili, vede soltanto sé stesso. Finance non concede accesso alle registrazioni personali dei colleghi.

### Se perdo l’accesso a una commessa, perdo lo storico personale?

La revoca dell’abilitazione conserva le ore precedenti e la loro consultazione personale. Una cancellazione definitiva della commessa elimina invece attività, blocchi e ore collegate di tutti gli utenti. Prima di un’eliminazione, il gestore deve considerare l’effetto sui dati storici; l’app mostra una conferma dedicata.

### È possibile annullare una cancellazione definitiva?

Non è presente un cestino. L’eventuale recupero richiede un backup e un intervento tecnico coordinato. Lo Storico conserva l’evento di eliminazione, ma non offre un pulsante che ripristina tutte le righe rimosse.

### Perché Valore contratti e Costo delle ore danno numeri diversi?

Misurano elementi differenti. Valore contratti mostra l’intero valore dei contratti collegati al lavoro selezionato, contando ogni contratto una volta. Costo delle ore applica le tariffe alle singole registrazioni. Il primo valore non viene distribuito fra giorni, commesse o persone e non rappresenta il ricavo maturato nel periodo.

### Perché alcune ore risultano senza costo?

Manca una tariffa valida per l’utente e la data della registrazione. Le ore rimangono indicate come non valorizzate; una tariffa esplicita di zero è un caso diverso. Modificare una tariffa può cambiare anche le stime relative al lavoro passato, perché non sono snapshot contabili congelati.

### Il collegamento a un’analisi concede accesso ai dati?

L’URL conserva vista e filtri. Chi lo apre deve comunque accedere e possedere i permessi necessari. Un operatore resta limitato alle proprie registrazioni; il gestore resta nella propria squadra e le viste economiche richiedono Finance, con aggregati e senza filtro per persona.

## Alert, documenti e integrazioni

### Quando compare un alert per una giornata con meno ore del previsto?

La giornata deve avere minuti attesi maggiori di zero e un totale inferiore all’atteso; devono essere trascorse 48 ore effettive dalla sua fine nel fuso Europe/Rome. L’anomalia viene rilevata al successivo controllo giornaliero della regola, oppure con Esegui ora quando la tolleranza è già scaduta. Per esempio, il lunedì termina martedì alle 00:00 e diventa verificabile giovedì alle 00:00.

### Il controllo considera ferie, festività e part time?

Sì. In **Orari e assenze** il gestore configura orari settimanali con decorrenza, festività ed eccezioni personali. Per una giornata vale prima l’eccezione personale, poi la festività, infine l’ultimo orario in vigore. Un’assenza completa prevede zero minuti; senza configurazione valgono otto ore lun–ven e zero sab–dom. Gli avvisi già generati conservano il contesto rilevato al controllo.

### “Commessa inesistente” significa che sono state salvate ore senza commessa?

Il controllo segnala tentativi respinti su identificativi inesistenti o eliminati. La registrazione non valida non viene salvata. Anche la cancellazione ordinaria di una commessa rimuove le relative ore nella stessa transazione, evitando di lasciare righe orfane.

### Perché una notifica è presente nell’app ma l’email non arriva?

I due canali hanno esiti separati. SMTP è da configurare: gli avvisi destinati alla posta possono restare in attesa di configurazione. Quando il canale sarà attivo, l’accettazione da parte del server SMTP non garantirà da sola la consegna nella casella. Un invio dall’esito incerto richiede verifica e non viene ripetuto automaticamente.

### Perché la mia regola non produce avvisi precedenti alla sua creazione?

Il monitoraggio parte dalla creazione della regola e dall’ingresso dell’utente nel relativo monitoraggio. Non è un controllo retroattivo indiscriminato di tutto lo storico. Le regole disattivate non vengono eseguite automaticamente; Esegui ora consente un controllo manuale. Le anomalie già notificate vengono deduplicate.

### Se disattivo una regola, si fermano anche le email già preparate?

La disattivazione sospende i controlli automatici, ma non cancella la coda email esistente. Quando SMTP sarà attivo, gli invii pendenti potranno essere elaborati se il proprietario è ancora un gestore attivo e l’indirizzo rimane tra i destinatari. Per gli alert sulle ore l’indirizzo deve anche corrispondere a un account attivo autorizzato a leggere il soggetto, ancora visibile al proprietario della regola. Una consegna già iniziata non viene annullata. Prima di attivare il provider, il referente tecnico deve quindi verificare anche la coda preesistente.

### Perché un operatore destinatario non riceve gli avvisi di altre persone?

Le notifiche interne rispettano l’ambito del destinatario. Un operatore riceve soltanto anomalie riferite al proprio lavoro; un gestore destinatario può ricevere quelle della propria squadra. Anche il proprietario della regola deve poter leggere le ore del soggetto; la revoca nasconde gli avvisi già prodotti e il relativo contatore. Selezionare qualcuno come destinatario non gli attribuisce il profilo Gestore.

### Il caricamento di un PDF sostituisce quello precedente?

I documenti contrattuali sono conservati come originali distinti. Un nuovo caricamento aggiunge un documento; non costituisce ancora un sistema di versioni contrattuali approvate. Il controllo di firma `%PDF-`, dimensione e impronta verifica alcuni requisiti tecnici, ma non esegue OCR, antivirus o una validazione legale del contratto.

### Dove collego il calendario Google e trasformo un evento in ore?

Il servizio e le API di sincronizzazione sono implementati. Google OAuth deve essere configurato e la schermata Calendario deve ancora essere realizzata. Anche l’associazione evento–commessa e la conversione dell’evento in ore sono nella roadmap; gli appuntamenti non generano automaticamente registrazioni di lavoro.

### Posso già emettere fatture o gestire più società e valute?

Il prodotto attuale offre contratti e stime economiche in EUR. Fatturazione, incassi, pagamenti, contesto societario e multivaluta sono descritti nel [piano finanziario](piano-modulo-finanziario.md) e restano da implementare.

## Informazioni utili per il supporto

Quando segnali un problema indica la sezione, l’azione, la data del lavoro, il messaggio mostrato e, se necessario, il codice della commessa. Evita di includere password, codici OAuth, cookie di sessione o contenuti riservati dei contratti. Per problemi di disponibilità o configurazione, il gestore può coinvolgere chi amministra il sistema usando la [guida operativa](../tehnical/operazioni-backup-ripristino.md).
