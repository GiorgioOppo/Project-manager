# Inventario degli asset

**Italiano** | [English](../en/prod/asset-inventory.md)

[Indice prodotto](README.md) · [Guida tecnica](../tehnical/asset-inventory.md)

L'Inventario raccoglie dispositivi IT, apparati di rete, beni fisici, software e licenze. Conserva collocazione, assegnazione, stato, acquisto e garanzia, con campi specifici e collegamenti tra schede. È disponibile agli utenti autenticati: tutti possono consultare catalogo, relazioni e storico; solo il **Gestore** può aggiungere o modificare schede, registrare relazioni e rimuoverle.

## Consultare i beni

Aprire la scheda **Asset inventory**. La ricerca e i filtri per stato, tipologia, sede e reparto restringono il catalogo; la ricerca comprende i dettagli specifici, per esempio hostname, editore e riferimento della licenza. Il reparto si restringe in base alla sede scelta; restano selezionabili le anagrafiche disattivate per trovare beni già registrati. La paginazione permette di leggere altri beni senza caricare tutto l'archivio. Il riepilogo **Intero inventario** mostra i conteggi globali, anche quando l'elenco è filtrato. Aprire un bene per leggere scheda, relazioni e storico delle revisioni.

Il **Cruscotto operativo** segue tutti i filtri, compreso **I miei asset**, e conta tutti i risultati oltre la pagina caricata. Mostra consegne aperte, restituzioni in ritardo, beni mai verificati, verifiche da ripetere e ubicazioni da controllare. Selezionare una scheda del cruscotto filtra l'elenco; premere di nuovo la stessa scheda oppure **Rimuovi filtro operativo** per togliere il filtro. **Azzera filtri** ripristina il catalogo completo. La distribuzione espandibile raggruppa i beni per sede, reparto e utente, mostrando al massimo i primi 20 gruppi per dimensione.

I conteggi di consegne e verifiche riguardano dispositivi IT, apparati di rete e beni fisici, escludendo dismessi e smarriti. Una restituzione risulta in ritardo dal giorno successivo alla data prevista. **Ripeti la verifica dopo** permette di scegliere 30, 90, 180 o 365 giorni: una verifica è da ripetere quando l'ultima data registrata più l'intervallo raggiunge il giorno corrente; i beni mai verificati hanno un conteggio separato. Le ubicazioni da controllare corrispondono all'ultima verifica con luogo osservato diverso da quello atteso. Cambiare l'intervallo nel cruscotto modifica la consultazione; l'intervallo delle regole alert si imposta nella singola regola.

| Stato | Significato operativo |
| --- | --- |
| Disponibile | Bene pronto per essere assegnato. |
| Assegnato | Bene affidato alla persona o alla funzione indicata nell'assegnatario. |
| In manutenzione | Bene temporaneamente indisponibile per manutenzione. |
| Dismesso | Bene ritirato dall'uso, mantenuto nell'archivio. |
| Smarrito | Bene non più reperibile. |

Gli stati sono aggiornati dal Gestore. Assegnazione, consegna e restituzione aggiornano anche lo stato; il semplice superamento di una data non lo modifica.

## Creare e aggiornare una scheda

Con **Nuovo asset** il Gestore inserisce un'etichetta inventario univoca e un nome. L'etichetta distingue il bene anche quando altri beni hanno lo stesso modello. La categoria è libera e parte da `hardware`; produttore, modello, matricola e note descrivono il bene. Posizione e assegnatario testuali restano compatibili con le vecchie schede; per affidare un bene a una persona censita, anche senza account Punto, usare l'assegnazione strutturata dalla scheda.

La tipologia seleziona i campi della scheda, mentre la categoria resta una classificazione libera:

| Tipologia | Informazioni specifiche |
| --- | --- |
| Dispositivo IT | Hostname, indirizzo IPv4 o IPv6, MAC, sistema operativo. |
| Apparato di rete | Campi del dispositivo IT, ruolo di rete, VLAN e numero di porte. |
| Bene fisico | Tipo di bene, reparto, prossima manutenzione. |
| Software | Editore, versione di catalogo, edizione, tipo di software. |
| Licenza | Software di riferimento, modello, posti acquistati, scadenza, data e modalità di rinnovo, riferimento amministrativo. |

Per una licenza occorre prima censire il software e indicare almeno un posto acquistato. Il riferimento amministrativo può essere un numero di ordine o contratto: non inserire chiavi di attivazione. Le schede già presenti prima dell'introduzione delle tipologie sono inizialmente dispositivi IT con dettagli vuoti; il Gestore può riclassificarle.

Acquisto e scadenza garanzia sono facoltativi. Se presenti devono essere date valide; la garanzia non può precedere l'acquisto. I limiti sono 64 byte per l'etichetta, 160 per nome, posizione e assegnatario, 80 per categoria, 120 per matricola, produttore e modello e 4000 per le note. I caratteri accentati possono occupare più byte.

La **Scheda asset** mostra i dettagli, le relazioni e lo storico con le copie delle revisioni; **Modifica** apre il modulo per i Gestori. Una tipologia con relazioni attive non può cambiare finché tali relazioni non vengono rimosse; un software usato da una scheda licenza deve restare software.

Ogni creazione e modifica conserva autore, data, versione e copia della scheda risultante. Se un altro Gestore salva prima di noi, l'app segnala il conflitto: la bozza resta visibile e occorre ricaricare esplicitamente la scheda aggiornata prima di ripetere le modifiche. L'operazione non sovrascrive silenziosamente il lavoro altrui.

Per ritirare un bene impostare **Dismesso**. Non è disponibile un comando di eliminazione: identificativo e storico rimangono consultabili.

## Persone e identità

I comandi **Persone** e **Identità** aprono due anagrafiche separate, consultabili dagli utenti e modificabili dai Gestori. Una persona rappresenta un individuo reale, anche senza accesso a Punto; un'identità rappresenta un account in un sistema o dominio. Per esempio, Mario può avere un account email e uno VPN; Mario e Anna possono usare la stessa utenza di reparto. La relazione è molti a molti, indipendentemente dalla classificazione personale, condivisa o di servizio dell'identità.

La persona ha un codice univoco, nome, email, organizzazione, ruolo e note. L'identità ha sistema/dominio e nome account, univoci come coppia, nome visualizzato, tipo e note. Le anagrafiche conservano stato attivo, autore, versione e storico. Non inserire credenziali nei campi: queste schede censiscono le utenze e non le abilitano all'accesso.

Dalla scheda identità il Gestore collega una o più persone con ruolo responsabile o utilizzatore. La scheda persona mostra tutte le identità associate. Ricerca e paginazione permettono di consultare e selezionare anche anagrafiche ampie. Le rimozioni conservano lo storico; disattivare una voce mantiene i riferimenti esistenti e impedisce nuovi collegamenti.

Nella scheda asset, **Persone e identità collegate** permette di indicare persone e account con ruolo responsabile, utilizzatore o amministratore. Dalle anagrafiche si consultano i beni collegati direttamente e si può filtrare il catalogo, il cruscotto e l'esportazione con **Vedi beni**. Questi elenchi e filtri considerano solo i collegamenti diretti: consegne, assegnazioni operative e licenze non vi aggiungono beni automaticamente. Un account condiviso collegato a un bene non assegna automaticamente quel bene a tutte le persone che lo usano. I collegamenti non cambiano lo stato del bene, le consegne, i posti licenza o i permessi di accesso. Le consegne precedenti a utenti Punto restano conservate.

## Sedi, ubicazioni e assegnazioni operative

Il Gestore può gestire le anagrafiche di **sedi**, **reparti** e **ubicazioni**, poi selezionarle nella scheda di un dispositivo IT, apparato di rete o bene fisico. Ogni ubicazione appartiene a una sede; un reparto può essere collegato a una sede. La scheda controlla che i luoghi scelti siano coerenti. Per cambiare la sede di un reparto o un'ubicazione già usati occorre prima rimuoverli dai contesti correnti dei beni. Le anagrafiche possono essere disattivate; le registrazioni precedenti conservano i nomi memorizzati nello storico.

L'assegnazione strutturata collega il bene a una persona attiva dell'anagrafica oppure a un utente Punto legacy e porta lo stato ad **Assegnato**. La persona può non avere accesso all'app; i due tipi di destinatario restano distinti. Rimuovere l'assegnazione riporta un bene assegnato a **Disponibile**, mantenendo la collocazione. Il filtro **I miei asset** e la distribuzione per utente riguardano solo gli utenti Punto, senza collegamento automatico alle persone; il catalogo completo rimane consultabile da tutti gli utenti autenticati.

Software e licenze usano i propri dettagli e relazioni: le licenze possono avere destinatari persona o identità secondo il modello, mantenendo le assegnazioni testuali precedenti. L'editor generale e l'importazione CSV non sostituiscono il flusso di assegnazione, consegna e restituzione dei beni già gestiti in modo strutturato. Non vengono convertiti testi o utenti precedenti in persone, né creati account di login o collegamenti diretti ai beni automaticamente.

## Consegnare, restituire e verificare un bene

Dalla scheda il Gestore può registrare una **consegna** a una persona attiva dell'anagrafica, anche senza account Punto, oppure a un utente Punto legacy, indicando la data, la restituzione prevista, le condizioni iniziali e le note. Il bene deve essere disponibile oppure già assegnato allo stesso destinatario in modo strutturato, senza una consegna aperta. La consegna lo assegna al destinatario; una restituzione prevista superata appare come ritardo nella scheda. I nomi vengono risolti dal server e conservati nello storico.

La **restituzione** registra data, condizioni finali e note, chiude la consegna e rimuove l'assegnazione personale. È consentita anche se nel frattempo la persona è stata disattivata. Il Gestore sceglie se rendere il bene disponibile oppure metterlo in manutenzione. Sede, reparto e ubicazione rimangono registrati. Le date effettive non possono essere future e la restituzione prevista non può precedere la consegna.

Con **Verifica fisica** si registrano data, condizioni, ubicazione osservata facoltativa e note. La scheda conserva l'ultima verifica e permette di consultare le precedenti. Un'ubicazione osservata diversa da quella attesa viene registrata come differenza: non sposta automaticamente il bene. Le verifiche sono operazioni manuali; per ricevere avvisi periodici occorre configurare una regola **Verifiche inventario**.

Consegna, restituzione e verifica mantengono autore, versione e storico. Un conflitto di versione richiede di ricaricare la scheda prima di confermare nuovamente.

## Etichette QR

**Etichetta QR** apre un'anteprima con codice inventario, nome e QR della scheda. È possibile scaricare il file SVG o stampare soltanto l'etichetta, nel formato 90 × 50 mm. Mantenere il bordo bianco e un buon contrasto quando si ridimensiona o stampa il codice.

La scansione apre la scheda nell'app e richiede l'autenticazione. Il QR contiene l'indirizzo dell'installazione e l'identificativo del bene; non contiene credenziali o dettagli della scheda. Generarlo dall'indirizzo definitivo raggiungibile dai telefoni: cambiando dominio o porta occorre ristampare le etichette. Il codice viene prodotto nel browser con una libreria locale, senza inviare i dati a un servizio esterno.

## Installazioni, rete e licenze

Le relazioni sono registrate manualmente dal Gestore e compaiono sia sulla sorgente sia sulla destinazione:

- Un **software** può essere installato su un dispositivo IT o un apparato di rete. La relazione può registrare una versione installata diversa da quella della scheda software.
- Un **dispositivo IT o apparato di rete** può essere collegato a un altro apparato di rete, indicando le porte sui due lati.
- Una **licenza** può essere assegnata a un dispositivo, a una persona censita oppure a un'identità secondo il modello scelto. Gli assegnatari testuali restano supportati per compatibilità.

Il modello **per dispositivo** richiede una scheda dispositivo IT o apparato di rete. **Per utente** consente una persona, un'identità oppure un assegnatario testuale legacy. **Concorrente** e **sito** consentono anche un dispositivo. Ogni relazione ha esattamente un destinatario; nuove assegnazioni strutturate richiedono persone o identità attive. La quantità della relazione consuma i posti indicati nella licenza: un'identità condivisa da tre persone, con quantità uno, consuma un posto. Il conteggio non moltiplica i posti per gli utilizzatori dell'identità e non verifica i termini del fornitore.

Il server conserva una copia del destinatario al momento dell'assegnazione: rinomina, disattivazione o cambiamenti dei membri dell'identità non riscrivono la relazione e il suo storico. L'assegnazione di una licenza non crea collegamenti diretti ai beni, consegne, associazioni persona-identità o account Punto.

Non si possono duplicare relazioni attive, superare i posti acquistati, assegnare una licenza scaduta o creare collegamenti con schede dismesse o smarrite. La capacità non può scendere sotto i posti già assegnati. Per cambiare software o modello di una licenza occorre prima rimuovere le sue assegnazioni attive.

La rimozione disattiva il collegamento, libera i posti assegnati e conserva l'evento nello storico. Anche le relazioni controllano la versione della scheda sorgente: una modifica concorrente richiede di ricaricare la scheda. L'aggiunta o la rimozione non modifica la revisione della destinazione.

## Importare ed esportare CSV

**Esporta CSV** scarica tutti gli asset corrispondenti ai filtri selezionati, anche oltre la pagina visibile e rispettando **I miei asset**. Il file comprende campi comuni, tipologia, dettagli JSON, identificativo e versione; relazioni, anagrafiche, assegnazioni strutturate, consegne e verifiche non vengono importate dal CSV. L’interfaccia limita ogni esportazione a 20 MiB: per archivi più grandi restringere i filtri.

Il Gestore può usare **Importa CSV**, scaricare il modello e caricare un file UTF-8 con separatore virgola, fino a 256 KiB e 200 righe. I campi obbligatori sono `asset_tag` e `name`; `details` contiene un oggetto JSON dei campi specifici. Le colonne sconosciute o duplicate, i dati non validi e i codici ripetuti nel file vengono segnalati nell’anteprima.

La modalità iniziale crea soltanto nuovi asset. **Crea e aggiorna per codice inventario** può aggiornare quelli esistenti: partire da un’esportazione recente e conservare `version`. Una colonna omessa lascia invariato quel campo; una cella vuota in una colonna presente lo svuota, se consentito. Non rimuovere la versione per forzare un aggiornamento. Per importare licenze, censire prima il software di riferimento: `software_id` deve indicare una scheda software già presente nello stesso inventario.

**Controlla file** mostra le operazioni e gli errori senza modificare le schede. La conferma è disponibile solo con tutte le righe valide e applica l’intero file insieme, conservando lo storico. L’anteprima scade dopo 15 minuti ed è utilizzabile una sola volta dallo stesso Gestore. Un conflitto annulla tutto il lotto e richiede una nuova anteprima.

I valori che potrebbero essere interpretati come formule nei fogli di calcolo vengono esportati con un apostrofo protettivo. La reimportazione rimuove tale prefisso; per un apostrofo letterale prima di `=`, `+`, `-` o `@`, oppure prima di un altro apostrofo, scriverne due nel CSV manuale.

## Avvisi su scadenze, restituzioni e verifiche

In **Regole alert**, il Gestore può creare il controllo **Scadenze inventario**, scegliere un anticipo da 1 a 365 giorni, l’orario giornaliero e i destinatari. Comprende fine garanzia, prossima manutenzione dei beni fisici, scadenza e rinnovo delle licenze, anche se già superati; esclude beni dismessi o smarriti.

Ogni destinatario selezionato riceve gli avvisi del catalogo condiviso. La notifica permette di aprire direttamente l’asset. La stessa combinazione di asset, tipo di scadenza, data e anticipo non viene notificata nuovamente dalla stessa regola; cambiando data o anticipo può essere generato un nuovo avviso. Nessuna regola o destinatario viene creato automaticamente. Il canale email usa la configurazione già prevista per gli altri alert.

**Restituzioni inventario scadute** segnala le consegne ancora aperte dal giorno successivo alla restituzione prevista. **Verifiche inventario** segnala i beni materiali mai verificati e quelli per cui è trascorso l'intervallo configurato, da 1 a 365 giorni, inizialmente 90. Entrambi i controlli escludono dismessi e smarriti. Ripetere il controllo non duplica gli avvisi già inviati; una nuova consegna o una nuova verifica possono produrre nuovi avvisi. La chiusura della consegna oppure la registrazione di una verifica recente tolgono il bene dai risultati successivi del rispettivo controllo, senza cancellare le notifiche precedenti. Queste regole usano il catalogo aziendale completo e non i filtri temporanei del cruscotto.

Se l’inventario è indisponibile, il controllo non viene segnato come completato: il pianificatore può riprovarlo. Gli avvisi non eseguono rinnovi o manutenzioni.

## Ambito e disponibilità

Il catalogo è condiviso nello spazio di lavoro e non applica i ruoli delle singole commesse. Quando il modulo Ticket è configurato, dalla scheda asset è possibile aprire i ticket collegati o crearne uno. Non comprende autodiscovery, scansione della rete, importazioni da agent o collegamenti a commesse, contratti e registrazioni ore. Installazioni, collegamenti, consegne e verifiche non vengono rilevati dai dispositivi. Il numero di licenze disponibili è un conteggio amministrativo delle assegnazioni censite; per le licenze concorrenti non misura le sessioni in uso e non verifica la conformità ai termini del fornitore. Le regole configurate dai Gestori possono generare avvisi sulle scadenze, sulle restituzioni e sulle verifiche; rinnovi e manutenzioni restano operazioni da eseguire manualmente.

L'inventario usa un servizio C e un archivio propri. Se il modulo non è configurato o il servizio è indisponibile, le richieste di inventario mostrano un errore; l'amministratore può verificare il servizio seguendo la [guida tecnica](../tehnical/asset-inventory.md).

## Protezione dei dati

Nell'installazione Compose l'inventario crea automaticamente una copia verificata ogni 24 ore, mantenendo le ultime 14 in un volume distinto dall'archivio. Il primo backup avviene all'avvio iniziale; i riavvii successivi rispettano la prossima scadenza. L'amministratore può modificare frequenza e conservazione e controllare ultima riuscita ed errori nello stato del servizio. Un errore del backup non interrompe la consultazione o il salvataggio delle schede.

Il backup generale MySQL non comprende l'inventario. Le copie locali richiedono anche una conservazione esterna per essere recuperabili in caso di perdita dell'host. La [procedura tecnica](../tehnical/asset-inventory.md#backup-ripristino-e-verifiche) consente di verificare una copia e ripristinarla in un nuovo file, senza sovrascrivere l'archivio attivo.
