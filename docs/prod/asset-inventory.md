# Inventario degli asset

[Indice prodotto](README.md) · [Guida tecnica](../tehnical/asset-inventory.md)

L'Inventario raccoglie dispositivi IT, apparati di rete, beni fisici, software e licenze. Conserva collocazione, assegnazione, stato, acquisto e garanzia, con campi specifici e collegamenti tra schede. È disponibile agli utenti autenticati: tutti possono consultare catalogo, relazioni e storico; solo il **Gestore** può aggiungere o modificare schede, registrare relazioni e rimuoverle.

## Consultare i beni

Aprire la scheda **Asset inventory**. La ricerca e i filtri per stato e tipologia restringono il catalogo; la ricerca comprende i dettagli specifici, per esempio hostname, editore e riferimento della licenza. La paginazione permette di leggere altri beni senza caricare tutto l'archivio. Il riepilogo mostra i conteggi globali, anche quando l'elenco è filtrato. Aprire un bene per leggere scheda, relazioni e storico delle revisioni.

| Stato | Significato operativo |
| --- | --- |
| Disponibile | Bene pronto per essere assegnato. |
| Assegnato | Bene affidato alla persona o alla funzione indicata nell'assegnatario. |
| In manutenzione | Bene temporaneamente indisponibile per manutenzione. |
| Dismesso | Bene ritirato dall'uso, mantenuto nell'archivio. |
| Smarrito | Bene non più reperibile. |

Gli stati sono aggiornati dal Gestore; l'app non li deduce automaticamente da date, contratti o altri dati.

## Creare e aggiornare una scheda

Con **Nuovo asset** il Gestore inserisce un'etichetta inventario univoca e un nome. L'etichetta distingue il bene anche quando altri beni hanno lo stesso modello. La categoria è libera e parte da `hardware`; produttore, modello, matricola, posizione e note descrivono il bene. L'assegnatario è un testo libero, non un collegamento automatico a un utente dell'app, ed è obbligatorio per lo stato **Assegnato**.

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

## Installazioni, rete e licenze

Le relazioni sono registrate manualmente dal Gestore e compaiono sia sulla sorgente sia sulla destinazione:

- Un **software** può essere installato su un dispositivo IT o un apparato di rete. La relazione può registrare una versione installata diversa da quella della scheda software.
- Un **dispositivo IT o apparato di rete** può essere collegato a un altro apparato di rete, indicando le porte sui due lati.
- Una **licenza** può essere assegnata a un dispositivo o a una persona secondo il modello scelto. La persona è un testo libero e non un account dell'app.

Il modello **per dispositivo** richiede una scheda dispositivo IT o apparato di rete. **Per utente** richiede un assegnatario testuale. **Concorrente** e **sito** consentono di registrare un dispositivo oppure un assegnatario, uno alla volta. La quantità della relazione consuma i posti indicati nella licenza: i conteggi mostrano posti assegnati e residui sulla base di queste registrazioni.

Non si possono duplicare relazioni attive, superare i posti acquistati, assegnare una licenza scaduta o creare collegamenti con schede dismesse o smarrite. La capacità non può scendere sotto i posti già assegnati. Per cambiare software o modello di una licenza occorre prima rimuovere le sue assegnazioni attive.

La rimozione disattiva il collegamento, libera i posti assegnati e conserva l'evento nello storico. Anche le relazioni controllano la versione della scheda sorgente: una modifica concorrente richiede di ricaricare la scheda. L'aggiunta o la rimozione non modifica la revisione della destinazione.

## Importare ed esportare CSV

**Esporta CSV** scarica tutti gli asset corrispondenti ai filtri selezionati, anche oltre la pagina visibile. Il file comprende campi comuni, tipologia, dettagli JSON, identificativo e versione; le relazioni restano nello storico e non vengono importate dal CSV. L’interfaccia limita ogni esportazione a 20 MiB: per archivi più grandi restringere i filtri.

Il Gestore può usare **Importa CSV**, scaricare il modello e caricare un file UTF-8 con separatore virgola, fino a 256 KiB e 200 righe. I campi obbligatori sono `asset_tag` e `name`; `details` contiene un oggetto JSON dei campi specifici. Le colonne sconosciute o duplicate, i dati non validi e i codici ripetuti nel file vengono segnalati nell’anteprima.

La modalità iniziale crea soltanto nuovi asset. **Crea e aggiorna per codice inventario** può aggiornare quelli esistenti: partire da un’esportazione recente e conservare `version`. Una colonna omessa lascia invariato quel campo; una cella vuota in una colonna presente lo svuota, se consentito. Non rimuovere la versione per forzare un aggiornamento.

**Controlla file** mostra le operazioni e gli errori senza modificare le schede. La conferma è disponibile solo con tutte le righe valide e applica l’intero file insieme, conservando lo storico. L’anteprima scade dopo 15 minuti ed è utilizzabile una sola volta dallo stesso Gestore. Un conflitto annulla tutto il lotto e richiede una nuova anteprima.

I valori che potrebbero essere interpretati come formule nei fogli di calcolo vengono esportati con un apostrofo protettivo. La reimportazione rimuove tale prefisso; per un apostrofo letterale prima di `=`, `+`, `-` o `@`, oppure prima di un altro apostrofo, scriverne due nel CSV manuale.

## Avvisi sulle scadenze

In **Regole alert**, il Gestore può creare il controllo **Scadenze inventario**, scegliere un anticipo da 1 a 365 giorni, l’orario giornaliero e i destinatari. Comprende fine garanzia, prossima manutenzione dei beni fisici, scadenza e rinnovo delle licenze, anche se già superati; esclude beni dismessi o smarriti.

Ogni destinatario selezionato riceve gli avvisi del catalogo condiviso. La notifica permette di aprire direttamente l’asset. La stessa combinazione di asset, tipo di scadenza, data e anticipo non viene notificata nuovamente dalla stessa regola; cambiando data o anticipo può essere generato un nuovo avviso. Nessuna regola o destinatario viene creato automaticamente. Il canale email usa la configurazione già prevista per gli altri alert.

Se l’inventario è indisponibile, il controllo non viene segnato come completato: il pianificatore può riprovarlo. Gli avvisi non eseguono rinnovi o manutenzioni.

## Ambito e disponibilità

Il catalogo è condiviso nello spazio di lavoro e non applica i ruoli delle singole commesse. Non comprende autodiscovery, scansione della rete, importazioni da agent, prestiti automatici o collegamenti a commesse, contratti e registrazioni ore. Installazioni, collegamenti e assegnazioni non vengono rilevati dai dispositivi. Il numero di licenze disponibili è un conteggio amministrativo delle assegnazioni censite; per le licenze concorrenti non misura le sessioni in uso e non verifica la conformità ai termini del fornitore. Le regole configurate dai Gestori possono generare avvisi sulle scadenze; rinnovi e manutenzioni restano operazioni da eseguire manualmente.

L'inventario usa un servizio C e un archivio propri. Se il modulo non è configurato o il servizio è indisponibile, le richieste di inventario mostrano un errore; l'amministratore può verificare il servizio seguendo la [guida tecnica](../tehnical/asset-inventory.md).

## Protezione dei dati

Nell'installazione Compose l'inventario crea automaticamente una copia verificata ogni 24 ore, mantenendo le ultime 14 in un volume distinto dall'archivio. Il primo backup avviene all'avvio iniziale; i riavvii successivi rispettano la prossima scadenza. L'amministratore può modificare frequenza e conservazione e controllare ultima riuscita ed errori nello stato del servizio. Un errore del backup non interrompe la consultazione o il salvataggio delle schede.

Il backup generale MySQL non comprende l'inventario. Le copie locali richiedono anche una conservazione esterna per essere recuperabili in caso di perdita dell'host. La [procedura tecnica](../tehnical/asset-inventory.md#backup-ripristino-e-verifiche) consente di verificare una copia e ripristinarla in un nuovo file, senza sovrascrivere l'archivio attivo.
