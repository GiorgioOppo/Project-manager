# Esempi di analisi del lavoro e dei costi

**Italiano** | [English](../en/prod/esempi-analisi-e-costi.md)

Il dataset seguente è interamente sintetico. Serve per capire e verificare le cinque viste di **Analisi**; non descrive dati presenti nell’installazione e non è stato inserito durante la redazione. Gli importi sono in EUR e i totali sono stati verificati con calcoli interi, applicando l’arrotondamento della singola registrazione usato dal server.

## 1. Dati di partenza

Usa un laboratorio senza altre registrazioni nelle date scelte, oppure filtri che isolino questo dataset. Il periodo dell’esempio è **12–13 ottobre 2026**, con estremi compresi. Le date future sono ammesse dal prodotto, ma nell’uso reale vanno seguite le regole organizzative sul consuntivo.

### Persone

| Persona | Profilo | Condizione dell’esempio |
| --- | --- | --- |
| Marta | Gestore + Commerciali + Finance | Prepara gli accordi e le tariffe; registra le proprie ore e consulta gli aggregati |
| Elena | Operatore | Collaboratore sulle commesse A e B |
| Lucia | Operatore | Collaboratore sulle commesse C ed E |
| Davide | Operatore | Collaboratore sulla commessa D |

Le abilitazioni coprono sia la data corrente del test sia le date del lavoro. Ogni persona inserisce le proprie righe dalla propria sessione; Marta non può attribuire direttamente ore ai colleghi dal modulo personale.

Assegna esplicitamente Commerciali e Finance a Marta in **Utenti → Profili** e inserisci tutte e quattro le persone in uno stesso gruppo organizzativo. La squadra rende visibili i report del lavoro; i profili separati consentono le operazioni commerciali ed economiche. Le righe personali riportate sotto spiegano il dataset di laboratorio: Finance riceve soltanto aggregati per commessa.

### Contratti e commesse

Censisci un fornitore fittizio attivo e tre contratti. Usa codici con prefisso `ANA-` per distinguerli dai casi di altre guide.

| Contratto | Valore EUR | Significato |
| --- | ---: | --- |
| ANA-C10, Accordo principale | 10.000,00 | Importo noto |
| ANA-CND, Accordo da valorizzare | Non compilato | Valore sconosciuto, distinto da zero |
| ANA-C00, Accordo senza corrispettivo | 0,00 | Zero esplicito |

| Commessa | Contratto corrente | Nome di esempio |
| --- | --- | --- |
| A, codice ANA-A | ANA-C10 | Portale |
| B, codice ANA-B | ANA-C10 | Supporto portale |
| C, codice ANA-C | ANA-CND | Analisi preliminare |
| D, codice ANA-D | Nessuno | Attività interna |
| E, codice ANA-E | ANA-C00 | Affiancamento |

A e B condividono lo stesso contratto: questo permette di verificare che il valore non venga contato due volte. Il campo del valore di ANA-CND va lasciato vuoto, non compilato con `0`.

### Tariffe

| Persona | Valida dal | Valida al | Tariffa EUR/ora |
| --- | --- | --- | ---: |
| Elena | 2026-10-12 | 2026-10-12 | 60,00 |
| Elena | 2026-10-13 | Senza fine | 66,00 |
| Marta | 2026-10-12 | Senza fine | 90,00 |
| Lucia | 2026-10-12 | Senza fine | 0,00 |
| Davide | Nessuna tariffa | — | Non disponibile |

Gli intervalli di Elena sono consecutivi senza sovrapporsi. La tariffa di Lucia valorizza esplicitamente il tempo a zero; l’assenza di tariffa di Davide lascia invece il costo sconosciuto.

### Registrazioni

Le sigle R1–R10 sono riferimenti di questo documento, non gli identificativi generati dall’app. Le note possono essere descrittive e non incidono sui calcoli.

| Riga | Giornata | Commessa | Minuti | Registrazioni | Minuti con tariffa | Minuti senza tariffa | Costo stimato |
| --- | ---: | ---: | ---: | ---: | ---: |
| A | 240 | 3 | 240 | 0 | 264,00 EUR |
| B | 150 | 3 | 150 | 0 | 183,00 EUR |
| C | 60 | 1 | 60 | 0 | 0,00 EUR |
| D | 60 | 2 | 0 | 60 | Non disponibile |
| E | 30 | 1 | 30 | 0 | 0,00 EUR |
| **5 commesse** | **540** | **10** | **480** | **60** | **447,00 EUR, stima parziale** |

Esempio di calcolo per Elena su A: 120 minuti del 12 ottobre a 60 EUR/ora valgono 120 EUR; 90 minuti del 13 ottobre a 66 EUR/ora valgono 99 EUR. La quota di Elena su A vale 219 EUR; con i 45 EUR di Marta, la riga della commessa A vale 264 EUR. Non si applica la tariffa corrente di 66 EUR a tutte le cinque ore della persona.

I totali per giorno del costo noto sono 225 EUR il 12 ottobre e 222 EUR il 13 ottobre. Per persona sono Elena 312 EUR, Marta 135 EUR e Lucia zero. Il costo di Davide rimane sconosciuto: **447 EUR non è il costo completo delle nove ore**.

Lo zero della tariffa di Lucia e quello del contratto ANA-C00 sono due informazioni indipendenti. Lucia ha costo noto zero anche su C, il cui contratto è senza valore. Non si deduce il costo orario dal valore del contratto.

## 7. Arrotondamento: un caso separato

Questo caso non fa parte delle dieci righe precedenti. Preparalo su una commessa e un intervallo separati per non alterare i totali di riferimento.

Con tariffa **1 EUR/ora**, una registrazione da un minuto vale 100/60 centesimi, arrotondati a **2 centesimi**. L’arrotondamento avviene per ogni riga, prima della somma:

| Registrazioni | Minuti totali | Costo risultante |
| --- | ---: | ---: |
| Tre righe da 1 minuto | 3 | 0,02 + 0,02 + 0,02 = **0,06 EUR** |
| Una riga da 3 minuti | 3 | **0,05 EUR** |

Quando il valore è esattamente a metà centesimo, si arrotonda verso l’alto: un minuto a **0,30 EUR/ora** vale 0,5 centesimi e diventa **0,01 EUR**. L’aggregazione per commessa non ripete l’arrotondamento sulla somma grezza dei minuti.

Questa regola serve a riprodurre il risultato, non a scegliere artificiosamente quante righe inserire. Le registrazioni devono rappresentare il lavoro svolto secondo le convenzioni adottate.

## 8. Cosa cambia modificando i dati

Esegui queste variazioni solo su una copia di laboratorio del dataset. Sono scenari indipendenti: riparti dalla base prima di ognuno.

### Cambio della tariffa passata

Modifica la tariffa di Elena valida il 12 ottobre da 60 a **72 EUR/ora**. Le tre ore di quel giorno aumentano di 36 EUR; il giorno 13 mantiene 66 EUR/ora. Il costo di Elena passa da 312 a **348 EUR** e il totale noto da 447 a **483 EUR**. I minuti restano 540, la copertura resta 480 minuti e le righe ore non vengono riscritte.

Questo dimostra che la stima corrente non è un consuntivo economico congelato. Il futuro processo finanziario dovrà salvare snapshot approvati separati.

### Nuovo contratto collegato a B

Crea un quarto contratto ANA-C25 con valore **2.500 EUR** e collega B a questo contratto, conservando le ore. La vista corrente del periodo mostrerà:

| Contratto | Ore collegate | Valore noto |
| --- | --- | ---: |
| ANA-C10, ora soltanto A | 4 h 00 min | 10.000,00 EUR |
| ANA-C25, B | 2 h 30 min | 2.500,00 EUR |
| ANA-CND, C | 1 h 00 min | Non definito |
| ANA-C00, E | 0 h 30 min | 0,00 EUR |

Il valore noto diventa **12.500 EUR**, con quattro contratti distinti e un valore mancante. D resta senza contratto per 60 minuti. Il report usa il collegamento corrente: non conserva il precedente legame di B per le ore del 12–13 ottobre. Il costo delle ore rimane 447 EUR nella base tariffaria originaria.

### Nessuna ora nel periodo

Seleziona una data senza registrazioni, per esempio il 14 ottobre nel laboratorio appena preparato. Le viste non devono inventare righe per utenti, commesse o contratti non movimentati. I conteggi del lavoro sono zero. Una lista di filtri ancora popolata non significa che esistano ore nel periodo: serve a scegliere altri dati.

### Commessa senza tariffa

Filtra la commessa D nella vista Costo delle ore. Il report contiene 60 minuti e due registrazioni, tutte senza tariffa, con stima **non disponibile**. Il valore numerico interno del totale noto può essere zero, ma l’interfaccia lo interpreta insieme alla copertura: non va letto come costo effettivo pari a zero.

## 9. Verifiche di coerenza

| Controllo | Risultato della base |
| --- | --- |
| Somma dei minuti delle dieci righe | 540 |
| Somma per giorno, commessa e persona | Sempre 540 |
| Conteggio righe | 10 |
| Minuti valorizzati + non valorizzati | 480 + 60 = 540 |
| Costo noto per giorno | 225 + 222 = 447 EUR |
| Costo noto per persona | 312 + 135 + 0 = 447 EUR, Davide escluso perché sconosciuto |
| Minuti con contratto + senza contratto | 390 + 60 + 30 + 60 = 540 |
| Valore noto dei contratti distinti | 10.000 + 0 = 10.000 EUR; ANA-CND rimane sconosciuto |

I comportamenti sono riscontrabili nei casi di `tests/test_reports.py` e `tests/test_labor_rates.py` e nelle query di `src/workspace.c`. Le regole generali sono in [Processi e regole](processi-e-regole.md); per una sessione di prova strutturata usa il [collaudo di accettazione](collaudo-accettazione.md).
