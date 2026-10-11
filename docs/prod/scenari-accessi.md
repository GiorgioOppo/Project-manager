# Scenari concreti di accesso alle commesse

Questi esempi illustrano le regole effettive del prodotto. Sono casi di ragionamento e di collaudo, non assegnazioni da applicare automaticamente. Tutte le persone e le commesse sono fittizie. La procedura per crearle è nella [guida del gestore](guida-gestore.md).

## 1. Distinguere persona, profilo e ruolo operativo

Una persona accede con un account **Gestore** oppure **Operatore**. Quando si assegna una commessa a un utente o gruppo, si sceglie invece un ruolo operativo: **Lettore**, **Collaboratore** o un ruolo personalizzato.

Un account Operatore con ruolo Collaboratore può consultare la commessa assegnata e registrarvi il proprio lavoro. Non può amministrare quella commessa, anche se il nome personalizzato del ruolo fosse «Responsabile». Un account Gestore dispone dei poteri globali sulle commesse dello spazio senza bisogno di un grant.

Nelle matrici seguenti:

| Simbolo | Significato |
| --- | --- |
| **L+O** | Può leggere la commessa e registrare le proprie ore nella data del caso |
| **L** | Può leggere, ma non registrare nuove ore |
| **—** | Nessun accesso corrente alla commessa |

L significa consultazione della commessa, non accesso ai contratti/PDF sottostanti o alle ore altrui. L+O non comprende la modifica delle registrazioni di un altro utente.

## 2. Un albero con assegnazioni diverse

Esempio di gruppi e appartenenze dirette:

```text
Area tecnica                       Paolo è membro diretto
├── Sviluppo                        Elena è membro diretto
└── Supporto                        Lucia è membro diretto

Luca                               nessun gruppo
Marta                              profilo Gestore
```

Paolo, Elena, Lucia e Luca sono account Operatore attivi. Gli intervalli sono illimitati in questo primo scenario. Crea queste concessioni:

| Commessa | Configurazione |
| --- | --- |
| ACC-A, Portale | Area tecnica: Collaboratore |
| ACC-B, Manuale | Sviluppo: Lettore |
| ACC-C, Affiancamento | Luca: Collaboratore diretto |
| ACC-G, Generale | Commessa comune |
| ACC-Z, Riservata | Nessuna abilitazione e non comune |

Esito atteso, per la data lavorata corrente:

| Persona | ACC-A | ACC-B | ACC-C | ACC-G | ACC-Z |
| --- | --- | --- | --- | --- | --- |
| Elena, Sviluppo | L+O | L | — | L+O | — |
| Lucia, Supporto | L+O | — | — | L+O | — |
| Paolo, Area tecnica | L+O | — | — | L+O | — |
| Luca, nessun gruppo | — | — | L+O | L+O | — |
| Marta, Gestore | L+O | L+O | L+O | L+O | L+O |

L’abilitazione al padre Area tecnica raggiunge membri diretti e discendenti. L’abilitazione a Sviluppo non risale per concedere ACC-B a Paolo e non passa al gruppo fratello Supporto. Marta mantiene anche i poteri di amministrazione; la matrice mostra soltanto lettura e registrazione personale.

Se un nuovo Operatore viene creato senza appartenenze o grant, potrà usare ACC-G, ma non acquisirà le quattro commesse riservate. Se lo aggiungi a Sviluppo, erediterà ACC-A e ACC-B dalla richiesta successiva.

## 3. Più percorsi di accesso non si annullano

Parti dall’esempio precedente, concentrandoti su Elena e ACC-A. Le modifiche elencate sono sequenziali; tutte le date rimangono illimitate.

| Passo | Situazione di Elena | Esito su ACC-A |
| --- | --- | --- |
| 1 | Area tecnica assegna Collaboratore | L+O |
| 2 | Aggiungi un grant diretto Lettore a Elena | L+O: il Lettore non annulla il Collaboratore del gruppo |
| 3 | Revoca il solo grant diretto | L+O: rimane il percorso di gruppo |
| 4 | Aggiungi Elena anche a un secondo gruppo con Collaboratore su ACC-A | L+O attraverso due percorsi |
| 5 | Revoca ACC-A ad Area tecnica | L+O: rimane il secondo gruppo |
| 6 | Revoca anche il secondo gruppo | —, se non esistono altri grant o proprietà comune |

L’app non ha un ruolo di «divieto» che prevalga sulle altre concessioni. Un ruolo personalizzato con **Solo consultazione** è equivalente a Lettore per questa capacità; un ruolo con **Consulta e registra le proprie ore** può abilitare la scrittura.

La modifica di un ruolo personalizzato agisce su tutte le assegnazioni che lo usano. Cambiarlo da scrittura a sola lettura può togliere la possibilità di registrare, ma solo agli utenti che non hanno un altro percorso valido di scrittura. I ruoli predefiniti non sono modificabili.

## 4. Una sola abilitazione deve coprire entrambe le date

Per questo esempio il server considera **oggi = 15 ottobre 2026** e l’utente vuole registrare il lavoro del **10 ottobre 2026**. Questi valori descrivono un caso; per ripeterlo senza cambiare l’orologio del server usa oggi e una data di cinque giorni prima, adattando gli intervalli.

Le date iniziali/finali sono comprese. «Senza limite» significa campo vuoto. La commessa non è comune e l’utente è un Operatore attivo.

| Grant disponibile | Legge il 15 ottobre? | Registra il 10 ottobre? | Motivo |
| --- | --- | --- | --- |
| Collaboratore dal 1° al 31 ottobre | Sì | Sì | Lo stesso grant copre entrambe le date |
| Collaboratore dal 15 al 31 ottobre | Sì | No | Copre oggi, ma non la giornata lavorata |
| Collaboratore dal 1° al 14 ottobre | No | No | È scaduto oggi, anche se copriva il lavoro |
| Collaboratore dal 16 ottobre senza fine | No | No | L’accesso non è ancora iniziato |
| Lettore dal 1° al 31 ottobre | Sì | No | Manca la capacità di registrazione |
| Collaboratore senza limiti | Sì | Sì | Nessun estremo esclude le due date |

Non è possibile costruire un diritto di scrittura combinando periodi diversi:

| Grant 1 | Grant 2 | Esito per il 10 ottobre |
| --- | --- | --- |
| Gruppo A, Collaboratore 1–10 ottobre | Gruppo B, Collaboratore 15–31 ottobre | Negato: nessuno dei due copre sia oggi sia il lavoro |
| Diretto, Lettore 1–31 ottobre | Gruppo B, Collaboratore 15–31 ottobre | Negato: il grant che copre entrambe le date è soltanto Lettore |
| Gruppo A, Collaboratore 1–10 ottobre | Gruppo B, Collaboratore 10–31 ottobre | Consentito: il secondo grant comprende il 10 e il 15 |

Lo stesso ragionamento vale per una registrazione futura. Un Collaboratore valido dal 1° al 31 ottobre, il giorno 15, può registrare il 20 ottobre ma non il 1° novembre. Il software permette date future entro i limiti autorizzativi; non le interpreta come un’approvazione di lavoro già svolto.

## 5. Commesse comuni e stato del progetto

Se attivi **Commessa comune**, ogni account attivo può consultare la commessa e registrarvi le proprie ore, senza un grant personale e senza limiti di data derivati da un’assegnazione.

| Configurazione per un Operatore attivo | Lettura | Nuove ore su una data valida |
| --- | --- | --- |
| Comune, nessun grant | Sì | Sì |
| Comune, grant diretto Lettore | Sì | Sì: l’accesso comune consente la registrazione |
| Comune, grant scaduto | Sì | Sì: la scadenza del grant non annulla la proprietà comune |
| Non comune, unico grant Lettore | Sì | No |
| Non comune, nessun grant valido oggi | No | No |

Disattivare la proprietà comune rimuove soltanto quel percorso. Chi mantiene un grant diretto o di gruppo continua ad avere i diritti concessi da quel grant.

Gli stati **Da avviare**, **In corso**, **In pausa**, **Completata** e **Annullata** non modificano questa decisione. Una commessa comune Annullata resta tecnicamente utilizzabile dagli account attivi. Per una chiusura operativa il gestore deve verificare anche accessi e proprietà comune; non esiste oggi un interruttore di «chiusura ore» separato.

## 6. Revoche, spostamenti e storia personale

Supponi che Elena abbia già registrato due ore su ACC-A e che il suo unico accesso provenga da Area tecnica tramite Sviluppo.

| Modifica | Accesso corrente a ACC-A | Ore pregresse di Elena |
| --- | --- | --- |
| Rimuovi Elena da Sviluppo | Revocato, in assenza di altri percorsi | Restano nel suo registro e nelle sue analisi |
| Sposti Sviluppo sotto un padre privo del grant | Il grant di Area tecnica non è più ereditato | Conservate |
| Ripristini l’appartenenza o il padre | Accesso ricalcolato con i grant correnti | Conservate, senza duplicazioni |
| Disattivi il solo ruolo di scrittura tramite un ruolo personalizzato | Può rimanere la lettura | Conservate |
| Elimini ACC-A | La commessa non esiste più | Tutte le ore su ACC-A sono eliminate, non solo quelle di Elena |

Una revoca viene controllata anche se l’utente aveva già aperto il modulo. Il salvataggio non usa un permesso «congelato» all’apertura della pagina. La UI aggiorna le scelte e può mantenere data, durata e nota mentre richiede una commessa ancora autorizzata.

Nel report personale, una commessa revocata può continuare a comparire perché contiene ore proprie. Quella presenza storica non la rende di nuovo disponibile per nuove ore. Dopo l’eliminazione definitiva non rimangono invece quelle righe nei report; lo Storico amministrativo conserva gli eventi e il riepilogo dell’eliminazione.

## 7. Disattivazione e cambi del profilo globale

La disattivazione dell’account impedisce l’accesso e revoca le sessioni esistenti, anche se la persona appartiene a gruppi o la commessa è comune. Una successiva riattivazione richiede un nuovo login. Le ore storiche restano nei report dei gestori.

Il passaggio da Gestore a Operatore elimina i poteri amministrativi dalla richiesta successiva e restringe i report allo storico personale. Per continuare a usare commesse riservate occorrono grant operativi validi; il fatto di averle create quando si era Gestore non concede un diritto permanente.

Non si può disattivare o declassare l’ultimo Gestore attivo. Prima di provare un cambio di profilo del proprio account in laboratorio, predisponi un secondo Gestore attivo. Il test non richiede di eseguire la stessa operazione sull’ultimo responsabile di uno spazio reale.

## 8. Accesso ai dettagli e limiti di riservatezza

| Risorsa | Operatore | Gestore |
| --- | --- | --- |
| Elenco corrente commesse | Solo autorizzate e comuni | Tutte |
| Registro ore | Proprio | Proprio, con modifica/eliminazione delle proprie righe |
| Report del lavoro | Proprie ore, anche dopo revoca | Aggregati di tutti gli utenti |
| Note nelle risposte dei report | Non presenti | Non presenti |
| Storico delle modifiche | Non accessibile | Accessibile; i dettagli prima/dopo delle ore possono includere note altrui |
| Fornitori, contratti e PDF | Non accessibili | Accessibili |
| Valore contratti e costo delle ore | Non accessibili | Accessibili |
| Notifiche | Solo proprie e relative a sé | Proprie notifiche, anche riferite ad altri soggetti |
| Calendario tramite API | Solo il proprio collegamento | Solo il proprio collegamento, senza estensione agli altri utenti |

La proprietà del Registro ore non è una garanzia di invisibilità delle note ai responsabili amministrativi: lo Storico può conservarle negli snapshot. Inserisci nella nota la descrizione necessaria del lavoro, secondo le regole dell’organizzazione.

Le autorizzazioni contrattuali non sono ereditate dall’abilitazione alla commessa. Un Lettore della commessa può quindi consultarne le informazioni consentite senza poter aprire il PDF del contratto.

## 9. Riferimenti per verificare i casi

Le decisioni descritte sono coerenti con il motore di accesso in `src/organization.c`, le verifiche delle registrazioni in `src/workspace.c` e i casi di `tests/test_groups.py`, `tests/test_assignments.py` e `tests/test_job_defaults.py`. Per ripeterle dall’interfaccia vedi il [collaudo di accettazione](collaudo-accettazione.md). Le API e i codici di errore sono nella [documentazione tecnica dell’app](../tehnical/api-app.md).
