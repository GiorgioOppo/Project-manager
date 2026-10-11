# Roadmap di prodotto e stato di avanzamento

**Italiano** | [English](../en/prod/roadmap.md)

Questa pagina aggiorna la lettura dei piani rispetto alla base applicativa documentata il **11 ottobre 2026**, schema **12**. Il [piano delle venti funzionalità](roadmap-20-funzionalita.md) e il [piano finanziario](piano-modulo-finanziario.md) conservano il disegno di dettaglio originario, redatto quando la base era lo schema 3. I numeri di schema, i conteggi e le descrizioni di «base attuale» presenti in quei piani sono riferimenti storici, non lo stato corrente.

Gli stati usati qui sono:

- **Implementato**: capacità disponibile nel codice e nella UI prevista.
- **Parziale**: esiste una parte utile della funzione o dell’infrastruttura, ma manca una componente del percorso descritto.
- **Pianificato**: il comportamento richiesto non è ancora disponibile come funzione completa.

La presenza di una tabella, di un componente simile o di un addon non chiude da sola una funzionalità di prodotto. Per la disponibilità operativa corrente vedere la [panoramica](panoramica.md).


Per i percorsi consegnati in schema 12 consulta i [flussi operativi migliorati](flussi-operativi.md). La matrice globale, la copia della sola giornata e il rilevamento dei duplicati rimangono estensioni distinte.
## Base già disponibile

Sono implementati login, profili Gestore/Operatore, commesse e stati, accessi diretti e per gruppi gerarchici, ruoli operativi, commesse comuni, registro ore personale, fornitori, contratti/PDF e storico. Si sono aggiunte cinque viste di Analisi, tariffe di costo EUR, cancellazione definitiva delle commesse con ore collegate, alert giornalieri e notifiche. La prima consegna del modulo Attività aggiunge attività e sottoattività, Kanban e registro dei blocchi; la cancellazione della commessa comprende ora anche questi elementi.

Gli addon email e Google Calendar sono realizzati come servizi C separati. Email attende la configurazione SMTP. Calendar espone già le API di sincronizzazione bidirezionale, ma attende OAuth e l’interfaccia di collegamento/consultazione. Entrambi sono quindi **parziali come percorso operativo per l’utente**, pur avendo il servizio tecnico implementato.

## Nuovo percorso: attività e collaborazione

La prima consegna comprende le tre priorità concordate: **attività e sottoattività, Kanban e registro dei blocchi**. I gestori organizzano il lavoro e scelgono responsabili, priorità e scadenze; i collaboratori aggiornano lo stato delle attività assegnate e segnalano o gestiscono i propri impedimenti. La [guida del modulo](attivita-kanban-e-blocchi.md) descrive comportamento e permessi.

Questo percorso si aggiunge alle venti migliorie originarie senza sostituirne la numerazione.

| Funzione proposta | Stato corrente | Evoluzione ancora da realizzare |
| --- | --- | --- |
| Attività e sottoattività | Implementato per organizzazione, avanzamento e ore collegate | Checklist separate |
| Bacheca Kanban | Implementato | Ulteriori preferenze di vista e automazioni solo se definite in un rilascio successivo |
| Registro rischi e blocchi | Parziale | Blocchi implementati; rischi futuri con probabilità e misure preventive da sviluppare |
| Dipendenze e cronoprogramma | Pianificato | Relazioni temporali, impatto dei ritardi e Gantt |
| Carico di lavoro del team | Pianificato | Stime di lavoro e confronto del carico con orari individuali e assenze già configurabili |
| Discussioni e decisioni | Pianificato | Commenti, menzioni e decisioni motivate con visibilità per commessa |
| Richieste di variazione | Pianificato | Valutazione e approvazione dell’impatto su lavoro, scadenze e budget |
| Consegne e accettazione | Pianificato | Versioni, revisione, osservazioni e approvazione dei risultati |
| Portale cliente | Pianificato | Accesso esterno limitato a contenuti e richieste esplicitamente condivisi |
| Panoramica di tutte le commesse | Parziale | Vista personale trasversale e riepilogo commessa implementati; portafoglio complessivo da sviluppare |
| Timer personale | Pianificato | Avvio/pausa, proposta da confermare e controllo delle sovrapposizioni |
| Chiusura guidata della commessa | Implementato | Riepilogo lavoro aperto, blocco nuove ore, archivio e riapertura motivata |

La bacheca attuale usa scelte di stato con conferma, accessibili da tastiera. Stato di attività, genitori, figli e blocchi rimangono indipendenti; non sono presenti avanzamento percentuale calcolato o approvazioni formali delle attività. Sono disponibili notifiche operative per assegnazioni e scadenze. Le revisioni proteggono le modifiche concorrenti di attività e blocchi, senza estendere automaticamente quel meccanismo a tutte le API storiche.

I prossimi passi consigliati sono pianificazione del carico e dipendenze, dopo aver definito stime di lavoro e confronto con la capacità; orari individuali e assenze sono già configurabili. Il collegamento ore–attività è disponibile come riferimento facoltativo nel registro e nel riepilogo commessa; la nota rimane descrittiva.

## Le venti migliorie operative

La numerazione rimane quella del piano originario per rendere confrontabili priorità e requisiti.

| N. | Funzionalità | Stato corrente | Da completare per chiuderla |
| --- | --- | --- | --- |
| 1 | Registro ore settimanale a griglia | Implementato | Sette giorni, dettaglio delle righe, inserimenti multipli atomici e totali coerenti |
| 2 | Copia giornata o settimana precedente | Parziale | Copia settimana precedente implementata con anteprima editabile; copia di una singola giornata ancora da sviluppare |
| 3 | Commesse preferite e recenti | Pianificato | Preferenze personali persistenti e ordinamento del selettore, sempre subordinati agli accessi correnti |
| 4 | Richiesta di correzione delle ore | Implementato | Richiesta motivata, approvazione/rifiuto/annullamento, storico e conflitti |
| 5 | Controlli sulle anomalie del registro | Parziale | Orari individuali versionati, assenze e festività implementati; avvisi sui possibili duplicati da sviluppare |
| 6 | Viste personali salvate | Parziale | I filtri delle analisi sono già nell’URL; mancano viste nominate, preferenze di colonne/ordinamento e persistenza per account |
| 7 | Ricerca globale | Pianificato | Un’unica ricerca tra risorse autorizzate; le ricerche locali di Commesse, Fornitori e Contratti esistono già |
| 8 | Comandi rapidi da tastiera | Pianificato | Barra Ctrl/Cmd+K con catalogo delle azioni e successiva ricerca dei record; restano disponibili i normali controlli da tastiera della UI |
| 9 | Interfaccia personalizzabile e accessibile | Parziale | UI responsive, etichette e navigazione da tastiera presenti; tema/densità, revisione del contrasto e verifica completa dei flussi restano da completare |
| 10 | App installabile con bozze offline | Pianificato | Installazione PWA, bozze personali con conservazione esplicita e conferma online, idempotenza e gestione del cambio account |
| 11 | Importazione guidata da CSV | Pianificato | Mappatura colonne, anteprima, errori per riga, conferma e successivo onboarding degli utenti senza password nel CSV |
| 12 | Operazioni su più elementi | Pianificato | Selezione multipla, anteprima di stati/abilitazioni/revoche e conferma con controllo delle revisioni |
| 13 | Creazione guidata e modelli di commessa | Implementato | Dati, contratto, squadra, attività e riepilogo; conferma atomica e modelli espliciti |
| 14 | Matrice degli accessi effettivi | Parziale | Spiegazione per persona e commessa implementata con fonti dirette/ereditate e date; matrice globale da sviluppare |
| 15 | Responsabili di area con deleghe limitate | Pianificato | Amministrazione circoscritta per gruppi/risorse, senza attribuire i poteri globali dell’attuale Gestore |
| 16 | Rilevamento dei duplicati | Parziale | Codici univoci e hash PDF già presenti; mancano confronto assistito, avvisi per partita IVA/contenuto e decisione motivata |
| 17 | Versioni e integrazioni contrattuali | Pianificato | Revisioni pubblicate immutabili e collegamento esplicito della commessa alla versione; più PDF sul contratto non equivalgono a questo flusso |
| 18 | Lettura e compilazione assistita dai PDF | Pianificato | Estrazione/OCR locale, proposte con pagina di provenienza e conferma del gestore, senza modificare gli originali |
| 19 | Registro degli obblighi contrattuali | Pianificato | Obblighi, responsabilità, scadenze, evidenze e agenda interna; gli alert ore esistenti non monitorano già le scadenze contrattuali |
| 20 | Qualificazione dei fornitori | Pianificato | Checklist versionate, requisiti, prove, scadenze e stato di qualificazione informativo |

«Parziale» nelle funzioni 6 e 16 indica una base riutilizzabile, non una vista salvata o un controllo duplicati già completo. I criteri di accettazione estesi restano nel [piano operativo](roadmap-20-funzionalita.md).

## Sequenza dei rilasci operativi

| Rilascio | Ambito | Risultato atteso |
| --- | --- | --- |
| R0 | Fondamenta comuni | Validatori condivisi, autorizzazione per risorsa, revisioni, ricevute di idempotenza e contesto temporale coerente |
| R1 | Navigazione e accessi: 3, 6, 8, 9, 14 | Scelte più rapide, preferenze personali e spiegazione degli accessi |
| R2 | Ore: 1, 2, 4, 5 | Compilazione settimanale, copie controllate, correzioni approvate e anomalie più aderenti all’orario reale |
| R3 | Ricerca e censimento: 7, 11, 12, 13, 16 | Meno inserimenti ripetuti, importazioni verificabili e operazioni multiple |
| R4 | Deleghe: 15 | Responsabili con poteri limitati al proprio ambito |
| R5 | Contratti e fornitori: 17, 19, 20 | Revisioni, obblighi ed evidenze di qualificazione |
| R6 | Mobilità: 10 | Bozze offline personali e salvataggio confermato al rientro online |
| R7 | Documenti: 18 | Lettura assistita dei PDF con revisione umana |

Questa è una sequenza di dipendenze, non un calendario già impegnato. R6 e R7 possono procedere separatamente quando le rispettive basi sono pronte. I controlli già introdotti sugli alert e gli addon vanno integrati nel disegno: non devono essere duplicati con un secondo motore equivalente.

La priorità pratica di R0 è evitare che griglia, copia, CSV, offline e operazioni multiple producano comportamenti diversi per gli stessi dati. Il salvataggio ripetuto della stessa operazione dovrà avere un solo effetto; una modifica su una revisione ormai cambiata dovrà restituire un conflitto visibile.

## Percorso finanziario

Il requisito concordato per il **primo rilascio finanziario F1** include già fatturazione e incassi, più società, più valute e società con sede fiscale in paesi diversi. F1 non è considerato completo consegnando soltanto una stima del costo delle ore.

I report EUR attuali sono il punto di partenza informativo. Non sono presenti società isolate, anagrafiche fiscali clienti, tariffe di vendita, fatture, movimenti di denaro, partite o chiusure finanziarie.

| Codice | Funzione prevista | Stato corrente / destinazione |
| --- | --- | --- |
| FIN01 | Società e paesi | Pianificato F1: contesto e autorizzazioni separati per società |
| FIN02 | Valute e cambi | Pianificato F1: valuta del documento, valuta funzionale, cambi storici e differenze di regolamento |
| FIN03 | Clienti e fornitori | Parziale: fornitori operativi presenti; controparti e dati fiscali versionati in F1 |
| FIN04 | Contratti commerciali | Parziale: contratti con fornitore presenti; distinzione acquisto/vendita e modelli a tempo, forfait o milestone in F1 |
| FIN05 | Budget delle commesse | Pianificato F1: versioni approvate, ricavi attesi e costi previsti |
| FIN06 | Tariffe e costo risorse | Parziale: costo EUR per utente e validità già disponibile; vendita, valuta, ambiti e precedenze in F1 |
| FIN07 | Valorizzazione delle ore | Parziale: stime ricalcolate disponibili; snapshot approvati, fonti fatturabili e rettifiche in F1 |
| FIN08 | Ricavi e avanzamento | Pianificato F1: ricavo gestionale distinto da fattura e incasso; previsione a finire in F2 |
| FIN09 | Acquisti e costi diretti | Pianificato F1 per costi e fatture; ordini e impegni in F2 |
| FIN10 | Note spese e rimborsi | Pianificato: spese manuali in F1, percorso dei collaboratori e rimborsi strutturati in F2 |
| FIN11 | Fatture clienti | Pianificato F1: bozza, righe, imposte, approvazione e documenti |
| FIN12 | Fatture fornitori | Pianificato F1: classificazione e collegamento ai costi/commesse |
| FIN13 | Note di credito e anticipi | Pianificato F1: rettifiche e somme da allocare; SAL e trattenute avanzati in F2 |
| FIN14 | Regole e canali fiscali | Pianificato F1: un percorso operativo verificato per ciascuna società attivata |
| FIN15 | Scadenziario e partite | Pianificato F1: rate, residui, scaduto e anzianità |
| FIN16 | Incassi e pagamenti | Pianificato F1: registrazione manuale, allocazioni parziali, anticipi e storni |
| FIN17 | Riconciliazione bancaria | Pianificato F3: import degli estratti, deduplicazione e abbinamenti proposti |
| FIN18 | Margini e flussi di cassa | Pianificato F1 per la situazione corrente, scenari in F2 |
| FIN19 | Registro e chiusure | Pianificato F1 per registro gestionale, quadrature, periodi ed export; ampliamenti in F4 |
| FIN20 | Gruppo e intercompany | Pianificato: riepiloghi per valuta in F1; intercompany, conversione ed eliminazioni in F4 |

Lo Storico applicativo corrente non è il registro finanziario previsto da FIN19. Un totale di valori contrattuali non è un ricavo contabilizzato; un costo orario stimato non è un pagamento; un totale convertito di società non è già un consolidato con eliminazioni.

### Incrementi interni di F1

1. **F1.0 — Società e isolamento:** attribuire società a dati, documenti e permessi esistenti, conservando gli identificativi e verificando l’assenza di accessi incrociati.
2. **F1.1 — Importi e capacità:** modelli monetari esatti, cataloghi di valuta, profili per paese, numerazioni e ruoli finanziari.
3. **F1.2 — Controparti e condizioni:** clienti/fornitori, contratti commerciali, conti e condizioni di pagamento.
4. **F1.3 — Controllo economico:** budget, costi, tariffe, valorizzazioni e ricavi gestionali con origine ricostruibile.
5. **F1.4 — Documenti:** fatture clienti/fornitori, note di credito, allegati e canali per i paesi del primo avvio.
6. **F1.5 — Regolamenti:** saldi iniziali, scadenze, incassi, pagamenti, allocazioni e differenze di cambio.
7. **F1.6 — Verifica gestionale:** panoramica, scadenziario, registro, periodi ed esportazione.
8. **F1.7 — Collaudo:** migrazioni, permessi, risultati numerici, duplicazioni, interfaccia e recupero degli esiti incerti.

F2 aggiunge acquisti e pianificazione, F3 banche e automazioni, F4 gestione di gruppo e chiusure avanzate. I canali necessari per emettere le fatture delle società del primo avvio fanno già parte di F1.

### Decisioni necessarie per F1

Prima dell’attivazione vanno identificati società, paesi, valute funzionali, dati fiscali, sistemi contabili esistenti e canali di scambio. Vanno inoltre definiti amministratori finanziari, approvatori, modelli commerciali, data di partenza e fonti dei saldi iniziali. Non è stato ancora selezionato l’elenco dei paesi da supportare nel primo avvio.

L’isolamento societario precede l’attivazione della seconda società. Il profilo operativo non deve diventare un accesso indiscriminato ai dati economici o alle altre società. Le fonti già valorizzate o fatturate devono essere protette anche dagli attuali percorsi di modifica/cancellazione: la conservazione finanziaria non può dipendere soltanto da una futura pagina di approvazione.

## Calendario ed email: completamento del percorso utente

Per l’email il passo operativo successivo è la configurazione del provider SMTP, con verifica di coda, esiti e destinatari. L’addon non richiede di introdurre una nuova identità utente nell’interfaccia.

Per Google Calendar occorrono la configurazione OAuth e il consenso individuale, poi una schermata per collegare/scollegare l’account, scegliere il calendario, consultare lo stato e affrontare gli eventuali conflitti. La base API già pronta evita di inserire il trasporto Google direttamente nella UI.

Il percorso evento–commessa dovrà consentire di scegliere solo commesse autorizzate, proporre una durata e richiedere conferma prima di creare ore. Deve distinguere tempo pianificato da tempo effettivamente lavorato, gestire gli eventi modificati o cancellati e impedire doppie registrazioni. Questa trasformazione non avviene oggi automaticamente.

## Condizioni trasversali di completamento

Ogni rilascio dovrà comprendere interfaccia, comportamento server, dati e migrazioni, controlli di autorizzazione, prove dei casi di errore e documentazione aggiornata. Una funzione che appare nel menu ma non gestisce permessi, salvataggio ed errori non è completa.

I criteri comuni sono: conservazione dei dati preesistenti; esiti riproducibili in SQLite e MySQL quando entrambi sono supportati; nessuna esposizione tramite filtri, conteggi o download; gestione degli invii ripetuti e dei conflitti; percorsi utilizzabili da tastiera e su schermi piccoli. I collaudi storici sono evidenze della versione esaminata e non sostituiscono le prove del rilascio successivo.

I dettagli di implementazione e accettazione sono nel [piano delle venti funzionalità](roadmap-20-funzionalita.md) e nel [piano finanziario](piano-modulo-finanziario.md); le capacità attuali restano descritte da [Processi e regole](processi-e-regole.md).
