# Avvio operativo dello spazio di lavoro

**Italiano** | [English](../en/prod/avvio-operativo.md)

Questo percorso porta un’organizzazione dal primo accesso a una commessa utilizzabile, con ore, analisi e controlli. È una procedura da eseguire nell’installazione scelta, non il resoconto di nuove operazioni compiute durante la redazione. Nomi, codici e dati dell’esempio sono fittizi.

Per i singoli comandi consulta anche la [guida del gestore](guida-gestore.md); per il lavoro quotidiano, la [guida utente](guida-utente.md).

## 1. Prerequisiti e responsabilità

| Responsabile | Preparazione | Evidenza prima di iniziare |
| --- | --- | --- |
| Referente tecnico | Installazione, URL, backup, account iniziale e disponibilità dei servizi | Login raggiungibile e account Gestore funzionante; procedura di ripristino nota |
| Responsabile operativo | Convenzioni per codici, commesse comuni, attribuzione del lavoro e stati | Elenco delle persone, delle squadre e delle commesse da censire |
| Gestore dello spazio | Anagrafiche, accessi, tariffe e regole di controllo | Dati verificati da chi ne ha la responsabilità |
| Operatore | Registrazione della propria attività | Account attivo e commessa corretta selezionabile |
| Referente di controllo | Lettura degli aggregati e valutazione delle anomalie | Consapevolezza della differenza tra ore, costo stimato e valore contrattuale |

Nell’app attuale il Gestore ha poteri globali nello spazio. Il titolo organizzativo «responsabile operativo» non è un terzo ruolo applicativo. Per amministrare una sola area con poteri limitati serviranno le deleghe previste dalla [roadmap](roadmap.md).

Il percorso non richiede SMTP o Google Calendar: i controlli producono notifiche interne e le ore si registrano manualmente. La configurazione di questi provider sarà completata separatamente. Per una seconda società con isolamento giuridico/operativo occorre il futuro modulo societario, non un semplice nuovo gruppo.

## 2. Preparare un esempio coerente

L’esempio «Avvio portale Aurora» usa queste entità:

| Oggetto | Valore fittizio | Funzione |
| --- | --- | --- |
| Gestore | Marta, `marta.avvio@example.test` | Censimento e controllo |
| Operatore | Elena, `elena.avvio@example.test` | Realizzazione e registrazione ore |
| Operatore in sola lettura | Luca, `luca.avvio@example.test` | Consultazione della commessa |
| Gruppo padre | Area tecnica | Ambito organizzativo |
| Sottogruppo | Sviluppo | Squadra di Elena |
| Fornitore | `AVV-F01`, Servizi Aurora | Controparte del contratto |
| Contratto | `AVV-C01`, Accordo servizi portale | Riferimento contrattuale della commessa |
| Commessa | `AVV-J01`, Avvio portale Aurora | Destinazione delle ore di progetto |
| Commessa comune | `AVV-GEN`, Generale | Attività generali dell’esempio |

Gli indirizzi `.test` servono soltanto per il laboratorio e non devono essere usati come recapiti reali. Il contratto con un fornitore è un riferimento dell’attuale anagrafica: il suo valore non genera ricavi o fatture.

Se ripeti il percorso in uno spazio già utilizzato, adotta codici distinti e verifica prima eventuali record esistenti. Non duplicare persone reali per aggirare un errore di accesso.

## 3. Censire persone e organizzazione

**Marta, Gestore:** apri **Utenti → Nuovo utente** e crea Elena e Luca con profilo **Operatore**. Imposta una password iniziale secondo le procedure dell’organizzazione; questa guida non prescrive credenziali comuni. Un account Operatore appena creato non riceve automaticamente tutte le commesse riservate.

Apri **Gruppi e ruoli → + Gruppo** e crea Area tecnica come Primo livello, poi Sviluppo con Area tecnica come padre. Seleziona Sviluppo e aggiungi Elena con **Aggiungi al gruppo**. Luca può rimanere senza gruppo in questo esempio, poiché riceverà un’abilitazione diretta.

Non creare un ruolo chiamato «Gestore» nella lista dei ruoli di commessa per attribuire poteri amministrativi. Per l’esempio bastano i due ruoli predefiniti: Collaboratore per Elena e Lettore per Luca.

**Controllo prima di proseguire:** Elena appare tra i membri diretti di Sviluppo; Sviluppo è figlio di Area tecnica; entrambi gli account sono attivi. Gli accessi effettivi cambiano se il gruppo viene spostato o se la persona cambia appartenenza.

## 4. Censire fornitore, contratto e originale PDF

**Marta:** in **Fornitori → Nuovo fornitore**, inserisci codice AVV-F01 e nome Servizi Aurora, lasciando lo stato Attivo. Aggiungi contatti e dati descrittivi soltanto se disponibili.

In **Contratti → Nuovo contratto**, crea AVV-C01 con titolo Accordo servizi portale e fornitore AVV-F01. Imposta lo stato Attivo e, nell’esempio, valore `10000,00` EUR. Le date sono facoltative; se compilate, la data finale non può precedere quella iniziale.

Apri **PDF · numero**, scegli un PDF sintetico di laboratorio sotto 10 MiB e premi **Carica PDF**. Verifica che compaia nell’elenco e scaricalo per controllare il documento atteso. In uso operativo il file dovrà essere l’originale autorizzato dalla tua organizzazione.

Ogni caricamento è aggiuntivo. Non caricare ripetutamente lo stesso file per «aggiornarlo»: l’app non sostituisce il precedente e non offre oggi eliminazione o versionamento formale del documento. La data finale del contratto non lo porta automaticamente a Scaduto.

## 5. Creare la commessa e le attività comuni

**Marta:** in **Commesse → Nuova commessa**, inserisci AVV-J01, Avvio portale Aurora e stato In corso. Scegli AVV-C01 nel campo Contratto. Lascia **Commessa comune** disattivata: questa è una commessa con accesso assegnato.

Crea anche AVV-GEN, Generale, con **Commessa comune** attiva e senza contratto. Per lo spazio reale, verifica o predisponi il catalogo concordato di Generale, Formazione, Riunioni e Attività interne. I nomi non sono riservati e non vengono generati obbligatoriamente dal software.

Definisci una convenzione prima di iniziare: nell’esempio le attività direttamente riferite al portale vanno su AVV-J01, mentre l’attività generale non attribuibile al progetto va su AVV-GEN. Questa classificazione è una scelta organizzativa, non un suggerimento automatico del sistema.

**Controllo:** entrambe le commesse esistono, ma soltanto Generale è comune. Il contratto è associato ad AVV-J01 e il codice evita confusione con progetti dai nomi simili.

## 6. Concedere e verificare gli accessi

**Marta:** nella riga AVV-J01 premi **Abilitazioni**.

1. In **Abilitazioni per gruppo**, assegna Area tecnica con ruolo Collaboratore e senza limiti di data per questo laboratorio.
2. In **Abilita un utente**, assegna Luca con ruolo Lettore, sempre senza limiti di data.

Elena riceve la capacità di registrare attraverso il percorso Area tecnica → Sviluppo. Luca può consultare la commessa ma non registrarvi ore. Entrambi possono usare AVV-GEN grazie alla proprietà comune.

**Elena:** accedi con il tuo account e controlla la presenza di AVV-J01 in Commesse e nel selettore **Registra ore**.

**Luca:** accedi con un’altra sessione e controlla che AVV-J01 sia consultabile ma non disponibile per una nuova registrazione. Il suo account rimane Operatore: Lettore è il ruolo sulla sola commessa.

Usa profili browser separati per evitare di scambiare le identità durante la verifica. Il gestore vede tutte le commesse e quindi il suo elenco non dimostra, da solo, che gli accessi dell’operatore siano corretti. I [casi di accesso](scenari-accessi.md) approfondiscono date, ruoli alternativi e revoche.

## 7. Registrare una giornata di esempio

**Elena:** scegli una giornata di laboratorio `D` e registra due righe:

| Commessa | Durata | Nota fittizia |
| --- | --- | --- |
| AVV-J01 | `6:30` | Preparazione del portale |
| AVV-GEN | `1:30` | Attività organizzative |
| **Totale** | **8:00** | **480 minuti, 2 registrazioni** |

Per ciascuna riga usa **Registra ore**, seleziona la commessa, controlla Giornata lavorativa e premi **Salva ore**. Nel Registro ore di D verifica due righe e 8 ore complessive.

Le ore appartengono a Elena e devono essere inserite dalla sua sessione. Marta non può attribuire dal proprio modulo ore a Elena. Se stai soltanto esplorando l’app, esegui questo esempio in un ambiente di prova e non nel consuntivo reale di una persona.

La soglia degli alert è di 8 ore, ma il salvataggio non impone giornate di 8 ore: il vincolo tecnico è tra 1 minuto per riga e 24 ore complessive per giornata. L’attività registrata deve sempre corrispondere al lavoro svolto nell’uso reale.

## 8. Leggere risultati e costo stimato

**Marta:** apri **Analisi**, imposta Dal = D, Al = D e Utente = Elena, poi premi Applica. Le viste del lavoro devono mostrare 480 minuti, 2 registrazioni, 2 commesse e 1 utente.

In **Valore contratti**, il contratto AVV-C01 compare con il suo valore intero di 10.000 EUR e 6 ore 30 minuti collegate. Le restanti 1 ora 30 minuti sono segnalate come lavoro senza contratto. Il valore non è riproporzionato alle 6 ore 30 minuti.

In **Costo delle ore → Tariffe orarie → Nuova tariffa**, inserisci per Elena una tariffa di 40 EUR/ora valida in D, senza sovrapporla ad altre tariffe della stessa persona. Il costo stimato del giorno sarà:

| Commessa | Calcolo | Stima |
| --- | --- | --- |
| AVV-J01 | 6,5 ore × 40 EUR | 260,00 EUR |
| AVV-GEN | 1,5 ore × 40 EUR | 60,00 EUR |
| **Totale** | **8 ore × 40 EUR** | **320,00 EUR** |

Una tariffa mancante lascia invece le ore non valorizzate. Non usare zero per indicare che l’importo è sconosciuto. Il report è dinamico: una successiva modifica della tariffa applicabile in D ricalcolerà questa stima. Non confrontare automaticamente 10.000 EUR e 320 EUR come margine della giornata: il primo è il valore intero del contratto, il secondo il solo costo stimato delle ore selezionate.

Per un esempio più ampio, con importi mancanti, zero e più persone, vedi [Esempi di analisi e costi](esempi-analisi-e-costi.md).

## 9. Attivare i controlli giornalieri

**Marta:** apri **Alert e trigger → Nuova regola** e configura un controllo Ore giornaliere mancanti alle 09:00 Europe/Rome. Seleziona Marta ed Elena tra gli utenti destinatari e lascia vuoto il canale email finché non è necessario e configurato. Puoi preparare una seconda regola per Commessa inesistente.

Il controllo delle ore monitora gli utenti attivi, non soltanto i destinatari scelti. Marta può quindi ricevere avvisi su tutti i soggetti monitorati; Elena soltanto quelli che la riguardano. Nell’esempio Luca e Marta, se attivi e senza 8 ore, possono essere rilevati come giornate incomplete: il ruolo Lettore sulla commessa non esonera Luca dal controllo.

La regola non valuta immediatamente le vecchie giornate precedenti alla propria creazione. Per un controllo rappresentativo, predisponila prima della giornata da osservare. Una giornata feriale diventa verificabile dopo 48 ore effettive dalla sua fine; **Esegui ora** non anticipa tale scadenza. Le regole non conoscono ancora ferie, festività, part time o esenzioni personali.

Dopo un’esecuzione verifica la scheda della regola e **Notifiche**. Una regola senza anomalie può correttamente creare zero avvisi. Un **Esegui ora** riuscito registra l’esecuzione odierna e impedisce un ulteriore turno automatico della stessa giornata; non usarlo aspettandoti due controlli distinti nello stesso giorno. Premere **Segna come letta** non modifica il registro ore.

## 10. Passaggio all’uso ordinario

Prima di rendere lo spazio operativo, verifica con ciascuna responsabilità questi risultati:

| Verifica | Chi la conferma |
| --- | --- |
| Codici e nomi rendono riconoscibili le commesse | Responsabile operativo |
| Le persone vedono solo le commesse previste e i ruoli sono corretti | Gestore con prova da Operatore/Lettore |
| Commesse comuni e convenzioni di rendicontazione sono note | Gestore e operatori |
| Fornitori, contratti e PDF sono associati alle risorse corrette | Responsabile dell’archivio |
| Valori mancanti, zero e tariffe sono interpretati correttamente | Referente di controllo |
| Le regole hanno proprietario, orario e destinatari appropriati | Gestore proprietario delle regole |
| Backup e recupero sono documentati | Referente tecnico |

Non cancellare una commessa di laboratorio se vi sono state accidentalmente registrate ore reali: l’eliminazione rimuove tutte le ore collegate di tutti gli utenti. Le prove distruttive vanno eseguite in uno spazio isolato secondo il [collaudo di accettazione](collaudo-accettazione.md).
