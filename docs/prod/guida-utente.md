# Guida utente

**Italiano** | [English](../en/prod/guida-utente.md)

Questa guida descrive il percorso dell’**Operatore** nell’interfaccia attuale. Le azioni amministrative sono nella [guida del gestore](guida-gestore.md). I nomi in grassetto corrispondono alle etichette dei comandi.

## Accedere e uscire

1. Apri l’indirizzo dell’app comunicato dal referente della tua organizzazione.
2. Inserisci **Email** e **Password** dell’account assegnato.
3. Se necessario, usa **Mostra password** per controllare ciò che hai digitato e **Nascondi password** per nasconderlo di nuovo.
4. Seleziona **Ricordami per 30 giorni** solo se vuoi mantenere la sessione più a lungo su quel dispositivo.
5. Premi **Accedi al tuo spazio**.

La pagina segnala campi mancanti, indirizzo email non valido e credenziali errate. L’opzione Ricordami conserva anche l’email nel browser; non vi salva la password. La durata ordinaria della sessione è di 8 ore. Il riavvio del servizio o la disattivazione dell’account possono richiedere un nuovo accesso anche prima della scadenza.

In alto sono visibili il tuo nome, email e profilo. Usa **Esci** al termine della sessione. L’interfaccia non comprende registrazione autonoma, recupero password o modifica della password: per problemi di accesso rivolgiti al referente dell’installazione. La sezione dimostrativa del login è un aiuto per l’ambiente demo e non sostituisce le credenziali assegnate dalla tua organizzazione.

## Orientarsi nello spazio di lavoro

| Sezione | A cosa serve |
| --- | --- |
| **Commesse** | Consultare le commesse a cui hai accesso e il loro stato |
| **Attività e blocchi** | Consultare la bacheca della commessa, aggiornare le attività assegnate e segnalare impedimenti quando abilitato |
| **Registro ore** | Inserire e consultare il tuo tempo per giornata |
| **Analisi** | Riepilogare le tue ore su un intervallo, per commessa o per utente |
| **Notifiche** | Leggere gli avvisi indirizzati al tuo account |

Le azioni disponibili dipendono dal tuo profilo e dalle abilitazioni correnti. La presenza di ore storiche su una commessa non ti concede automaticamente il diritto di registrarne altre.

Su schermi piccoli le tabelle possono scorrere orizzontalmente nel loro riquadro. Le finestre possono essere chiuse con **Annulla**, con la croce o con Escape quando non è in corso un salvataggio. La chiusura del modulo non salva il lavoro: verifica sempre il messaggio di conferma dopo aver premuto il pulsante di salvataggio.

## Trovare una commessa

1. Apri **Commesse**.
2. Usa **Cerca per nome, codice o cliente…** per restringere l’elenco.
3. Se utile, scegli uno **Stato**: Da avviare, In corso, In pausa, Completata o Annullata.
4. Controlla codice e nome prima di registrare il lavoro, soprattutto quando esistono progetti con nomi simili.
5. Usa **Azzera i filtri** quando la ricerca non restituisce ciò che ti aspetti.

L’elenco mostra soltanto le commesse attualmente autorizzate e quelle comuni a tutti. Una commessa assegnata con ruolo **Lettore** può essere consultata ma non usata per nuove registrazioni. Il ruolo **Collaboratore**, o un ruolo equivalente configurato dal gestore, permette anche di registrare ore.

Le ore visualizzate nell’elenco delle commesse sono **le tue ore complessive**, su tutte le date. Per il totale di un periodo usa Analisi. Lo stato della commessa non determina da solo la possibilità di registrare ore: se la procedura aziendale richiede un blocco, il gestore deve intervenire sulle abilitazioni.

## Seguire attività e impedimenti

1. Apri **Attività e blocchi** e seleziona la **Commessa**, oppure premi **Attività** nella sua riga in Commesse.
2. Consulta le colonne **Da fare**, **In corso**, **Bloccato**, **Da verificare** e **Completato**. Ricerca, stato e priorità restringono le attività mostrate.
3. Scegli **Elenco e sottoattività** nel campo **Vista** per leggere l’organizzazione gerarchica. Nella bacheca le colonne possono scorrere orizzontalmente.
4. Per un’attività assegnata a te, se hai un ruolo che permette di collaborare, scegli il nuovo **Stato** e premi **Applica**. Il titolo, il responsabile, la scadenza e la struttura vengono definiti dal gestore.
5. Usa **Segnala blocco** per descrivere un impedimento della commessa o di una specifica attività. Indica titolo, descrizione, impatto, prossima azione e, se serve, scadenza. Il nuovo blocco viene assegnato a te.
6. Nel **Registro dei blocchi**, premi **Gestisci** per aggiornare i blocchi di cui sei autore o responsabile. Passa a **Risolto** quando l’impedimento è superato. Il filtro iniziale nasconde i blocchi risolti; scegli **Tutti** per ritrovarli.

Il ruolo Lettore consente la sola consultazione. Perdere l’abilitazione impedisce di aggiornare un’attività anche se risulti ancora responsabile. Il gestore può riassegnarla. Le descrizioni di attività e blocchi sono condivise con chi può leggere quella commessa.

Gli stati dell’attività, dei suoi figli e dei blocchi non si aggiornano a vicenda: risolvere un blocco non sposta automaticamente l’attività. Le ore si registrano ancora sulla commessa; il campo **Attività svolta / nota** è testo libero e non collega la registrazione a una scheda della bacheca.

Se un’altra persona ha già salvato una modifica, l’app segnala il conflitto. Non ripetere il salvataggio della vecchia versione: usa **Chiudi e ricarica la versione aggiornata**, controlla il contenuto corrente e riporta le variazioni ancora necessarie. La bozza chiusa non viene conservata. **Aggiorna** rilegge attività e blocchi della commessa.

La [guida del modulo](attivita-kanban-e-blocchi.md) descrive stati, limiti e cancellazioni.

## Registrare il lavoro della giornata

1. Premi **Registra ore** nella parte alta della pagina oppure apri **Registro ore** e premi **Aggiungi ore**.
2. Nel campo **Commessa**, scegli una voce dell’elenco. Non è previsto l’inserimento manuale di un nuovo codice.
3. Imposta **Giornata lavorativa**. Controlla la data anche quando stai recuperando registrazioni arretrate.
4. Inserisci **Ore lavorate** nel formato che preferisci.
5. Compila, se utile, **Attività svolta / nota** per descrivere il lavoro.
6. Premi **Salva ore** e attendi la conferma.
7. Nel Registro ore della giornata verifica la nuova riga e il **Totale della giornata**.

Esempi di durate:

| Testo inserito | Tempo registrato |
| --- | --- |
| `1,5` oppure `1.5` | 1 ora e 30 minuti |
| `1:30` | 1 ora e 30 minuti |
| `0:15` oppure `0,25` | 15 minuti |
| `8` | 8 ore |

Il formato con i due punti indica ore e minuti: `1:30` non equivale a `1,30`, che significa 1,3 ore e quindi 1 ora e 18 minuti. Dopo i due punti i minuti devono essere compresi tra 00 e 59. Le durate decimali devono corrispondere a minuti interi; per durate brevi il formato `0:MM` evita ambiguità.

Ogni riga deve durare almeno un minuto. Il totale di tutte le commesse della stessa giornata non può superare 24 ore. Sono ammesse più registrazioni sulla stessa commessa, con note diverse, e al massimo 1000 righe per utente nella stessa giornata. La nota può contenere fino a 1000 caratteri.

Le ore vengono sempre attribuite al tuo account. Puoi inserire giornate passate o future quando l’abilitazione lo permette. Una stessa abilitazione deve autorizzare sia la data di oggi sia la giornata che stai registrando: una commessa visibile oggi potrebbe non essere utilizzabile per una data precedente al tuo ingresso nel progetto.

## Usare le commesse comuni

Se il gestore le ha configurate, **Generale**, **Formazione**, **Riunioni** e **Attività interne** compaiono tra le commesse disponibili. Usa la voce coerente con le regole della tua organizzazione. Per esempio, una riunione dedicata a uno specifico progetto può essere rendicontata direttamente sulla commessa del progetto, se questa è la convenzione adottata.

Le commesse comuni non richiedono un’abilitazione personale e rimangono disponibili anche agli utenti aggiunti successivamente. Il gestore può modificarne nomi, stato e visibilità come per le altre commesse.

## Consultare le ore già salvate

1. Apri **Registro ore**.
2. Imposta **Giornata lavorativa** oppure usa i comandi del giorno precedente e successivo.
3. Premi **Oggi** per tornare alla giornata corrente del browser.
4. Leggi le registrazioni e il totale della giornata.

Il registro rimane personale. Se perdi l’abilitazione a una commessa, le tue registrazioni già salvate restano consultabili finché la commessa esiste. L’eliminazione definitiva della commessa, eseguita dal gestore, rimuove invece anche tutte le ore collegate, oltre alle attività e ai blocchi.

Come Operatore non puoi modificare o eliminare una registrazione salvata. In caso di errore contatta il gestore indicando data, commessa, durata e correzione richiesta. Non inserire una seconda riga per simulare una correzione: aumenterebbe il totale. La gestione delle richieste di rettifica e dell’approvazione è ancora pianificata; il gestore non dispone oggi di un editor generico delle ore altrui.

## Analizzare il proprio lavoro

1. Apri **Analisi**.
2. Scegli **Per periodo**, **Per commessa** oppure **Per utente**.
3. Imposta **Dal** e **Al**. Per una singola giornata usa la stessa data in entrambi i campi.
4. Se vuoi, seleziona una **Commessa**.
5. Premi **Applica**. I pulsanti **Oggi**, **Questa settimana** e **Questo mese** preparano intervalli rapidi usando la data restituita dal server.
6. Leggi il riepilogo e le righe della tabella.

L’intervallo comprende entrambi gli estremi e non può superare 366 giorni. **Per periodo** raggruppa per giornata; **Per commessa** raggruppa il lavoro per progetto. **Per utente** rimane limitata al tuo account quando sei Operatore. Non sono disponibili il filtro sugli altri utenti né le due viste economiche riservate ai gestori.

Puoi passare dalla riga di una giornata alle relative commesse e dalla riga di una commessa al suo andamento per periodo. La vista e i filtri sono riportati nell’indirizzo della pagina: puoi conservare il collegamento per riaprire la stessa selezione. Chi lo apre vede soltanto i dati consentiti al proprio account.

Le analisi includono il tuo storico su commesse successivamente revocate. Non contengono le note delle singole registrazioni né aggregati delle schede Kanban. Un elenco vuoto significa che nessuna registrazione corrisponde ai filtri, non che una commessa o un account siano stati cancellati.

## Leggere le notifiche

1. Apri **Notifiche** quando compare un contatore nella navigazione o quando vuoi controllare gli avvisi.
2. Leggi titolo, messaggio, data dell’avviso ed eventuale **Giornata** interessata.
3. Premi **Segna come letta** dopo aver preso visione.
4. Usa **Aggiorna** per ricaricare subito. L’interfaccia effettua anche un aggiornamento automatico ogni minuto.

Sono mostrate le ultime 100 notifiche accessibili. Il contatore indica quelle non lette e può comprendere avvisi precedenti rispetto all’elenco visualizzato. Le notifiche sono personali: l’operatore riceve soltanto anomalie che riguardano il proprio lavoro.

I controlli disponibili sono:

- **Commessa inesistente**: un tentativo di salvare ore su una commessa inesistente o eliminata è stato respinto; quelle ore non sono state salvate.
- **Ore giornaliere mancanti**: una giornata dal lunedì al venerdì ha meno di 8 ore dopo la tolleranza prevista. La regola non tiene ancora conto di festività, ferie o part time.

La lettura non corregge automaticamente la registrazione e non elimina la notifica. Il messaggio conserva il contesto rilevato quando il controllo è stato eseguito. Se hai poi integrato le ore, confronta la notifica con il registro aggiornato.

## Affrontare un problema

| Situazione | Cosa fare |
| --- | --- |
| Commessa assente dall’elenco | Azzera i filtri; se continua a mancare, chiedi al gestore di verificare abilitazione e relative date |
| Commessa visibile ma non selezionabile per le ore | Verifica con il gestore il ruolo operativo: potrebbe essere di sola consultazione |
| Abilitazione revocata mentre registravi ore | La richiesta viene rifiutata; scegli una commessa ancora autorizzata dopo aver verificato la destinazione corretta. La UI conserva data, durata e nota quando aggiorna le scelte |
| Accesso negato o elemento non disponibile in Attività e blocchi | L’app svuota la bacheca e chiude le bozze; seleziona una commessa ancora abilitata. Le modifiche non salvate non vengono conservate |
| Non posso cambiare lo stato di un’attività | Verifica di esserne responsabile e di avere ancora un accesso Collaboratore alla commessa; un Lettore può soltanto consultare |
| Un blocco risolto non compare | Nel registro dei blocchi scegli **Tutti** o **Risolti** |
| Conflitto durante una modifica di attività o blocchi | Rileggi la versione aggiornata e confrontala con le tue variazioni; la versione precedente non viene sovrascritta |
| Durata rifiutata | Usa minuti interi, controlla il formato e il totale già presente nella giornata |
| Sessione terminata | Effettua nuovamente il login; una bozza non confermata non è una registrazione salvata |
| Errore o attesa durante un salvataggio | Prima di ripetere l’invio, controlla nel registro se la riga è già comparsa; la registrazione ore non dispone ancora della deduplicazione generale prevista dalla roadmap |
| Avviso di 8 ore durante ferie o festività | Il controllo attuale usa una soglia fissa: segnala al gestore il contesto, senza inserire lavoro non svolto |
| Manca una schermata Calendario | La sincronizzazione è predisposta tramite API, ma la schermata e il collegamento evento–commessa sono ancora da implementare |

Le regole complete sono in [Processi e regole](processi-e-regole.md). La [roadmap](roadmap.md) elenca le evoluzioni previste per griglia settimanale, copie, rettifiche e calendario.
