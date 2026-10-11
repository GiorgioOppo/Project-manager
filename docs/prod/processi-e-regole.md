# Processi e regole del prodotto

Questo documento raccoglie il comportamento corrente che incide sull’organizzazione del lavoro e sull’interpretazione dei dati. Le procedure con i comandi dell’interfaccia sono nelle guide [utente](guida-utente.md) e [gestore](guida-gestore.md).

## 1. Due livelli di autorizzazione

Il profilo dell’account stabilisce i poteri nello spazio; il ruolo sulla commessa regola la consultazione e la registrazione dell’operatore su quella risorsa.

| Azione | Gestore | Operatore |
| --- | --- | --- |
| Consultare commesse | Tutte | Comuni o con abilitazione valida oggi |
| Creare, modificare ed eliminare commesse | Sì | No |
| Leggere attività e blocchi | Tutte le commesse | Comuni o con abilitazione valida oggi, anche Lettore |
| Creare, organizzare ed eliminare attività | Sì | No |
| Cambiare stato di un’attività | Tutte | Solo quelle assegnate a sé, con accesso Collaboratore valido oggi |
| Segnalare un blocco | Sì | Con accesso Collaboratore valido oggi |
| Modificare un blocco | Tutti, compreso il responsabile | Solo se autore o responsabile e ancora Collaboratore; non cambia il responsabile |
| Eliminare un blocco | Sì | No |
| Registrare le proprie ore | Su tutte le commesse esistenti | Su commesse comuni o con abilitazione alla registrazione valida oggi e nella giornata lavorata |
| Leggere il proprio registro | Sì | Sì, anche dopo revoca dell’abilitazione |
| Modificare o eliminare le proprie ore | Sì | No |
| Modificare le singole ore altrui | No nel flusso corrente | No |
| Consultare Analisi del lavoro | Aggregati di tutti gli utenti | Solo le proprie registrazioni |
| Consultare valori contrattuali e tariffe | Sì | No |
| Amministrare fornitori, contratti e PDF | Sì | No |
| Amministrare utenti, gruppi, ruoli e abilitazioni | Sì | No |
| Creare e modificare regole di alert | Solo le proprie regole | No |
| Leggere notifiche | Destinate al proprio account, anche relative ad altri utenti | Destinate al proprio account e relative a sé |
| Consultare Storico | Sì | No |
| Usare le API Calendar | Solo il proprio collegamento | Solo il proprio collegamento |

La verifica viene ripetuta dal server. Nascondere un pulsante è una conseguenza dei permessi, non la protezione principale. Modifiche a ruolo, abilitazioni, gruppi e ruoli operativi si applicano anche alle sessioni aperte. La disattivazione dell’account revoca le sessioni; la riattivazione richiede un nuovo login.

Il gestore dispone dello storico amministrativo, che può contenere informazioni sulle modifiche alle ore di altri utenti, e degli alert configurati. La limitazione del Registro ore personale non equivale quindi a una promessa che qualsiasi dettaglio del lavoro sia invisibile ai gestori.

## 2. Come si determina l’accesso alla commessa

Per un operatore il sistema considera:

1. Abilitazione diretta alla commessa.
2. Abilitazioni assegnate ai gruppi di appartenenza e ai rispettivi gruppi padre.
3. Opzione **Commessa comune**.

Le abilitazioni si sommano senza un meccanismo di negazione esplicita. Il ruolo Lettore non prevale su un Collaboratore ottenuto attraverso un altro percorso. Un’abilitazione a un gruppo padre raggiunge i membri dei sottogruppi; una concessione al sottogruppo non abilita i gruppi fratelli o tutti i membri del padre.

Le date di inizio e fine sono inclusive. L’assenza di una data elimina soltanto il limite su quel lato. Per consultare serve un’abilitazione valida oggi. Per scrivere ore serve **lo stesso collegamento autorizzativo**, con capacità di registrazione, valido sia oggi sia nella data lavorata.

Esempio: un Collaboratore abilitato dal 1° al 31 ottobre può registrare il 15 ottobre il lavoro del 10 ottobre. Il 1° novembre, una volta scaduto il periodo, non può più usare quel collegamento per nuove registrazioni del 10 ottobre. Un altro grant valido soltanto a novembre non combina retroattivamente i due periodi.

La revoca di un solo collegamento può lasciare attivi altri percorsi. Rendere una commessa non comune non rimuove assegnazioni dirette o di gruppo. Il profilo Gestore non dipende da questi grant per operare sulle commesse.

## 3. Stati, chiusura e cancellazione

Gli stati della commessa sono Da avviare, In corso, In pausa, Completata e Annullata. Sono classificazioni gestite manualmente; non esistono passaggi obbligatori o approvazioni. **Nessuno stato, da solo, impedisce nuove ore.** Per limitare la registrazione si interviene su accessi e proprietà di commessa comune.

Gli stati dei contratti sono Bozza, Attivo, Scaduto, Chiuso e Annullato. Anche questi vengono modificati dal gestore; la data finale non genera una transizione automatica. Contratti Chiusi o Annullati non possono essere scelti per un nuovo collegamento a una commessa, mentre i collegamenti precedenti possono rimanere.

| Operazione | Ore esistenti | Accessi | Altri dati |
| --- | --- | --- | --- |
| Cambiare stato della commessa | Conservate | Invariati | Aggiornamento nello Storico |
| Revocare un grant | Conservate e consultabili dall’autore | Rimosso quel percorso; altri possono rimanere | La commessa rimane |
| Disattivare un utente | Conservate negli aggregati e nello storico | Login bloccato | Account conservato |
| Eliminare una propria registrazione, da Gestore | Rimossa quella riga | Invariati | Evento nello Storico |
| Eliminare una commessa | Rimosse tutte le ore collegate di tutti gli utenti | Rimosse abilitazioni dirette e di gruppo della commessa | Rimosse attività e blocchi; utenti, gruppi, ruoli e contratti conservati; riepilogo nello Storico |

La cancellazione della commessa è definitiva nell’app e viene eseguita in un’unica transazione con la rimozione di collegamenti, ore, attività e blocchi. Lo Storico non è un cestino. La conservazione richiesta dal futuro modulo finanziario introdurrà regole ulteriori per le fonti valorizzate o fatturate; tali regole non sono già attive.

### Attività, gerarchia e impedimenti

L’attività appartiene a una sola commessa, immutabile dopo la creazione. Il genitore eventuale deve appartenere alla stessa commessa: sono vietati cicli e gerarchie oltre otto livelli, contando la radice come primo. Spostare un ramo deve rispettare il limite anche per tutti i suoi discendenti. La parentela organizza il lavoro ma non costituisce una dipendenza temporale.

Gli stati delle attività sono Da fare, In corso, Bloccato, Da verificare e Completato; le priorità sono Bassa, Normale, Alta e Urgente. Stati del genitore, dei figli e della commessa rimangono indipendenti. Il gestore può cambiare tutti i dettagli; il Collaboratore assegnato può aggiornare solo lo stato. Per le attività si verifica l’accesso valido **oggi**; la scadenza non è una data lavorata né un’ulteriore condizione del grant.

Un blocco riguarda una commessa e, facoltativamente, una sua attività. Ha impatto Basso, Medio, Alto o Critico; stato Aperto, In gestione o Risolto; responsabile, prossima azione e scadenza facoltativi. Un operatore ne diventa inizialmente responsabile. L’autore o il responsabile possono aggiornarlo finché Collaboratori; solo un gestore può cambiarne il responsabile o eliminarlo. Il cambio di responsabile non cambia l’autore.

Assegnare una persona richiede che sia attiva e Gestore o Collaboratore di quella commessa. Un riferimento preesistente può restare anche dopo disattivazione o revoca, ma non concede accesso. Il cambio del responsabile deve scegliere una persona attualmente abilitata. La bacheca non rende visibili contratti, PDF o ore personali altrui.

Ogni modifica o eliminazione di un’attività o blocco esistente verifica una revisione. Una revisione non più corrente produce un conflitto senza sovrascrittura automatica. Anche lo scollegamento dei blocchi da un’attività eliminata cambia la loro revisione. Le modifiche e gli eventi di Storico sono atomici.

L’eliminazione dell’attività è rifiutata se ha figli; i blocchi collegati vengono conservati e scollegati. L’eliminazione della commessa elimina invece tutte le attività e i blocchi. Segnalare o risolvere un blocco non cambia lo stato dell’attività; completare un’attività non registra ore e non genera notifiche automatiche. La [guida del modulo](attivita-kanban-e-blocchi.md) descrive i comandi dell’interfaccia.

## 4. Regole del registro ore

Ogni registrazione contiene persona, commessa, giornata, minuti e nota facoltativa. La nota non è un collegamento a una delle attività del Kanban. La persona è ricavata dalla sessione e identificata in modo stabile; non è un campo liberamente modificabile dal browser.

- Durata per riga: da 1 a 1440 minuti.
- Totale per persona e giornata: massimo 1440 minuti, sommando tutte le commesse.
- Più righe sulla stessa commessa e giornata: consentite.
- Massimo di 1000 registrazioni per persona nella stessa giornata.
- Note: fino a 1000 caratteri.
- Date passate e future: consentite se valide e compatibili con le abilitazioni.
- Orari di inizio e fine: non registrati; il sistema conserva una durata, non fasce orarie.

Le durate sono minuti interi. Il formato decimale viene convertito esattamente: `1,5` = 90 minuti; `1:30` = 90 minuti; `1,30` = 78 minuti. Un valore che richiederebbe frazioni di minuto viene rifiutato.

Il limite di 24 ore è un vincolo di integrità. La soglia di 8 ore degli alert è un controllo informativo separato: si possono salvare meno o più di 8 ore, entro il limite di 24, senza un’approvazione automatica del foglio ore.

Il prodotto non contiene ancora griglia settimanale, copia giornata, approvazione delle ore, rettifiche richieste dall’operatore o salvataggio offline. Le normali registrazioni non dispongono ancora della ricevuta generale di idempotenza progettata nella roadmap: se un esito è incerto, verificare il registro prima di ripetere l’inserimento.

## 5. Relazione tra fornitori, contratti, documenti e commesse

Un contratto richiede un fornitore; una commessa può collegare un contratto oppure rimanere senza contratto. Non si deve dedurre un rapporto commerciale di vendita dal solo campo Cliente della commessa o dal valore di un contratto con un fornitore.

I codici di fornitori, contratti e commesse sono univoci nelle rispettive anagrafiche. Disattivare un fornitore conserva i riferimenti storici ma lo esclude dai nuovi collegamenti. I valori contrattuali sono facoltativi ed espressi in EUR; zero e valore mancante sono distinti.

I PDF sono originali aggiuntivi: ogni caricamento crea un nuovo documento, anche quando nome o contenuto coincidono. Il sistema salva l’impronta del file e la controlla al download, ma non dispone ancora di un flusso di riconoscimento dei duplicati o versionamento contrattuale. Non sono disponibili modifica o eliminazione dei documenti tramite le API correnti.

Il limite per file è 10 MiB e sono ammessi fino a 1000 documenti per contratto. Sono controllati nome, firma iniziale, dimensione e integrità; l’accettazione del file non certifica completezza, validità legale o assenza di contenuti malevoli del documento. Il contenuto non viene estratto automaticamente.

## 6. Interpretare correttamente Analisi

I filtri data includono il primo e l’ultimo giorno e ammettono al massimo 366 giorni. Una data singola usa lo stesso valore in Dal e Al. I conteggi derivano da tutte le registrazioni filtrate; i gruppi di risultato sono limitati a 1000 e un superamento produce un errore esplicito.

Il gestore può aggregare più persone, anche disattivate. L’operatore vede soltanto se stesso, compreso il proprio storico su commesse revocate. Le note delle registrazioni non sono incluse nei report; le attività del Kanban non sono aggregate nelle analisi delle ore. Conservare o condividere un URL con filtri non concede permessi aggiuntivi.

### Valore contratti

Il report individua i contratti **attualmente collegati** alle commesse con ore nel periodo e conta ogni contratto una sola volta. Non assegna una quota del valore alle ore, alla persona o all’intervallo. Un contratto senza importo resta sconosciuto; un contratto da zero euro è valorizzato esplicitamente a zero.

Se due commesse con ore nel periodo condividono un contratto da 10.000 EUR, il totale noto è 10.000 EUR. Se cambia il contratto collegato a una commessa, cambia anche l’analisi delle sue ore passate. Non è un calcolo automatico dei ricavi o del margine.

### Costo delle ore

Per ogni registrazione si cerca la tariffa della persona valida nella data del lavoro. Le date delle tariffe sono inclusive; due intervalli della stessa persona non possono sovrapporsi. Il costo della singola riga è:

```text
costo in centesimi = arrotondamento al centesimo di
                    (minuti × tariffa oraria in centesimi / 60)
```

La metà viene arrotondata verso l’alto e il calcolo avviene **prima della somma**, per ogni riga. A una tariffa di 1 EUR/ora, tre registrazioni da un minuto valgono 0,02 EUR ciascuna, per 0,06 EUR; una singola registrazione da tre minuti vale 0,05 EUR. La differenza dipende dalla regola di arrotondamento per registrazione.

La tariffa zero attribuisce un costo zero noto. In assenza di tariffa la durata è segnalata come non valorizzata e non contribuisce come costo zero. Se tutte le ore sono senza tariffa, la stima non è disponibile; se ne è coperta soltanto una parte, la stima è parziale.

Le tariffe sono in EUR, con massimo 999.999,99 EUR/ora. Il report viene ricalcolato: la modifica della tariffa cambia le stime storiche senza modificare i minuti. Non esistono ancora tariffe di vendita, snapshot contabili, cambi valuta o periodi economici bloccati.

## 7. Alert: tempo, destinatari e significato

Una regola contiene proprietario, tipo, nome, orario quotidiano, stato attivo e destinatari. Ogni gestore gestisce le proprie regole. Se il proprietario non è più attivo o non è più Gestore, l’esecuzione automatica è sospesa.

### Commessa inesistente

Il controllo raccoglie tentativi rifiutati di registrare ore su un identificativo di commessa inesistente o eliminato. Le ore non sono mai state salvate. L’eliminazione regolare di una commessa rimuove le sue ore: l’alert non è quindi un controllo che ricostruisce le ore cancellate né una generica scansione di integrità del database.

Gli eventi già precedenti alla creazione della regola non vengono notificati come nuove anomalie. Una mancata abilitazione su una commessa esistente non produce di per sé questo tipo di evento.

### Ore giornaliere mancanti

Sono esaminate le giornate dal lunedì al venerdì degli utenti attivi monitorati. Il totale inferiore a 480 minuti genera un avviso dopo almeno **48 ore effettive dalla fine della giornata**, interpretata in Europe/Rome.

Esempio senza cambio d’ora: la giornata di lunedì termina martedì alle 00:00. La tolleranza scade giovedì alle 00:00; una regola delle 09:00 la valuta giovedì alle 09:00. Il fine settimana è escluso come giornata da completare, ma le sue ore cronologiche contano nella tolleranza. Una regola può essere eseguita anche sabato o domenica per rilevare una giornata feriale diventata verificabile.

Il cambio tra ora solare e legale non trasforma la tolleranza in «due date di calendario»: restano 48 ore effettive. Non sono configurabili festività, ferie, part time o soglie individuali. Sono monitorati tutti gli utenti attivi, anche i gestori; i destinatari scelti determinano chi riceve l’esito.

Il monitoraggio comincia dalla creazione della regola. Un utente aggiunto successivamente è incluso dal primo controllo che lo registra come soggetto, senza recupero di periodi anteriori al proprio monitoraggio. Una giornata già esaminata non è continuamente ricontrollata: la notifica conserva il totale rilevato, anche se le ore vengono poi integrate o cancellate. Non rappresenta uno stato sempre aggiornato di approvazione della giornata.

### Programmazione e canali

Il fuso dell’orario scelto è Europe/Rome. L’automatismo salva l’ultima giornata eseguita e non ripete lo stesso turno dopo un normale riavvio. Una regola nuova creata dopo il suo orario odierno attende il giorno successivo. **Esegui ora** consente un controllo manuale, anche con regola disattivata; la deduplicazione evita di notificare di nuovo le anomalie già elaborate dalla medesima regola.

Un’esecuzione manuale aggiorna il giorno già elaborato e impedisce un secondo turno automatico nella stessa giornata. Sospendere la regola non elimina le email pendenti; la consegna controlla proprietario e destinatario ancora ammessi, separatamente dall’attivazione del turno giornaliero. Il [motore alert](../tehnical/motore-alert.md) descrive cursori e casi di ripartenza.

Sono ammessi fino a 50 utenti destinatari e 50 indirizzi email per regola, con almeno un destinatario complessivo. Le notifiche in app rispettano il profilo del destinatario: un Operatore riceve soltanto i propri eventi, un Gestore può ricevere quelli di tutti i soggetti monitorati. Un indirizzo email è invece una destinazione configurata esplicitamente dal gestore, non un utente sottoposto al filtro personale della UI.

Il canale email conserva gli invii in coda mentre SMTP è assente. Gli esiti possono distinguere attesa di configurazione, nuovo tentativo, accettazione SMTP, consegna incerta ed errore. Una consegna incerta richiede verifica e non è automaticamente reinviata. Gli stati tecnici non sono ancora una pagina di amministrazione dell’outbox nell’interfaccia.

## 8. Notifiche e storico

La notifica appartiene a un destinatario. **Segna come letta** aggiorna la presa visione, non risolve la causa dell’avviso. L’elenco mostra le ultime 100 notifiche accessibili, mentre il contatore può includere notifiche non lette più vecchie.

Lo Storico è un registro delle modifiche autorizzate. Salvataggio applicativo ed evento sono atomici; un errore nello storico impedisce anche la modifica. Gli eventi conservano autore, data e dettagli prima/dopo. Non sono memorizzate password, derivazioni o sali delle password nel payload di audit. Gli errori rifiutati non diventano eventi di modifica riuscita; i tentativi su commesse inesistenti hanno invece il canale eventi specifico degli alert.

## 9. Date e fusi visualizzati

Le giornate lavorate sono date, senza un orario di ingresso e uscita. Le analisi usano la data restituita dal server per i periodi rapidi. Il Registro ore e alcune indicazioni UI usano la data locale del browser; il server mantiene l’autorità sulla validità dell’accesso. Vicino alla mezzanotte, con dispositivi in fusi differenti, le indicazioni possono divergere: verifica la data esplicita della registrazione.

Le notifiche e gli orari degli alert sono mostrati in Europe/Rome; lo Storico visualizza gli istanti nel fuso locale del browser. L’unificazione completa del contesto temporale aziendale è prevista nella base tecnica della roadmap.

## 10. Capacità operative

| Oggetto | Limite corrente |
| --- | --- |
| Utenti | 200 |
| Gruppi | 200, massimo 16 livelli |
| Ruoli sulle commesse | 100 complessivi |
| Commesse | 1000 |
| Attività | 500 per commessa, incluse le completate; massimo 8 livelli |
| Blocchi | 500 per commessa, inclusi i risolti |
| Testi attività e blocchi | Titolo fino a 160 caratteri, descrizione fino a 2000, prossima azione fino a 1000 |
| Fornitori | 1000 |
| Contratti | 1000 |
| PDF | 10 MiB per file, 1000 per contratto |
| Tariffe orarie | 1000 |
| Registrazioni ore | 1000 per utente e giornata; totale massimo 24 ore |
| Analisi | Intervallo massimo 366 giorni; massimo 1000 gruppi di risultato |
| Regole alert | 100 per proprietario |
| Destinatari alert | 50 utenti e 50 email per regola |
| Elenco notifiche | Ultime 100 accessibili; contatore non letto calcolato separatamente |

Il raggiungimento di un limite non va aggirato creando identità duplicate o rendicontazioni fittizie. I moduli con un limite di censimento restituiscono un errore esplicito; un ampliamento richiede una modifica verificata del prodotto.

## 11. Integrazioni e protezione dei dati

Google Calendar dispone di un servizio e di API per il collegamento personale, selezione del calendario, lettura degli eventi e modifiche bidirezionali. OAuth è ancora da configurare e il percorso grafico non è disponibile. Un evento non genera automaticamente ore e il gestore non ottiene accesso al calendario privato di altri utenti.

Il servizio rileva conflitti sulle modifiche remote e non promette la sovrascrittura automatica della versione più recente. L’archivio locale conserva anche serie ricorrenti ed eccezioni, senza trasformarle oggi in tutte le occorrenze di una vista grafica. Il collegamento evento–commessa e la proposta di durata dovranno essere progettati con una conferma dell’utente.

I permessi applicativi non equivalgono a cifratura di tutti i dati. L’archivio Calendar cifra token e contenuti a riposo, ma li decifra per l’elaborazione. Le password di accesso sono conservate come derivazioni non reversibili; campi applicativi e PDF non dispongono ancora del vault richiesto in prospettiva. La replica dei database protegge la disponibilità rispetto ad alcuni guasti, non recupera automaticamente cancellazioni propagate: per questo servono i [backup](../tehnical/operazioni-backup-ripristino.md).
