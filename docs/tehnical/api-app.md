# API dell'applicazione

**Italiano** | [English](../en/tehnical/api-app.md)

Contratto verificato sul codice l’**11 ottobre 2026**, schema dati 13. Le API usano il prefisso `/api` sull'origine pubblica dell'app, per esempio `https://progetti.tuaazienda.it`. I nomi e i metodi sotto sono quelli implementati: l'aggiornamento usa `POST`, non `PUT` o `PATCH`.

Fonti: [router HTTP](../../src/server.c), [utenti](../../src/accounts.c), [commesse, ore, report, tariffe](../../src/workspace.c), [organizzazione](../../src/organization.c), [attività e blocchi](../../src/work_board.c), [fornitori e contratti](../../src/procurement.c), [alert](../../src/alerts.c), [audit](../../src/storage.c), [bridge calendario](../../src/calendar_client.c). Tipi persistenti nel [modello dati](modello-dati.md); chiamate interne in [API addon](api-addon.md).

## Policy corrente dello schema 13

Questa sezione ha precedenza sulle descrizioni di accesso delle tabelle preesistenti sottostanti. Il server risolve da database ruolo, profili e squadra a ogni richiesta, anche con sessioni aperte: conoscere un ID o conservare un URL non concede accesso.

- `operator` è implicito per ogni account attivo; `manager` deriva dal ruolo dell’identità. `sales` e `finance` sono assegnazioni locali esplicite, anche per Keycloak. Nessun manager le riceve automaticamente.
- Fornitori, contratti, metadati PDF e download richiedono Sales oppure Finance. Creazione/modifica e caricamento PDF richiedono Sales. Finance è in sola lettura; il solo ruolo manager riceve `403`. Cambiare il contratto di una commessa richiede manager + Sales; i campi contrattuali sono omessi dalle commesse per chi non può leggerli.
- `view=date|job|user` usa scope `personal` o `team`: sé stesso, membri dei gruppi organizzativi del manager e sottogruppi. Senza gruppi o con gruppi gestiti indisponibili rimane soltanto sé stesso. Gli utenti fuori ambito non sono ammessi nei filtri. Registro dettagliato, settimana, riepiloghi, rettifiche, audit e alert sulle ore rispettano lo stesso ambito corrente.
- `view=cost|labor_cost` richiede Finance e restituisce scope `financial`; `user_id` viene rifiutato con `403`. `labor_cost` raggruppa per commessa, senza `user_id`, nome o email dei lavoratori nelle righe. Finance non concede il registro personale altrui. Lettura tariffe: Finance; scrittura: manager + Finance.
- Le notifiche sulle ore sono filtrate prima di elenco e conteggio non lette; originali/proposte delle rettifiche e decisioni restano nella squadra. Le email sulle ore richiedono sia il soggetto nella squadra del proprietario sia un destinatario corrispondente a un account attivo autorizzato. La revoca vale anche per payload già salvati; inventory conserva il proprio ambito del catalogo condiviso.

| Metodo e percorso | Contratto corrente |
| --- | --- |
| `GET /api/access-groups` | Manager; catalogo dei quattro profili predefiniti. |
| `GET /api/users/{id}/access-groups` | Manager; `user_id`, `active`, `role`, `selected` espliciti ed `effective` derivati. |
| `POST /api/users/{id}/access-groups` | Manager; unico campo form `groups`, array JSON di `sales` e/o `finance`; `[]` revoca entrambi. Sostituzione atomica e audit; funziona anche con identità gestita da Keycloak. |
| `GET /api/time-team` | Utente attivo, senza query; `{scope,users:[{id,name,email,active}]}` contiene soltanto le persone attualmente visibili, inclusi gli account storici disattivati nella squadra. |
| `GET /api/time-entries?date=YYYY-MM-DD&user_id=N` | `user_id` facoltativo, predefinito sé stesso; un collega deve appartenere alla squadra corrente. Le righe altrui sono in sola lettura. |
| `GET /api/time-entries/week?date=YYYY-MM-DD&user_id=N` | Settimana lunedì–domenica contenente la data; stesso ambito personale/squadra. |

`GET /api/session` ora include `user_id` e flag UI distinti: `view_contracts`, `view_suppliers`, `view_costs`, `view_labor_rates`, `manage_labor_rates`, `view_team_hours`, `manage_access_groups`, oltre ai flag amministrativi. `manage_contracts` e `manage_suppliers` dipendono da Sales; i flag di costo da Finance. I flag descrivono la UI e non sostituiscono i controlli live dei moduli.

Fonti della policy: [controllo accessi](../../src/access_control.c), [rettifiche](../../src/time_corrections.inc), [settimana](../../src/time_week.inc). Per i nuovi flussi operativi vedere la [guida schema 13](../prod/flussi-operativi.md).

## Convenzioni HTTP

- Risposte API JSON UTF-8, salvo download PDF e redirect OAuth. Le liste non hanno tutte `ok`: usare lo status HTTP e la struttura specifica.
- I `POST` login, business e calendario richiedono `Content-Type: application/x-www-form-urlencoded`; i valori devono essere codificati percent-encoding. Anche un `POST` senza campi deve dichiarare questo formato. Il caricamento PDF usa `application/pdf` binario, non multipart. Logout è un caso separato: il router verifica metodo/origine e non legge campi dal corpo.
- Gli ID sono interi positivi, i minuti e i centesimi sono interi decimali senza separatori. Le date sono `YYYY-MM-DD`; i flag in ingresso sono generalmente `0`/`1`.
- Parametri sconosciuti o duplicati vengono rifiutati. Le query non elencate nel singolo endpoint sono rifiutate dai moduli business; non aggiungere arbitrariamente paginazione o filtri. Le mutazioni con `DELETE` richiedono corpo vuoto.
- Il parser HTTP richiede richieste ben formate, limita header a 16 KiB, corpo ordinario a 32 KiB, login a 8 KiB e PDF a 10 MiB. I `POST` sulle collezioni `/api/tasks` e `/api/blockers`, sui relativi elementi `/{id}` e sulle cancellazioni `/{id}/delete` ammettono 64 KiB di form. Non supporta `Transfer-Encoding`/chunked. Il target HTTP ha un buffer di 8192 byte. È prevista una richiesta per connessione (`Connection: close`).
- Header delle risposte: `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer` e CSP restrittiva.
- Le risposte di errore business hanno normalmente `{"ok":false,"error":"messaggio"}`. Non fare affidamento sul testo italiano come codice stabile: i codici macchina dell'addon Calendar sono un contratto distinto.

### Autenticazione, sessioni e CSRF

L’autenticazione può essere locale, ibrida o solo aziendale (`AUTH_MODE=local|hybrid|oidc`). `GET /api/auth/config` restituisce `{"sso":true|false,"local":true|false}` senza segreti. `GET /auth/login` avvia il redirect verso Keycloak; `GET /auth/callback` consuma code/state monouso, verifica il token e crea la sessione opaca Punto per un account locale attivo esplicitamente collegato. Le rotte SSO restituiscono `404` se disabilitate, `405` per metodi diversi da GET; in modalità `oidc`, `POST /api/login` restituisce `403`. Errori callback restituiscono un messaggio generico e cancellano il cookie transitorio. Non vengono restituiti token OAuth al browser. Dettagli in [Keycloak](keycloak.md).

L'accesso usa il cookie `punto_session`, token casuale di 32 byte rappresentato da 64 caratteri esadecimali. Flag `Path=/; HttpOnly; SameSite=Lax`; `Secure` quando l'origine è configurata come HTTPS. Non è un JWT né un bearer token pubblico. La durata interna è 8 ore, oppure 30 giorni con `remember=on`/`true`; nel secondo caso viene aggiunto `Max-Age`. L'app conserva al massimo 64 sessioni in memoria; un riavvio le perde e una nuova sessione può sostituire quella con scadenza più vicina.

Ogni richiesta protetta rilegge l'account: ruolo e stato non sono copiati indefinitamente nel cookie. Disabilitare un account ne invalida l'accesso; il blocco temporaneo del database produce indisponibilità senza cancellare automaticamente un token valido.

Per login e mutazioni il server verifica `Origin` rispetto a `Host` e alle origini consentite e rifiuta `Sec-Fetch-Site: cross-site`. Un client CLI senza `Origin` è ammesso se l'host è valido; non esiste un campo `csrf_token` da ottenere o inviare. Nelle chiamate browser usare la medesima origine e il cookie. La callback Google è un `GET` di ritorno cross-site protetto da state monouso e legame alla sessione.

| Status | Significato generale |
| --- | --- |
| `200` | Lettura, aggiornamento, cancellazione o operazione idempotente riuscita. |
| `201` | Entità business creata. |
| `202` | Modifica calendario accodata o sincronizzazione richiesta; non significa già applicata su Google. |
| `303` | Callback OAuth conclusa con redirect all'app. |
| `400` | Form/query/ID/data/valore non valido o campi non consentiti. |
| `401` | Credenziali login errate o sessione assente/scaduta. |
| `403` | Ruolo, assegnazione, destinatario o origine non autorizzati. |
| `404` | Risorsa assente o non disponibile nello scope dell'utente. |
| `405` | Metodo non implementato per il percorso. |
| `408`, `413`, `415`, `431` | Timeout di lettura, corpo troppo grande, formato non supportato, header troppo grandi. |
| `409` | Conflitto business, unicità, sovrapposizione, limite raggiunto o conflitto calendario. |
| `429` | Limite dei tentativi di login; eventualmente risposta inoltrata dal calendario. |
| `500`, `503` | Errore interno o dipendenza temporaneamente indisponibile. Rileggere lo stato prima di ripetere una scrittura senza idempotenza. |

## Accesso e salute

| Metodo e percorso | Accesso e ingresso | Esito |
| --- | --- | --- |
| `GET /healthz`, `HEAD /healthz` | Senza sessione; il percorso deve essere esatto, senza query. | `200 {"ok":true,"database":"mysql"}` oppure `sqlite`; `503 {"ok":false}` se lo storage non è sano. `HEAD` non invia il corpo. |
| `POST /api/login` | Senza sessione; `email`, `password`, `remember` facoltativo. Email max 254 byte, password max 1024 byte. | `200 {"ok":true}` e `Set-Cookie`; `401` credenziali errate, `429` dopo 8 fallimenti nella finestra globale di 60 secondi. Un successo azzera il conteggio. |
| `GET /api/session` | Cookie facoltativo. | `200 {"authenticated":false}` oppure identità e permessi, esempio sotto. |
| `POST /api/logout` | Verifica origine; cookie se presente. | Invalida la sessione e scade il cookie, `200 {"ok":true}`. |

Risposta autenticata:

```json
{
  "authenticated": true,
  "email": "utente@example.invalid",
  "name": "Utente di esempio",
  "role": "worker",
  "permissions": {
    "manage_jobs": false,
    "manage_users": false,
    "edit_time_entries": false,
    "manage_assignments": false,
    "view_audit": false,
    "manage_suppliers": false,
    "manage_contracts": false,
    "manage_groups": false,
    "manage_job_roles": false
  }
}
```

L’esempio sopra mostra i flag originari; il contratto corrente include `user_id` e i nuovi flag della policy schema 13. I permessi sono descrittivi per la UI; il backend controlla comunque ogni operazione. Il contatore login è per processo, non per singolo account o IP.

## Utenti

Tutti gli endpoint di questa sezione richiedono `manager`. Query non ammesse.

| Metodo e percorso | Corpo | Esito |
| --- | --- | --- |
| `GET /api/users` | — | `200 {"users":[...]}` con `id`, `name`, `email`, `role`, `active`. |
| `POST /api/users` | `name`, `email`, `password`, `role` | `201 {"ok":true,"id":...}`. Creato attivo. |
| `POST /api/users/{id}` | `name`, `role`, `active` | `200 {"ok":true}`. Non accetta cambio email o password. |

Nome UTF-8 1–120 caratteri; email validata/canonicalizzata e univoca; password 8–1024 byte UTF-8 senza tab/CR/LF; ruolo `manager` o `worker`; massimo 200 utenti. `409` per email duplicata, limite raggiunto o tentativo di disabilitare/declassare l'ultimo gestore attivo. Non sono esposti reset password, cancellazione utente o endpoint di registrazione pubblica.

## Commesse e registrazioni ore

| Metodo e percorso | Accesso | Campi/query | Esito |
| --- | --- | --- | --- |
| `GET /api/jobs` | Gestore: tutte; lavoratore: comuni e abilitate. | Nessuna query. | `200 {"jobs":[...]}`. |
| `POST /api/jobs` | Gestore. | `code`, `name`, `client`, `description`, `status`, facoltativi `contract_id`, `is_common`. | `201 {"ok":true,"id":...}`. |
| `POST /api/jobs/{id}` | Gestore. | Stessi campi. Codice, nome e stato vanno inviati; `contract_id`/`is_common` omessi conservano il valore precedente. | `200 {"ok":true,"id":...}`. |
| `DELETE /api/jobs/{id}` | Gestore. | Corpo vuoto. | `200 {"ok":true}`; elimina anche attività, blocchi, ore, assegnazioni e grant della commessa. |
| `GET /api/time-entries?date=YYYY-MM-DD` | Utente autenticato. | `date` obbligatoria; nessun `user_id`. | `200 {"entries":[...],"total_minutes":...}` delle **sole proprie ore**, anche per il gestore. |
| `POST /api/time-entries` | Gestore o lavoratore autorizzato. | `job_id`, `date`, `minutes`, `note` facoltativa. | `201 {"ok":true,"id":...}`; proprietario derivato dalla sessione. |
| `POST /api/time-entries/{id}` | Gestore, solo proprie registrazioni. | `job_id`, `date`, `minutes`, `note` facoltativa. | `200 {"ok":true,"id":...}`. |
| `DELETE /api/time-entries/{id}` | Gestore, solo proprie registrazioni. | Corpo vuoto. | `200 {"ok":true}`. |

Commessa: codice 1–40 caratteri, nome 1–120, cliente fino a 120, descrizione fino a 1000 anche multilinea; stato `planned`, `active`, `paused`, `completed`, `cancelled`; `is_common=0/1`. `contract_id=` svuota il collegamento; un nuovo collegamento richiede contratto esistente non chiuso/annullato. Limite 1000 commesse e codice univoco (`409`).

Ogni voce di `jobs` contiene `id`, `code`, `name`, `client`, `description`, `status`, `contract_id`, `contract_code`, `contract_title`, `supplier_name`, `is_common`, `total_minutes`, `assigned_users`, `assigned_groups`, `can_log_time`. `total_minutes` è il totale personale dell'utente corrente, non l'intero consuntivo della commessa. I conteggi di assegnazioni sono esposti al gestore; usare i report per aggregati aziendali.

Ore: 1–1440 minuti per riga, massimo 1440 minuti totali e 1000 registrazioni per utente/giorno, nota fino a 1000 caratteri. Ogni voce restituita include `id`, `job_id`, `job_code`, `job_name`, `date`, `minutes`, `note`, `owner`, `user_id`. Il backend non accetta `owner`/`user_id` da un form ore.

Per il lavoratore l'abilitazione a consuntivare deve risultare valida sia oggi sia nella data lavorata, e consentire `can_log_time`. I ruoli locali vengono combinati con assegnazioni dirette, gruppi antenati e commesse comuni secondo [processi e regole](../prod/processi-e-regole.md). Un ID commessa inesistente viene rifiutato e può produrre un evento per l'alert `invalid_job`; non vengono salvate ore orfane.

## Attività, sottoattività, Kanban e blocchi

La bacheca richiede una commessa visibile oggi. Il gestore modifica tutte le attività; un collaboratore attuale cambia soltanto lo stato delle attività assegnate a lui. I blocchi possono essere creati da gestori e collaboratori, poi modificati dal gestore oppure da un collaboratore che ne sia autore o responsabile. Soltanto il gestore riassegna o elimina. Un lettore consulta soltanto. La responsabilità di un elemento non concede automaticamente accesso alla commessa.

| Metodo e percorso | Ingresso | Risposta |
| --- | --- | --- |
| `GET /api/work-board?job_id=N` | Unica query `job_id`, obbligatoria; corpo vuoto. | `200 {"ok":true,"job_id":N,"tasks":[],"blockers":[],"assignees":[],"permissions":{...}}`. |
| `POST /api/tasks` | `job_id`, `title`; facoltativi `parent_id`, `assigned_user_id`, `description`, `status`, `priority`, `due_date`. | `201 {"ok":true,"id":N,"revision":1}`. |
| `POST /api/tasks/{id}` | `revision` obbligatoria e campi modificati, escluso `job_id`. Il collaboratore invia soltanto `status` e `revision`. | `200` con `ok`, `id` e revisione incrementata. |
| `POST /api/tasks/{id}/delete` | Solo `revision`; gestore. | `200` con `ok`, `id` e ultima revisione eliminata; `409` se ha figli. |
| `POST /api/blockers` | `job_id`, `title`; facoltativi `task_id`, `description`, `impact`, `owner_id`, `status`, `next_action`, `due_date`. | `201` con `ok`, `id`, `revision=1`. Per un collaboratore il responsabile è sempre lui stesso. |
| `POST /api/blockers/{id}` | `revision` e campi modificati, escluso `job_id`; solo il gestore può cambiare `owner_id`. | `200` con `ok`, `id` e revisione incrementata. |
| `POST /api/blockers/{id}/delete` | Solo `revision`; gestore. | `200` con `ok`, `id` e ultima revisione eliminata. |

Nessuna query è ammessa sulle mutazioni. I campi duplicati/sconosciuti, `job_id` su update, `revision` su create, identità autore o timestamp inviati dal client sono rifiutati. Gli aggiornamenti sono parziali: i campi omessi conservano il valore precedente. ID opzionale o data vuoti azzerano il riferimento; in risposta il valore è `null`.

| Risorsa | Campi restituiti oltre a `id` e `job_id` |
| --- | --- |
| Attività | `parent_id`, `assigned_user_id`, `assigned_user_name`, `title`, `description`, `status`, `priority`, `due_date`, `revision`, `created_at`, `updated_at`, `can_edit`, `can_update_status`. |
| Blocco | `task_id`, `title`, `description`, `impact`, `owner_id`, `owner_name`, `status`, `next_action`, `due_date`, `created_by`, `revision`, `created_at`, `updated_at`, `can_edit`. |
| Assegnatario selezionabile | `id`, `name`; elenco solo per gestori, `assignees:[]` per operatori. Sono eleggibili gestori attivi e collaboratori attivi abilitati oggi alla commessa. |
| Permessi della bacheca | `manage_tasks` e `report_blockers`, booleani. Le autorizzazioni effettive di ogni elemento sono nei rispettivi flag. |

Titolo 1–160 caratteri UTF-8; descrizione fino a 2000; prossima azione fino a 1000. Scadenza facoltativa `YYYY-MM-DD` valida; timestamp UTC `YYYY-MM-DDTHH:MM:SSZ`. Stato attività: `todo` (default), `in_progress`, `blocked`, `in_review`, `done`; priorità: `low`, `normal` (default), `high`, `urgent`. Stato blocco: `open` (default), `in_progress`, `resolved`; impatto: `low`, `medium` (default), `high`, `critical`. Gli stati di blocco, attività, padre e commessa restano indipendenti.

Ogni commessa ammette 500 attività e 500 blocchi; la creazione oltre il limite produce `409`. Le sottoattività hanno al massimo otto livelli inclusa la radice, senza cicli e nella stessa commessa. Anche un blocco può riferirsi soltanto a un’attività della propria commessa. Il nuovo assegnatario deve essere eleggibile oggi; un assegnatario precedente invariato può essere conservato anche dopo una revoca, senza concedergli permessi.

Una `revision` superata produce `409` senza sovrascrivere i dati; rileggere la bacheca prima di decidere la modifica. La cancellazione di un’attività senza figli mantiene i blocchi, svuota `task_id` e ne incrementa le revisioni nella stessa transazione. L’audit registra `tasks.create/update/delete`, `blockers.create/update/delete` e `blockers.task_detached`. Elementi inesistenti e non visibili restituiscono lo stesso `404` generico. Dettagli in [implementazione attività/blocchi](attivita-kanban-e-blocchi.md).

## Assegnazioni dirette, gruppi e ruoli locali

Tutti gli endpoint sotto richiedono `manager`; query non ammesse. Una data vuota indica estremo aperto.

| Metodo e percorso | Corpo | Risposta |
| --- | --- | --- |
| `GET /api/jobs/{job_id}/assignments` | — | `200 {"job_id":...,"assignments":[...]}`. Dati utente, ruolo locale, `starts_on`, `ends_on`, `can_log_time`. |
| `POST /api/jobs/{job_id}/assignments` | `user_id`, facoltativi `starts_on`, `ends_on`, `role_id` (default Collaboratore ID 2). | `201` nuova oppure `200` aggiornata, `{"ok":true,"id":job_id}`. Una riga per coppia. |
| `DELETE /api/jobs/{job_id}/assignments/{user_id}` | Vuoto. | `200`, rimozione assegnazione. |
| `GET /api/groups` | — | `200 {"groups":[...]}` con ID, nome, padre e conteggi. |
| `POST /api/groups` | `name`, `parent_id` facoltativo/vuoto. | `201 {"ok":true,"id":...}`. |
| `POST /api/groups/{id}` | `name`, `parent_id` (gli omessi conservano il valore precedente). | `200 {"ok":true,"id":...}`. |
| `DELETE /api/groups/{id}` | Vuoto. | `200`; `409` se ha sottogruppi, membri o grant. |
| `GET /api/groups/{id}/members` | — | `200 {"members":[...]}`; appartenenze dirette. |
| `POST /api/groups/{id}/members` | `user_id` | `201` nuovo membro, `200` se già presente; richiede utente attivo. |
| `DELETE /api/groups/{id}/members/{user_id}` | Vuoto. | `200`. |
| `GET /api/job-roles` | — | `200 {"roles":[...]}` con `id`, `code`, `name`, `can_log_time`, `builtin`. |
| `POST /api/job-roles` | `code`, `name`, `can_log_time=0/1` | `201`. |
| `POST /api/job-roles/{id}` | Stessi campi; omessi conservati. | `200`; `409` per un ruolo built-in. |
| `GET /api/jobs/{job_id}/group-grants` | — | `200 {"grants":[...]}` con gruppo, ruolo e validità. |
| `POST /api/jobs/{job_id}/group-grants` | `group_id`, `role_id`, facoltativi `starts_on`, `ends_on`. | `201` nuovo, `200` aggiornato. |
| `DELETE /api/jobs/{job_id}/group-grants/{group_id}` | Vuoto. | `200`. |

Limiti: 200 gruppi, profondità 16, 100 ruoli locali, 200 membri per gruppo, 200 assegnazioni dirette per commessa, nomi fino a 120 caratteri e codici ruolo fino a 40. Cicli nell'alberatura, date invertite, riferimenti inesistenti e ruoli non validi vengono rifiutati. La rimozione di un'abilitazione non elimina ore storiche; la cancellazione della commessa invece sì. Non esiste un `DELETE` dei ruoli locali.

Le liste espongono: gruppo = `id`, `name`, `parent_id` (numero o `null`), `members_count`, `children_count`; membro = `user_id`, `name`, `email`, `role`, `active`; grant = `group_id`, `group_name`, `role_id`, `role_code`, `role_name`, `can_log_time`, `starts_on`, `ends_on`. Le date di assegnazioni/grant senza estremo sono serializzate come stringa vuota. Il `POST` di assegnazione diretta sostituisce le date con quelle inviate (omissione = estremo aperto) e il ruolo omesso vale 2; il `POST` di grant di gruppo conserva invece le date omesse di una riga esistente. Le risposte di modifica organizzativa includono `ok` e `id` dell'entità interessata; per grant e assegnazione è l'ID commessa.

## Fornitori, contratti e PDF

Tutti gli endpoint richiedono `manager`.

| Metodo e percorso | Corpo/query | Esito |
| --- | --- | --- |
| `GET /api/suppliers` | Nessuna query. | `200 {"suppliers":[...]}`. |
| `POST /api/suppliers` | `code`, `name`; opzionali `vat_number`, `email`, `phone`, `address`, `notes`, `active`. | `201`, `ok` e `id`. |
| `POST /api/suppliers/{id}` | Stessi campi; omessi conservati. | `200`, `ok` e `id`. |
| `GET /api/contracts` | Nessuna query. | `200 {"contracts":[...]}` con dati del fornitore e conteggi associati. |
| `POST /api/contracts` | `code`, `title`, `supplier_id`; opzionali `status`, `starts_on`, `ends_on`, `value_cents`, `currency`, `notes`. | `201`, `ok` e `id`. |
| `POST /api/contracts/{id}` | Stessi campi; omessi conservati. | `200`, `ok` e `id`. |
| `GET /api/contracts/{id}/documents` | Nessuna query. | `200 {"documents":[...]}` con manifesto PDF. |
| `POST /api/contracts/{id}/documents?filename=contratto.pdf` | Corpo PDF originale e `Content-Type: application/pdf`; solo query `filename`. | `201`, `ok` e ID documento; `503` se archivio documentale indisponibile. |
| `GET /api/contract-documents/{id}/download`, `HEAD` sullo stesso percorso | Nessuna query. | `200 application/pdf`, `Content-Disposition: attachment` con filename UTF-8. `HEAD` omette il contenuto. |

Codici 1–40 caratteri; nome/titolo 1–120; partita IVA fino a 32, email 254, telefono 40, indirizzo 240, note 1000. Massimo 1000 fornitori e 1000 contratti. Nuovo fornitore attivo per default; nuovo contratto `draft` e valuta `EUR`. Date opzionali, fine non precedente all'inizio, valore vuoto = sconosciuto (`NULL`), altrimenti 0–999999999999 centesimi. Non sono ammessi formati con virgola decimale.

Stati contratto: `draft`, `active`, `expired`, `closed`, `cancelled`. Non si può collegare una nuova commessa a un contratto chiuso o annullato. Il server valida esistenza e abilitazione del fornitore nelle operazioni rilevanti. I dati del contratto non rappresentano fatture emesse o ricevute.

PDF: nome fino a 180 byte, estensione `.pdf`, niente separatori di directory o caratteri di controllo; dimensione 5 byte–10 MiB; firma `%PDF-`. Il download verifica dimensione e SHA-256. Non sono esposti sostituzione o cancellazione dei PDF: ogni caricamento crea un documento immutabile e un evento audit.

Le righe fornitore contengono `id`, `code`, `name`, `vat_number`, `email`, `phone`, `address`, `notes`, `active`; in questa API `active` è un **numero 0/1**. Le righe contratto contengono `id`, `code`, `title`, `supplier_id`, `status`, `starts_on`, `ends_on`, `value_cents`, `currency`, `notes`, `supplier_name`, `documents_count`; date e valore assenti sono `null`. Il manifesto documento contiene `id`, `contract_id`, `file_name`, `byte_size`, `sha256`, `uploaded_by`, `created_at`; la chiave storage interna non viene esposta. Limite di 1000 PDF per contratto.

## Tariffe e cinque viste report

| Metodo e percorso | Accesso/campi | Esito |
| --- | --- | --- |
| `GET /api/labor-rates` | Gestore, nessuna query. | `200 {"ok":true,"rates":[...]}` con ID, utente, tariffa e validità. |
| `POST /api/labor-rates` | Gestore: `user_id`, `hourly_cents`, `starts_on`, `currency=EUR`; `ends_on` facoltativa. | `201 {"ok":true,"id":...}`. |
| `POST /api/labor-rates/{id}` | Gestore, stessi campi, `user_id` immutabile. | `200`; `409` se gli intervalli si sovrappongono. |
| `GET /api/reports` | `view`, `from`, `to` obbligatori; `job_id`, `user_id` opzionali. | `200` struttura descritta sotto; `409` oltre 1000 righe senza risultati parziali. |

Tariffa 0–99999999 centesimi/ora, massimo 1000 tariffe totali, date inclusive e non sovrapposte per utente; `ends_on=` lascia aperto l'intervallo. Nessun endpoint di cancellazione tariffa. Il report prende la tariffa valida nella data di ciascuna registrazione, non quella corrente.

Ogni tariffa restituita ha `id`, `user_id`, `user_name`, `user_email`, `hourly_cents`, `currency`, `starts_on`, `ends_on` (stringa vuota se aperta). L'elenco rifiuta più di 1000 tariffe con `409`, senza paginazione.

| `view` | Scope | Campi delle righe oltre ai conteggi |
| --- | --- | --- |
| `date` | Gestore: tutti o filtri; lavoratore: solo sé stesso. | `date`, `minutes`, `entry_count`, `job_count`, `user_count`. |
| `job` | Come sopra. | `job_id`, `code`, `name`, `client`, `status`, `minutes`, `entry_count`, `user_count`. |
| `user` | Come sopra. | `user_id`, `name`, `email`, `active`, `minutes`, `entry_count`, `job_count`. |
| `cost` | Solo gestore. | Un record per contratto coinvolto nelle ore filtrate: `contract_id`, `code`, `title`, `supplier_name`, `status`, `starts_on`, `ends_on`, `value_cents`, `currency`, `job_count`, `minutes`. |
| `labor_cost` | Solo gestore. | Coppia utente/commessa: `user_id`, `name`, `email`, `job_id`, `code`, `job_name`, `minutes`, `entry_count`, `priced_minutes`, `unpriced_minutes`, `cost_cents`. |

Il periodo include 1–366 giorni, estremi inclusivi. Un lavoratore che specifica un altro `user_id` riceve `403`. Una tariffa mancante produce minuti non valorizzati; `cost_cents=null` se nessuna registrazione della riga ha tariffa. La vista contratti conta una volta il valore integrale di ciascun contratto selezionato dalle ore: non ripartisce automaticamente il valore per durata, commessa o utente e non è il costo delle ore.

Risposta comune: `ok`, `view`, `from`, `to`, `today`, `scope` (`all`/`personal`), `summary`, `options: {jobs,users}`, `rows`. `summary` contiene `minutes`, `entry_count`, `job_count`, `user_count`; per `cost` aggiunge `contract_count`, `known_value_cents`, `unpriced_contract_count`, `uncontracted_minutes`; per `labor_cost` aggiunge `estimated_cost_cents`, `priced_minutes`, `unpriced_minutes`. Totali e righe sono ottenuti da una stessa query SQL; non sommare soltanto righe di un risultato troncato, perché il server rifiuta il superamento del limite.

## Alert e notifiche

| Metodo e percorso | Accesso/campi | Risposta |
| --- | --- | --- |
| `GET /api/alert-rules` | Gestore, proprie regole. | `200 {rules,users,smtp_configured,timezone,daily_target_minutes,grace_hours}`. |
| `POST /api/alert-rules` | Gestore; campi sotto. | `201`, regola salvata. |
| `POST /api/alert-rules/{id}` | Gestore proprietario; campi sotto. | `200`; la proprietà è verificata nel backend. |
| `POST /api/alert-rules/{id}/run` | Gestore proprietario, corpo vuoto. | `200`, esito dell'esecuzione e conteggi; non elimina le protezioni dai duplicati. |
| `GET /api/notifications` | Utente autenticato, proprie notifiche. | `200 {"notifications":[...],"unread_count":...}`; ultime 100, nessun cursore. |
| `POST /api/notifications/{id}/read` | Destinatario autorizzato, corpo vuoto. | `200 {"ok":true}`; ripetere non cambia il primo `read_at`. |

Campi regola: `name` (1–120 caratteri), `type=invalid_job|missing_hours|inventory_due|inventory_return|inventory_verification`, `run_time=HH:MM`, `enabled=0|1`, `recipient_user_ids` elenco separato da virgole, `recipient_emails` elenco separato da virgole o righe. Almeno un destinatario, massimo 50 utenti e 50 email senza duplicati; massimo 100 regole per gestore. I destinatari interni devono essere utenti esistenti e attivi; per gli alert sulle ore un worker riceve soltanto notifiche relative a sé stesso; le tre regole inventory notificano a tutti i destinatari selezionati gli eventi del catalogo globale. Non c'è endpoint di cancellazione regola: disabilitarla mantiene lo storico.

`inventory_due` accetta `days_before` 1–365 (predefinito 30), segnala garanzie, manutenzioni, scadenze e rinnovi anche superati, esclude beni dismessi/smarriti e deduplica per regola, destinatario, asset, campo, data e anticipo. Se l’inventario è indisponibile non consuma l’esecuzione. Dettagli nel [contratto inventory](asset-inventory.md).

`inventory_return` segnala consegne ancora aperte con restituzione prevista prima di oggi. `inventory_verification` accetta `verification_days` 1–365 (predefinito 90): segnala subito i beni mai verificati e quelli la cui verifica cronologicamente più recente risale ad almeno tale intervallo. Entrambe riguardano soltanto dispositivi IT, dispositivi di rete e beni fisici non dismessi/smarriti. La deduplicazione include l'identificativo della consegna o verifica, la data e, per le verifiche, l'intervallo: una nuova consegna o un nuovo verbale possono produrre un nuovo avviso. La sorgente deve essere acquisita e convalidata interamente prima della transazione; un errore non crea avvisi parziali e non consuma l'esecuzione. `days_before` e `verification_days` rimangono campi distinti, sempre validati nei rispettivi intervalli.

`missing_hours` controlla 480 minuti per ogni lunedì–venerdì, con 48 ore di tolleranza e avanzamento persistente dei giorni già esaminati; non sottrae automaticamente ferie o festività. `invalid_job` osserva tentativi rifiutati su ID inesistenti. L'orario giornaliero usa `Europe/Rome`; lo scheduler dell'app verifica circa ogni 30 secondi e può eseguire dopo l'ora prevista se l'app era occupata. L'esecuzione giornaliera è tracciata per evitare duplicazioni. Le risposte di lista includono ultimo/prossimo run e destinatari; nessuna credenziale SMTP viene esposta.

Le notifiche hanno `id`, `rule_id`, `subject_user_id`, `title`, `message`, `work_date`, `recorded_minutes`, `created_at`, `read_at`. Per un utente attualmente worker il backend restringe ulteriormente le notifiche a quelle che lo riguardano come soggetto. Le email sono accodate anche con SMTP non configurato; `smtp_configured=false` non impedisce le notifiche interne. Stati e semantica della consegna in [API addon](api-addon.md).

Ogni regola in lista include `id`, `name`, `type`, `run_time`, `enabled`, `days_before`, `verification_days`, `created_at`, `updated_at`, `last_run_at`, `last_run_on`, `next_run_at`, `recipient_user_ids`, `recipient_emails`. Le notifiche inventory includono `event_kind`, `asset_id`, `due_kind`, `due_on`; quelle operative includono anche `source_id` e, per la verifica, `verification_days`. I POST di creazione/aggiornamento restituiscono `{"ok":true,"id":...}`; il run manuale restituisce `ok`, `notifications_created`, `emails_queued`, `next_run_at`. Alcune esecuzioni non applicabili restituiscono `{"ok":true,"skipped":true}`. Il conteggio delle non lette comprende tutte le notifiche visibili non lette, non solo le ultime 100 restituite.

## Asset inventory

Gli utenti attivi consultano il catalogo condiviso e il cruscotto; le scritture richiedono un Gestore. Il [contratto completo](asset-inventory.md) descrive schede, persone, identità, relazioni, CSV, luoghi, consegne e verifiche. Tutte le chiamate pubbliche passano dal bridge autenticato, con TLS verificato e token privato verso il servizio separato.

`GET /api/inventory/assets`, `/api/inventory/assets/export` e `/api/inventory/dashboard` condividono i filtri `q`, `status`, `asset_type`, `mine=1`, `site_id`, `department_id`, `person_id`, `identity_id`, `attention`, `verification_days` e `today`. Gli ID sono positivi; i filtri persona/identità usano solo collegamenti diretti attivi, senza propagazione dalle utenze condivise; `mine` viene trasformato nel backend nell'ID dell'utente corrente e non accetta un assegnatario arbitrario. `attention` può essere `open_custody`, `overdue_returns`, `never_verified`, `verification_due`, `location_mismatch`. `verification_days` vale 90 per impostazione predefinita (1–365); `today` è una data gregoriana reale, predefinita sul giorno aziendale. Solo catalogo/export accettano `limit` 1–100 e `after_id` positivo.

Il cruscotto restituisce `ok`, `today`, `verification_days`, `counts` (`total`, `open_custody`, `overdue_returns`, `never_verified`, `verification_due`, `location_mismatch`), `by_status`, `by_type`, `by_site`, `by_department`, `by_user` e `groups_truncated`. I conteggi coprono tutto l'insieme filtrato. I gruppi `{id,name,count}` includono i non assegnati con ID nullo e sono limitati ai primi 20 per sezione. I conteggi operativi riguardano beni tangibili attivi; `verification_due` conta solo beni già verificati, mentre `never_verified` è separato. La verifica di riferimento è quella con `verified_on` più recente, a parità di data con ID maggiore.

`GET /api/inventory/operational-due` accetta `kind=return|verification`, `today`, `verification_days`, `limit`, `after_id`. Restituisce `{ok,items,next_after_id}` con una riga per bene, ID decrescenti; ogni riga contiene `asset_id`, `asset_tag`, `name`, `asset_type`, `due_kind=return_due|verification_due`, `due_on`, `days_remaining`, `source_id`. Per beni mai verificati `source_id=0` e `due_on` è la data di creazione. L'endpoint preesistente `/api/inventory/due` conserva i quattro tipi di scadenza e i propri parametri. Lo scheduler acquisisce al massimo 100 pagine/10000 eventi entro 8 secondi, rifiutando risultati incompleti o incoerenti.

## Audit

`GET /api/audit` richiede un gestore. Query: `limit` 1–100, default 30; `before_id` cursore positivo; `entity_type` facoltativo tra `job`, `time_entry`, `assignment`, `user`, `system`, `supplier`, `contract`, `contract_document`, `group`, `group_member`, `job_role`, `group_grant`, `labor_rate`, `alert_rule`.

Risposta `200 {"events":[...],"next_before_id":...}`; cursore `null` alla fine, ordinamento ID decrescente. Ogni evento contiene `id`, `created_at`, `actor_id`, `actor_name`, `actor_email`, `action`, `entity_type`, `entity_id`, `before`, `after`; i due ultimi campi sono JSON o `null`. Non sono esposti modifica/cancellazione dell'audit. Conservare i filtri quando si usa il cursore.

## Bridge Google Calendar

Tutti gli endpoint riguardano **il calendario dell'utente autenticato**, sia gestore sia lavoratore. Non accettano `user_id`, bearer addon o `session_binding` dal browser. L'identità e il legame OAuth vengono calcolati nell'app.

| Metodo e percorso pubblico | Ingresso | Esito principale |
| --- | --- | --- |
| `GET /api/calendar/status` | Nessun parametro. | `200`, configurazione OAuth, connessione, calendario, ultimo sync e contatori coda. |
| `POST /api/calendar/oauth/start` | Corpo vuoto. | `200 {authorization_url,expires_in:600}`; il client apre il consenso Google. |
| `GET /api/calendar/oauth/callback` | Query `state`, `code` oppure `error`; metadati Google `scope`, `authuser`, `prompt` accettati e scartati. | `303 /app?calendar=connected` oppure `/app?calendar=error`; mai il codice OAuth nel redirect. |
| `GET /api/calendar/calendars` | `page_token`, `limit` opzionali, massimo 50 effettivo. | `200 {items,next_page_token}`. |
| `POST /api/calendar/selection` | `calendar_id` | `200`; richiede ruolo Google writer/owner. |
| `GET /api/calendar/events` | `from`, `to` insieme, `limit`, `after_id` opzionali. | `200`, pagina della cache locale. |
| `GET /api/calendar/changes` | Nessun parametro. | `200`, ultimi esiti della coda. |
| `POST /api/calendar/events/create` | `idempotency_key`, `event` JSON codificato nel form. | `202` accodato, `200` se ripetizione identica. |
| `POST /api/calendar/events/update` | `idempotency_key`, `event_id`, `etag`, `event`. | Come sopra; ETag protegge dal sovrascrivere modifiche altrui. |
| `POST /api/calendar/events/delete` | `idempotency_key`, `event_id`, `etag`. | Come sopra. |
| `POST /api/calendar/sync` | Corpo vuoto. | `202`; avvio richiesto in background. |
| `POST /api/calendar/disconnect` | Corpo vuoto. | `200`; rimozione archivio utente e tentativo revoca consenso. |

Lo schema degli eventi, le pagine, i conflitti e i codici sono documentati per esteso in [API addon](api-addon.md). Il bridge impone 32 KiB di form, massimo 24576 byte per il valore `event`, risposta addon fino a 128 KiB. Chiave di idempotenza 8–128 caratteri ASCII `[A-Za-z0-9_.:-]`. La risposta dell'addon viene verificata come JSON e non può contenere campi sensibili riconosciuti come token OAuth o segreti; errori di trasporto e risposte inattese vengono sanitizzati.

Timeout app→addon: connessione 1 secondo; totale 25 secondi per callback, lista calendari e selezione, 15 per disconnessione, 5 per le altre operazioni. Non tutti questi timeout indicano che il provider non abbia eseguito un'azione: controllare stato/coda e riusare la stessa chiave dopo un esito ambiguo.

Se il bridge nativo non è configurato, `/status` restituisce `enabled:false`, `oauth_configured:false`, `connected:false`, provider `google`; le altre operazioni danno `503 calendar_not_configured`. Se l'addon gira ma mancano le credenziali Google, espone `oauth_configured:false` e le operazioni OAuth danno `503 oauth_not_configured`. La configurazione Google è stata rinviata: l'integrazione API non implica un account già collegato né una UI calendario già presente.

## Esempi per client e integrazioni

Gli esempi usano un cookie jar locale già ottenuto tramite login. `COOKIE_JAR` deve puntare a un file privato; `APP_ORIGIN` va sostituito con l'origine configurata. Non inserire credenziali o cookie nel repository.

```sh
APP_ORIGIN='https://progetti.tuaazienda.it'
COOKIE_JAR='/percorso/privato/session.cookies'

curl --fail-with-body --cookie "$COOKIE_JAR" \
  "$APP_ORIGIN/api/session"

curl --fail-with-body --cookie "$COOKIE_JAR" \
  --header "Origin: $APP_ORIGIN" \
  --header 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode 'job_id=123' \
  --data-urlencode 'date=2026-10-12' \
  --data-urlencode 'minutes=120' \
  --data-urlencode 'note=Analisi requisiti' \
  "$APP_ORIGIN/api/time-entries"

curl --fail-with-body --cookie "$COOKIE_JAR" --get \
  --data-urlencode 'view=labor_cost' \
  --data-urlencode 'from=2026-10-01' \
  --data-urlencode 'to=2026-10-31' \
  "$APP_ORIGIN/api/reports"

curl --fail-with-body --cookie "$COOKIE_JAR" \
  --header "Origin: $APP_ORIGIN" \
  --header 'Content-Type: application/pdf' \
  --data-binary '@/percorso/contratto.pdf' \
  "$APP_ORIGIN/api/contracts/123/documents?filename=contratto.pdf"
```

Gli ID sono segnaposto: recuperarli dalle liste prima delle mutazioni. Nessun esempio richiede di disabilitare la verifica TLS. In sviluppo locale l'origine sarà HTTP localhost prevista dalla configurazione; in produzione usare HTTPS.

## Concorrenza e limiti di integrazione

Il server C principale gestisce le connessioni in sequenza e fa girare lo scheduler nello stesso processo. La sincronizzazione remota Calendar ha un worker separato nell'addon, ma chiamate sincrone a provider/SMTP e download possono comunque incidere sulla latenza dell'app. L'app non è attualmente un cluster di processi con sessioni condivise.

Le mutazioni business e l'audit sono transazionali, con controllo di certificazione MySQL tramite `cluster_write_guard`. Attività e blocchi hanno una `revision` obbligatoria su update/delete, verificata nella stessa transazione; gli editor obsoleti ricevono `409`. Non esistono ETag/versioni per gli aggiornamenti delle commesse: due aggiornamenti serializzati possono applicarsi entrambi e l'ultimo può sostituire campi del precedente. Non sono previsti webhook pubblici, OpenAPI generato, GraphQL, token API personali, endpoint fatture o collegamento automatico evento→commessa/ore.

Test di contratto e regressione: [suite](../../tests), in particolare `test_server.py`, `test_roles.py`, `test_workspace.py`, `test_groups.py`, `test_assignments.py`, `test_procurement.py`, `test_alerts.py`, `test_reports.py`, `test_labor_rates.py`, [test attività/blocchi](../../tests/test_work_board.py) e [test bridge calendario](../../tests/test_calendar_client.py). Per l'esecuzione della suite vedere [sviluppo e test](sviluppo-test.md).
