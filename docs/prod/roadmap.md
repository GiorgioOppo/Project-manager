# Roadmap di prodotto e stato di avanzamento

**Italiano** | [English](../en/prod/roadmap.md)

Questa pagina aggiorna la lettura dei piani rispetto alla base applicativa documentata il **11 ottobre 2026**, schema **14**. Il [piano delle venti funzionalità](roadmap-20-funzionalita.md) e il [piano finanziario](piano-modulo-finanziario.md) conservano il disegno di dettaglio originario, redatto quando la base era lo schema 3. I numeri di schema, i conteggi e le descrizioni di «base attuale» presenti in quei piani sono riferimenti storici, non lo stato corrente.

Gli stati usati qui sono:

- **Implementato**: capacità disponibile nel codice e nella UI prevista.
- **Parziale**: esiste una parte utile della funzione o dell’infrastruttura, ma manca una componente del percorso descritto.
- **Pianificato**: il comportamento richiesto non è ancora disponibile come funzione completa.

La presenza di una tabella, di un componente simile o di un addon non chiude da sola una funzionalità di prodotto. Per la disponibilità operativa corrente vedere la [panoramica](panoramica.md).


Le **venti migliorie della verifica dei flussi**, consegnate in schema 14 e descritte nei [flussi operativi migliorati](flussi-operativi.md), hanno una numerazione distinta dalle **venti funzionalità del piano storico** riportate più avanti. Le migliorie della verifica non completano automaticamente tutti i requisiti dei piani originari; matrice globale degli accessi e rilevamento assistito dei duplicati restano estensioni da sviluppare.
## Base già disponibile

Sono implementati login, amministrazione dello spazio e deleghe ai Gestori area, profili funzionali Commerciali/Finance, commesse e stati, accessi diretti e per gruppi gerarchici, ruoli operativi, commesse comuni, registro ore personale, fornitori, contratti/PDF e storico. Sono disponibili cinque viste di Analisi, tariffe di costo EUR, alert giornalieri e notifiche. Il modulo Attività comprende attività e sottoattività, Kanban, registro dei blocchi, stime e confronto con la capacità personale. Chiusura e archivio conservano i dati; la cancellazione definitiva separata è bloccata quando la commessa contiene dati consolidati o riferimenti economici protetti.

Gli addon email e Google Calendar sono realizzati come servizi C separati e hanno percorsi UI disponibili. Email richiede un provider SMTP configurato per spedire: coda, annullamento degli invii pendenti e revisione degli esiti incerti sono consultabili dai responsabili autorizzati. Calendar offre collegamento, consultazione e sincronizzazione; l’uso effettivo richiede configurazione OAuth del provider e consenso personale. Le proposte ore da eventi personali passano da una bozza e da una conferma esplicita.

## Nuovo percorso: attività e collaborazione

La prima consegna comprende le tre priorità concordate: **attività e sottoattività, Kanban e registro dei blocchi**. I gestori organizzano il lavoro e scelgono responsabili, priorità e scadenze; i collaboratori aggiornano lo stato delle attività assegnate e segnalano o gestiscono i propri impedimenti. La [guida del modulo](attivita-kanban-e-blocchi.md) descrive comportamento e permessi.

Questo percorso si aggiunge alle venti migliorie originarie senza sostituirne la numerazione.

| Funzione proposta | Stato corrente | Evoluzione ancora da realizzare |
| --- | --- | --- |
| Attività e sottoattività | Implementato per organizzazione, avanzamento e ore collegate | Checklist separate |
| Bacheca Kanban | Implementato | Ulteriori preferenze di vista e automazioni solo se definite in un rilascio successivo |
| Registro rischi e blocchi | Parziale | Blocchi implementati; rischi futuri con probabilità e misure preventive da sviluppare |
| Dipendenze e cronoprogramma | Pianificato | Relazioni temporali, impatto dei ritardi e Gantt |
| Carico di lavoro del team | Parziale | Stime delle attività e confronto con capacità personale implementati; pianificazione aggregata del team, dipendenze e livellamento da sviluppare |
| Discussioni e decisioni | Pianificato | Commenti, menzioni e decisioni motivate con visibilità per commessa |
| Richieste di variazione | Pianificato | Valutazione e approvazione dell’impatto su lavoro, scadenze e budget |
| Consegne e accettazione | Pianificato | Versioni, revisione, osservazioni e approvazione dei risultati |
| Portale cliente | Pianificato | Accesso esterno limitato a contenuti e richieste esplicitamente condivisi |
| Panoramica di tutte le commesse | Parziale | Vista personale trasversale e riepilogo commessa implementati; portafoglio complessivo da sviluppare |
| Timer personale | Pianificato | Avvio/pausa, proposta da confermare e controllo delle sovrapposizioni |
| Chiusura guidata della commessa | Implementato | Riepilogo lavoro aperto, checklist con riferimenti ticket/dotazioni verificati manualmente, blocco nuove ore, archivio e riapertura motivata |

La bacheca attuale usa scelte di stato con conferma, accessibili da tastiera. Stato di attività, genitori, figli e blocchi rimangono indipendenti; non sono presenti avanzamento percentuale calcolato o approvazioni formali delle attività. Sono disponibili notifiche operative per assegnazioni e scadenze. Le revisioni proteggono le modifiche concorrenti di attività e blocchi, senza estendere automaticamente quel meccanismo a tutte le API storiche.

I prossimi passi consigliati sono dipendenze, cronoprogramma e pianificazione aggregata del team. Stime, capacità personale, orari individuali e richieste di assenza approvate sono già disponibili. Il collegamento ore–attività è disponibile come riferimento facoltativo nel registro e nel riepilogo commessa; la nota rimane descrittiva.

## Le venti funzionalità del piano storico

La numerazione di questa tabella rimane quella del piano originario per rendere confrontabili priorità e requisiti. Per le venti migliorie emerse dalla successiva verifica usare la tabella separata dei [flussi operativi](flussi-operativi.md#venti-migliorie-della-verifica-schema-14).

| N. | Funzionalità | Stato corrente | Da completare per chiuderla |
| --- | --- | --- | --- |
| 1 | Registro ore settimanale a griglia | Implementato | Sette giorni, dettaglio delle righe, inserimenti multipli atomici e totali coerenti |
| 2 | Copia giornata o settimana precedente | Implementato | Copia settimana precedente e copia selettiva di righe/giornate con destinazione esplicita, anteprima editabile e conferma atomica; nessun salvataggio automatico |
| 3 | Commesse preferite e recenti | Pianificato | Preferenze personali persistenti e ordinamento del selettore, sempre subordinati agli accessi correnti |
| 4 | Richiesta di correzione delle ore | Implementato | Richiesta motivata, approvazione/rifiuto/annullamento, storico e conflitti |
| 5 | Controlli sulle anomalie del registro | Parziale | Orari individuali versionati, assenze e festività implementati; avvisi sui possibili duplicati da sviluppare |
| 6 | Viste personali salvate | Parziale | I filtri delle analisi sono già nell’URL; mancano viste nominate, preferenze di colonne/ordinamento e persistenza per account |
| 7 | Ricerca globale | Pianificato | Un’unica ricerca tra risorse autorizzate; le ricerche locali di Commesse, Fornitori e Contratti esistono già |
| 8 | Comandi rapidi da tastiera | Pianificato | Barra Ctrl/Cmd+K con catalogo delle azioni e successiva ricerca dei record; restano disponibili i normali controlli da tastiera della UI |
| 9 | Interfaccia personalizzabile e accessibile | Parziale | UI responsive, etichette e navigazione da tastiera presenti; tema/densità, revisione del contrasto e verifica completa dei flussi restano da completare |
| 10 | App installabile con bozze offline | Parziale | Bozze personali sul dispositivo per ore, inserimenti multipli e creazione guidata implementate con scadenza di sette giorni, pulizia al logout/cambio account e recupero esplicito; installazione PWA e disponibilità completa offline da sviluppare |
| 11 | Importazione guidata da CSV | Pianificato | Mappatura colonne, anteprima, errori per riga, conferma e successivo onboarding degli utenti senza password nel CSV |
| 12 | Operazioni su più elementi | Pianificato | Selezione multipla, anteprima di stati/abilitazioni/revoche e conferma con controllo delle revisioni |
| 13 | Creazione guidata e modelli di commessa | Implementato | Dati, contratto, squadra, attività e riepilogo; conferma atomica, modelli versionati con archivio e ripristino esplicito |
| 14 | Matrice degli accessi effettivi | Parziale | Spiegazione per persona e commessa implementata con fonti dirette/ereditate e date; matrice globale da sviluppare |
| 15 | Responsabili di area con deleghe limitate | Implementato | Amministratore dello spazio distinto dal Gestore area; deleghe per gruppo e sottogruppi, capacità esplicite, date inclusive e revoca immediata alle richieste successive; le ore mantengono i limiti della squadra |
| 16 | Rilevamento dei duplicati | Parziale | Codici univoci e hash PDF già presenti; mancano confronto assistito, avvisi per partita IVA/contenuto e decisione motivata |
| 17 | Versioni e integrazioni contrattuali | Implementato | Commerciali prepara e pubblica revisioni immutabili dei dati e dei riferimenti PDF; collegamento esplicito della commessa a una versione pubblicata del proprio contratto |
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

Questa è la sequenza storica di dipendenze, non un calendario già impegnato né un elenco di funzioni tutte ancora da realizzare. Le parti già consegnate sono indicate nelle tabelle di stato. R6 e R7 possono procedere separatamente quando le rispettive basi sono pronte. I controlli già introdotti sugli alert e gli addon vanno integrati nel disegno: non devono essere duplicati con un secondo motore equivalente.

La priorità pratica di R0 è evitare che griglia, copia, CSV, offline e operazioni multiple producano comportamenti diversi per gli stessi dati. Il salvataggio ripetuto della stessa operazione dovrà avere un solo effetto; una modifica su una revisione ormai cambiata dovrà restituire un conflitto visibile.

## Percorso finanziario

Il requisito concordato per il **primo rilascio finanziario F1** include già fatturazione e incassi, più società, più valute e società con sede fiscale in paesi diversi. F1 non è considerato completo consegnando soltanto una stima del costo delle ore.

I report EUR e il controllo economico interno sono disponibili a Finance: budget per commessa, costo residuo previsto, consuntivo dei periodi aperti, fotografie approvate immutabili e rettifiche economiche separate e motivate. L’approvazione di un periodo globale blocca le modifiche alle ore sottostanti; le vecchie fotografie non vengono ricalcolate al cambio delle tariffe. Queste funzioni **non completano F1**: mancano società isolate, anagrafiche fiscali clienti, tariffe di vendita, fatture, movimenti di denaro, partite e chiusure contabili multi-società.

| Codice | Funzione prevista | Stato corrente / destinazione |
| --- | --- | --- |
| FIN01 | Società e paesi | Pianificato F1: contesto e autorizzazioni separati per società |
| FIN02 | Valute e cambi | Pianificato F1: valuta del documento, valuta funzionale, cambi storici e differenze di regolamento |
| FIN03 | Clienti e fornitori | Parziale: fornitori operativi presenti; controparti e dati fiscali versionati in F1 |
| FIN04 | Contratti commerciali | Parziale: contratti con fornitore presenti; distinzione acquisto/vendita e modelli a tempo, forfait o milestone in F1 |
| FIN05 | Budget delle commesse | Parziale: budget interno EUR, costo residuo e previsione finale disponibili; versioni approvate del budget, ricavi attesi e contesto multi-società da completare in F1 |
| FIN06 | Tariffe e costo risorse | Parziale: costo EUR per utente e validità già disponibile; vendita, valuta, ambiti e precedenze in F1 |
| FIN07 | Valorizzazione delle ore | Parziale: stime live, snapshot di costo approvati immutabili, blocco delle ore e rettifiche economiche esplicite disponibili; fonti fatturabili, tariffe di vendita e contesto societario/fiscale da completare in F1 |
| FIN08 | Ricavi e avanzamento | Pianificato F1 per ricavi gestionali e avanzamento, distinti da fattura e incasso; la previsione interna dei costi già disponibile non valorizza ricavi o flussi di cassa |
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
| FIN19 | Registro e chiusure | Parziale: periodi interni di costo approvati e rettifiche disponibili; registro gestionale multi-società, quadrature, chiusure contabili ed export da realizzare in F1, ampliamenti in F4 |
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

Per l’email resta necessaria la configurazione del provider SMTP nel deployment. La UI della coda permette di consultare gli esiti, annullare un invio ancora pendente e registrare una verifica presso il fornitore quando l’esito è incerto. Una verifica non reinvia il messaggio; un invio già iniziato non è annullabile. L’addon non richiede una nuova identità utente nell’interfaccia.

Per Google Calendar la schermata di collegamento/scollegamento, scelta del calendario, consultazione dello stato e gestione delle operazioni da verificare è disponibile. Restano prerequisiti operativi la configurazione OAuth del provider e il consenso individuale. Il trasporto Google e i token rimangono nell’addon.

Il percorso evento–commessa è disponibile tramite le bozze ore: verifica l’evento nel calendario personale, normalizza giorno e durata, controlla gli accessi aggiornati e richiede conferma prima di registrare le ore. Gli eventi senza orario o cancellati non vengono trasformati in ore; se l’evento è stato modificato, controllare nuovamente la proposta. Un riferimento Ticket può essere inserito come proposta da controllare, senza attribuirgli autorità sui permessi. La conferma della stessa bozza è idempotente. Il tempo pianificato resta distinto da quello confermato: non esiste una conversione automatica né una successiva sincronizzazione delle ore salvate con l’evento.

## Condizioni trasversali di completamento

Ogni rilascio dovrà comprendere interfaccia, comportamento server, dati e migrazioni, controlli di autorizzazione, prove dei casi di errore e documentazione aggiornata. Una funzione che appare nel menu ma non gestisce permessi, salvataggio ed errori non è completa.

I criteri comuni sono: conservazione dei dati preesistenti; esiti riproducibili in SQLite e MySQL quando entrambi sono supportati; nessuna esposizione tramite filtri, conteggi o download; gestione degli invii ripetuti e dei conflitti; percorsi utilizzabili da tastiera e su schermi piccoli. I collaudi storici sono evidenze della versione esaminata e non sostituiscono le prove del rilascio successivo.

I dettagli di implementazione e accettazione sono nel [piano delle venti funzionalità](roadmap-20-funzionalita.md) e nel [piano finanziario](piano-modulo-finanziario.md); le capacità attuali restano descritte da [Processi e regole](processi-e-regole.md).
