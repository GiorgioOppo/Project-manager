# Documentazione tecnica

**Italiano** | [English](../en/tehnical/README.md)

[Indice generale](../README.md) · [Documentazione di prodotto](../prod/README.md) · [README del progetto](../../README.it.md)

Queste guide descrivono il sistema implementato all’**11 ottobre 2026**: server C, interfaccia HTML/CSS/JavaScript, schema applicativo 10, deployment Docker con tre membri MySQL e tre addon C. Le configurazioni native e Docker sono ambienti distinti.

## Guide

| Documento | Contenuto |
| --- | --- |
| [Architettura](architettura.md) | Componenti, responsabilità, flussi, persistenza, transazioni, disponibilità e limiti |
| [Nomenclatura Docker](nomi-servizi-e-immagini.md) | Nomi servizi e immagini, alias TLS, tag locali e passaggio dai container precedenti |
| [Migrazione container dell’11 ottobre 2026](migrazione-container-2026-10-11.md) | Rapporto operativo: backup, conservazione dei dati, recupero del cluster e verifiche del deployment |
| [Installazione e configurazione](installazione-configurazione.md) | Requisiti, primo avvio, modalità native, variabili, certificati e attivazione dei provider |
| [Keycloak e autenticazione multi app](keycloak.md) | Realm condiviso, client Punto e Ticket, modalità di accesso, collegamento utenti, segreti e deployment |
| [Reverse proxy Ticket](ticket-reverse-proxy.md) | Pubblicazione di Ticket tramite Nginx Proxy Manager, hostname, inoltro e verifiche |
| [Rete e sicurezza](rete-sicurezza.md) | Otto reti, indirizzi e porte, Squid, TLS, autenticazione, autorizzazioni, segreti e cifratura effettiva |
| [Modello dati](modello-dati.md) | Database, dizionario delle tabelle, tipi, relazioni, vincoli, indici, migrazioni e archivi degli addon |
| [API dell’app](api-app.md) | Endpoint pubblici, sessione, controlli di origine, permessi, parametri, risposte, errori e limiti |
| [Asset inventory](asset-inventory.md) | Servizio C separato, API, permessi, rete inventory, persistenza, avvio e backup |
| [API degli addon](api-addon.md) | Contratti HTTPS privati per email e Calendar, autenticazione, code, idempotenza, OAuth e sincronizzazione |
| [Operazioni, backup e ripristino](operazioni-backup-ripristino.md) | Monitoraggio, aggiornamenti, recupero del cluster, backup coerenti, recupero dei dati e diagnosi |
| [Sviluppo e test](sviluppo-test.md) | Struttura del codice, build, verifiche native, suite opzionali, criteri di modifica e risultati registrati |
| [Verifica attività dell'11 ottobre 2026](verifica-attivita-2026-10-11.md) | Suite, integrità MySQL, browser desktop/mobile, schermate e stato della distribuzione del modulo schema 8 |
| [Verifica UI del 10 ottobre 2026](verifica-ui-2026-10-10.md) | Rapporto storico della fase schema 3, dati sintetici, schermate e limiti del collaudo |

## Approfondimenti sull’implementazione

| Documento | Contenuto |
| --- | --- |
| [Interni del server C](interni-server-c.md) | Avvio, moduli, HTTP, gestione delle risorse, errori e confini di concorrenza |
| [Frontend e interfaccia](frontend-e-interfaccia.md) | Stato della pagina, navigazione, richieste concorrenti, moduli, rendering e accessibilità |
| [Migrazioni e consistenza](migrazioni-e-consistenza.md) | Evoluzione dello schema, transazioni, differenze fra backend e gestione degli esiti incerti |
| [Attività, Kanban e blocchi](attivita-kanban-e-blocchi.md) | Schema 8, API, gerarchie, revisioni, permessi e cancellazioni |
| [Motore alert](motore-alert.md) | Orari, cursori, deduplicazione, soggetti, destinatari, ripartenza e semantica dell’outbox |
| [Flussi delle integrazioni](flussi-integrazioni.md) | Sequenze email e OAuth, sincronizzazione, conflitti e consistenza con i provider |

## Amministrazione e manutenzione

| Documento | Contenuto |
| --- | --- |
| [Diagnostica e osservabilità](diagnostica-e-osservabilita.md) | Lettura dei segnali disponibili e percorsi di diagnosi per sintomo |
| [Rilascio e compatibilità](rilascio-e-compatibilita.md) | Preparazione del rilascio, componenti, schema, verifiche e limiti del ritorno a una versione precedente |
| [Certificati e segreti](gestione-certificati-e-segreti.md) | Inventario dei materiali, consumatori, verifiche, scadenze e vincoli di rotazione |

## Come usarle

Per una nuova installazione partire dall’architettura e dalla configurazione. Per modificare il software leggere il modello dati e le API prima delle istruzioni di sviluppo. Per intervenire su un’installazione esistente usare le procedure operative e verificare il perimetro dell’azione: **primo seed, aggiornamento, recupero del quorum e ripristino da backup sono operazioni differenti**.

Le pagine indicano i file sorgente da consultare per confermare i dettagli. I manuali specifici di [email](../../addon/README.it.md) e [Calendar](../../calendar-addon/README.it.md) e [Asset Inventory](../../inventory-addon/README.it.md) restano accanto ai rispettivi servizi.

La [roadmap di prodotto](../prod/roadmap.md) raccoglie i requisiti futuri. In particolare, il progetto di finanza completa, l’isolamento fra società e la cifratura dei campi business non descrivono componenti già implementati.
