# Glossario

[Indice prodotto](README.md) · [Processi e regole](processi-e-regole.md)

I termini seguenti descrivono il prodotto attuale. Le voci riferite a sviluppi futuri lo indicano esplicitamente.

## Persone e accessi

| Termine | Significato in punto |
| --- | --- |
| Utente / account | Identità personale con nome, email, password derivata e stato attivo o disattivato. Le registrazioni appartengono al suo identificativo stabile. |
| Gestore | Profilo dell’account che amministra lo spazio di lavoro: anagrafiche, commesse, organizzazione, tariffe e proprie regole di alert. Consulta anche analisi aggregate di più utenti. |
| Operatore | Profilo che consulta le commesse abilitate e registra le proprie ore quando il ruolo sulla commessa lo consente. |
| Ruolo dell’account | Profilo globale Gestore oppure Operatore. È distinto dal ruolo attribuito su una commessa. |
| Ruolo sulla commessa | Insieme di permessi operativi su una commessa: consultazione e, se abilitata, registrazione delle ore. Non concede amministrazione globale. |
| Lettore | Ruolo predefinito sulla commessa che permette la consultazione senza inserimento delle ore. Non è un terzo profilo di account. |
| Collaboratore | Ruolo predefinito sulla commessa che permette consultazione e registrazione delle proprie ore. |
| Gruppo | Insieme di utenti organizzabile in una gerarchia. Un utente può appartenere a più gruppi. |
| Sottogruppo | Gruppo figlio che beneficia delle abilitazioni attribuite ai suoi gruppi antenati. |
| Appartenenza | Collegamento diretto fra un utente e un gruppo. È distinto dall’abilitazione del gruppo a una commessa. |
| Abilitazione / grant | Collegamento fra commessa e utente o gruppo, con ruolo e intervallo di validità. |
| Accesso effettivo | Risultato dei collegamenti diretti, dei gruppi, della validità temporale, dei ruoli e delle regole sulle commesse comuni. |
| Revoca | Rimozione di un’abilitazione. Altri percorsi validi possono continuare a concedere accesso. Lo storico delle ore già inserite rimane personale. |
| Disattivazione | Blocco dell’accesso di un account, conservando i suoi dati storici. È diversa dall’eliminazione di una commessa. |

## Lavoro e documenti

| Termine | Significato in punto |
| --- | --- |
| Commessa | Unità a cui attribuire lavoro: codice univoco, nome, cliente, descrizione, stato ed eventuale contratto. Le richieste che parlano di “progetto” fanno riferimento a questa entità. |
| Commessa comune | Commessa che il gestore rende consultabile e utilizzabile da tutti gli account attivi, senza singole assegnazioni. |
| Stato della commessa | Etichetta del ciclo di lavoro: Da avviare, In corso, In pausa, Completata o Annullata. Da sola non blocca l’inserimento delle ore. |
| Registrazione ore | Riga personale composta da commessa, data del lavoro, durata in minuti e nota facoltativa. Non contiene una fascia di inizio/fine attività. |
| Giornata lavorata | Data a cui si riferiscono le ore, distinta dall’istante in cui vengono salvate. |
| Minuti | Unità intera di persistenza delle durate. La UI permette input e visualizzazione in ore, ma il salvataggio non ammette frazioni di minuto. |
| Storico personale | Registrazioni dell’utente, consultabili anche quando una relativa abilitazione viene revocata; una cancellazione definitiva della commessa elimina invece le ore collegate. |
| Fornitore | Anagrafica del soggetto a cui sono collegati i contratti. Il catalogo non costituisce ancora un’anagrafica finanziaria completa clienti/fornitori. |
| Contratto | Accordo censito con fornitore, codice, titolo, stato, date, eventuale valore EUR e documenti PDF. Può essere collegato a più commesse. |
| Documento contrattuale | PDF originale caricato sul contratto, conservato con metadati e impronta di integrità. Caricare un altro PDF aggiunge un documento. |
| Integrità del documento | Verifica che dimensione e impronta del PDF scaricato coincidano con quelle registrate. Non equivale a validazione legale, antivirus o firma digitale. |
| Eliminazione definitiva | Rimozione senza cestino applicativo. Per una commessa comprende ore di tutti gli utenti e abilitazioni collegate. |
| Storico delle modifiche / audit | Registro di chi ha eseguito le modifiche, quando e con quali valori prima/dopo. È diverso dal registro giornaliero delle ore e non sostituisce un backup. |

## Analisi ed economia

| Termine | Significato in punto |
| --- | --- |
| Periodo | Intervallo di date selezionato nelle analisi; entrambi gli estremi sono compresi. Per una singola giornata, Dal e Al coincidono. |
| Aggregato | Riepilogo di più registrazioni per data, commessa o utente. Non espone automaticamente le note dettagliate delle ore altrui. |
| Valore contrattuale | Importo completo del contratto, contato una volta nella vista quando è collegato al lavoro selezionato. Non è ripartito per giornata o per commessa. |
| Tariffa oraria | Importo EUR per ora attribuito a un utente e a un intervallo di date, usato per stimare il costo del suo lavoro. |
| Costo stimato delle ore | Somma dei costi delle singole registrazioni coperte da una tariffa, con arrotondamento al centesimo per registrazione. |
| Ore non valorizzate | Ore senza una tariffa applicabile. Non corrispondono a lavoro con costo zero. |
| Importo non definito | Valore mancante. È distinto da un importo esplicitamente uguale a zero. |
| EUR / centesimi | Valuta attualmente supportata e unità intera usata per i valori economici. |
| Fattura, incasso, pagamento | Entità del modulo finanziario pianificato. Le pagine attuali di analisi non emettono fatture e non registrano movimenti finanziari. |
| Multisocietà / multivaluta | Isolamento e calcoli per società e valute diverse previsti nel piano finanziario; non disponibili nello schema corrente. |

## Controlli e integrazioni

| Termine | Significato in punto |
| --- | --- |
| Regola di alert | Controllo personale del gestore, con tipo, nome, orario giornaliero, stato attivo e destinatari. |
| Trigger | Condizione che produce una segnalazione: tentativo su commessa inesistente oppure giornata incompleta dopo la tolleranza. |
| Tolleranza | Per le ore mancanti, 48 ore effettive dalla fine della giornata lun–ven. La segnalazione arriva al successivo controllo della regola. |
| Destinatario | Account dell’app o indirizzo email scelto per una regola. Il ruolo del destinatario limita le informazioni ricevibili nell’app. |
| Notifica interna | Avviso dell’account, consultabile in Notifiche e marcabile come letto. |
| Coda email / outbox | Archivio persistente degli avvisi da spedire e dei loro esiti. La presenza in coda non significa che l’email sia stata inviata. |
| Accettazione SMTP | Conferma del server di posta che ha accettato un messaggio. Non certifica la ricezione nella casella finale. |
| Addon | Servizio separato chiamato dall’app attraverso API private: uno per le email e uno per Google Calendar. |
| OAuth | Consenso con cui l’utente autorizza il collegamento del proprio account Google. Il servizio deve essere prima configurato dall’amministratore tecnico. |
| Sincronizzazione bidirezionale | Acquisizione delle modifiche dal calendario Google e invio a Google delle operazioni richieste attraverso le API dell’app. |
| Conflitto calendario | Evento remoto modificato rispetto alla versione su cui si basa una richiesta: viene segnalato senza sovrascrittura automatica. |
| Idempotenza | Un invio ripetuto della stessa operazione conserva il risultato già registrato, evitando di produrre un nuovo effetto. |

Per tipi dei campi, identificativi e stati API consultare il [modello dati](../tehnical/modello-dati.md) e il [catalogo API](../tehnical/api-app.md).
