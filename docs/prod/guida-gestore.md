# Guida del gestore

**Italiano** | [English](../en/prod/guida-gestore.md)

Il **Gestore** amministra lo spazio di lavoro e usa anche il proprio Registro ore. Le funzioni personali di accesso, registrazione e notifiche sono descritte nella [guida utente](guida-utente.md). Questo documento segue l’ordine consigliato per predisporre persone, commesse e controlli.


Per creazione guidata, spiegazione degli accessi, orari individuali, rettifiche e archivio consulta i [flussi operativi migliorati](flussi-operativi.md).
## Preparare lo spazio di lavoro

1. Censisci le persone in **Utenti**.
2. Organizza reparti e squadre in **Gruppi e ruoli**, quando una gestione per gruppi riduce le assegnazioni individuali.
3. Con il profilo **Commerciali**, censisci **Fornitori** e **Contratti** per le commesse che hanno un accordo di riferimento.
4. Crea le **Commesse** e definisci quali sono comuni e quali richiedono abilitazioni specifiche.
5. Assegna utenti o gruppi con il ruolo e le date appropriati.
6. Organizza **Attività e blocchi** per ogni commessa, con responsabili, scadenze e prossime azioni.
7. Con **Gestore + Finance**, inserisci le **Tariffe orarie** per ottenere una stima del costo del lavoro.
8. Configura **Alert e trigger** con orario e destinatari.
9. Usa **Analisi**, **Notifiche** e **Storico** per i controlli successivi.

Le impostazioni non attribuiscono ore automaticamente. La rendicontazione rimane un’azione della persona che ha svolto il lavoro.

## Assegnare i profili funzionali

Il ruolo dell’identità e i profili funzionali sono separati. Ogni account attivo è automaticamente **Operatore**; **Gestore** deriva dal ruolo dell’account. **Commerciali** e **Finance** richiedono un’assegnazione esplicita, anche per un gestore e anche per gli account gestiti da Keycloak. I gestori già esistenti non ricevono automaticamente questi due profili.

| Profilo | Cosa abilita |
| --- | --- |
| Operatore | Lavoro e ore personali sulle commesse autorizzate |
| Gestore | Amministrazione operativa; ore dettagliate e analisi della propria squadra; decisioni sulle rettifiche altrui della squadra |
| Commerciali | Creazione e modifica di fornitori e contratti, caricamento e download dei PDF |
| Finance | Consultazione di fornitori, contratti e PDF; valori contrattuali e costi aggregati per commessa; lettura delle tariffe |

Per assegnare i due profili espliciti, apri **Utenti → Profili**, seleziona **Commerciali** e/o **Finance** e salva. Deselezionare revoca il profilo dalla richiesta successiva. **Finance** da solo è in sola lettura; per creare o modificare tariffe servono insieme **Gestore + Finance**. Per cambiare il collegamento contrattuale di una commessa servono **Gestore + Commerciali**.

La squadra per le ore comprende il gestore stesso, i membri dei suoi gruppi organizzativi e dei relativi sottogruppi. Un gestore senza gruppi vede soltanto le proprie ore; se i gruppi aziendali non sono disponibili, l’ambito si restringe alla sola persona. I gruppi organizzativi non assegnano Commerciali o Finance e questi profili non estendono la visibilità delle ore personali. Finance riceve aggregati economici per commessa, senza registrazioni personali dei colleghi né filtro per persona.

## Creare e gestire gli utenti

Apri **Utenti** e premi **Nuovo utente**. Compila Nome, Email, Password iniziale e Ruolo, quindi premi **Crea utente**. La password iniziale deve avere almeno 8 caratteri; non viene mostrata nell’elenco o nello storico. Consegna le credenziali secondo il canale adottato dalla tua organizzazione.

Scegli **Operatore** per chi deve consultare commesse autorizzate e registrare il proprio lavoro. Scegli **Gestore** per chi deve amministrare lo spazio: il ruolo consente l’amministrazione operativa dello spazio; ore personali altrui e funzioni economiche rispettano gli ambiti descritti sopra.

Con **Identità** puoi cambiare il nome, il ruolo e lo stato **Attivo / Disattivato** degli account locali. Per le identità gestite da Keycloak questi dati si amministrano nel provider; **Profili** resta gestibile localmente. L’email identifica l’account e non è modificabile. Non è presente in questo modulo un reset della password.

| Operazione | Effetto |
| --- | --- |
| Cambiare nome | Aggiorna l’identità visualizzata senza trasferire ore a un altro account |
| Passare da Gestore a Operatore | Riduce i poteri dalla richiesta successiva, anche con sessione aperta |
| Disattivare | Revoca le sessioni e impedisce l’accesso; lo storico viene conservato |
| Riattivare | Permette un nuovo login; non ripristina le sessioni revocate |
| Disattivare o declassare l’ultimo Gestore attivo | Operazione rifiutata per mantenere un responsabile dello spazio |

La gestione corrente ammette fino a 200 utenti. Gli account disattivati possono ancora comparire nelle analisi storiche dei gestori della loro squadra.

## Costruire l’albero dei gruppi

1. Apri **Gruppi e ruoli**.
2. Premi **+ Gruppo** nella sezione **Albero dei gruppi**.
3. Inserisci il **Nome** e scegli **Gruppo padre**, oppure lascia **Primo livello**.
4. Premi **Crea gruppo**.
5. Seleziona il gruppo nell’albero.
6. In **Aggiungi un utente attivo**, scegli la persona e premi **Aggiungi al gruppo**.

La scheda mostra i membri diretti del gruppo selezionato. Un utente può appartenere a più gruppi. Le abilitazioni assegnate a un gruppo valgono per i suoi membri e per quelli dei sottogruppi, anche quando questi ultimi non compaiono nell’elenco dei membri diretti.

Esempio: se «Area tecnica» contiene «Sviluppo» e «Supporto», abilitare Area tecnica a una commessa include entrambi i sottogruppi. Abilitare soltanto Sviluppo non abilita Supporto né tutti i membri del gruppo padre.

Con **Modifica** puoi rinominare o spostare un gruppo. Lo spostamento può cambiare immediatamente gli accessi ereditati dai membri e dai sottogruppi. Il sistema impedisce cicli e gerarchie oltre 16 livelli; sono ammessi al massimo 200 gruppi. **Elimina** è consentito soltanto quando il gruppo non ha membri, sottogruppi o abilitazioni a commesse: rimuovi o ricolloca prima i riferimenti pertinenti.

## Definire i ruoli sulle commesse

I ruoli predefiniti sono:

| Ruolo | Consultazione | Registrazione delle proprie ore |
| --- | --- | --- |
| Lettore | Sì | No |
| Collaboratore | Sì | Sì |

In **Ruoli sulle commesse**, premi **+ Ruolo** per aggiungere una variante. Inserisci Codice, Nome e la scelta **Solo consultazione** oppure **Consulta e registra le proprie ore**. Il codice inizia con una lettera minuscola e può contenere lettere minuscole, numeri, trattini e underscore, fino a 40 caratteri.

I ruoli che consentono la registrazione delle ore abilitano anche la collaborazione nella bacheca: l’operatore aggiorna lo stato delle attività assegnate e crea o modifica i propri blocchi. I Lettori consultano soltanto. La scelta del responsabile di un’attività o blocco non crea un’abilitazione alla commessa.

I ruoli predefiniti non sono modificabili. Le modifiche a un ruolo personalizzato valgono per tutte le abilitazioni che lo usano. Il limite è di 100 ruoli complessivi. Un ruolo operativo non permette di creare commesse, gestire account o accedere ai PDF contrattuali.

## Censire un fornitore

Per creare o modificare serve **Commerciali**. Finance consulta l’anagrafica in sola lettura; il solo ruolo Gestore non apre questa sezione.

1. Apri **Fornitori** e premi **Nuovo fornitore**.
2. Inserisci **Codice** univoco e **Ragione sociale / nome**.
3. Completa, quando disponibili, Partita IVA, Email, Telefono, Indirizzo e Note.
4. Lascia lo stato **Attivo** per poterlo scegliere nei nuovi contratti.
5. Premi **Crea fornitore**.

La ricerca lavora per nome, codice o partita IVA; il filtro distingue Attivi e Disattivati. La modifica dei dati e la disattivazione non eliminano i contratti collegati. Un nuovo contratto può scegliere soltanto un fornitore attivo; un collegamento già esistente può essere conservato. Non esiste in questa sezione un processo di qualificazione del fornitore o un controllo documentale delle certificazioni.

## Censire un contratto e i suoi PDF

Per creare o modificare il contratto e caricare PDF serve **Commerciali**. Finance può consultare e scaricare; il solo ruolo Gestore non concede accesso.

1. Apri **Contratti** e premi **Nuovo contratto**.
2. Inserisci **Codice** univoco e **Titolo**.
3. Seleziona il **Fornitore**. Se manca, censiscilo prima nella sezione Fornitori.
4. Scegli lo **Stato**.
5. Compila facoltativamente **Data iniziale**, **Data finale**, **Valore in euro** e Note.
6. Premi **Crea contratto**.

| Stato | Significato organizzativo suggerito |
| --- | --- |
| Bozza | Accordo in preparazione |
| Attivo | Accordo in corso |
| Scaduto | Termine decorso, da verificare o rinnovare |
| Chiuso | Accordo concluso |
| Annullato | Accordo non più utilizzato |

Queste etichette vengono selezionate da chi ha il profilo Commerciali: il passaggio della data finale non cambia automaticamente lo stato. La fine non può precedere l’inizio. Il valore è facoltativo e usa EUR; il campo vuoto significa **valore non definito**, mentre `0` è un importo esplicito. Scrivi l’importo senza separatori delle migliaia e con al massimo due decimali, per esempio `12500,00`.

Per i documenti:

1. Nella riga del contratto premi **PDF · numero**.
2. Nel campo **Aggiungi il PDF originale** scegli il file.
3. Premi **Carica PDF** e attendi che compaia nell’elenco.
4. Usa il comando di download del documento per ottenere l’originale.

Sono accettati PDF fino a 10 MiB ciascuno. Ogni caricamento aggiunge un documento: non sostituisce quello con lo stesso nome. Non sono disponibili eliminazione, modifica o preview inline dei documenti. I metadati comprendono l’impronta del contenuto e il download verifica l’integrità dei byte. Il caricamento non esegue OCR, classificazione automatica o controllo antivirus.

L’archivio è consultabile con Commerciali o Finance; soltanto Commerciali carica documenti. Abilitare un operatore a una commessa collegata non gli concede accesso a contratti o PDF.

## Creare, aggiornare e chiudere una commessa

1. Apri **Commesse** e premi **Nuova commessa**.
2. Inserisci **Codice commessa** univoco e **Nome commessa**.
3. Imposta lo **Stato**.
4. Compila facoltativamente Cliente e Descrizione.
5. Se hai anche Commerciali, seleziona facoltativamente un **Contratto** censito.
6. Decidi se attivare **Commessa comune**.
7. Premi **Crea commessa**.

Gli stati disponibili sono **Da avviare**, **In corso**, **In pausa**, **Completata** e **Annullata**. Non esiste un ordine obbligato di transizione: il gestore può aggiornare lo stato in base al lavoro. Le etichette di stato non bloccano da sole la registrazione. Usa la chiusura o l’archiviazione motivata per impedire nuove ore a tutti, gestori compresi, conservando i dati; la riapertura richiede un motivo. Il riepilogo mostra attività e blocchi aperti prima della decisione.

Una commessa può riferirsi a un solo contratto, mentre lo stesso contratto può essere usato da più commesse. Un nuovo collegamento non può usare un contratto Chiuso o Annullato. Se un contratto già collegato cambia stato, il riferimento può rimanere. Selezionare **Nessun contratto** rimuove il collegamento.

Il Cliente è un campo descrittivo. Non crea automaticamente un cliente finanziario, un contratto di vendita o una fattura.

Per predisporre le attività comuni, verifica o crea **Generale**, **Formazione**, **Riunioni** e **Attività interne**, usando codici distinti e la casella **Commessa comune**. Tutti gli account attivi le potranno consultare e usare per le proprie ore. La creazione delle quattro voci è un’attività del gestore, non un seed obbligatorio di una nuova installazione.

## Abilitare utenti e gruppi

1. Nella riga della commessa premi **Abilitazioni**.
2. Per una persona, usa **Abilita un utente**, seleziona Utente e **Ruolo sulla commessa**.
3. Compila, se necessarie, **Dal giorno** e **Al giorno**, quindi premi **Abilita utente**.
4. Per una squadra, usa **Abilitazioni per gruppo**, scegli Gruppo, Ruolo e date, quindi premi **Abilita gruppo**.

Gli estremi sono inclusivi; un campo data vuoto lascia quel lato senza limite. Una sola assegnazione diretta è configurata per la coppia utente–commessa e una per la coppia gruppo–commessa: la modifica aggiorna ruolo e periodo del collegamento.

La consultazione dell’operatore richiede un’abilitazione valida oggi. La registrazione richiede che **una stessa abilitazione** consenta di scrivere e comprenda sia oggi sia la data lavorata. Due abilitazioni separate, ciascuna valida soltanto in una delle due date, non si sommano per questo controllo.

Una persona può ricevere permessi attraverso più percorsi. Lettore non annulla Collaboratore; se almeno un percorso valido permette di registrare, l’azione rimane consentita. Con **Conferma revoca** rimuovi il collegamento selezionato. Per togliere completamente l’accesso verifica anche gli altri gruppi, i gruppi padre, le assegnazioni dirette e l’eventuale casella Commessa comune. Il ruolo Gestore conserva l’amministrazione delle commesse indipendentemente da queste abilitazioni; non estende l’ambito delle ore altrui o i profili economici.

Le modifiche si applicano anche alle sessioni aperte. Le registrazioni personali pregresse restano consultabili dopo una semplice revoca.

## Organizzare attività, sottoattività e blocchi

1. Apri **Attività e blocchi** e scegli la commessa, oppure usa **Attività** nella sua riga.
2. Premi **Nuova attività**, inserisci **Titolo** e, se utile, **Descrizione**.
3. Scegli **Responsabile**, **Priorità**, **Stato** e l’eventuale **Scadenza**. Il responsabile deve essere attivo e avere accesso Collaboratore alla commessa, oppure essere Gestore.
4. Per una sottoattività scegli **Attività principale**, oppure parti dal comando **+ Sottoattività** sulla scheda. Puoi raggiungere otto livelli complessivi; i riferimenti circolari sono rifiutati.
5. Premi **Crea attività**. Usa **Modifica** per rivedere i dettagli o il controllo **Stato → Applica** per spostare rapidamente la scheda.
6. Consulta **Bacheca Kanban** o **Elenco e sottoattività** e usa ricerca, stato e priorità per focalizzare il lavoro.

I cinque stati sono Da fare, In corso, Bloccato, Da verificare e Completato. Non esiste una sequenza obbligatoria; completare un genitore non completa i figli. Ogni attività mantiene responsabile e scadenza propri. Il modulo non contiene ancora stime di durata, dipendenze o diagrammi Gantt.

Per gli impedimenti usa **Segnala blocco**, compila impatto e prossima azione e scegli un responsabile o lascia **Da assegnare**. Il blocco può riferirsi all’intera commessa o a un’attività. Dal registro usa **Gestisci** per cambiarne lo stato in Aperto, In gestione o Risolto. Lo stato del blocco non sposta la scheda Kanban. Il filtro iniziale include solo aperti e in gestione.

L’operatore può segnalare blocchi e aggiornare quelli di cui è autore o responsabile, mantenendo l’accesso Collaboratore. Solo il Gestore cambia i responsabili, organizza le attività o elimina gli elementi. Se una persona perde l’accesso, il riferimento storico rimane: valuta la riassegnazione del lavoro a una persona abilitata.

L’eliminazione di un’attività richiede prima lo spostamento o l’eliminazione dei figli. Gli eventuali blocchi restano nella commessa senza il collegamento all’attività eliminata. La conferma è dedicata all’elemento e non modifica le ore registrate. Sono ammessi fino a 500 attività e 500 blocchi per commessa, contando anche quelli completati o risolti.

Le modifiche usano una revisione: un salvataggio basato su una versione precedente viene rifiutato. In caso di conflitto, annota le tue variazioni prima di chiudere la bozza, ricarica i dati e confrontali. Lo Storico registra attività, blocchi e lo scollegamento dei blocchi quando elimini un’attività.

Le ore rimangono associate alla commessa e possono collegare facoltativamente un’attività. Assegnazioni e scadenze alimentano avvisi operativi personali con controllo degli accessi; non generano automaticamente email. Dettagli e matrice dei permessi: [Attività, Kanban e blocchi](attivita-kanban-e-blocchi.md).

## Eliminare definitivamente una commessa

Apri **Modifica** sulla commessa e usa **Elimina commessa**. Leggi il riepilogo nella finestra dedicata; la conferma finale è **Elimina commessa e ore**.

L’operazione elimina la commessa, **attività, sottoattività, blocchi e tutte le ore collegate di tutti gli utenti**, oltre alle abilitazioni dirette e per gruppo. I dati scompaiono dai registri e dalle analisi. Utenti, gruppi, ruoli, contratti e storico delle modifiche rimangono; nello storico viene aggiunto il riepilogo dell’eliminazione.

La cancellazione non dispone di cestino né annullamento nell’app. Per conservare il lavoro storico, mantieni la commessa e gestisci stato e accessi. Per un recupero dopo l’eliminazione occorre valutare un backup con il referente tecnico.

## Gestire le proprie ore

Il gestore può registrare tempo sulle commesse senza un’assegnazione individuale. Nel proprio **Registro ore** dispone anche di **Modifica** e dell’eliminazione con conferma **Elimina registrazione**. Questi comandi riguardano le proprie righe.

Nel Registro ore il selettore della persona permette di consultare giorno e settimana dei membri della propria squadra in sola lettura. Modifica ed eliminazione diretta restano personali. La coda delle rettifiche mostra soltanto le richieste della squadra; un gestore diverso dall’autore può approvare o rifiutare una proposta motivata. L’approvazione ricontrolla revisione, accessi e limiti; una registrazione cambiata o eliminata produce un conflitto. Non usare l’eliminazione della commessa per correggere un singolo errore: cancellerebbe tutte le registrazioni collegate, le attività e i blocchi.

## Consultare le cinque viste di Analisi

Nelle viste del lavoro i filtri **Dal**, **Al**, **Commessa** e **Utente** si combinano entro la squadra visibile. Le viste economiche Finance usano date e commessa, senza filtro per persona. Le date sono inclusive; l’intervallo massimo è di 366 giorni. Usa **Applica**, **Azzera** e i periodi rapidi. La selezione compare nell’URL, ma il collegamento non estende i permessi di chi lo apre.

| Vista | Domanda a cui risponde | Lettura corretta |
| --- | --- | --- |
| **Per periodo** | Quanto lavoro è stato registrato ogni giorno? | Somma le registrazioni filtrate; consente di passare alle commesse di una giornata |
| **Per commessa** | Su quali progetti si concentra il tempo? | Ore, registrazioni e persone coinvolte, con stato corrente della commessa |
| **Per utente** | Come è distribuito il lavoro tra le persone? | Comprende anche account disattivati che hanno ore nel periodo |
| **Valore contratti** | Quali accordi sono collegati al lavoro registrato? | Conta una volta il valore intero di ogni contratto distinto; non lo ripartisce per ore |
| **Costo delle ore** | Quanto vale il lavoro secondo le tariffe inserite? | Stima aggregata per commessa, con copertura e ore senza tariffa |

Le prime tre viste mostrano al gestore gli aggregati della propria squadra e all’operatore soltanto i propri dati. Le note individuali non sono incluse nei report. Le ultime due richiedono Finance e restituiscono aggregati economici, senza righe personali dei colleghi; Commerciali da solo non concede accesso ai costi.

Nella vista Valore contratti, un contratto da 10.000 EUR collegato a due commesse viene contato per 10.000 EUR complessivi, non per 20.000. Anche se il periodo contiene una sola ora, il valore mostrato è l’intero valore dell’accordo. Cambiare il collegamento della commessa modifica il risultato delle analisi anche per le ore storiche.

## Inserire le tariffe orarie

Finance può leggere le tariffe. Per eseguire i passi di creazione o modifica servono insieme **Gestore + Finance**.

1. Apri **Analisi → Costo delle ore**.
2. Nella sezione **Tariffe orarie**, premi **Nuova tariffa**.
3. Scegli **Utente** e inserisci **Tariffa in EUR per ora**.
4. Imposta **Valida dal** e, se necessario, **Valida al**.
5. Premi **Crea tariffa**.

Sono ammessi importi da `0` a `999999,99` EUR, con al massimo due decimali. Lo zero è una tariffa valida; l’assenza di tariffa è invece indicata come dato mancante. Le date sono inclusive e gli intervalli dello stesso utente non possono sovrapporsi. Per passare da una tariffa a un’altra il 1° novembre, termina la precedente il 31 ottobre.

La tariffa si può modificare, ma non eliminare tramite l’interfaccia/API corrente né spostare a un altro utente. Si possono definire tariffe anche per account disattivati per valorizzarne lo storico. **Modificare una tariffa ricalcola le stime delle ore passate** che ricadono nel suo intervallo; non esistono ancora consuntivi economici congelati.

Ogni registrazione è valorizzata separatamente e arrotondata al centesimo prima della somma. Controlla la copertura: se mancano tariffe, il costo mostrato è parziale, non il costo complessivo del lavoro. La sezione non comprende ancora spese, acquisti, fatture, ricavi o margini.

## Configurare Alert e trigger

Le regole appartengono al gestore che le crea. Un altro gestore non vede né modifica automaticamente queste regole, anche se può essere tra i destinatari degli avvisi.

1. Apri **Alert e trigger**.
2. Premi **Nuova regola** oppure **Crea questo controllo** sotto il tipo desiderato.
3. Inserisci **Nome regola**.
4. Scegli **Tipo di controllo**: Commessa inesistente oppure Ore giornaliere mancanti.
5. Imposta **Esegui ogni giorno alle** nel formato ore/minuti. Il fuso è **Europe/Rome**.
6. Lascia **Regola attiva** selezionato per esecuzioni automatiche.
7. Scegli gli **Utenti destinatari nell’app** e, facoltativamente, i **Destinatari email**.
8. Premi **Crea regola**.

Occorre almeno un destinatario tra i due canali. Ogni regola ammette fino a 50 utenti attivi e 50 indirizzi email. Separa le email con virgole o righe; la UI elimina i duplicati. Sono consentite fino a 100 regole personali.

**Commessa inesistente** segnala tentativi rifiutati su commesse eliminate o inesistenti. Non segnala una registrazione invalida già accettata: il salvataggio è bloccato. Un normale errore di abilitazione su una commessa esistente non equivale a questo controllo.

**Ore giornaliere mancanti** confronta il totale con i minuti attesi per persona, trascorse 48 ore effettive dalla fine della giornata. Valgono prima l’eccezione personale, poi la festività, infine l’ultimo orario in vigore; senza configurazione usa 480 minuti dal lunedì al venerdì. Il monitoraggio inizia con la regola; gli utenti successivi vengono monitorati dal primo controllo che li include. Sono considerati gli utenti attivi nella squadra del proprietario della regola, non soltanto quelli selezionati come destinatari. Il destinatario riceve dettagli sulle ore soltanto se può ancora consultarli: l’operatore vede sé stesso, il gestore la propria squadra. Anche le email sulle ore richiedono un indirizzo associato a un account attivo autorizzato; gli indirizzi esterni non ricevono questi dettagli. I permessi vengono ricontrollati prima della consegna e quando si apre la lista; una revoca nasconde anche gli avvisi storici non più accessibili.

La regola automatica viene eseguita una sola volta per giornata locale; ultima e prossima esecuzione compaiono nella scheda. Se crei una regola quando l’orario odierno è già passato, la prima esecuzione programmata sarà il giorno successivo. Il servizio deve essere attivo: l’orario è una scadenza di elaborazione, non una garanzia di consegna al secondo.

Premi **Esegui ora** per controllare manualmente, anche su una regola sospesa. Il risultato riporta nuove notifiche ed eventuali email accodate. Le anomalie già elaborate non generano continuamente duplicati. Disattivare **Regola attiva** sospende le esecuzioni automatiche e conserva la configurazione; non è prevista una cancellazione delle regole nell’interfaccia attuale.

Un controllo manuale consuma anche il turno automatico della stessa giornata. Disattivare la regola non cancella le email già accodate: con SMTP attivo queste possono ancora essere inviate ai destinatari rimasti configurati. La UI non offre un comando per annullare l’intera coda.

Le 8 ore sono la soglia predefinita degli avvisi, non un limite obbligatorio della registrazione. In **Orari e assenze** configura orari settimanali con data di decorrenza, eccezioni personali e festività; un’assenza completa ha zero minuti attesi. Il cambio del tipo di controllo riavvia il relativo monitoraggio; cambiare nome, orario o destinatari non va interpretato come una riesecuzione completa dello storico.

Il canale email è implementato, ma **SMTP è ancora da configurare**. Gli avvisi destinati alle email restano in attesa e non sono spediti. Le notifiche in app sono disponibili senza SMTP. Un’eventuale accettazione futura da parte di SMTP attesterà l’accettazione del messaggio dal server, non la sua lettura o consegna finale.

Se il proprietario viene disattivato o perde il profilo Gestore, le sue regole non vengono più eseguite automaticamente. Prima di un cambio di responsabilità verifica quali controlli dovranno essere ricreati dal nuovo gestore.

## Consultare lo Storico

Apri **Storico** per vedere le modifiche in ordine dalla più recente. Usa il filtro **Mostra**, espandi i dettagli precedenti/successivi e premi **Carica precedenti** per continuare. **Aggiorna** ricarica gli eventi.

Lo storico comprende commesse, attività, blocchi, ore, abilitazioni, account, gruppi, ruoli, fornitori, contratti, caricamenti documentali, tariffe e regole. Le password non sono incluse. Il salvataggio della modifica e del relativo evento avviene insieme: se l’app non riesce a registrare lo storico, annulla anche la modifica.

Gli eventi relativi alle ore e alle rettifiche sono limitati alla squadra visibile. I dettagli commerciali e le tariffe rispettano inoltre i profili funzionali correnti; conoscere l’ID di un evento non supera questi controlli.

Lo Storico spiega cosa è cambiato, ma non è un comando di ripristino. Dopo una cancellazione le informazioni storiche non ricreano automaticamente i record rimossi.

## Calendari e prossimi moduli

Il servizio di sincronizzazione Google Calendar e le API per il calendario del singolo utente sono presenti. La configurazione OAuth sarà completata in seguito. Nell’interfaccia corrente non esiste ancora una sezione Calendario, un pulsante di collegamento dell’account o una conversione degli eventi in ore su commessa.

Fatture, incassi, pagamenti, multi-società e multi-valuta sono nel [piano finanziario](piano-modulo-finanziario.md). I miglioramenti del lavoro quotidiano sono nella [roadmap](roadmap.md). Per limiti e casi di confine consulta [Processi e regole](processi-e-regole.md).
