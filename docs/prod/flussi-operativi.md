# Flussi operativi migliorati

**Italiano** | [English](../en/prod/flussi-operativi.md)

Codice del workspace all'11 ottobre 2026, schema **13**. Questa guida descrive le dieci migliorie operative introdotte nell'interfaccia. La compilazione o il collaudo locale non aggiornano automaticamente il deployment Docker esistente.

## Profili e ambito delle ore

Ogni account attivo è Operatore; Gestore deriva dal ruolo dell’identità. In **Utenti → Profili** il gestore assegna separatamente Commerciali e Finance, anche agli account Keycloak. Commerciali modifica fornitori, contratti e PDF; Finance li consulta e legge valori, tariffe e costi aggregati per commessa, senza ore personali di terzi o filtro Utente. Le tariffe si modificano con Gestore + Finance. Il solo ruolo Gestore non concede questi accessi.

Per le ore il gestore vede sé stesso e i membri dei propri gruppi organizzativi e sottogruppi. Senza gruppi, o se i gruppi aziendali non sono disponibili, vede soltanto sé stesso. Il selettore persona del Registro ore consente una consultazione di squadra in sola lettura, giorno e settimana. Report del lavoro, riepiloghi, rettifiche, Storico e alert applicano la stessa squadra; revoche e profili vengono verificati a ogni richiesta.

## La mia giornata

La vista **La mia giornata** raccoglie attività assegnate, impedimenti da seguire, ore di oggi, dotazioni e avvisi personali. Aprire una voce porta alla commessa e alla bacheca corrispondenti. Gli accessi vengono ricontrollati dal server: un'assegnazione rimasta sul proprio nome non permette di leggere una commessa revocata. Il collegamento **I miei ticket** apre la coda personale dell'app di assistenza. Ticket mantiene account e sessione separati; per le risorse Punto si usa **Collega Punto**.

Le liste personali restituiscono fino a 200 attività e 200 blocchi, con conteggio e indicazione degli eventuali altri risultati. L'archivio rimane consultabile tramite le commesse, mentre non affolla la coda del lavoro corrente.

## Registro settimanale e copia

In **Registro ore** si può scegliere **Giorno** oppure **Settimana**. La settimana va da lunedì a domenica, anche quando attraversa mesi o anni. Le celle mostrano i totali per commessa; il dettaglio conserva le singole registrazioni e le loro note.

L'inserimento multiplo e la copia della settimana precedente preparano un'anteprima modificabile. Controllare destinazioni, date, durate e note, rimuovere le righe non desiderate e confermare. La copia conserva anche l'attività collegata, che si può rimuovere dall'anteprima; cambiare commessa rimuove il collegamento precedente. La copia aggiunge registrazioni, senza sostituire quelle esistenti. Non salva automaticamente il lavoro.

Ogni conferma contiene al massimo 100 righe. Il server salva tutto oppure nulla, verifica il totale finale di ogni giornata, il limite di 1000 registrazioni e l'abilitazione valida oggi e nel giorno lavorato. Una commessa chiusa o archiviata rifiuta nuove ore. Ripetere la stessa richiesta dopo un'interruzione di rete restituisce l'esito precedente senza duplicare le registrazioni; cambiare il contenuto richiede una nuova operazione.

## Rettificare le ore

Dalla propria registrazione si apre **Richiedi rettifica**. Si possono proporre commessa, giornata, durata e nota differenti, oppure chiedere la cancellazione. La motivazione è obbligatoria. La registrazione originale rimane invariata finché la richiesta non viene approvata.

La coda mostra all'operatore le proprie richieste e al gestore quelle della propria squadra. Un gestore può approvare o respingere una richiesta altrui della squadra; non può decidere sulla propria. L'autore può annullare una richiesta pendente. Il rifiuto richiede una motivazione.

Esiste una sola richiesta pendente per registrazione. Prima dell'approvazione il server ricontrolla revisione, contenuto originale, permessi e limiti giornalieri. Se la registrazione è stata nel frattempo modificata o eliminata, la richiesta passa **In conflitto** e conserva originale e proposta. Una decisione ripetuta non modifica nuovamente le ore.

Dopo una revoca o la chiusura della commessa rimane possibile chiedere una riduzione o cancellazione delle ore originali sulla stessa commessa e giornata. Un aumento o uno spostamento richiede invece un accesso attuale valido sulla destinazione. Un cambio di commessa rimuove l'eventuale collegamento all'attività precedente. Approvazione, modifica delle ore e storico sono salvati nella stessa transazione.

## Ore sulle attività

Il modulo ore offre un'attività facoltativa appartenente alla commessa selezionata. Dalla bacheca si può anche aprire direttamente la registrazione con quel riferimento. Il server rifiuta attività di altre commesse.

Il riepilogo commessa mostra il tempo effettivo per attività, personale per l'operatore e relativo alla propria squadra per il gestore. La nota rimane testo descrittivo. Eliminare un'attività conserva le ore sulla commessa e rimuove soltanto il riferimento alla scheda eliminata.

## Orari, assenze e festività

Il gestore configura i minuti attesi per ciascun giorno della settimana e una data di inizio. Un nuovo orario conserva i precedenti periodi. Le eccezioni personali possono indicare ferie, permessi o turni particolari; le festività aziendali valgono per tutti.

Per una giornata si applica, nell'ordine: **eccezione personale → festività aziendale → ultimo orario valido → valore predefinito**. Il valore predefinito resta otto ore dal lunedì al venerdì, zero nel fine settimana. Un'eccezione con minuti positivi può rappresentare un turno durante una festività.

Il controllo delle ore mancanti conserva la tolleranza di 48 ore dalla fine della giornata nel fuso Europe/Rome. Controlla anche sabato e domenica quando l'orario prevede lavoro. Non interpreta automaticamente assenze o contratti di lavoro: gli orari devono essere configurati dal gestore. Modificare una configurazione non elimina le notifiche storiche già emesse e non ripete automaticamente i giorni già verificati.

## Capire gli accessi

Dall'elenco commesse il gestore apre **Spiega accesso**, sceglie una persona e legge perché ha accesso. La spiegazione include assegnazione diretta, gruppi diretti o ereditati, proprietà comune e profilo Gestore, con ruoli e date.

Le fonti si sommano: revocare una concessione non annulla quelle ancora valide. Una fonte scaduta o un gruppo aziendale temporaneamente indisponibile viene indicato come inattivo. La facoltà effettiva di inserire ore considera anche chiusura e archivio.

## Riepilogo commessa

Il riepilogo riunisce stato, attività, blocchi e tempo registrato, con percorsi contestuali verso bacheca, registrazione ore e assistenza. I gestori consultano anche le persone abilitate. Il contratto e i suoi documenti si aprono soltanto con Commerciali o Finance. Gli operatori conservano il proprio ambito personale per le ore; il riepilogo non concede accesso all'archivio PDF contrattuale.

## Notifiche operative

Le assegnazioni delle attività producono un avviso personale. Il feed operativo segnala anche attività e blocchi in scadenza nei tre giorni successivi o già scaduti. Gli avvisi vengono deduplicati; la lettura non modifica lo stato dell'attività. Risorse completate, riassegnate o revocate non rimangono esposte come lavoro attuale della persona.

Dagli avvisi inventario si può aprire il bene e, quando Ticket è configurato, avviare una richiesta collegata o consultare quelle esistenti. La creazione del ticket resta una scelta esplicita. Il feed interno non dipende dall'attivazione SMTP.

## Creazione guidata e modelli

Il gestore prepara i dati e, con anche Commerciali, il contratto facoltativo, sceglie squadra e ruoli, aggiunge le attività iniziali e controlla il riepilogo prima della conferma. Il salvataggio di commessa, accessi e attività è atomico. Un riferimento non valido annulla tutta la creazione. Il responsabile di un'attività deve possedere un accesso operativo valido; l'assegnazione da sola non lo concede.

I modelli conservano soltanto configurazioni esplicite di commessa, squadra e attività. Non copiano ore o documenti, non riutilizzano il codice della commessa e non applicano autorizzazioni senza il riepilogo da confermare. Utenti, gruppi, ruoli e contratto vengono ricontrollati al salvataggio. Sono ammessi fino a 100 attività iniziali, 200 assegnazioni dirette, 200 assegnazioni di gruppo e 200 modelli.

## Chiusura, archivio e riapertura

Il gestore apre **Chiusura / archivio**, controlla attività e blocchi ancora aperti e il riepilogo dei dati conservati. **Chiudi registrazione ore** impedisce nuovi inserimenti; **Archivia** chiude le ore e sposta la commessa fuori dall'elenco corrente. Entrambe conservano ore, attività, blocchi e consultazione autorizzata. **Mostra archiviate** permette di ritrovare le commesse archiviate.

La riapertura è esplicita e motivata. Il motivo e la revisione proteggono il salvataggio e ne registrano l'autore nello Storico. Lo stato descrittivo Completata o Annullata rimane distinto dalla chiusura delle registrazioni.

La cancellazione definitiva preesistente rimane un comando separato e distruttivo: non equivale all'archivio e continua a rimuovere i dati collegati. Usare la chiusura e l'archivio per conservare lo storico del lavoro.
