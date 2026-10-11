# Documentazione di punto

La documentazione descrive la web app per commesse e registrazione delle ore, i servizi che la eseguono e i processi di lavoro. È organizzata in **`tehnical/`** per chi sviluppa o amministra il sistema e **`prod/`** per chi lo configura e lo utilizza. Il nome della cartella `tehnical` è mantenuto come richiesto dal progetto.

**Riferimento:** codice del workspace all’11 ottobre 2026, schema applicativo 9, 24 tabelle MySQL, 11 servizi Docker (12 con Ticket) e 8 reti. La modalità nativa usa 22 tabelle funzionali SQLite e documenti locali; la modalità Docker usa MySQL e MongoDB GridFS. Il [README principale](../README.md) conserva le istruzioni generali di avvio.

## Percorsi di lettura

| Destinatario | Percorso consigliato |
| --- | --- |
| Operatore | [Panoramica](prod/panoramica.md) → [Guida utente](prod/guida-utente.md) → [Domande frequenti](prod/faq.md) |
| Gestore / project manager | [Guida gestore](prod/guida-gestore.md) → [Processi e regole](prod/processi-e-regole.md) → [Stato e roadmap](prod/roadmap.md) |
| Amministratore di sistema | [Architettura](tehnical/architettura.md) → [Installazione e configurazione](tehnical/installazione-configurazione.md) → [Rete e sicurezza](tehnical/rete-sicurezza.md) → [Operazioni e ripristino](tehnical/operazioni-backup-ripristino.md) |
| Sviluppatore / integratore | [Modello dati](tehnical/modello-dati.md) → [API dell’app](tehnical/api-app.md) → [API degli addon](tehnical/api-addon.md) → [Sviluppo e test](tehnical/sviluppo-test.md) |
| Responsabile di prodotto | [Panoramica](prod/panoramica.md) → [Roadmap aggiornata](prod/roadmap.md) → [Piano delle 20 funzionalità](prod/roadmap-20-funzionalita.md) → [Piano finanziario](prod/piano-modulo-finanziario.md) |

Gli [indici tecnici](tehnical/README.md) e [di prodotto](prod/README.md) elencano tutti i documenti con il loro scopo.

## Approfondimenti per attività

| Attività | Documenti dedicati |
| --- | --- |
| Gestire inventario e assegnazioni dei beni | [Guida inventory](prod/asset-inventory.md), [servizio e API](tehnical/asset-inventory.md) |
| Organizzare attività e impedimenti | [Guida operativa](prod/attivita-kanban-e-blocchi.md), [implementazione e API](tehnical/attivita-kanban-e-blocchi.md), [collaudo dell'11 ottobre](tehnical/verifica-attivita-2026-10-11.md) |
| Preparare il primo utilizzo | [Avvio operativo](prod/avvio-operativo.md), [scenari di accesso](prod/scenari-accessi.md) |
| Interpretare ore e importi | [Esempi di analisi e costi](prod/esempi-analisi-e-costi.md), [regole del prodotto](prod/processi-e-regole.md) |
| Comprendere una parte del codice | [Server C](tehnical/interni-server-c.md), [frontend](tehnical/frontend-e-interfaccia.md), [motore alert](tehnical/motore-alert.md) |
| Cambiare dati o integrazioni | [Migrazioni e consistenza](tehnical/migrazioni-e-consistenza.md), [flussi email e Calendar](tehnical/flussi-integrazioni.md) |
| Uniformare servizi e immagini Docker | [Nomenclatura e migrazione](tehnical/nomi-servizi-e-immagini.md) |
| Preparare un rilascio | [Rilascio e compatibilità](tehnical/rilascio-e-compatibilita.md), [collaudo di accettazione](prod/collaudo-accettazione.md) |
| Diagnosticare un problema | [Diagnostica e osservabilità](tehnical/diagnostica-e-osservabilita.md), [certificati e segreti](tehnical/gestione-certificati-e-segreti.md) |

Gli esempi usano dati fittizi. Le procedure di collaudo descrivono cosa verificare e il risultato atteso; non attestano una nuova esecuzione delle prove. Gli approfondimenti tecnici indicano le fonti nel repository e separano il comportamento attuale dalle indicazioni per sviluppi successivi.

## Disponibilità delle funzioni

| Stato | Significato | Esempi |
| --- | --- | --- |
| Implementato e utilizzabile | Codice, API e interfaccia disponibili, salvo configurazione ordinaria dell’installazione | Commesse, attività/sottoattività, Kanban, blocchi, ore, utenti, gruppi, contratti/PDF, analisi, tariffe, notifiche interne, asset inventory |
| Implementato, provider da configurare | Servizio e API presenti; l’integrazione reale richiede credenziali e configurazione esterna | Invio email SMTP e collegamento Google OAuth |
| Disponibile tramite API | Funzionalità utilizzabile dal bridge applicativo; una schermata dedicata non è ancora presente | Sincronizzazione Google Calendar e gestione eventi |
| Pianificato / parziale | Requisito o progetto di rilascio; eventuali parti esistenti sono indicate nella roadmap | Registro settimanale, approvazioni, finanza completa, più società/valute, collegamento eventi–commesse–ore |
| Evidenza storica | Risultati di verifiche eseguite in una fase precedente, con il relativo perimetro | Collaudo UI dello schema 3 |

I piani descrivono anche tabelle e API future: la loro presenza in un documento di pianificazione non implica che esistano nel software. Lo [schema implementato](tehnical/modello-dati.md) e il [catalogo API corrente](tehnical/api-app.md) identificano le risorse effettive.

## Organizzazione dei file

```text
docs/
├── README.md
├── tehnical/
│   ├── README.md
│   ├── architettura.md
│   ├── nomi-servizi-e-immagini.md
│   ├── interni-server-c.md
│   ├── frontend-e-interfaccia.md
│   ├── installazione-configurazione.md
│   ├── rete-sicurezza.md
│   ├── gestione-certificati-e-segreti.md
│   ├── modello-dati.md
│   ├── migrazioni-e-consistenza.md
│   ├── api-app.md
│   ├── api-addon.md
│   ├── asset-inventory.md
│   ├── flussi-integrazioni.md
│   ├── attivita-kanban-e-blocchi.md
│   ├── motore-alert.md
│   ├── operazioni-backup-ripristino.md
│   ├── diagnostica-e-osservabilita.md
│   ├── rilascio-e-compatibilita.md
│   ├── sviluppo-test.md
│   ├── verifica-attivita-2026-10-11.md
│   └── verifica-ui-2026-10-10.md
├── prod/
│   ├── README.md
│   ├── panoramica.md
│   ├── avvio-operativo.md
│   ├── guida-utente.md
│   ├── guida-gestore.md
│   ├── asset-inventory.md
│   ├── attivita-kanban-e-blocchi.md
│   ├── processi-e-regole.md
│   ├── scenari-accessi.md
│   ├── esempi-analisi-e-costi.md
│   ├── collaudo-accettazione.md
│   ├── faq.md
│   ├── glossario.md
│   ├── roadmap.md
│   ├── roadmap-20-funzionalita.md
│   └── piano-modulo-finanziario.md
├── verifica-ui/       # Schermate del collaudo storico
├── analisi/           # Evidenze delle viste analitiche
├── alert/             # Evidenze delle notifiche
├── attivita/          # Evidenze del modulo attività e blocchi
└── commesse/          # Evidenze del catalogo comune
```

Le immagini esistenti mantengono i propri percorsi. I manuali locali dei servizi restano disponibili in [addon/README.md](../addon/README.md) e [calendar-addon/README.md](../calendar-addon/README.md) e [inventory-addon/README.md](../inventory-addon/README.md).

## Criteri di manutenzione

1. Aggiornare le guide di prodotto quando cambia un flusso, un permesso, un’etichetta o una regola di validazione.
2. Aggiornare modello dati e catalogo API insieme a migrazioni, endpoint e formati. Una modifica allo schema richiede anche l’aggiornamento del riferimento di versione.
3. Aggiornare architettura, configurazione e procedure operative insieme a servizi, reti, volumi, certificati o dipendenze.
4. Riportare i test come risultati datati, indicando cosa è stato eseguito, cosa è opzionale e cosa usa provider simulati. Un risultato storico non certifica un nuovo ambiente.
5. Usare esempi sintetici e nomi segnaposto. Credenziali, cookie reali, file `.env.docker`, chiavi private e dati degli utenti non appartengono alla documentazione.
6. Verificare i link relativi dopo uno spostamento. I riferimenti al sorgente sono relativi alla radice del repository, senza percorsi personali del computer.

Le guide correnti derivano dal codice e dalla configurazione versionabile. Le scelte ancora da decidere rimangono nella roadmap; non vengono trasformate in funzionalità o garanzie già disponibili.
