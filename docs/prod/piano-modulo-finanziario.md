# Piano del modulo finanziario di punto

> **Documento di progettazione, modulo finanziario completo da implementare.** La base iniziale era lo schema 3; il codice corrente usa lo schema 7 e offre valori contrattuali e stime del costo delle ore in EUR. Fatture, incassi, più società, più valute e canali fiscali descritti qui restano pianificati. Consulta [stato e roadmap](roadmap.md) per la distinzione fra disponibilità attuale e obiettivi.

Il modulo finanziario coprirà il percorso dalla previsione economica della commessa alla fattura, alla partita aperta e al suo regolamento. Il primo rilascio finanziario includerà già **fatturazione e incassi**, **più società**, **più valute** e **società con sede fiscale in paesi diversi**. Questi requisiti entrano nelle fondamenta del modello e nel collaudo della prima consegna.

Il piano integra la [roadmap delle 20 funzionalità](roadmap-20-funzionalita.md). F1 è un rilascio completo, sviluppato in incrementi interni; F2, F3 e F4 aggiungono automazioni, acquisti e reporting di gruppo. La base della prima stesura era lo schema 3: le entità e le API seguenti sono il progetto da implementare.

## Funzioni finanziarie previste

| Codice | Funzione | Contenuto | Consegna |
| --- | --- | --- | --- |
| FIN01 | Società e paesi | Società titolare, profilo fiscale, esercizio, fuso, utenti e poteri separati | F1 |
| FIN02 | Valute e cambi | Valuta documento e funzionale, cambi storici e differenze di regolamento | F1; rivalutazioni F4 |
| FIN03 | Clienti e fornitori | Controparti censite, ruoli commerciali e dati di fatturazione versionati | F1 |
| FIN04 | Contratti commerciali | Vendita distinta dagli acquisti, modello a tempo, forfait o milestone | Base F1; preventivi e canoni F2 |
| FIN05 | Budget delle commesse | Versioni approvate di ricavi, lavoro, acquisti e spese, con scostamenti | F1 |
| FIN06 | Tariffe e costo risorse | Costo e prezzo di vendita con validità, valuta e precedenze esplicite | F1 |
| FIN07 | Valorizzazione delle ore | Snapshot di ore e tariffe, fonti fatturabili e rettifiche | F1 |
| FIN08 | Ricavi e avanzamento | Ricavo gestionale maturato secondo regola approvata, distinto da fattura e incasso | F1; previsioni a finire F2 |
| FIN09 | Acquisti e costi diretti | Costi per commessa, fatture collegate e impegni residui | Costi e fatture F1; ordini F2 |
| FIN10 | Note spese e rimborsi | Spese documentate, richiesta, approvazione e rimborso senza doppio costo | Spese manuali F1; flusso collaboratori F2 |
| FIN11 | Fatture clienti | Bozze, righe, imposte, approvazione, documenti e canale per paese | F1 |
| FIN12 | Fatture fornitori | Registrazione, verifica, classificazione per commessa e debiti | F1 |
| FIN13 | Note di credito e anticipi | Rettifiche, documenti originari e somme da allocare | F1; SAL e trattenute avanzati F2 |
| FIN14 | Regole e canali fiscali | Profili per paese, controlli, scambio ed esiti documentati | Canale operativo per ogni società attivata in F1 |
| FIN15 | Scadenziario e partite | Rate, residui, scaduto e anzianità di crediti e debiti | F1 |
| FIN16 | Incassi e pagamenti | Movimenti manuali, allocazioni parziali, acconti e storni | F1 |
| FIN17 | Riconciliazione bancaria | Import estratti conto, duplicati e abbinamenti proposti | F3 |
| FIN18 | Margini e flussi di cassa | Risultati per commessa, società, cliente e periodo; previsioni | Situazione corrente F1; scenari F2 |
| FIN19 | Registro e chiusure | Eventi immutabili, quadrature, periodi bloccati ed export contabile | Base F1; chiusure avanzate F4 |
| FIN20 | Gruppo e intercompany | Rapporti tra società, riepiloghi convertiti ed eliminazioni | Riepilogo per valuta F1; consolidamento F4 |

## F1 Prima consegna completa

F1 è concluso quando dall'interfaccia si può configurare una società e il suo profilo, associare cliente e condizioni economiche a una commessa, valorizzare costi e ricavi, preparare una fattura, completarne il canale operativo previsto, registrare un incasso e verificarne allocazione, residuo e impatto di cassa. Lo stesso rilascio gestisce fatture fornitori e pagamenti manuali.

| Incremento interno | Lavoro da completare | Evidenza di completamento |
| --- | --- | --- |
| F1.0 | Contesto societario, migrazione e isolamento delle API esistenti | Nessun accesso incrociato; dati precedenti conservati |
| F1.1 | Moneta esatta, paesi, profili, cambi, capacità e numerazioni | Importi riproducibili e ruoli verificati |
| F1.2 | Controparti, contratti commerciali, conti e condizioni di pagamento | Anagrafiche utilizzabili per documenti completi |
| F1.3 | Budget, tariffe, costi, valorizzazione ore e ricavi gestionali | Scheda economica con origini ricostruibili |
| F1.4 | Fatture clienti/fornitori, note di credito, file e canali per paese | Flusso utilizzabile per le società del primo avvio |
| F1.5 | Saldi iniziali, rate, incassi, pagamenti, allocazioni, storni e differenze cambio | Partite e disponibilità riconciliate dalla data di avvio |
| F1.6 | Panoramica, scadenziario, registro, periodi ed export | Totali spiegabili e accessi rispettati |
| F1.7 | Collaudo SQLite/MySQL, migrazione, cluster e browser | Scenari completi superati prima del go-live |

F1 ha complessità alta perché comprende anche il modello societario. Gli incrementi servono a costruire e verificare il rilascio; non spostano fatture e incassi a una consegna successiva. La durata verrà dimensionata sugli effettivi paesi, canali e capacità del team.

## Società e isolamento dei dati

Ogni commessa, contratto, gruppo e documento appartiene a una società. Le identità utente rimangono globali, mentre appartenenza, profilo operativo e capacità finanziarie sono attribuiti per società. Lo stesso utente può essere gestore in A, operatore in B e non avere accesso a C.

Introdurre `companies`, `company_memberships`, ruoli amministrativi per società, capacità e assegnazioni. Aggiungere `company_id` alle entità esistenti e relazioni composte `(company_id,id)` per impedire riferimenti incrociati. I codici sono univoci nel proprio ambito societario.

Le API nuove portano la società nel percorso, per esempio `/api/companies/{company_id}/invoices`. Il server verifica società e risorsa a ogni richiesta. Il selettore UI non modifica una variabile globale di sessione: due schede possono operare su società diverse. Cache e stato frontend sono separati per società e le risposte relative a un contesto precedente vengono ignorate.

Le API precedenti possono risolvere automaticamente la società soltanto quando l'utente ha un unico ambito accessibile; altrimenti richiedono un contesto esplicito. Ricerca, conteggi, matrice, audit, esportazioni, errori e download applicano lo stesso filtro. Conoscere la chiave di un blob GridFS non concede il diritto di leggerlo.

Il gestore operativo non acquisisce poteri su tutte le società. L'amministrazione tecnica della piattaforma non concede automaticamente accesso economico; l'assegnazione dei poteri societari è esplicita e tracciata. Una commessa già movimentata non cambia società con una semplice modifica del campo.

Il limite delle 24 ore rimane per persona e data ISO dichiarata, sommando le registrazioni anche se il lavoro è ripartito tra società. Il fuso societario determina oggi e la validità delle abilitazioni, senza trasformare la durata in fasce orarie. I controlli interni possono calcolare il limite, mentre il revisore vede soltanto il proprio ambito. La revoca di una commessa conserva lo storico personale finché la membership della società resta valida; la revoca della membership blocca i dati aziendali di quella società. Un eventuale accesso personale dopo l'uscita richiede una politica esplicita.

## Permessi finanziari

Lettore, Collaboratore e ruoli personalizzati delle commesse continuano a regolare consultazione e ore. I permessi finanziari sono separati e limitabili a una società o a commesse della medesima società.

| Profilo proposto | Capacità principali | Ambito |
| --- | --- | --- |
| Controllo commesse | Budget, costi aggregati, ricavi gestionali e margini | Commesse assegnate |
| Fatturazione | Preparazione fatture, note di credito e documenti | Società assegnata; approvazione distinta |
| Tesoreria | Incassi, pagamenti, conti e allocazioni | Società e conti autorizzati |
| Amministrazione finanziaria | Configurazione economica, approvazioni e periodi | Società assegnata |
| Consultazione finanziaria | Report e scadenziario | Risorse autorizzate |
| Amministrazione società | Membri e attribuzione delle capacità consentite | Società assegnata |

Capacità indicative: `finance.read`, `budgets.manage`, `costs.aggregate`, `rates.read`, `rates.manage`, `invoices.draft`, `invoices.approve`, `invoices.finalize`, `invoices.credit`, `payments.record`, `payments.allocate`, `payments.reverse`, `balances.prepare`, `balances.approve`, `fx.manage`, `periods.lock`, `periods.reopen`, `finance.export` e `finance.consolidate`.

Costi individuali, tariffe e coordinate bancarie richiedono capacità specifiche. Aggregare economicamente le ore non espone le note personali degli altri utenti. Le operazioni sensibili distinguono preparazione e approvazione; dove è ammessa l'approvazione del medesimo autore, la regola è esplicita e tracciata. Revoche e scadenze sono ricontrollate tra anteprima e conferma.

## Valute e calcoli esatti

Ogni società ha una valuta funzionale; documento e movimento mantengono la valuta originale. Il report di gruppo può avere una valuta di presentazione ulteriore. Queste informazioni rimangono distinte e visibili.

Dopo la prima registrazione finanziaria la valuta funzionale non è modificabile dalla configurazione ordinaria; un eventuale cambiamento richiede una procedura dedicata successiva. Le versioni dell'esponente fotografate nei documenti rimangono quelle originarie anche dopo l'aggiornamento del catalogo.

Gli importi finali usano interi a 64 bit in unità monetarie minime, con codice e versione dell'esponente della valuta. `cents` rimane soltanto per la compatibilità dei valori EUR attuali. Il catalogo valutario sarà versionato e aggiornabile dai dati dell'autorità ISO 4217. [SIX e ISO 4217](https://www.six-group.com/en/products-services/financial-information/market-reference-data/data-standards.html)

Prezzi, quantità, aliquote e cambi usano coefficienti interi e scale esplicite o frazioni esatte. Un modulo C comune applica limiti, controlli di overflow e arrotondamenti definiti; nessun `float` o `double`. In JSON importi e coefficienti sono stringhe, formattate senza passaggi in virgola mobile. SQLite e MySQL devono produrre gli stessi esiti.

Per un cambio definito come una unità maggiore origine uguale a `num/den` unità maggiori destinazione:

```text
destinazione_minor = round(
  origine_minor × num × 10^esponente_destinazione
  / (den × 10^esponente_origine)
)
```

La politica di arrotondamento è versionata per profilo, con regole per righe, imposte, totale e differenze. Tutti gli intermedi hanno limiti verificati. Un cambio mancante produce un dato incompleto esplicito, mai uno zero o un cambio assunto uguale a uno.

Il documento confermato conserva importi originali e funzionali, data, fonte e snapshot del cambio. Il regolamento conserva il cambio effettivo e la differenza rispetto al valore della partita. Aggiornare il listino non riscrive il passato. I cambi BCE possono alimentare report o proposte con provenienza: la BCE li pubblica a scopo informativo e ne scoraggia l'uso come tassi di transazione. [BCE sui cambi di riferimento](https://www.ecb.europa.eu/stats/exchange_rates/html/index.en.html)

I riepiloghi mostrano importi per valuta oppure una conversione esplicita. Un totale convertito di più società non viene presentato come consolidamento con eliminazioni intercompany.

## Clienti e contratti commerciali

I contratti attuali richiedono `supplier_id`, mentre `jobs.client` è una descrizione libera. Il loro valore non è quindi una fonte automatica di ricavo né un'anagrafica sufficiente per fatturare.

Introdurre controparti per società con ruolo cliente, fornitore o entrambi e dati fiscali/commerciali versionati. Conservare i fornitori esistenti con mapping esplicito. Distinguere acquisto e vendita; la commessa può avere un contratto commerciale e più riferimenti di approvvigionamento senza reinterpretare quelli precedenti.

Il contratto di vendita definisce cliente, valuta, validità, condizioni di pagamento e modello a tempo, forfait o milestone. Prima di finalizzare una fattura verificare anagrafica e profilo. Il documento salva uno snapshot: cambiare indirizzo o condizioni non modifica fatture già confermate.

## Budget costi e ricavi delle commesse

Il budget ha versioni in bozza, presentate, approvate e superate. La baseline approvata contiene ricavo atteso, minuti/costo lavoro, acquisti e spese; ogni nuova approvazione conserva la versione precedente.

Tariffe di costo e vendita hanno validità, valuta e ambito. La priorità tra contratto, commessa, ruolo e persona è esplicita; sovrapposizioni ambigue sono rifiutate. Il costo del personale può includere la componente aziendale definita dal controller senza esporre retribuzioni agli operatori.

La valorizzazione delle ore salva utente, commessa, data, minuti, revisione della registrazione, tariffa, cambio e risultato. I dati mancanti rimangono da completare. Modificare una tariffa non altera consuntivi approvati. Rettificare ore produce una nuova proposta e, se necessario, storno e sostituzione dell'effetto economico.

F1 intercetta anche aggiornamenti ed eliminazioni attraverso le API delle ore già esistenti. Le fonti valorizzate o fatturate conservano revisioni e cancellazione logica: nessuna modifica distrugge lo snapshot economico o libera automaticamente quantità già fatturate. Le rettifiche commerciali richiedono il flusso documentale applicabile, anche in un periodo ancora aperto.

| Indicatore | Origine | Regola |
| --- | --- | --- |
| Budget | Versione approvata | Previsione, non movimento effettivo |
| Impegno | Ordine/obbligazione approvata | Residuo impegnato, distinto dal costo maturato |
| Costo gestionale maturato | Lavoro valorizzato, spesa o acquisto riconosciuto | Una fonte produce un solo costo attivo |
| Ricavo gestionale maturato | Regola T&M, milestone o evento manuale approvato | Non coincide automaticamente con ore × prezzo |
| Fatturato netto | Documenti attivi validi, al netto di imposte e rettifiche | Non risomma ricavi già riconosciuti |
| Incassato e pagato | Movimenti di tesoreria registrati | Flussi distinti da ricavi e costi |
| Credito e debito residui | Partite e allocazioni | Valore da regolare nella valuta della partita |
| Margine gestionale | Ricavo gestionale meno costo gestionale | Periodo, valuta e criterio dichiarati |

Una fattura fornitore collegata a una spesa già riconosciuta la collega al debito o sostituisce la rilevazione tramite un legame esplicito, senza doppio costo. Le fonti fatturabili conservano minuti, quantità e importo residui per impedire doppie fatturazioni.

## Fatturazione e profili per paese

Il motore comune gestisce testata, righe, quantità, prezzi, sconti, componenti fiscali, scadenze e fonti fatturabili. Una fattura può ripartire righe su più commesse della stessa società. Aliquote, esenzioni, reverse charge, ritenute e altri componenti sono configurazioni versionate del profilo applicabile, non deduzioni dalla sola valuta.

Ogni società registra paese e ulteriori registrazioni fiscali necessarie. Un profilo documentale identifica tipo di operazione, formato, calcoli, numerazione, controlli e canale. La Commissione europea mantiene schede nazionali che distinguono piattaforme, specifiche e reporting: il piano prevede adattatori per paese. [Schede eInvoicing della Commissione europea](https://ec.europa.eu/digital-building-blocks/sites/spaces/DIGITAL/pages/467108874/eInvoicing%2BCountry%2BFactsheets%2Bfor%2Beach%2BMember%2BState%2Band%2Bother%2Bcountries)

Gli stati sono separati:

- **Documento interno:** bozza, presentato, approvato, finalizzato; la finalizzazione congela contenuto e numero.
- **Canale fiscale/consegna:** da predisporre, pronto, trasmesso, scartato, accettato o consegnato secondo il protocollo. Non forzare tutti i paesi agli stessi eventi.
- **Registrazione gestionale:** da registrare, registrato o stornato secondo il profilo.
- **Regolamento:** non regolato, parziale o saldato, derivato dalle allocazioni; lo scaduto è una proprietà delle rate aperte.

Il primo avvio include un canale operativo validato per ciascuna società attivata. Modalità previste: generazione/scambio file con il sistema fiscale o contabile già usato e acquisizione degli esiti, oppure connettore testato al provider scelto. Lo scambio tramite file mantiene l'app senza uscita Internet; il connettore richiede un componente dedicato, destinazioni autorizzate, outbox e ricevute persistenti.

Per l'Italia l'adattatore prevede XML ed esiti SdI secondo le specifiche applicabili. Il PDF è la rappresentazione leggibile e non sostituisce il flusso strutturato previsto dal profilo. [Fatturazione elettronica dell'Agenzia delle Entrate](https://agenziaentrate.gov.it/portale/web/english/electronic-invoicing)

Il supporto generale a società estere non attiva automaticamente ogni regime fiscale. L'elenco nominativo dei paesi e dei canali entra nella definizione di F1: per ogni società si convalidano formato, operazioni, esiti e conservazione prima dell'emissione nel paese interessato. Dove il sistema esterno resta quello di emissione, punto ne acquisisce documenti definitivi e riferimenti senza presentarli come generati o trasmessi internamente.

Le fatture finalizzate si correggono con note di credito o procedure del profilo, senza riscrittura. Gli scarti seguono il flusso di correzione del canale, conservando i tentativi senza duplicare effetti finanziari. La numerazione usa una sequenza transazionale per società, serie, tipo ed esercizio, con vincolo univoco; non `MAX(numero)+1` o l'ID automatico. I numeri dei documenti esterni rimangono originali.

Le fatture fornitori conservano riferimento esterno, fornitore, data documento e data ricezione distinte. Il controllo duplicati considera società, controparte, riferimenti e impronta: stesso numero di fornitori diversi non è un duplicato automatico.

## Incassi pagamenti e partite

Ogni documento produce una o più scadenze con valuta, importo dovuto e data. Incasso o pagamento identifica società, conto, controparte, data, valuta, importo, fonte e riferimento. Registrare un pagamento nel gestionale è distinto dall'eseguire un trasferimento bancario.

Ogni conto di tesoreria ha una valuta. Il movimento di cassa è espresso in quella valuta; un documento regolato può usarne un'altra tramite l'allocazione e il cambio esplicito. Trasferimenti o cambi tra conti usano movimenti distinti collegati, con eventuali commissioni separate, senza conversioni implicite del saldo. L'allocazione richiede stessa società, controparte e direzione compatibile: un incasso cliente non regola un debito fornitore. Pagamenti di terzi e compensazioni tra partite richiedono un flusso dedicato prima di essere ammessi.

Il movimento può essere parziale, coprire più scadenze o restare un anticipo non allocato. Ogni allocazione conserva la quota nella valuta del movimento e in quella della partita, il controvalore funzionale e la differenza cambio. Non si sottraggono direttamente EUR da una partita USD.

I residui derivano da allocazioni, crediti e storni; nessun comando imposta arbitrariamente una fattura come pagata. L'eccedenza rimane un anticipo. Una nota di credito viene allocata una sola volta, senza ridurre sia il totale originario sia il residuo attraverso due percorsi.

Registrazione, allocazione e storno richiedono revisione, UUID, autorizzazione aggiornata e transazione con audit. Le allocazioni non superano residuo della partita o disponibilità del movimento. Stornare un pagamento neutralizza le sue allocazioni nella stessa transazione e riapre le partite; una restituzione effettiva di denaro è un movimento distinto.

## Data di avvio e saldi iniziali

F1 definisce una data di avvio per società. Un lotto di apertura raccoglie saldi dei conti, crediti e debiti residui, anticipi non allocati e relativi documenti di provenienza. Ogni partita conserva controparte, scadenza, valuta originale e controvalore funzionale, con cambio e criterio documentati.

Preparazione e approvazione sono distinte. L'apertura produce scritture idempotenti e quadrate contro conti di apertura, senza generare nuovamente ricavi o costi già maturati. La verifica riconcilia conti e partite con le fonti alla data scelta; differenze o dati mancanti impediscono di presentare il saldo come completo. Importare successivamente la fattura storica la collega alla partita di apertura, senza un secondo debito o credito.

Le ore precedenti conservano il loro valore operativo. Non vengono valorizzate automaticamente dalla migrazione: eventuali recuperi economici richiedono tariffe, intervallo, fonti e approvazione espliciti, evitando sovrapposizioni con le aperture.

## Registro gestionale e periodi

Introdurre un registro immutabile degli effetti finanziari, con fonte e versione univoche. Ogni effetto indica società, competenza gestionale, periodo, conti/categorie, commessa e importi. Le scritture adottano quadratura dare/avere nella valuta funzionale e rendono ricostruibili saldi, arrotondamenti e differenze cambio. La partita doppia gestionale non attribuisce automaticamente conformità alla contabilità statutaria dei paesi; tracciati e regole di integrazione sono validati per società.

Il periodo può essere aperto o bloccato. Il blocco copre anche modifiche indirette: ore già valorizzate, riallocazioni o tariffe non riscrivono effetti storici. Le rettifiche vanno in un periodo aperto; riaprire richiede capacità specifica, motivo e audit.

Le scritture delle fatture separano imponibile, imposte, partita e collegamento a ricavi/costi già riconosciuti. La cassa regola la partita senza creare nuovamente ricavo o costo. Le regole di posting sono versionate, con esempi di quadratura per ogni evento prima dello sviluppo.

## Interfaccia e report

La sezione **Finanza** comprende Panoramica, Commesse, Clienti e fornitori, Budget, Fatture, Incassi e pagamenti, Scadenziario, Registro e Configurazione. Ogni pagina indica società, periodo e valuta; sezioni e azioni dipendono dalle capacità dell'attore.

La scheda economica della commessa mostra budget, costo maturato, ricavo gestionale, margine, fatture, incassi allocati e residui. Ogni totale apre le sue fonti entro i permessi; costi individuali oscurati quando è consentita solo l'aggregazione.

La fattura mostra stato interno, canale, rate, allocazioni, rettifiche e cronologia. La tesoreria distingue movimenti registrati, non allocato e previsioni. Il saldo indica l'ultima riconciliazione; non appare come saldo bancario in tempo reale senza collegamento e verifica.

F1 include il riepilogo delle società autorizzate per valuta; ogni conversione dichiara cambi e copertura. F4 aggiunge consolidamento gestionale, rapporti di gruppo ed eliminazioni. Accesso a una società non concede il dettaglio delle altre.

## Modello dati proposto

Il numero definitivo delle tabelle sarà stabilito nello schema dettagliato.

| Modulo | Entità principali |
| --- | --- |
| Società e accessi | `companies`, `company_memberships`, `company_roles`, `company_role_capabilities`, `company_role_bindings`, ambiti per commessa |
| Controparti | `counterparties`, ruoli, anagrafiche/indirizzi versionati, mapping fornitori |
| Moneta e profili | `currencies`, `fx_rates`, `company_finance_settings`, `tax_rule_versions`, `invoice_profiles`, `number_sequences` |
| Contratti commerciali | Vendita e condizioni, riferimenti acquisto, `job_finance_settings` |
| Economia commessa | `budget_versions`, `budget_lines`, `rate_versions`, `time_valuations`, `revenue_events`, `expense_items` |
| Documenti | `invoices`, `invoice_lines`, `invoice_line_job_allocations`, `invoice_source_allocations`, `invoice_installments`, `invoice_document_links`, `invoice_transmissions` |
| Tesoreria | `treasury_accounts`, `payments`, `payment_allocations`, `credit_allocations`, lotti e righe di apertura |
| Registro | `financial_periods`, `financial_events`, `financial_event_lines`; riuso di audit e `mutation_receipts` |
| Estensioni | Ordini/impegni, note spese, estratti conto, matching, scenari e rapporti intercompany |

Ogni entità societaria usa `company_id`, revisione, autore e date tecniche. Importi con valuta ed esponente; effetti registrati anche con controvalore funzionale, cambio e regola. Fattura, commessa, conto, controparte e allocazione richiedono la stessa società, salvo i collegamenti intercompany espliciti.

PDF, XML, eventuali buste firmate, ricevute e giustificativi sono in un archivio GridFS finanziario separato, con metadati SQL e hash. Il validatore è distinto da quello attuale dei soli PDF contrattuali. Import XML senza entità esterne o DTD, con limiti di struttura, dimensione e archivi. Backup e integrità dei blob rimangono distinti dalle procedure di conservazione del profilo paese.

SQL e MongoDB non condividono una transazione distribuita: usare stati persistenti da preparare, pronto o fallito, generazione da snapshot, retry idempotenti e riconciliazione degli orfani. Il canale esterno ha un'outbox e identifica univocamente ricevute e tentativi.

## API proposte

Le rotte finanziarie sono nel contesto `/api/companies/{company_id}`. I percorsi seguenti sono relativi a questo prefisso.

| Percorso | Funzione |
| --- | --- |
| `/finance-settings`, `/memberships`, `/finance-roles` | Configurazione e capacità |
| `/counterparties`, `/sales-contracts` | Controparti e contratti commerciali |
| `/currencies`, `/fx-rates`, `/invoice-profiles`, `/tax-rules` | Valute, cambi e profili |
| `/jobs/{job_id}/finance` e sottorisorse `/budgets`, `/rates`, `/valuations`, `/revenue-events` | Economia commessa |
| `/invoices` | Documenti clienti/fornitori, fatture/note di credito |
| `/invoices/{id}/submit`, `/approve`, `/finalize` | Transizioni autorizzate |
| `/invoices/{id}/installments`, `/documents`, `/transmissions` | Rate, file ed esiti |
| `/payments`, `/payments/{id}/post`, `/allocations`, `/reverse` | Movimenti e allocazioni |
| `/opening-balances`, `/opening-balances/{id}/approve` | Preparazione e approvazione delle aperture |
| `/dashboard`, `/aging`, `/cash-flow`, `/journal` | Report e registro |
| `/periods/{id}/lock`, `/reopen` | Chiusura e riapertura |

Le scritture specificano revisione e UUID; importi come stringhe canoniche. Il server esegue i calcoli vincolanti. Le anteprime non producono effetti economici; la conferma ripete controlli e autorizzazioni nella transazione.

## Evoluzioni F2 F3 e F4

| Rilascio | Contenuto | Dipendenza |
| --- | --- | --- |
| F2 Acquisti e pianificazione | Preventivi, ordini, impegni, confronto ordine/ricezione/fattura, note spese, canoni, SAL, trattenute, proposte ricorrenti, cash-flow previsionale e margine a finire | F1 e approvazioni |
| F3 Banche e automazioni | Estratti conto, deduplicazione, riconciliazione assistita, matching, altri connettori e scambio contabile | Partite F1; importazioni R3 |
| F4 Gruppo e controllo | Intercompany con documenti autonomi, matching, conversione di gruppo, eliminazioni, rivalutazioni e chiusure avanzate | F1; riconciliazioni e profili verificati |

I connettori necessari ai paesi del primo go-live appartengono già a F1. F3 aggiunge automazioni e canali, non la prima possibilità di fatturare. Le banche iniziano da acquisizione e riconciliazione dei dati; eventuali flussi dispositivi sono processi separati con autorizzazioni esplicite.

## Integrazione con la roadmap esistente

- **R0 obbligatorio:** revisioni, idempotenza, validatori, autorizzazione per risorsa e transazioni con audit. F1.0 estende il contesto alla società prima di attivare il secondo ambito.
- **R1 riutilizzabile:** componenti accessibili, viste, comandi e matrice. Non occorre completare tutte le migliorie visive per iniziare il dominio finanziario.
- **R2 alimenta i consuntivi:** griglia e richieste di rettifica dialogano con valorizzazioni e periodi. F1 anticipa obbligatoriamente revisioni, cancellazione logica e protezione delle fonti valorizzate o fatturate su tutti i percorsi esistenti; soltanto il flusso completo delle richieste può attendere R2.
- **R3 fornisce import e batch:** aggiungere validatori finanziari senza importare posting impliciti o movimenti duplicati.
- **R4 amplia le deleghe:** capacità economiche solo se assegnate; F1 ha già capacità finanziarie per società.
- **R5 revisioni e obblighi:** le fatture F1 hanno comunque snapshot propri; nuove revisioni non modificano documenti già finalizzati.
- **R6 e R7:** offline inizialmente sulle ore. Finalizzazione fatture, posting e pagamenti online; OCR propone campi senza approvare movimenti.

## Migrazione dei dati esistenti

1. Backup SQL/GridFS, conteggi e impronte; provare su copia dello schema 3.
2. Creare una società iniziale per i dati attuali, mantenendo ID, hash credenziali, storage key, ore e grant. Completare i dati fiscali nell'onboarding.
3. Trasferire i poteri operativi nella società iniziale senza attribuire lettura finanziaria implicita. Nominare gli amministratori finanziari esplicitamente nel setup.
4. Rendere gruppi, ruoli e grant societari. Risolvere Lettore/Collaboratore per codice nella società, eliminando i riferimenti fissi agli ID 1 e 2 prima di creare altri cataloghi.
5. Conservare valori EUR come unità minime con esponente 2; migrare `value_cents` e il vincolo EUR senza conversioni economiche. Compatibilità legacy solo nelle rotte che dichiarano EUR.
6. Conservare `jobs.client` fino al mapping esplicito della controparte e il lato fornitore dei vecchi contratti. Nessuna generazione di fatture o ricavi nella migrazione.
7. Aggiungere società all'audit senza alterarne payload storici o immutabilità, con migrazione controllata dei metadati e nessuna riscrittura degli eventi di dominio.
8. DDL una sola volta sul nodo scelto, scritture sospese e verifica di allineamento. Test di accesso e integrità prima della seconda società.

Migrazione equivalente SQLite/MySQL. Dopo movimenti o trasmissioni finanziarie, il recupero deve rispettare documenti, ricevute e sistemi esterni coinvolti: nessuna migrazione inversa distruttiva automatica.

## Collaudo e condizioni di rilascio

F1 deve superare almeno questi scenari:

1. A e B con stessi codici e permessi diversi; nessuna fuga da elenchi, audit, ricerca, export, PDF o errori. Due schede con contesti indipendenti.
2. Gestore operativo senza finanza; costi aggregati accessibili soltanto con capacità dedicata, senza note individuali.
3. Valute con esponenti diversi, arrotondamenti, quantità frazionarie e overflow; stessi risultati C/SQLite/MySQL. Cambio assente segnalato.
4. Fattura su più commesse, rate e valuta diversa dalla funzionale; numerazione unica con richieste sui tre nodi.
5. Incassi parziali, anticipo, nota di credito e storni; residui e differenze cambio riconciliati.
6. Rettifica ore valorizzate e fattura fornitore su spesa esistente; nessun doppio costo/ricavo o consumo eccessivo di fonti fatturabili.
7. Doppio invio e timeout dopo commit/failover; una sola fattura, scrittura e allocazione.
8. Periodo bloccato anche rispetto a modifiche indirette; rettifiche in periodo aperto e riapertura tracciata.
9. Profili dei paesi del go-live: file, controlli, esiti, scarti e documenti esterni coerenti. Nessuna emissione fiscale dedotta dal solo PDF.
10. Crash tra SQL, documenti e canale: ripresa, hash invariati e nessuna doppia contabilizzazione.
11. Dati EUR, permessi operativi e documenti precedenti conservati; schema e dati concordanti sui tre nodi.
12. Aperture approvate e riconciliate, import ripetuto senza effetti ulteriori e documento storico collegato senza duplicare la partita.
13. Allocazioni tra società, controparti o direzioni incompatibili rifiutate; trasferimenti tra conti in valute diverse con saldi e commissioni riconciliati.
14. Valuta funzionale bloccata dopo la prima registrazione; aggiornamento del catalogo senza cambiamenti ai documenti storici.

Esempio numerico senza componenti fiscali: fattura di 1.000 USD valorizzata a 900 EUR; incasso di 368 EUR allocato a 400 USD, incasso di 285 EUR allocato a 300 USD, nota di credito di 100 USD valorizzata a 90 EUR e allocata alla fattura. Residuo atteso: 200 USD, valore originario residuo 180 EUR e differenza cambio realizzata sui due incassi 23 EUR. Cassa: 653 EUR, senza un secondo ricavo. Il posting effettivo segue il profilo approvato.

## Dati da definire prima del go-live

Raccogliere elenco di società e paesi, valute funzionali, registrazioni fiscali, esercizi, sistemi contabili/fiscali esistenti e modalità di scambio. Definire amministratori, approvatori, costo delle risorse, modelli commerciali, arrotondamenti, data di avvio e fonti dei saldi iniziali.

La raccolta avviene all'inizio di F1 in parallelo al contesto societario e al modulo monetario. Il disegno non presume società tutte italiane o valute tutte con due decimali.
