# Documentazione di prodotto

**Italiano** | [English](../en/prod/README.md)

[Indice generale](../README.md) · [Documentazione tecnica](../tehnical/README.md) · [README del progetto](../../README.it.md)

Queste guide spiegano come utilizzare punto, come organizzare l’accesso alle commesse, attività e blocchi, e come interpretare ore, analisi e notifiche. Il riferimento è il prodotto implementato al **11 ottobre 2026**, schema **10**. I piani di sviluppo sono riconoscibili come tali.

## Guide operative

| Documento | A chi serve | Contenuto |
| --- | --- | --- |
| [Panoramica](panoramica.md) | Tutti | Scopo, persone coinvolte, moduli disponibili e stato delle integrazioni |
| [Guida utente](guida-utente.md) | Operatori | Accesso, commesse visibili, registrazione giornaliera, storico personale, analisi e notifiche |
| [Asset inventory](asset-inventory.md) | Tutti e Gestori | Catalogo beni, stati, assegnazioni, garanzie e storico; scrittura riservata ai Gestori |
| [Attività, Kanban e blocchi](attivita-kanban-e-blocchi.md) | Gestori e collaboratori | Bacheca, sottoattività, responsabili, impedimenti, revisioni e permessi |
| [Guida gestore](guida-gestore.md) | Gestori e project manager | Utenti, gruppi, ruoli, contratti, PDF, commesse, tariffe, controlli e audit |
| [Processi e regole](processi-e-regole.md) | Gestori, utenti e supporto | Flussi completi, criteri di accesso, date, stati, durate, importi e comportamento degli alert |
| [Domande frequenti](faq.md) | Tutti | Problemi comuni e spiegazione dei comportamenti meno immediati |
| [Glossario](glossario.md) | Tutti | Termini utilizzati nell’app e nella documentazione |

## Scenari ed esempi

| Documento | A chi serve | Contenuto |
| --- | --- | --- |
| [Avvio operativo](avvio-operativo.md) | Gestori e persone incaricate della configurazione | Percorso di primo utilizzo, dall’organizzazione alle prime ore e verifiche |
| [Scenari di accesso](scenari-accessi.md) | Gestori e supporto | Esempi di gruppi, ruoli, date, revoche, commesse comuni e visibilità |
| [Esempi di analisi e costi](esempi-analisi-e-costi.md) | Project manager | Dataset fittizio, cinque viste, calcoli verificabili e interpretazione dei risultati |
| [Collaudo di accettazione](collaudo-accettazione.md) | Chi prepara e verifica un rilascio | Casi manuali, precondizioni, azioni e risultati attesi; non un verbale di prove già eseguite |

## Pianificazione

| Documento | Stato e uso |
| --- | --- |
| [Stato e roadmap](roadmap.md) | Confronto aggiornato fra capacità attuali, parti già realizzate e prossimi rilasci |
| [Piano delle 20 funzionalità](roadmap-20-funzionalita.md) | Progetto dettagliato originariamente basato sullo schema 3, conservato con dipendenze e criteri di accettazione |
| [Piano del modulo finanziario](piano-modulo-finanziario.md) | Progetto per più società/paesi/valute, fatturazione e incassi; il modulo completo resta da implementare |

Per il lavoro quotidiano usare le guide operative. Per decidere le priorità usare la roadmap aggiornata; i due piani dettagliati contengono anche nomi di schermate, entità e API ancora proposti.

## Confini del prodotto attuale

Il profilo **Gestore** amministra lo spazio di lavoro; il profilo **Operatore** usa le commesse a cui è abilitato. I ruoli sulle singole commesse regolano consultazione e registrazione delle ore, aggiornamento delle attività assegnate e gestione dei propri blocchi. Il Gestore organizza le attività; il Lettore può consultare la bacheca. Il registro dettagliato rimane personale anche per i gestori; le analisi consentono ai gestori di consultare aggregati di più utenti.

Le pagine economiche disponibili trattano valori contrattuali e costo stimato delle ore in **EUR**. Le notifiche interne sono operative. L’invio email richiede la configurazione SMTP; la sincronizzazione Google è esposta tramite API e richiede OAuth. La schermata Calendario e l’associazione degli eventi a commesse e ore sono sviluppi successivi.
