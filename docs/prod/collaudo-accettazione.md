# Piano di collaudo di accettazione

Questo documento definisce prove manuali riproducibili e risultati attesi per il prodotto corrente. **Non attesta che queste nuove prove siano state eseguite.** I rapporti di esecuzione vanno compilati separatamente con versione, ambiente, data, osservazioni ed evidenze.

I criteri sono ricavati dall’interfaccia, dal codice e dai test esistenti. La [verifica UI storica](../tehnical/verifica-ui-2026-10-10.md) documenta soltanto il perimetro esaminato in quella sessione; non sostituisce l’esecuzione di questo piano sulla versione da consegnare.

## 1. Ambiente e dati di prova

Esegui il piano su un’installazione isolata di laboratorio, con dati sintetici e una destinazione chiaramente distinguibile dall’ambiente operativo. Le prove di cancellazione, revoca, disattivazione e cambio ruolo modificano realmente i dati se vengono eseguite. Non usare commesse, ore, documenti o persone di produzione.

Prima di iniziare registra:

| Informazione | Contenuto da annotare |
| --- | --- |
| Versione | Commit o identificativo della build e schema applicativo |
| Ambiente | Nativo SQLite oppure Docker MySQL/GridFS; URL di laboratorio |
| Client | Browser/versione, dimensione finestra e fuso del dispositivo |
| Data e fuso server | Giorno corrente e Europe/Rome per i controlli alert |
| Integrazioni | Stato SMTP e OAuth Google, che possono rimanere non configurati |
| Responsabile | Persona che esegue e persona che valuta gli esiti |
| Base di ripristino | Snapshot/backup del solo laboratorio, se si riutilizza lo stesso ambiente |

Predisponi almeno quattro account fittizi:

- **Marta**, Gestore che esegue i censimenti.
- **Bruno**, secondo Gestore attivo per mantenere l’accesso amministrativo durante le prove sul profilo di Marta.
- **Elena**, Operatore che registra ore.
- **Luca**, Operatore con ruolo Lettore sulla commessa di prova.

Usa profili browser separati, non soltanto schede con il medesimo cookie. Non riportare password, cookie, token o segreti nelle evidenze. Per gli screenshot sono sufficienti identità sintetiche, comandi, risultati e messaggi.

Prepara un PDF sintetico valido sotto 10 MiB, un file di testo rinominato con estensione `.pdf` e, per il limite dimensionale, un documento di prova superiore a 10 MiB. Non utilizzare contratti reali per il collaudo dei caricamenti.

## 2. Registrare l’esito di ciascun caso

Per ogni identificativo usa **Superato**, **Fallito**, **Bloccato** oppure **Non eseguito**. Un caso bloccato per assenza di configurazione o per attesa temporale non è un caso superato.

Annota precondizioni, passi realmente eseguiti, risultato osservato, risultato atteso e identificativi sintetici utili a ricostruire la prova. Quando un caso modifica la base, ripristina il laboratorio oppure dichiara esplicitamente la nuova precondizione prima di procedere.

Le prove API indicate più avanti completano la UI: un pulsante assente dimostra soltanto il comportamento grafico, non il rifiuto del server. Il client di collaudo deve usare la sessione del profilo previsto e un’origine ammessa, per non confondere un rifiuto per permessi con un errore di sessione o protezione dell’origine.

## 3. Accesso e identità

### UAT-01 — Login, errori e logout

**Profilo:** Elena. **Precondizione:** account attivo, sessione chiusa.

1. Prova a inviare il login con campi vuoti e poi con email formalmente errata.
2. Prova credenziali non valide.
3. Inserisci le credenziali assegnate, controlla Mostra/Nascondi password e premi Accedi al tuo spazio.
4. Verifica nome, email e profilo nell’intestazione, poi premi Esci.

**Atteso:** i campi invalidi sono segnalati; le credenziali errate non aprono lo spazio; il login valido mostra l’identità corretta; dopo il logout le API autenticate non sono più accessibili con quella sessione. Non sono esposte informazioni di altri account nei messaggi di errore.

### UAT-02 — Differenza tra Gestore, Operatore e Lettore

**Precondizione:** una commessa riservata, Elena Collaboratore e Luca Lettore.

Apri lo spazio con tutti e tre i profili. Elena deve vedere Commesse, Registro ore, Analisi e Notifiche. Luca ha lo stesso profilo Operatore, ma la commessa di prova è consultabile senza possibilità di registrarvi nuove ore. Marta vede anche Utenti, Gruppi e ruoli, Fornitori, Contratti, Alert e trigger e Storico, oltre alle due viste economiche di Analisi.

**Atteso:** il ruolo sulla commessa cambia le capacità su quella risorsa, non le funzioni amministrative dell’account. Marta non dispone comunque di un modulo per attribuire le proprie nuove ore a Elena.

### UAT-03 — Disattivazione e sessione già aperta

**Profilo:** Elena in sessione; Marta amministratrice.

Marta disattiva Elena da Utenti. Elena tenta di aggiornare una sezione o salvare una nuova registrazione. Marta riattiva l’account; Elena effettua un nuovo login.

**Atteso:** le richieste della sessione revocata non producono nuove modifiche. La riattivazione non ripristina il vecchio cookie; serve un nuovo accesso. Le ore già presenti restano nei report amministrativi.

### UAT-04 — Declassamento e ultimo Gestore

**Precondizione:** Marta e Bruno sono gli unici due Gestori attivi del laboratorio. Conta anche l’eventuale account iniziale di bootstrap: se è un terzo Gestore attivo, questo caso non raggiunge la condizione da verificare. Predisponi la base isolata in modo che uno dei due account coincida con il gestore iniziale, oppure considera esplicitamente tutti i Gestori attivi prima della prova.

Marta cambia il proprio profilo in Operatore. Verifica nella sessione già aperta la scomparsa dei moduli riservati e la restrizione delle analisi alle proprie ore. Accedi come Bruno e, nel laboratorio, tenta di declassare o disattivare anche quest’ultimo Gestore attivo.

**Atteso:** il primo cambio è applicato; il secondo è rifiutato e Bruno conserva il profilo necessario ad amministrare il laboratorio. Bruno può ripristinare Marta per i casi successivi. Gli eventuali dati amministrativi caricati dalla precedente sessione non devono ricomparire tramite risposte tardive nella UI.

## 4. Censimenti e documenti

### UAT-05 — Fornitore e contratto

**Profilo:** Marta.

Crea un fornitore attivo, poi un contratto con codice e titolo univoci, quel fornitore e valore `1250,50`. Prova un altro contratto con lo stesso codice, un valore con tre decimali e una fine precedente all’inizio. Salva infine una modifica valida dello stato e delle note.

**Atteso:** il primo contratto è leggibile con importo esatto 1.250,50 EUR. Duplicato, precisione eccessiva e intervallo invertito sono rifiutati senza nuove righe valide. La modifica ammessa compare nello Storico. La data finale non provoca da sola un cambio automatico dello stato.

### UAT-06 — Disattivazione del fornitore e stati del contratto

Collega una commessa al contratto, poi disattiva il fornitore. Verifica che il contratto esistente rimanga. Prova a selezionare il fornitore per un nuovo collegamento. Porta il contratto a Chiuso o Annullato e prova a collegarlo a una nuova commessa.

**Atteso:** i riferimenti già esistenti possono essere conservati; i nuovi collegamenti richiedono un fornitore attivo e non consentono un contratto Chiuso/Annullato. Il cambio non elimina automaticamente commesse o ore precedenti.

### UAT-07 — Caricamento e download PDF

Apri PDF sul contratto di prova, carica il PDF sintetico valido e scaricalo. Confronta dimensione e impronta SHA-256 dell’originale e del file scaricato con uno strumento locale. Carica una seconda volta un file con lo stesso nome.

**Atteso:** byte e impronta coincidono; il download è un allegato. Il secondo caricamento aggiunge un documento distinto invece di sovrascrivere il primo. Non compaiono comandi di modifica o eliminazione del documento.

### UAT-08 — PDF rifiutati e accesso vietato

Prova il file di testo rinominato `.pdf`, il file oltre 10 MiB e una richiesta senza un file valido. Come Elena e come Luca prova l’URL autenticato di download del documento di laboratorio attraverso le [API documentali dell’app](../tehnical/api-app.md).

**Atteso:** i caricamenti invalidi non aggiungono documenti; i due Operatori non ottengono i byte del PDF. Un’abilitazione alla commessa collegata non concede accesso all’archivio contrattuale. La verifica profonda del contenuto e l’antivirus non sono funzioni da dichiarare superate con questi controlli.

## 5. Commesse e accessi

### UAT-09 — Creazione, ricerca e stati

Crea una commessa con codice, nome, cliente, descrizione e contratto. Cercala per codice, nome e cliente; applica un filtro di stato che la esclude e poi Azzera i filtri. Modificala attraverso i cinque stati disponibili.

**Atteso:** ricerca e filtri sono coerenti, un codice duplicato è rifiutato e i dati modificati persistono al ricaricamento. Gli stati non impongono un ordine di transizione. Con un Operatore autorizzato, una registrazione valida rimane tecnicamente consentita anche su commessa In pausa, Completata o Annullata.

### UAT-10 — Gruppo padre e sottogruppo

Crea Area tecnica con figli Sviluppo e Supporto. Aggiungi Elena soltanto a Sviluppo e Luca soltanto a Supporto. Assegna al padre Collaboratore sulla commessa A; assegna a Sviluppo Lettore sulla commessa B.

**Atteso:** entrambi possono registrare su A; soltanto Elena consulta B e non vi registra. Il permesso di Sviluppo non si trasmette al fratello Supporto. Rimuovendo Elena da Sviluppo, i permessi ereditati scompaiono dalla richiesta successiva, salvo altri grant.

### UAT-11 — Percorsi alternativi e ruolo personalizzato

Assegna a Elena Collaboratore tramite un gruppo e Lettore diretto sulla stessa commessa. Revoca il grant diretto, poi aggiungi un secondo gruppo con scrittura e revoca il primo. Crea anche un ruolo personalizzato inizialmente di sola lettura e verifica l’effetto della sua modifica alla capacità di registrare su una commessa distinta.

**Atteso:** Lettore non annulla la scrittura ottenuta dal gruppo. Una revoca incompleta lascia valido il percorso alternativo. Le modifiche al ruolo personalizzato si applicano ai grant che lo usano; i due ruoli predefiniti non sono modificabili.

### UAT-12 — Date inclusive e stesso grant

Usa come `T` la data corrente del server e come `W` cinque giorni prima. Sulla commessa non comune prepara un grant Collaboratore che copra W e T: salva un minuto in W. Poi sostituiscilo con due grant di gruppi differenti, uno che copre soltanto W e uno che copre soltanto T, senza un singolo intervallo che li comprenda entrambi. Riprova un nuovo minuto in W.

**Atteso:** il primo salvataggio riesce; il secondo è rifiutato, pur essendo la commessa visibile oggi. Ripeti con un grant Lettore che copra W e T più un Collaboratore valido soltanto oggi: la registrazione in W resta vietata. Un estremo uguale esattamente alla giornata è incluso.

### UAT-13 — Commessa comune

Crea una commessa comune senza assegnazioni. Verifica da Elena e Luca la consultazione e la registrazione personale. Crea un nuovo Operatore di laboratorio e verifica lo stesso comportamento. Aggiungi un grant diretto Lettore a Elena e riprova.

**Atteso:** tutti gli account attivi possono registrare grazie alla proprietà comune, anche con un Lettore diretto. Disattivando Commessa comune rimangono soltanto gli accessi espliciti validi; Elena resta in lettura se il suo unico grant residuo è Lettore.

### UAT-14 — Revoca durante la compilazione

Elena apre Registra ore su una commessa riservata, prepara data, durata e nota senza inviare. Marta revoca il suo unico accesso. Elena prova a salvare o aggiorna le scelte.

**Atteso:** nessuna nuova riga viene salvata sulla commessa revocata. La UI non mantiene una scelta utilizzabile basandosi sul vecchio permesso; quando ricarica le opzioni conserva data, durata e nota per consentire una scelta valida. Le ore precedenti rimangono nel registro e nelle analisi personali.

## 6. Ore e analisi

### UAT-15 — Formati e proprietà delle registrazioni

Da Elena salva tre righe su una commessa autorizzata usando `1,5`, `1:30` e `0:15`. Ricarica il Registro ore della giornata. Da Marta controlla il proprio registro della stessa data.

**Atteso:** Elena ha 90 + 90 + 15 = **195 minuti**, tre righe, totale 3 h 15 min. Le righe non compaiono come ore personali di Marta. I comandi di modifica/eliminazione non sono disponibili a Elena; il suo nome non è un campo che possa essere sostituito con quello di un altro utente.

### UAT-16 — Durate non valide e limite giornaliero

Su una giornata distinta prova zero, durata negativa, `1:60` e una frazione decimale che richiede frazioni di minuto, per esempio `0,01`. Salva quindi 24 ore valide e prova ad aggiungere un ulteriore minuto sulla stessa giornata, anche su una commessa diversa.

**Atteso:** i valori invalidi sono rifiutati. Le 24 ore possono essere salvate; il minuto successivo non lo è e il totale resta 1440. Il limite riguarda la persona e la giornata, non la singola commessa.

### UAT-17 — Modifica ed eliminazione delle sole ore proprie

Da Marta inserisci una propria registrazione, modificala e usa Elimina registrazione. Da Elena conserva una sua riga sulla stessa commessa. Verifica che le normali azioni del registro di Marta non permettano di modificare la riga di Elena; il controllo API di UAT-26 completa la prova.

**Atteso:** i comandi agiscono sulla riga di Marta e aggiornano i suoi totali. La riga di Elena rimane. Lo Storico conserva prima/dopo delle operazioni ammesse e può includere note: il registro personale non implica che le note siano sempre invisibili al Gestore.

### UAT-18 — Cinque viste con dati verificabili

Carica in un laboratorio dedicato il dataset di [Esempi di analisi e costi](esempi-analisi-e-costi.md). Imposta il periodo indicato e confronta tutte le tabelle con gli attesi del documento.

**Atteso comune:** **540 minuti, 10 registrazioni, 5 commesse e 4 persone**. Valore contratti noto **10.000 EUR**, un contratto senza valore e 60 minuti senza contratto. Costo noto **447 EUR** su 480 minuti valorizzati, con 60 minuti senza tariffa e indicazione di stima parziale. Il valore del contratto condiviso viene contato una sola volta.

### UAT-19 — Filtri, link e intervalli

Nello stesso dataset combina data, commessa e utente. Per Elena e A nel periodo completo attendi 210 minuti e due righe. Seleziona la stessa data in Dal e Al; prova poi fine precedente all’inizio e un periodo di 367 giorni. Copia un URL di analisi e aprilo nella sessione di un Operatore.

**Atteso:** filtri combinati e date inclusive; intervalli invalidi rifiutati. Il link non concede i diritti del Gestore: l’Operatore rimane limitato ai propri dati e non accede alle viste economiche. Un periodo vuoto mostra zero risultati senza inventare righe. I periodi rapidi usano la data fornita dal server.

### UAT-20 — Tariffe, zero, mancante e sovrapposizione

Prova una tariffa zero, nessuna tariffa e due tariffe consecutive per la stessa persona. Tenta poi di creare un intervallo che condivida almeno un giorno con uno esistente. Modifica una tariffa del passato come nello scenario numerico della guida.

**Atteso:** zero e assenza sono distinti; gli estremi sono inclusi e la sovrapposizione è rifiutata. La tariffa valida alla data del lavoro determina il costo. La modifica del passato ricalcola la stima, senza riscrivere i minuti. Non è presente un comando di eliminazione tariffa.

### UAT-21 — Arrotondamento per registrazione

Su dati separati usa una tariffa di 1 EUR/ora. Inserisci tre righe da un minuto e confronta il costo con una singola riga da tre minuti su un’altra commessa. Ripeti un minuto con tariffa 0,30 EUR/ora.

**Atteso:** tre righe da un minuto valgono 0,06 EUR, la riga da tre minuti 0,05 EUR. Il minuto a 0,30 EUR/ora vale 0,01 EUR. Gli arrotondamenti avvengono prima della somma.

## 7. Alert e notifiche

### UAT-22 — Regola personale, destinatari e orario

Da Marta crea una regola con nome, tipo, orario e almeno un utente attivo. Prova poi una regola priva di destinatari, un indirizzo email non valido e un utente disattivato. Da Bruno apri Le tue regole.

**Atteso:** soltanto configurazioni valide vengono salvate; fino a 50 utenti e 50 email. Bruno non vede automaticamente le regole di Marta. La scheda mostra orario Europe/Rome, stato, ultima e prossima esecuzione. Se la regola è creata dopo l’orario odierno, il prossimo turno è domani. Il cambio automatico tra ora solare e legale richiede un caso temporale dedicato nella suite tecnica; una sola prova odierna non lo certifica.

### UAT-23 — Tentativo su commessa inesistente

Nel laboratorio crea una commessa temporanea e annota il suo ID. Eliminala secondo UAT-27, poi crea la regola Commessa inesistente prima di generare il tentativo da osservare. Con un client di collaudo autenticato come Elena, invia una registrazione valida per data e durata a quell’ID ormai inesistente. Usa il contratto di `POST /api/time-entries` nella [guida API](../tehnical/api-app.md). Imposta Marta ed Elena come destinatari e premi Esegui ora.

**Atteso:** il salvataggio delle ore viene rifiutato; nessuna riga invalida appare nel registro. Marta ed Elena ricevono l’avviso pertinente, mentre un Operatore destinatario diverso non riceve quello di Elena. Una seconda esecuzione non duplica la stessa anomalia. La semplice revoca di accesso a una commessa esistente non va usata al posto di un ID inesistente per questo test.

### UAT-24 — Ore mancanti dopo 48 ore

Questo caso manuale richiede il trascorrere del tempo: non può essere dichiarato superato premendo Esegui ora subito dopo la creazione della regola.

1. Il lunedì `D`, crea la regola Ore giornaliere mancanti prima della fine della giornata, con Marta ed Elena tra i destinatari.
2. In D registra per Elena **479 minuti** e per un altro Operatore attivo di prova **480 minuti**.
3. Esegui un controllo prima che siano trascorse 48 ore dalla mezzanotte che termina D.
4. Eseguilo nuovamente dopo la scadenza; in una settimana senza cambio d’ora, D è verificabile dal giovedì alle 00:00.
5. Esegui ancora il controllo e verifica la deduplicazione.

**Atteso:** prima della soglia nessun avviso relativo a D; dopo la soglia Elena è incompleta, la persona con 480 minuti non lo è. Altri utenti attivi possono produrre ulteriori avvisi se anch’essi incompleti: il monitoraggio non è limitato ai destinatari. Sabato e domenica non sono giornate da completare. La notifica di D conserva il totale rilevato e non viene continuamente aggiornata quando le ore vengono corrette.

Per una verifica rapida delle soglie esatte, dell’ora legale e del recupero dopo riavvio, usare i test temporali tecnici dedicati in `tests/test_alerts.py`, su dati temporanei. Non cambiare l’orologio di un ambiente condiviso per abbreviare questo piano manuale.

### UAT-25 — Lettura, sospensione e email non configurata

Apri una notifica non letta di Elena e premi Segna come letta; confronta il contatore e il Registro ore. Sospendi la regola e usa comunque Esegui ora su una nuova anomalia. Predisponi un indirizzo sintetico nel canale email mantenendo SMTP non configurato.

**Atteso:** la marcatura aggiorna solo la presa visione. Una regola sospesa non si esegue automaticamente, ma il controllo manuale rimane disponibile. Un controllo manuale riuscito registra anche l’esecuzione odierna: il turno automatico della stessa giornata non si ripete. Sospendere la regola non annulla le email già accodate. L’interfaccia dichiara email in attesa; nessuna spedizione reale deve essere presentata come completata. La verifica dell’accettazione SMTP e della consegna richiede una configurazione/fixture dedicata e non è coperta da questa prova.

## 8. Controlli server e operazioni distruttive

### UAT-26 — Rifiuti delle API per profilo e proprietà

Usa sessioni valide di laboratorio e richieste con origine corretta. Per campi, metodi e formato completo consulta [API dell’app](../tehnical/api-app.md). Esegui almeno:

| Richiesta | Profilo | Risultato atteso |
| --- | --- | --- |
| `GET /api/users` o `/api/audit` | Operatore | Rifiuto di accesso amministrativo |
| `POST /api/jobs` con dati altrimenti validi | Operatore | Nessuna creazione |
| `GET /api/labor-rates` | Operatore | Nessuna esposizione di tariffe |
| Report economico o filtro su un’altra persona | Operatore | Rifiuto, senza dati del collega |
| Modifica/eliminazione dell’ID di un’ora di Elena | Gestore Marta | Nessuna modifica alla registrazione altrui |
| Aggiunta di `user_id` alla normale registrazione ore | Operatore | Campo non ammesso; nessun trasferimento di proprietà |
| Lettura o modifica della regola di Marta | Gestore Bruno | Regola non accessibile come propria |
| Marcatura letta della notifica di Elena | Luca | Nessuna modifica alla notifica altrui |

I codici possono distinguere 403, 404 o 400 in base al tipo di controllo descritto dalle API. La verifica principale è che non escano dati riservati e non avvenga una modifica. Un 401 dovuto a una sessione assente non dimostra il controllo sul profilo.

### UAT-27 — Eliminazione definitiva della commessa

**Solo laboratorio.** Prepara una commessa dedicata con almeno una riga di Marta e una di Elena, un grant diretto e uno di gruppo. Apri Modifica → Elimina commessa; verifica il testo e annulla la prima volta. Riapri e conferma Elimina commessa e ore.

**Atteso:** l’annullamento conserva i dati. La conferma rimuove commessa, tutte le sue ore e i suoi grant in un’unica operazione; i totali si riducono in entrambi i registri e nelle analisi. Utenti, gruppi, ruoli, contratto e PDF rimangono. Lo Storico conserva il riepilogo, ma non offre un ripristino. Non eseguire il caso sulla commessa che serve ad altri test ancora da completare.

### UAT-28 — Storico e riservatezza delle evidenze

Da Marta filtra lo Storico per commesse, ore, abilitazioni, contratti, tariffe e regole. Apri i dettagli prima/dopo, ricarica e usa Carica precedenti quando disponibile. Come Elena tenta l’accesso alla stessa API.

**Atteso:** autore, entità e modifica sono ricostruibili; l’Operatore non accede allo storico amministrativo. Password, token e segreti non sono nei payload di audit. Le note delle ore possono invece essere presenti nei dettagli: le evidenze devono usare testi sintetici. Questo caso non simula un guasto della scrittura audit; l’atomicità in caso di errore è coperta da test tecnici dedicati.

## 9. Interfaccia e integrazioni ancora da attivare

### UAT-29 — Uso su schermi piccoli e tastiera

Ripeti accesso, apertura del modulo ore, scelta commessa, compilazione, annullamento e navigazione tra le sezioni a larghezze indicative 1440, 768, 390 e 320 px. Usa Tab, frecce dove previste per le schede ed Escape. Controlla etichette, errori, focus e scorrimento delle tabelle.

**Atteso:** i comandi essenziali restano raggiungibili e nessuna azione richiede esclusivamente il mouse. Le tabelle larghe possono scorrere nel proprio riquadro. Registra i problemi di contrasto o accessibilità osservati: questa prova non attribuisce una certificazione WCAG all’intera applicazione.

### UAT-30 — Stato delle integrazioni e limiti del rilascio

Verifica che Alert e trigger dichiari correttamente l’assenza di SMTP. Controlla tramite le API Calendar lo stato di configurazione previsto dall’ambiente e la disponibilità del servizio tecnico. Nell’installazione attuale OAuth Google è da configurare.

**Atteso:** la mancata configurazione non è presentata come un account Google collegato o un’email spedita. Non esiste ancora una sezione Calendario nella UI, né conversione evento–commessa/ore. Non sono presenti fatturazione, incassi, multi-società o multi-valuta nel flusso corrente. Una schermata assente per una funzione pianificata non è un difetto del collaudo di questa versione.

Le prove reali di consenso Google, lettura/scrittura di un calendario autorizzato, revoca, conflitti remoti e invio SMTP andranno registrate in un collaudo d’integrazione separato quando il provider sarà configurato. I test con fixture locali non attestano il collegamento a un account reale.

## 10. Chiusura del collaudo

Il responsabile della sessione compila un riepilogo con i 30 identificativi, gli esiti, i casi non eseguiti e le anomalie ancora aperte. Non dichiarare «tutte le funzionalità verificate» se sono mancati i casi temporali, i controlli server o un backend previsto dal rilascio.

Una sessione nativa SQLite non dimostra automaticamente il comportamento di MySQL/GridFS. Il laboratorio Docker richiede inoltre le verifiche tecniche pertinenti di cluster, documenti, TLS e rete. Questo piano non richiede prove di guasto dell’infrastruttura né riavvii dello stack operativo.

Conserva le evidenze sintetiche e lo stato della build. Per i criteri del prodotto vedi [Processi e regole](processi-e-regole.md); per i test automatizzati e il rilascio consulta [Sviluppo e test](../tehnical/sviluppo-test.md).
