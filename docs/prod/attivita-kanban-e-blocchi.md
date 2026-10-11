# Attività, Kanban e registro dei blocchi

**Italiano** | [English](../en/prod/attivita-kanban-e-blocchi.md)

Stato documentato: **11 ottobre 2026**, schema dati **8**.

Il modulo **Attività e blocchi** organizza il lavoro all'interno delle commesse. Un'attività descrive un risultato da ottenere; un blocco descrive un impedimento da affrontare. Questa prima consegna comprende attività, sottoattività, bacheca e registro dei blocchi. Dipendenze, pianificazione del carico, rischi probabilistici e collegamento delle ore alle singole attività restano sviluppi successivi.

## Preparare la commessa

Aprire **Attività e blocchi** e selezionare una commessa, oppure usare **Attività** nella riga della commessa. Il selettore contiene le commesse visibili all'utente, comprese quelle comuni. Le attività e i blocchi sono condivisi con le persone autorizzate a consultare quella commessa; non inserire nelle descrizioni informazioni riservate a un pubblico diverso.

Il gestore crea le attività indicando titolo, descrizione, responsabile facoltativo, priorità e scadenza. Può scegliere un'attività superiore della stessa commessa per creare una sottoattività. Sono consentiti al massimo otto livelli, contando l'attività principale come primo livello. Il sistema rifiuta riferimenti circolari e collegamenti fra commesse differenti.

Il responsabile deve essere un utente attivo con ruolo Gestore oppure abilitato a collaborare sulla commessa. Assegnare un'attività non attribuisce nuove autorizzazioni: le abilitazioni si gestiscono sempre attraverso commesse, utenti e gruppi. Una revoca impedisce gli aggiornamenti anche se il nome resta associato per conservare il contesto; il gestore può riassegnare il lavoro.

## Usare la bacheca

Le attività attraversano cinque stati:

| Stato | Significato operativo |
| --- | --- |
| Da fare | Attività definita e ancora da iniziare |
| In corso | Lavoro avviato |
| Bloccato | L'attività non può proseguire |
| Da verificare | Lavoro preparato, in attesa di una verifica |
| Completato | Attività terminata |

Il cambio di stato usa controlli accessibili anche da tastiera. Non è obbligatorio seguire ogni colonna in sequenza e non esiste ancora un'approvazione formale del passaggio a Completato. Il gestore può aggiornare qualunque attività; l'operatore può aggiornare lo stato di quelle assegnate a lui, purché mantenga un ruolo che consente di collaborare.

Usare **Cerca attività**, **Stato** e **Priorità** per restringere i risultati. La ricerca include titolo, descrizione e nome del responsabile. Il selettore **Vista** passa da **Bacheca Kanban** a **Elenco e sottoattività**. Sulla bacheca, scegliere lo stato nella scheda e premere **Applica**; su schermi piccoli le colonne scorrono nel loro riquadro. **Aggiorna** rilegge i dati della commessa.

La vista gerarchica aiuta a leggere le sottoattività. Se il filtro nasconde un genitore, la sottoattività corrispondente rimane visibile con il riferimento all’attività principale. Completare un'attività superiore non completa automaticamente i figli. Le scadenze e le priorità sono informazioni operative; non modificano automaticamente lo stato della commessa e non generano nuove regole email.

## Segnalare un blocco

Premere **Segnala blocco** nella pagina o nella scheda di un’attività. Un collaboratore può segnalare un blocco scegliendo titolo, descrizione, impatto, prossima azione e scadenza facoltativa. Il collegamento a una specifica attività è opzionale: il blocco può riguardare tutta la commessa.

Il comando **Gestisci** apre il modulo di un blocco modificabile; da qui si aggiornano descrizione, stato e prossima azione. Il filtro **Mostra blocchi** propone inizialmente **Aperti e in gestione** e permette anche **Tutti** o **Risolti**.

Gli impatti disponibili sono Basso, Medio, Alto e Critico. Gli stati sono Aperto, In gestione e Risolto. L'impatto indica la gravità operativa e non è un calcolo economico automatico.

Un blocco creato dall'operatore viene assegnato inizialmente a lui. Il gestore può attribuirlo a un'altra persona abilitata. Il gestore e, finché autorizzati a collaborare sulla commessa, l'autore o il responsabile del blocco possono aggiornarlo. Solo il gestore può cambiarne il responsabile o eliminarlo.

Lo stato Bloccato dell'attività e il registro dei blocchi sono distinti: segnalare o risolvere un impedimento non sposta automaticamente la scheda. Questo permette di gestire più impedimenti per una stessa attività senza completarla per errore.

## Permessi

| Operazione | Gestore | Collaboratore della commessa | Lettore |
| --- | --- | --- | --- |
| Leggere bacheca e blocchi | Sì | Sì | Sì |
| Creare e organizzare attività | Sì | No | No |
| Cambiare stato attività | Tutte | Solo quelle assegnate a lui | No |
| Segnalare un blocco | Sì | Sì | No |
| Modificare un blocco | Tutti | Solo se autore o responsabile | No |
| Cambiare responsabile o eliminare un’attività o un blocco | Sì | No | No |

Nel seguito, Collaboratore include anche un ruolo personalizzato che consente di registrare ore; la commessa comune concede questa capacità agli utenti attivi. Per la bacheca il permesso deve essere valido oggi, indipendentemente dalla scadenza dell’attività.

I controlli si applicano anche alle richieste dirette alle API. Avere conservato un vecchio collegamento non permette di aggirare una revoca. Se il server segnala accesso negato o elemento non disponibile, l’interfaccia svuota la bacheca e chiude i moduli aperti; una bozza non salvata non viene conservata. Scegliere di nuovo una commessa attualmente abilitata.

## Modifiche contemporanee e cancellazioni

Ogni salvataggio di un elemento esistente verifica la sua revisione. Se un'altra persona lo ha modificato nel frattempo, il server rifiuta la sovrascrittura: rileggere l'elemento e confrontare le modifiche prima di riprovare. Nessuna fusione automatica viene applicata. Nel modulo, **Chiudi e ricarica la versione aggiornata** chiude la bozza e rilegge i dati: annotare prima le proprie variazioni se devono essere riportate. Per il cambio rapido di stato la pagina ricarica l’elenco e mostra il conflitto.

Per eliminare un'attività occorre prima spostare o eliminare le sue sottoattività. Gli eventuali blocchi associati restano nella commessa e vengono scollegati dall'attività eliminata. L'eliminazione definitiva di una commessa rimuove invece anche tutte le attività e tutti i blocchi, oltre alle ore e alle abilitazioni già previste. Lo Storico conserva gli eventi e i conteggi; non sostituisce un backup.

Titolo: da 1 a 160 caratteri; descrizione: fino a 2000; prossima azione di un blocco: fino a 1000. La scadenza è una data facoltativa. Le priorità delle attività sono Bassa, Normale, Alta e Urgente.

Il limite della prima consegna è di **500 attività e 500 blocchi per commessa**, inclusi quelli completati o risolti. Al raggiungimento del limite il sistema rifiuta nuove creazioni; le liste non vengono tagliate silenziosamente.

Riferimenti: [guida gestore](guida-gestore.md), [scenari di accesso](scenari-accessi.md), [API e modello tecnico](../tehnical/attivita-kanban-e-blocchi.md).
