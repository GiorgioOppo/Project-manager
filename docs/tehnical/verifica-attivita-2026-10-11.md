# Verifica di attività, Kanban e blocchi — 11 ottobre 2026

**Italiano** | [English](../en/tehnical/verifica-attivita-2026-10-11.md)

Questo verbale riguarda il primo modulo **Attività e blocchi**, con schema applicativo **8**. Le prove usano utenti, commesse e contenuti sintetici, database temporanei e un browser isolato. Non attestano un aggiornamento del database operativo.

## Compilazione e suite

| Verifica eseguita | Risultato |
| --- | --- |
| Compilazione nativa SQLite, C11 e warning come errori | Superata |
| `make test` con ambiente di fixture, senza abilitare le prove sul deployment | 292 casi: **263 superati, 29 saltati, 0 falliti**; durata 218,558 s |
| Suite `test_work_board.py`, ripetuta dopo l'unificazione delle risposte 404 | **20/20 superati**; i 20 casi sono già inclusi nel totale precedente |
| Build Docker dell'app con backend MySQL | Superata; immagine candidata locale `punto:work-board-schema8` |
| Sintassi JavaScript dell'interfaccia | Superata |

La suite dedicata verifica accessi diretti, gruppi e commesse comuni; revoche e account disabilitati; assegnatari; gerarchie e profondità; modifiche concorrenti; validazione e Unicode; limiti HTTP; audit atomico; eliminazioni; migrazione 7→8. Include l'uguaglianza delle risposte per elementi inesistenti e non visibili, per evitare la distinzione fra i due casi tramite ID.

I 29 casi saltati richiedono opzioni, dipendenze o ambienti aggiuntivi. Non sono conteggiati come prove superate. Il risultato della suite descrive il codice di questo modulo al momento del collaudo; modifiche successive o altri lavori nello stesso workspace richiedono le proprie verifiche.

## Integrità su MySQL

Un container temporaneo **MySQL 8.4.11**, senza rete, porte pubblicate o volumi persistenti, ha eseguito tutte le **24 istruzioni CREATE TABLE** estratte da `src/mysql_schema.c`. Il container è stato rimosso al termine.

Sono stati verificati:

- Rifiuto di collegamenti attività–genitore e blocco–attività fra commesse diverse, anche in aggiornamento.
- Vincoli verso gli utenti, stati consentiti e revisione positiva.
- Campi Unicode ai limiti previsti: titolo 160, descrizione 2000, prossima azione 1000 caratteri.
- Rifiuto delle cancellazioni incompatibili con le relazioni; riuscita della pulizia esplicita nell'ordine previsto.
- Rollback delle modifiche e conservazione di una commessa estranea alla prova.

Questa verifica riguarda **DDL e integrità relazionale**. Non è una prova HTTP dell'app collegata al cluster MySQL e non ripete le simulazioni di guasto HA.

## Interfaccia e autorizzazioni

Il collaudo con Chrome/Playwright ha usato i file reali di `public/` e il server C con SQLite temporaneo. Ulteriori prove con risposte API simulate hanno verificato gli stati di errore e il rendering di contenuti ostili come testo.

Durante la verifica finale erano in corso modifiche parallele ai moduli inventario e ticket. Per non includere codice incompleto, l'ultima prova HTTP e l'immagine candidata usano gli asset della precedente build del modulo, aggiornati con il blocco `work*` finale, verificato identico a quello nel workspace. La suite UI con API simulate è passata sia su questa copia stabile sia sul workspace dopo l'arrivo delle definizioni dell'inventario. Questo non estende il collaudo funzionale ai moduli paralleli.

| Scenario | Esito osservato |
| --- | --- |
| Gestore | Creazione di attività assegnata e sottoattività; cinque colonne Kanban e vista gerarchica |
| Collaboratore | Cambio di stato della propria attività; segnalazione di un blocco con autore e responsabile derivati dal server |
| Lettore | Consultazione senza pulsanti di scrittura o cambio stato |
| Concorrenza | Modifica tramite API mentre il modulo è aperto: 409, salvataggio bloccato e ricaricamento esplicito |
| Revoca | Revoca durante una modifica: 404, chiusura del modulo e rimozione dei dati della bacheca, della bozza e dei testi contestuali nei dialog |
| Focus | Ritorno alla scheda dopo salvataggio; destinazione accessibile dopo eliminazione o filtro; focus sull'errore di conflitto e dopo la ricarica |
| Layout | Desktop 1440 px e mobile 390/320 px; nessuno scorrimento orizzontale dell'intera pagina; colonne nel proprio riquadro scorrevole |
| JavaScript | Nessun errore non gestito durante il percorso |

Le schermate mostrano esclusivamente dati dimostrativi:

- [Bacheca desktop](../attivita/desktop-2026-10-11.png).
- [Bacheca e blocchi su smartphone](../attivita/mobile-2026-10-11.png).

La verifica del layout mobile ha incluso schermate della viewport e una nuova sessione aperta direttamente alla larghezza mobile, oltre al controllo delle dimensioni DOM.

## Stato della distribuzione e limiti

È stata compilata un'immagine candidata separata, `punto:work-board-schema8`, con ID locale `sha256:d1c685c8f5eb9a6a63b5a48c93e468a7e91864c240ed59aa4991b8f0e84d51d1`. Include il modulo Attività con le correzioni finali, senza i moduli paralleli ancora in lavorazione. I container applicativi esistenti sono rimasti arrestati e il database operativo non è stato migrato allo schema 8 durante questa attività. La migrazione viene eseguita all'avvio della nuova versione secondo la [procedura di rilascio](rilascio-e-compatibilita.md), dopo il backup previsto.

Le ore restano attribuite alla commessa. Questo modulo non introduce il collegamento ore–attività, le dipendenze fra attività, il Gantt, il calcolo della capacità, probabilità dei rischi o notifiche automatiche delle assegnazioni. Le integrazioni SMTP e Google reali non fanno parte di questo collaudo.

Riferimenti: [guida di prodotto](../prod/attivita-kanban-e-blocchi.md), [implementazione e API](attivita-kanban-e-blocchi.md), [sviluppo e test](sviluppo-test.md).
