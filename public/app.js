"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const states = { planned: "Da avviare", active: "In corso", paused: "In pausa", completed: "Completata", cancelled: "Annullata" };
  const roles = { manager: "Gestore", worker: "Operatore" };
  const contractStates = { draft: "Bozza", active: "Attivo", expired: "Scaduto", closed: "Chiuso", cancelled: "Annullato" };
  const tabs = ["jobs", "hours", "suppliers", "contracts", "users", "audit"];
  const state = { session: null, jobs: [], entries: [], users: [], todayMinutes: null, todayCount: null, jobsReady: false, entriesReady: false, usersReady: false, selectedDate: localDate(new Date()), entriesRequest: 0, usersRequest: 0, refreshRequest: 0, deleteEntry: null, openEntryAfterJob: false, activeTab: "jobs", assignmentJob: null, assignments: [], assignmentUsers: [], assignmentsReady: false, assignmentRequest: 0, editingAssignment: null, revokeAssignment: null, auditEvents: [], auditReady: false, auditRequest: 0, auditLoading: false, auditCursor: null, auditRetryMore: false };
  Object.assign(state, { suppliers: [], contracts: [], suppliersReady: false, contractsReady: false, suppliersRequest: 0, contractsRequest: 0, suppliersLoading: false, contractsLoading: false, editingJob: null, editingContract: null, documentsContract: null, documents: [], documentsReady: false, documentsRequest: 0 });
  const busyDialogs = new Set();
  let noticeTimer;

  function localDate(date) {
    return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}-${String(date.getDate()).padStart(2, "0")}`;
  }

  function validDate(value) {
    if (!/^\d{4}-\d{2}-\d{2}$/.test(value)) return false;
    const [year, month, day] = value.split("-").map(Number);
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    const leap = year % 4 === 0 && (year % 100 !== 0 || year % 400 === 0);
    return day <= [31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1];
  }

  function minutesLabel(minutes) {
    const value = Number(minutes) || 0;
    return `${Math.floor(value / 60)} h ${String(value % 60).padStart(2, "0")} min`;
  }

  function dateLabel(value) {
    const [year, month, day] = value.split("-").map(Number);
    const date = new Date(0);
    date.setFullYear(year, month - 1, day);
    date.setHours(12, 0, 0, 0);
    return date.toLocaleDateString("it-IT", { day: "numeric", month: "long", year: "numeric" });
  }

  function hoursToMinutes(value) {
    const normalized = value.trim().replace(",", ".");
    if (/^\d{1,2}:\d{2}$/.test(normalized)) {
      const [hours, minutes] = normalized.split(":").map(Number);
      if (minutes > 59) throw new Error("Dopo i due punti inserisci i minuti, da 00 a 59.");
      const total = hours * 60 + minutes;
      if (total < 1 || total > 1440) throw new Error("Inserisci una durata da 1 minuto a 24 ore.");
      return total;
    }
    if (!/^(?:\d+(?:\.\d+)?|\.\d+)$/.test(normalized)) throw new Error("Inserisci le ore come 1,5 oppure 1:30.");
    const [whole, fraction = ""] = normalized.split(".");
    const denominator = 10n ** BigInt(fraction.length);
    const numerator = BigInt((whole || "0") + fraction) * 60n;
    if (numerator % denominator !== 0n) throw new Error("La durata deve corrispondere a minuti interi. Usa ad esempio 0:15 per 15 minuti.");
    const minutes = Number(numerator / denominator);
    if (minutes < 1 || minutes > 1440) throw new Error("Inserisci una durata da 1 minuto a 24 ore.");
    return minutes;
  }

  function node(tag, className, text) {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text !== undefined) element.textContent = text;
    return element;
  }

  async function api(path, options = {}) {
    const controller = new AbortController();
    const { timeoutMs = 15000, ...fetchOptions } = options;
    const timeout = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const response = await fetch(path, { credentials: "same-origin", cache: "no-store", ...fetchOptions, signal: controller.signal });
      if (response.status === 401) {
        window.location.replace("/");
        throw new Error("La sessione è scaduta. Accedi di nuovo.");
      }
      let data;
      try { data = await response.json(); } catch (_) { throw new Error("Il server ha restituito una risposta non valida. Riprova."); }
      if (!response.ok || data.ok === false) {
        if (response.status === 403) {
          showNotice(data.error || "Il tuo account non è abilitato a questa operazione.", true);
          if (path !== "/api/session") refreshSession().catch(() => {});
        }
        throw new Error(data.error || (response.status === 403 ? "Il tuo account non è abilitato a questa operazione." : "Non siamo riusciti a completare l’operazione. Riprova."));
      }
      return data;
    } catch (error) {
      if (error.name === "AbortError") throw new Error("Il server sta impiegando troppo tempo. Controlla la connessione e riprova.");
      if (error instanceof TypeError) throw new Error("Non riusciamo a raggiungere il server. Controlla la connessione e riprova.");
      throw error;
    } finally {
      clearTimeout(timeout);
    }
  }

  function post(path, fields) {
    return api(path, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8" }, body: new URLSearchParams(fields) });
  }

  function showNotice(message, isError = false) {
    clearTimeout(noticeTimer);
    $("notice-text").textContent = message;
    $("notice").classList.toggle("notice-error", isError);
    $("notice").setAttribute("role", isError ? "alert" : "status");
    $("notice").hidden = false;
    if (!isError) noticeTimer = setTimeout(() => { $("notice").hidden = true; }, 7000);
  }

  function showLoadError(error) {
    $("load-error-text").textContent = error.message;
    $("load-error").hidden = false;
  }

  function can(permission) {
    return state.session?.permissions?.[permission] === true;
  }

  function requirePermission(permission) {
    if (can(permission)) return true;
    showNotice("Il tuo account non è abilitato a questa operazione.", true);
    return false;
  }

  function availableTabs() {
    const permissions = { users: "manage_users", audit: "view_audit", suppliers: "manage_suppliers", contracts: "manage_contracts" };
    return tabs.filter((tab) => !permissions[tab] || can(permissions[tab]));
  }

  function applySession(session) {
    if (!session.authenticated) { window.location.replace("/"); return false; }
    state.session = session;
    $("session-name").textContent = session.name || session.email;
    $("session-name").title = session.name || session.email;
    $("session-email").textContent = session.email;
    $("session-email").title = session.email;
    $("session-role").textContent = roles[session.role] || "Operatore";
    $("session-role").hidden = false;
    $("user-avatar").textContent = (session.name || session.email || "P").charAt(0).toUpperCase();
    $("new-job").hidden = !can("manage_jobs");
    $("tab-suppliers").hidden = !can("manage_suppliers");
    $("tab-contracts").hidden = !can("manage_contracts");
    $("tab-users").hidden = !can("manage_users");
    $("tab-audit").hidden = !can("view_audit");
    $("new-user").hidden = !can("manage_users");
    document.querySelector(".jobs-table").classList.toggle("jobs-with-assignments", can("manage_assignments"));
    $("entries-actions-heading").hidden = !can("edit_time_entries");
    document.querySelector(".entries-table").classList.toggle("entries-readonly", !can("edit_time_entries"));
    $("permission-hint").textContent = can("manage_jobs") ? "Gestore · Gestisci commesse, contratti, fornitori e persone. Ogni modifica ha la sua traccia." : "Operatore · Registra il lavoro sulle commesse abilitate per te. Le ore inserite restano nel tuo registro.";
    $("jobs-panel-description").textContent = can("manage_jobs") ? "Dal primo passo all’ultimo dettaglio, tutto sotto controllo." : "Le commesse abilitate per te, pronte per registrare il lavoro svolto.";
    if (!can("manage_jobs")) {
      state.openEntryAfterJob = false;
      if ($("job-dialog").open) $("job-dialog").close();
    }
    if (!can("edit_time_entries")) {
      if ($("entry-dialog").open && $("entry-id").value) $("entry-dialog").close();
      if ($("delete-dialog").open) $("delete-dialog").close();
      state.deleteEntry = null;
    }
    if (!can("manage_users")) {
      state.users = [];
      state.usersReady = false;
      state.usersRequest++;
      $("users-body").replaceChildren();
      if ($("user-dialog").open) $("user-dialog").close();
    }
    if (!can("manage_assignments")) {
      state.assignmentRequest++;
      state.assignmentJob = null;
      if ($("assignment-dialog").open) $("assignment-dialog").close();
    }
    if (!can("view_audit")) {
      state.auditRequest++;
      state.auditEvents = [];
      state.auditReady = false;
      state.auditLoading = false;
      $("audit-list").replaceChildren();
    }
    for (const [kind, permission] of [["suppliers", "manage_suppliers"], ["contracts", "manage_contracts"]]) {
      if (!can(permission)) {
        state[kind + "Request"]++;
        state[kind] = [];
        state[kind + "Ready"] = false;
        state[kind + "Loading"] = false;
        $(kind + "-body").replaceChildren();
        $(kind + "-count").textContent = "0";
        $(kind + "-visible-count").textContent = "";
        $(kind + "-error-text").textContent = "";
        if ($(kind === "suppliers" ? "supplier-dialog" : "contract-dialog").open) $(kind === "suppliers" ? "supplier-dialog" : "contract-dialog").close();
      }
    }
    if (!can("manage_contracts")) {
      state.documentsRequest++;
      state.documentsContract = null;
      state.documents = [];
      state.documentsReady = false;
      state.editingContract = null;
      $("documents-list").replaceChildren();
      $("job-contract").replaceChildren();
      if ($("documents-dialog").open) $("documents-dialog").close();
    }
    $("job-contract-field").hidden = !can("manage_contracts");
    selectTab(availableTabs().includes(state.activeTab) ? state.activeTab : "jobs");
    renderJobs();
    renderEntries();
    return true;
  }

  async function refreshSession() {
    return applySession(await api("/api/session"));
  }

  function selectTab(name, moveFocus = false) {
    if (!availableTabs().includes(name)) return;
    state.activeTab = name;
    tabs.forEach((tab) => {
      const selected = tab === name;
      $("tab-" + tab).classList.toggle("is-active", selected);
      $("tab-" + tab).setAttribute("aria-selected", String(selected));
      $("tab-" + tab).tabIndex = selected ? 0 : -1;
      $("panel-" + tab).hidden = !selected;
    });
    if (moveFocus) $("tab-" + name).focus();
    if (name === "users" && !state.usersReady) refreshUsers();
    if (name === "audit" && !state.auditReady && !state.auditLoading) refreshAudit();
    if (["suppliers", "contracts"].includes(name) && !state[name + "Ready"] && !state[name + "Loading"]) refreshRegistry(name);
  }

  function renderStats() {
    if (state.jobsReady) {
      const active = state.jobs.filter((job) => job.status === "active").length;
      $("stat-active").textContent = active;
      $("stat-jobs").textContent = state.jobs.length;
      $("nav-jobs-count").textContent = state.jobs.length;
      $("jobs-count").textContent = state.jobs.length;
      $("stat-active-caption").textContent = active === 1 ? "Una commessa prende forma" : "Il lavoro che prende forma";
    }
    if (state.todayMinutes !== null) {
      const h = Math.floor(state.todayMinutes / 60);
      const m = state.todayMinutes % 60;
      $("stat-today").replaceChildren(node("span", "", h), node("span", "unit", "h"));
      if (m > 0) $("stat-today").append(node("span", "minute-value", String(m).padStart(2, "0")), node("span", "unit", "m"));
      $("stat-today").setAttribute("aria-label", minutesLabel(state.todayMinutes));
      $("stat-today-caption").textContent = state.todayCount === 1 ? "1 registrazione nella giornata" : `${state.todayCount} registrazioni nella giornata`;
    }
  }

  function jobIdentity(code, name) {
    const cell = node("div", "job-cell");
    const identity = node("div", "job-identity");
    identity.append(node("strong", "", name), node("span", "", code));
    cell.append(identity);
    return cell;
  }

  function action(label, ariaLabel, handler, danger = false) {
    const button = node("button", "row-button" + (danger ? " row-button-danger" : ""), label);
    button.type = "button";
    button.setAttribute("aria-label", ariaLabel);
    button.addEventListener("click", handler);
    return button;
  }

  function renderJobs() {
    if (!state.jobsReady) return;
    $("jobs-loading").hidden = true;
    const search = $("job-search").value.trim().toLocaleLowerCase("it-IT");
    const filter = $("job-filter").value;
    const jobs = state.jobs.filter((job) => (!filter || job.status === filter) && [job.code, job.name, job.client, ...(can("manage_contracts") ? [job.contract_code, job.contract_title, job.supplier_name] : [])].some((value) => (value || "").toLocaleLowerCase("it-IT").includes(search)));
    $("jobs-body").replaceChildren();
    for (const job of jobs) {
      const row = node("tr");
      const nameCell = node("td");
      nameCell.append(jobIdentity(job.code, job.name));
      if (can("manage_contracts") && job.contract_id) nameCell.querySelector(".job-identity").append(node("span", "job-contract-label", `Contratto ${job.contract_code || job.contract_id}${job.supplier_name ? " · " + job.supplier_name : ""}`));
      const clientCell = node("td", job.client ? "" : "no-value", job.client || "—");
      const statusCell = node("td");
      statusCell.append(node("span", "status-badge status-" + (Object.hasOwn(states, job.status) ? job.status : "planned"), states[job.status] || "Da avviare"));
      const hoursCell = node("td", "hours-cell", minutesLabel(job.total_minutes));
      const actionsCell = node("td");
      const actions = node("div", "row-actions");
      if (can("manage_jobs")) actions.append(action("Modifica", "Modifica la commessa " + job.name, () => openJob(job)), node("span", "row-divider"));
      if (can("manage_assignments")) {
        const count = Number(job.assigned_users) || 0;
        actions.append(action("Abilitazioni · " + count, "Gestisci le abilitazioni della commessa " + job.name + ": " + count + " utenti", () => openAssignments(job)), node("span", "row-divider"));
      }
      actions.append(action("+ Ore", "Registra ore per " + job.name, () => openEntry(null, job.id)));
      actionsCell.append(actions);
      row.append(nameCell, clientCell, statusCell, hoursCell, actionsCell);
      $("jobs-body").append(row);
    }
    const empty = jobs.length === 0;
    const noJobs = state.jobs.length === 0;
    $("jobs-table-wrap").hidden = empty;
    $("jobs-empty").hidden = !empty;
    $("jobs-table-footer").hidden = empty;
    $("jobs-empty-title").textContent = noJobs ? can("manage_jobs") ? "La prossima commessa parte da qui." : "Nessuna commessa abilitata." : "Nessuna commessa corrisponde alla ricerca.";
    $("jobs-empty-description").textContent = noJobs ? can("manage_jobs") ? "Aggiungi la prima commessa, scegli il suo stato e inizia a registrare il lavoro." : "Un gestore deve abilitarti a una commessa prima che tu possa registrare il lavoro svolto. Le ore già inserite restano nel tuo registro." : "Prova con un altro nome, codice o cliente, oppure cambia il filtro di stato.";
    $("empty-new-job").hidden = !noJobs || !can("manage_jobs");
    $("clear-filters").hidden = noJobs;
    $("jobs-visible-count").textContent = `${jobs.length} ${jobs.length === 1 ? "commessa" : "commesse"}${jobs.length !== state.jobs.length ? " su " + state.jobs.length : ""}`;
    renderStats();
  }

  function renderEntries() {
    if (!state.entriesReady) return;
    $("entries-loading").hidden = true;
    $("entries-body").replaceChildren();
    for (const entry of state.entries) {
      const row = node("tr");
      const nameCell = node("td");
      nameCell.append(jobIdentity(entry.job_code, entry.job_name));
      const noteCell = node("td", entry.note ? "entry-note" : "no-value", entry.note || "—");
      noteCell.title = entry.note || "Nessuna nota";
      const hoursCell = node("td", "hours-cell", minutesLabel(entry.minutes));
      row.append(nameCell, noteCell, hoursCell);
      if (can("edit_time_entries")) {
        const actionsCell = node("td");
        const actions = node("div", "row-actions");
        actions.append(action("Modifica", "Modifica le ore per " + entry.job_name, () => openEntry(entry)), node("span", "row-divider"), action("Elimina", "Elimina le ore per " + entry.job_name, () => openDelete(entry), true));
        actionsCell.append(actions);
        row.append(actionsCell);
      }
      $("entries-body").append(row);
    }
    $("entries-table-wrap").hidden = state.entries.length === 0;
    $("entries-empty").hidden = state.entries.length !== 0;
    $("day-total").textContent = minutesLabel(state.entries.reduce((sum, entry) => sum + Number(entry.minutes), 0));
    $("day-entries-count").textContent = `${state.entries.length} ${state.entries.length === 1 ? "registrazione" : "registrazioni"}`;
    $("go-today").disabled = state.selectedDate === localDate(new Date());
    renderStats();
  }

  function renderUsers() {
    if (!can("manage_users") || !state.usersReady) return;
    $("users-loading").hidden = true;
    $("users-table-wrap").hidden = state.users.length === 0;
    $("users-body").replaceChildren();
    for (const user of state.users) {
      const row = node("tr");
      const identityCell = node("td");
      const identity = node("div", "job-identity");
      identity.append(node("strong", "", user.name), node("span", "", user.email));
      if (user.email === state.session.email) identity.append(node("span", "user-self", "Il tuo account"));
      identityCell.append(identity);
      const roleCell = node("td");
      roleCell.append(node("span", "role-badge", roles[user.role] || "Operatore"));
      const accessCell = node("td");
      accessCell.append(node("span", "status-badge " + (user.active ? "status-active" : "status-cancelled"), user.active ? "Attivo" : "Disattivato"));
      const actionsCell = node("td");
      actionsCell.append(action("Modifica", "Modifica l’utente " + user.name, () => openUser(user)));
      row.append(identityCell, roleCell, accessCell, actionsCell);
      $("users-body").append(row);
    }
    $("users-count").textContent = state.users.length;
    const active = state.users.filter((user) => user.active).length;
    $("users-active-count").textContent = `${active} ${active === 1 ? "utente attivo" : "utenti attivi"}`;
  }

  function euroLabel(cents) {
    if (cents === null || cents === undefined || cents === "") return "—";
    return new Intl.NumberFormat("it-IT", { style: "currency", currency: "EUR" }).format(Number(cents) / 100);
  }

  function valueToCents(value) {
    const normalized = value.trim().replace(",", ".");
    if (!normalized) return "";
    if (!/^\d{1,10}(?:\.\d{1,2})?$/.test(normalized)) throw new Error("Inserisci un importo positivo, senza separatori delle migliaia e con al massimo due decimali.");
    const [whole, fraction = ""] = normalized.split(".");
    const cents = BigInt(whole) * 100n + BigInt(fraction.padEnd(2, "0"));
    if (cents > 999999999999n) throw new Error("Il valore massimo è 9.999.999.999,99 €.");
    return String(cents);
  }

  function activeSupplier(supplier) {
    return supplier.active === true || supplier.active === 1 || supplier.active === "1";
  }

  function renderSuppliers() {
    if (!can("manage_suppliers") || !state.suppliersReady) return;
    const search = $("supplier-search").value.trim().toLocaleLowerCase("it-IT");
    const filter = $("supplier-filter").value;
    const suppliers = state.suppliers.filter((supplier) => (!filter || String(Number(activeSupplier(supplier))) === filter) && [supplier.code, supplier.name, supplier.vat_number].some((value) => (value || "").toLocaleLowerCase("it-IT").includes(search)));
    $("suppliers-body").replaceChildren();
    for (const supplier of suppliers) {
      const row = node("tr");
      const identity = node("td");
      identity.append(jobIdentity(supplier.code, supplier.name));
      const contact = node("td", "registry-contact");
      contact.append(node("span", "", supplier.email || "—"));
      if (supplier.phone) contact.append(node("span", "", supplier.phone));
      const status = node("td");
      status.append(node("span", "status-badge " + (activeSupplier(supplier) ? "status-active" : "status-cancelled"), activeSupplier(supplier) ? "Attivo" : "Disattivato"));
      const actions = node("td");
      actions.append(action("Modifica", "Modifica il fornitore " + supplier.name, () => openSupplier(supplier)));
      row.append(identity, node("td", supplier.vat_number ? "" : "no-value", supplier.vat_number || "—"), contact, status, actions);
      $("suppliers-body").append(row);
    }
    $("suppliers-count").textContent = state.suppliers.length;
    $("suppliers-table-wrap").hidden = suppliers.length === 0;
    $("suppliers-empty").hidden = suppliers.length !== 0;
    $("suppliers-empty-title").textContent = state.suppliers.length ? "Nessun fornitore corrisponde alla ricerca." : "Il primo partner parte da qui.";
    $("suppliers-empty-text").textContent = state.suppliers.length ? "Cambia la ricerca o il filtro di stato." : "Censisci un fornitore per associarlo ai tuoi contratti.";
    $("suppliers-visible-count").textContent = `${suppliers.length} di ${state.suppliers.length} fornitori`;
  }

  function contractPeriod(contract) {
    if (contract.starts_on && contract.ends_on) return `${dateLabel(contract.starts_on)} – ${dateLabel(contract.ends_on)}`;
    if (contract.starts_on) return `Dal ${dateLabel(contract.starts_on)}`;
    if (contract.ends_on) return `Fino al ${dateLabel(contract.ends_on)}`;
    return "Non specificata";
  }

  function renderContracts() {
    if (!can("manage_contracts") || !state.contractsReady) return;
    const search = $("contract-search").value.trim().toLocaleLowerCase("it-IT");
    const filter = $("contract-filter").value;
    const contracts = state.contracts.filter((contract) => (!filter || contract.status === filter) && [contract.code, contract.title, contract.supplier_name].some((value) => (value || "").toLocaleLowerCase("it-IT").includes(search)));
    $("contracts-body").replaceChildren();
    for (const contract of contracts) {
      const row = node("tr");
      const identity = node("td");
      const info = jobIdentity(contract.code, contract.title);
      info.querySelector(".job-identity").append(node("span", "contract-supplier-label", contract.supplier_name || "Fornitore non disponibile"));
      identity.append(info);
      const status = node("td");
      const statusClass = { draft: "planned", active: "active", expired: "paused", closed: "completed", cancelled: "cancelled" }[contract.status] || "planned";
      status.append(node("span", "status-badge status-" + statusClass, contractStates[contract.status] || contract.status));
      const actionsCell = node("td");
      const actions = node("div", "row-actions");
      actions.append(action("Modifica", "Modifica il contratto " + contract.title, () => openContract(contract)), node("span", "row-divider"), action(`PDF · ${Number(contract.documents_count) || 0}`, "Apri i documenti del contratto " + contract.title, () => openDocuments(contract)));
      actionsCell.append(actions);
      row.append(identity, node("td", "contract-period", contractPeriod(contract)), node("td", "contract-value", euroLabel(contract.value_cents)), status, actionsCell);
      $("contracts-body").append(row);
    }
    $("contracts-count").textContent = state.contracts.length;
    $("contracts-table-wrap").hidden = contracts.length === 0;
    $("contracts-empty").hidden = contracts.length !== 0;
    $("contracts-empty-title").textContent = state.contracts.length ? "Nessun contratto corrisponde alla ricerca." : "Ogni commessa ha un punto di partenza.";
    $("contracts-empty-text").textContent = state.contracts.length ? "Cambia la ricerca o il filtro di stato." : "Crea un contratto e raccogli i documenti del tuo accordo.";
    $("contracts-visible-count").textContent = `${contracts.length} di ${state.contracts.length} contratti`;
  }

  async function refreshRegistry(kind) {
    const permission = kind === "suppliers" ? "manage_suppliers" : "manage_contracts";
    if (!can(permission)) return false;
    const request = ++state[kind + "Request"];
    state[kind + "Loading"] = true;
    $(kind + "-loading").hidden = false;
    $(kind + "-error").hidden = true;
    $(kind + "-retry").disabled = true;
    $(kind + "-empty").hidden = true;
    if (!state[kind + "Ready"]) $(kind + "-table-wrap").hidden = true;
    try {
      const data = await api("/api/" + kind);
      if (request !== state[kind + "Request"] || !can(permission)) return false;
      if (!Array.isArray(data[kind])) throw new Error("Il server ha restituito un elenco non valido. Riprova.");
      state[kind] = data[kind];
      state[kind + "Ready"] = true;
      if (kind === "suppliers") {
        renderSuppliers();
        if ($("contract-dialog").open) renderContractSuppliers();
      } else {
        renderContracts();
        if ($("job-dialog").open) renderJobContracts();
      }
      return true;
    } catch (error) {
      if (request === state[kind + "Request"]) {
        $(kind + "-error-text").textContent = error.message;
        $(kind + "-error").hidden = false;
      }
      return false;
    } finally {
      if (request === state[kind + "Request"]) {
        state[kind + "Loading"] = false;
        $(kind + "-loading").hidden = true;
        $(kind + "-retry").disabled = false;
      }
    }
  }

  function renderJobContracts(initial = false) {
    const current = initial ? state.editingJob?.contract_id || "" : $("job-contract").value;
    const first = node("option", "", "Nessun contratto");
    first.value = "";
    const options = state.contracts.filter((contract) => !["closed", "cancelled"].includes(contract.status) || String(contract.id) === String(current)).map((contract) => {
      const option = node("option", "", `${contract.code} · ${contract.title} · ${contractStates[contract.status] || contract.status}`);
      option.value = contract.id;
      return option;
    });
    if (current && !options.some((option) => String(option.value) === String(current))) {
      const fallback = node("option", "", `${state.editingJob?.contract_code || "Contratto"} · ${state.editingJob?.contract_title || "Collegamento esistente"}`);
      fallback.value = current;
      options.push(fallback);
    }
    $("job-contract").replaceChildren(first, ...options);
    $("job-contract").value = current;
    $("job-contract").disabled = !state.contractsReady || busyDialogs.has("job-dialog");
    $("job-contract-help").textContent = state.contractsReady ? "Collega la commessa a un contratto censito. I contratti chiusi o annullati conservano i collegamenti esistenti." : "Caricamento dell’elenco dei contratti necessario per salvare la commessa.";
    $("job-contract-retry").hidden = state.contractsReady;
    $("job-save").disabled = !state.contractsReady || busyDialogs.has("job-dialog");
  }

  async function loadJobContracts() {
    $("job-contract-retry").disabled = true;
    const loaded = await refreshRegistry("contracts");
    if (!$("job-dialog").open || !can("manage_jobs")) return;
    renderJobContracts();
    $("job-contract-retry").disabled = false;
    if (!loaded && !state.contractsReady) $("job-contract-help").textContent = "L’elenco dei contratti non è disponibile. Ricaricalo prima di salvare.";
  }

  function openSupplier(supplier = null) {
    if (!requirePermission("manage_suppliers") || busyDialogs.has("supplier-dialog")) return;
    $("supplier-form").reset();
    $("supplier-error").hidden = true;
    $("supplier-id").value = supplier?.id || "";
    $("supplier-dialog-title").textContent = supplier ? "Modifica fornitore" : "Nuovo fornitore";
    $("supplier-save").textContent = supplier ? "Salva modifiche" : "Crea fornitore";
    if (supplier) {
      ["code", "name", "vat_number", "email", "phone", "address", "notes"].forEach((key) => { $("supplier-" + key).value = supplier[key] || ""; });
      $("supplier-active").value = activeSupplier(supplier) ? "1" : "0";
    }
    $("supplier-dialog").showModal();
    $("supplier-code").focus();
  }

  function renderContractSuppliers(initial = false) {
    const current = initial ? state.editingContract?.supplier_id || "" : $("contract-supplier").value;
    const first = node("option", "", "Seleziona un fornitore attivo");
    first.value = "";
    const options = state.suppliers.filter((supplier) => activeSupplier(supplier) || String(supplier.id) === String(state.editingContract?.supplier_id)).map((supplier) => {
      const option = node("option", "", `${supplier.code} · ${supplier.name}${activeSupplier(supplier) ? "" : " (disattivato)"}`);
      option.value = supplier.id;
      return option;
    });
    if (current && !options.some((option) => String(option.value) === String(current))) {
      const fallback = node("option", "", state.editingContract?.supplier_name || "Fornitore del contratto");
      fallback.value = current;
      options.push(fallback);
    }
    $("contract-supplier").replaceChildren(first, ...options);
    $("contract-supplier").value = current;
    $("contract-supplier").disabled = !state.suppliersReady || busyDialogs.has("contract-dialog");
    $("contract-save").disabled = !state.suppliersReady || busyDialogs.has("contract-dialog");
    $("contract-supplier-retry").hidden = state.suppliersReady;
    $("contract-supplier-help").textContent = !state.suppliersReady ? "Caricamento dell’elenco dei fornitori necessario per salvare il contratto." : options.length ? "Puoi scegliere un fornitore attivo. Il fornitore già collegato resta disponibile in modifica." : "Crea prima un fornitore attivo nella sezione Fornitori.";
  }

  async function loadContractSuppliers() {
    $("contract-supplier-retry").disabled = true;
    const loaded = await refreshRegistry("suppliers");
    if (!$("contract-dialog").open || !can("manage_contracts")) return;
    renderContractSuppliers();
    $("contract-supplier-retry").disabled = false;
    if (!loaded && !state.suppliersReady) $("contract-supplier-help").textContent = "L’elenco dei fornitori non è disponibile. Ricaricalo prima di salvare.";
  }

  function openContract(contract = null) {
    if (!requirePermission("manage_contracts") || busyDialogs.has("contract-dialog")) return;
    state.editingContract = contract;
    $("contract-form").reset();
    $("contract-value").setCustomValidity("");
    $("contract-ends_on").setCustomValidity("");
    $("contract-error").hidden = true;
    $("contract-id").value = contract?.id || "";
    $("contract-dialog-title").textContent = contract ? "Modifica contratto" : "Nuovo contratto";
    $("contract-save").textContent = contract ? "Salva modifiche" : "Crea contratto";
    if (contract) {
      ["code", "title", "status", "starts_on", "ends_on", "notes"].forEach((key) => { $("contract-" + key).value = contract[key] || ""; });
      if (contract.value_cents !== null && contract.value_cents !== undefined && contract.value_cents !== "") {
        const cents = BigInt(contract.value_cents);
        $("contract-value").value = `${cents / 100n},${String(cents % 100n).padStart(2, "0")}`;
      }
    }
    renderContractSuppliers(true);
    $("contract-dialog").showModal();
    $("contract-code").focus();
    if (!state.suppliersReady) loadContractSuppliers();
  }

  function fileSize(bytes) {
    return Number(bytes) < 1024 * 1024 ? `${(Number(bytes) / 1024).toLocaleString("it-IT", { maximumFractionDigits: 1 })} KiB` : `${(Number(bytes) / (1024 * 1024)).toLocaleString("it-IT", { maximumFractionDigits: 2 })} MiB`;
  }

  function renderDocuments() {
    if (!state.documentsReady || !state.documentsContract || !can("manage_contracts")) return;
    $("documents-list").replaceChildren();
    for (const document of state.documents) {
      const item = node("li", "document-item");
      const info = node("div", "document-identity");
      info.append(node("strong", "", document.file_name), node("span", "", `${fileSize(document.byte_size)} · ${auditTime(document.created_at)}`));
      const link = node("a", "row-button document-download", "Scarica PDF");
      link.href = `/api/contract-documents/${encodeURIComponent(document.id)}/download`;
      link.setAttribute("aria-label", "Scarica il documento " + document.file_name);
      item.append(info, link);
      $("documents-list").append(item);
    }
    $("documents-empty").hidden = state.documents.length !== 0;
  }

  async function loadDocuments() {
    if (!state.documentsContract || !can("manage_contracts")) return;
    const request = ++state.documentsRequest;
    const id = state.documentsContract.id;
    state.documentsReady = false;
    $("documents-loading").hidden = false;
    $("documents-error").hidden = true;
    $("documents-retry").hidden = true;
    $("documents-empty").hidden = true;
    $("documents-list").replaceChildren();
    try {
      const data = await api(`/api/contracts/${encodeURIComponent(id)}/documents`);
      if (request !== state.documentsRequest || !can("manage_contracts")) return;
      if (!Array.isArray(data.documents)) throw new Error("Il server ha restituito un elenco non valido. Riprova.");
      state.documents = data.documents;
      state.documentsReady = true;
      renderDocuments();
    } catch (error) {
      if (request !== state.documentsRequest) return;
      dialogError("documents-error", error);
      $("documents-retry").hidden = false;
    } finally {
      if (request === state.documentsRequest) $("documents-loading").hidden = true;
    }
  }

  function openDocuments(contract) {
    if (!requirePermission("manage_contracts") || busyDialogs.has("documents-dialog")) return;
    state.documentsContract = contract;
    state.documents = [];
    state.documentsReady = false;
    $("document-form").reset();
    $("document-file").setCustomValidity("");
    $("documents-dialog-title").textContent = "PDF · " + contract.code;
    $("documents-dialog-description").textContent = contract.title + ". Scarica i documenti originali o aggiungi un PDF all’archivio.";
    $("documents-dialog").showModal();
    loadDocuments();
  }

  function assignmentPeriod(assignment) {
    if (assignment.starts_on && assignment.ends_on) return `${dateLabel(assignment.starts_on)} – ${dateLabel(assignment.ends_on)}`;
    if (assignment.starts_on) return `Dal ${dateLabel(assignment.starts_on)}, senza scadenza`;
    if (assignment.ends_on) return `Fino al ${dateLabel(assignment.ends_on)}`;
    return "Da subito, senza scadenza";
  }

  function assignmentStatus(assignment) {
    const today = localDate(new Date());
    if (!assignment.active) return ["Utente disattivato", "cancelled"];
    if (assignment.starts_on && assignment.starts_on > today) return ["Programmata", "planned"];
    if (assignment.ends_on && assignment.ends_on < today) return ["Scaduta", "completed"];
    return ["Abilitato", "active"];
  }

  function setAssignmentBusy(busy) {
    if (busy) busyDialogs.add("assignment-dialog"); else busyDialogs.delete("assignment-dialog");
    $("assignment-dialog").setAttribute("aria-busy", String(busy));
    $("assignment-dialog").querySelectorAll("input, select, button").forEach((element) => { element.disabled = busy || element.dataset.assignmentInactive === "1"; });
    $("assignment-user").disabled = busy || Boolean(state.editingAssignment) || !state.assignmentsReady;
    $("assignment-save").disabled = busy || !state.assignmentsReady;
    $("assignment-save").textContent = busy ? "Salvataggio…" : state.editingAssignment ? "Salva abilitazione" : "Abilita utente";
    $("assignment-revoke-save").textContent = busy ? "Revoca in corso…" : "Conferma revoca";
  }

  function resetAssignmentForm(assignment = null) {
    state.editingAssignment = assignment;
    $("assignment-form").reset();
    $("assignment-end").setCustomValidity("");
    const first = node("option", "", "Seleziona un utente attivo");
    first.value = "";
    const users = state.assignmentUsers.filter((user) => user.active || assignment && Number(user.id) === Number(assignment.user_id));
    const options = users.map((user) => {
      const option = node("option", "", `${user.name} · ${user.email}${user.active ? "" : " (disattivato)"}`);
      option.value = user.id;
      return option;
    });
    $("assignment-user").replaceChildren(first, ...options);
    if (assignment) {
      $("assignment-user").value = assignment.user_id;
      $("assignment-start").value = assignment.starts_on || "";
      $("assignment-end").value = assignment.ends_on || "";
    }
    $("assignment-form-title").textContent = assignment ? "Modifica periodo di abilitazione" : "Abilita un utente";
    $("assignment-reset").hidden = !assignment;
    setAssignmentBusy(busyDialogs.has("assignment-dialog"));
  }

  function renderAssignments() {
    if (!state.assignmentsReady || !state.assignmentJob || !can("manage_assignments")) return;
    $("assignment-loading").hidden = true;
    $("assignment-retry-wrap").hidden = true;
    $("assignment-list").replaceChildren();
    for (const assignment of state.assignments) {
      const item = node("li", "assignment-item");
      const identity = node("div", "assignment-identity");
      identity.append(node("strong", "", assignment.name), node("span", "assignment-email", assignment.email));
      const [label, status] = assignmentStatus(assignment);
      const meta = node("div", "assignment-meta");
      meta.append(node("span", "status-badge status-" + status, label), node("span", "assignment-period", assignmentPeriod(assignment)));
      if (assignment.role === "manager") meta.append(node("span", "field-help", "Gestore · ha già accesso a tutte le commesse"));
      identity.append(meta);
      const controls = node("div", "row-actions");
      const editButton = action("Modifica", "Modifica abilitazione di " + assignment.name, () => {
        if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
        state.revokeAssignment = null;
        $("assignment-revoke").hidden = true;
        resetAssignmentForm(assignment);
        $("assignment-start").focus();
      });
      editButton.disabled = !assignment.active;
      if (!assignment.active) editButton.title = "Riattiva l’utente nella sezione Utenti per modificare l’abilitazione.";
      editButton.dataset.assignmentInactive = assignment.active ? "0" : "1";
      controls.append(editButton, action("Revoca", "Revoca abilitazione di " + assignment.name, () => {
        if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
        state.revokeAssignment = assignment;
        $("assignment-revoke-description").textContent = `Revocare l’abilitazione di ${assignment.name}? ${assignment.role === "manager" ? "Come gestore manterrà l’accesso alla commessa." : "Non potrà aggiungere altro lavoro su questa commessa. Le ore già registrate saranno conservate."}`;
        $("assignment-revoke").hidden = false;
        $("assignment-revoke-cancel").focus();
      }, true));
      item.append(identity, controls);
      $("assignment-list").append(item);
    }
    $("assignment-empty").hidden = state.assignments.length !== 0;
    setAssignmentBusy(busyDialogs.has("assignment-dialog"));
  }

  async function loadAssignments() {
    if (!state.assignmentJob || !can("manage_assignments")) return;
    const request = ++state.assignmentRequest;
    const jobId = state.assignmentJob.id;
    state.assignmentsReady = false;
    $("assignment-loading").hidden = false;
    $("assignment-loading").textContent = "Caricamento delle abilitazioni…";
    $("assignment-error").hidden = true;
    $("assignment-retry-wrap").hidden = true;
    setAssignmentBusy(busyDialogs.has("assignment-dialog"));
    try {
      const [data, users] = await Promise.all([api(`/api/jobs/${encodeURIComponent(jobId)}/assignments`), api("/api/users")]);
      if (request !== state.assignmentRequest || !state.assignmentJob || !can("manage_assignments")) return;
      state.assignments = data.assignments;
      state.assignmentUsers = users.users;
      state.assignmentsReady = true;
      renderAssignments();
      resetAssignmentForm();
    } catch (error) {
      if (request !== state.assignmentRequest) return;
      $("assignment-loading").hidden = true;
      $("assignment-retry-wrap").hidden = false;
      dialogError("assignment-error", error);
    }
  }

  function openAssignments(job) {
    if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
    state.assignmentJob = job;
    state.assignments = [];
    state.assignmentUsers = [];
    state.assignmentsReady = false;
    state.revokeAssignment = null;
    $("assignment-dialog-title").textContent = "Abilitazioni · " + job.code;
    $("assignment-dialog-description").textContent = `${job.name}. Scegli chi può utilizzare questa commessa. Una revoca conserva tutte le ore già registrate.`;
    $("assignment-list").replaceChildren();
    $("assignment-empty").hidden = true;
    $("assignment-revoke").hidden = true;
    resetAssignmentForm();
    $("assignment-dialog").showModal();
    loadAssignments();
  }

  const auditActions = {
    "jobs.create": "Commessa creata", "jobs.update": "Commessa aggiornata",
    "time_entries.create": "Ore registrate", "time_entries.update": "Ore aggiornate", "time_entries.delete": "Ore eliminate",
    "assignments.create": "Utente abilitato", "assignments.update": "Abilitazione aggiornata", "assignments.delete": "Abilitazione revocata",
    "suppliers.create": "Fornitore creato", "suppliers.update": "Fornitore aggiornato",
    "contracts.create": "Contratto creato", "contracts.update": "Contratto aggiornato", "contract_documents.create": "PDF contrattuale archiviato",
    "users.create": "Utente creato", "users.update": "Utente aggiornato", "system.migration": "Database aggiornato"
  };
  const auditTypes = { job: "Commessa", time_entry: "Registrazione ore", assignment: "Abilitazione", user: "Utente", supplier: "Fornitore", contract: "Contratto", contract_document: "Documento contrattuale", system: "Sistema" };
  const auditLabels = {
    id: "ID", user_id: "ID utente", job_id: "ID commessa", owner_id: "ID autore", owner: "Account", email: "Email", name: "Nome", role: "Ruolo", active: "Accesso attivo",
    title: "Titolo", supplier_id: "ID fornitore", supplier_name: "Fornitore", contract_id: "ID contratto", contract_code: "Codice contratto", contract_title: "Contratto", vat_number: "Partita IVA", phone: "Telefono", address: "Indirizzo", notes: "Note", value_cents: "Valore", currency: "Valuta", file_name: "Nome documento", byte_size: "Dimensione",
    code: "Codice", status: "Stato", description: "Descrizione", client: "Cliente", date: "Giornata", minutes: "Ore lavorate", starts_on: "Abilitato dal", ends_on: "Abilitato fino al",
    user_name: "Utente", user_email: "Email utente", job_name: "Commessa", job_code: "Codice commessa", note: "Attività / nota", version: "Versione", schema_version: "Versione database", migrated_entries: "Registrazioni migrate", migrated_users: "Utenti migrati", mapped_entries: "Registrazioni collegate agli utenti", archived_entries: "Registrazioni storiche conservate"
  };

  function auditTime(value) {
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString("it-IT", { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
  }

  function auditValue(key, value) {
    if (value === null || value === undefined || value === "") return "—";
    if (key === "minutes") return minutesLabel(value);
    if (key === "value_cents") return euroLabel(value);
    if (key === "byte_size") return fileSize(value);
    if (key === "role") return roles[value] || String(value);
    if (key === "status") return states[value] || contractStates[value] || String(value);
    if (key === "active") return value === true || value === 1 || value === "1" ? "Attivo" : "Disattivato";
    if (["date", "starts_on", "ends_on"].includes(key) && validDate(String(value))) return dateLabel(value);
    if (typeof value === "boolean") return value ? "Sì" : "No";
    return String(value);
  }

  function auditSnapshot(snapshot, title) {
    const section = node("section", "audit-snapshot");
    section.append(node("h4", "", title));
    if (!snapshot || typeof snapshot !== "object") {
      section.append(node("p", "no-value", title === "Prima" ? "Nessun dato precedente" : "Elemento rimosso"));
      return section;
    }
    const list = node("dl");
    function addFields(fields, prefix = "", depth = 0) {
      for (const [key, value] of Object.entries(fields)) {
        if (/password|hash|salt|token|secret/i.test(key)) continue;
        const label = prefix + (auditLabels[key] || key.replaceAll("_", " "));
        if (value && typeof value === "object" && !Array.isArray(value) && depth < 3) { addFields(value, label + " · ", depth + 1); continue; }
        list.append(node("dt", "", label), node("dd", "", Array.isArray(value) ? value.map((item) => typeof item === "object" ? "Elemento" : auditValue(key, item)).join(", ") : auditValue(key, value)));
      }
    }
    addFields(snapshot);
    section.append(list);
    return section;
  }

  function renderAudit() {
    if (!can("view_audit")) return;
    $("audit-list").replaceChildren();
    for (const event of state.auditEvents) {
      const item = node("li", "audit-event");
      const top = node("div", "audit-event-heading");
      const title = node("h3", "", auditActions[event.action] || event.action);
      const time = node("time", "", auditTime(event.created_at));
      time.dateTime = event.created_at;
      top.append(title, time);
      const snapshot = event.after || event.before || {};
      const subject = snapshot.job_name || snapshot.name || snapshot.job_code || snapshot.code;
      const type = auditTypes[event.entity_type] || "Elemento";
      const object = `${type}${event.entity_id ? " #" + event.entity_id : ""}${subject ? " · " + subject : ""}`;
      const actor = event.actor_name || event.actor_email || "Sistema";
      const byline = node("p", "audit-byline", actor + (event.actor_name && event.actor_email ? " · " + event.actor_email : ""));
      const details = node("details", "audit-details");
      details.append(node("summary", "", "Visualizza prima e dopo"));
      const snapshots = node("div", "audit-snapshots");
      snapshots.append(auditSnapshot(event.before, "Prima"), auditSnapshot(event.after, "Dopo"));
      details.append(snapshots);
      item.append(top, node("p", "audit-object", object), byline, details);
      $("audit-list").append(item);
    }
    $("audit-empty").hidden = !state.auditReady || state.auditEvents.length > 0;
    $("audit-more").hidden = !state.auditCursor;
    $("audit-count").textContent = `${state.auditEvents.length} ${state.auditEvents.length === 1 ? "modifica caricata" : "modifiche caricate"}`;
  }

  async function refreshAudit(more = false) {
    if (!can("view_audit") || more && (!state.auditCursor || state.auditLoading)) return;
    const request = ++state.auditRequest;
    const query = new URLSearchParams({ limit: "30" });
    if ($("audit-filter").value) query.set("entity_type", $("audit-filter").value);
    if (more) query.set("before_id", state.auditCursor);
    else { state.auditEvents = []; state.auditCursor = null; state.auditReady = false; renderAudit(); }
    state.auditLoading = true;
    state.auditRetryMore = more;
    $("audit-error").hidden = true;
    $("audit-loading").hidden = false;
    $("audit-loading").textContent = more ? "Caricamento delle modifiche precedenti…" : "Caricamento dello storico…";
    $("audit-more").disabled = true;
    $("audit-refresh").disabled = true;
    try {
      const data = await api("/api/audit?" + query.toString());
      if (request !== state.auditRequest || !can("view_audit")) return;
      state.auditEvents = more ? [...state.auditEvents, ...data.events] : data.events;
      state.auditCursor = data.next_before_id;
      state.auditReady = true;
      renderAudit();
    } catch (error) {
      if (request !== state.auditRequest) return;
      $("audit-error-text").textContent = error.message;
      $("audit-error").hidden = false;
    } finally {
      if (request === state.auditRequest) {
        state.auditLoading = false;
        $("audit-loading").hidden = true;
        $("audit-more").disabled = false;
        $("audit-refresh").disabled = false;
      }
    }
  }

  async function refreshUsers() {
    if (!can("manage_users")) return;
    const request = ++state.usersRequest;
    $("users-loading").hidden = false;
    $("users-loading").textContent = "Caricamento degli utenti…";
    try {
      const data = await api("/api/users");
      if (request !== state.usersRequest || !can("manage_users")) return;
      state.users = data.users;
      state.usersReady = true;
      renderUsers();
    } catch (error) {
      if (request !== state.usersRequest) return;
      $("users-loading").textContent = "Gli utenti non sono disponibili. Usa Riprova per ricaricare i dati.";
      showLoadError(error);
    }
  }

  async function refreshEntries() {
    const request = ++state.entriesRequest;
    const date = state.selectedDate;
    state.entriesReady = false;
    $("entries-loading").hidden = false;
    $("entries-loading").textContent = "Caricamento delle ore…";
    $("entries-table-wrap").hidden = true;
    $("entries-empty").hidden = true;
    $("day-total").textContent = "—";
    $("day-entries-count").textContent = "";
    try {
      const data = await api("/api/time-entries?date=" + encodeURIComponent(date));
      if (request !== state.entriesRequest) return;
      state.entries = data.entries;
      state.entriesReady = true;
      if (date === localDate(new Date())) {
        state.todayMinutes = Number(data.total_minutes);
        state.todayCount = data.entries.length;
      }
      renderEntries();
    } catch (error) {
      if (request === state.entriesRequest) {
        $("entries-loading").textContent = "Le ore non sono disponibili. Usa Riprova per ricaricare i dati.";
        showLoadError(error);
      }
    }
  }

  async function refreshAll() {
    const request = ++state.refreshRequest;
    $("load-error").hidden = true;
    $("retry-load").disabled = true;
    if (!state.jobsReady) $("jobs-loading").textContent = "Caricamento delle commesse…";
    const jobsTask = api("/api/jobs").then((data) => {
      if (request !== state.refreshRequest) return;
      state.jobs = data.jobs;
      state.jobsReady = true;
      renderJobs();
    }).catch((error) => {
      if (request !== state.refreshRequest) return;
      if (!state.jobsReady) $("jobs-loading").textContent = "Le commesse non sono disponibili. Usa Riprova per ricaricare i dati.";
      showLoadError(error);
    });
    const tasks = [jobsTask, refreshEntries()];
    if (can("manage_users")) tasks.push(refreshUsers());
    if (can("manage_suppliers")) tasks.push(refreshRegistry("suppliers"));
    if (can("manage_contracts")) tasks.push(refreshRegistry("contracts"));
    if (can("view_audit") && state.activeTab === "audit") tasks.push(refreshAudit());
    if (state.selectedDate !== localDate(new Date())) {
      tasks.push(api("/api/time-entries?date=" + localDate(new Date())).then((data) => {
        if (request !== state.refreshRequest) return;
        state.todayMinutes = Number(data.total_minutes);
        state.todayCount = data.entries.length;
        renderStats();
      }).catch(showLoadError));
    }
    await Promise.all(tasks);
    if (request === state.refreshRequest) $("retry-load").disabled = false;
  }

  function dialogError(id, error) {
    $(id).textContent = error.message;
    $(id).hidden = false;
  }

  function dialogBusy(id, busy, submitId, label) {
    if (busy) busyDialogs.add(id); else busyDialogs.delete(id);
    $(id).querySelectorAll("input, select, textarea, button").forEach((element) => { element.disabled = busy; });
    $(id).setAttribute("aria-busy", String(busy));
    $(submitId).textContent = busy ? "Salvataggio…" : label;
  }

  function openJob(job = null) {
    if (!requirePermission("manage_jobs")) return;
    if (busyDialogs.has("job-dialog")) return;
    state.editingJob = job;
    $("job-form").reset();
    $("job-error").hidden = true;
    $("job-id").value = job ? job.id : "";
    $("job-dialog-title").textContent = job ? "Modifica commessa" : "Nuova commessa";
    $("job-save").textContent = job ? "Salva modifiche" : "Crea commessa";
    if (job) {
      ["code", "name", "client", "description", "status"].forEach((field) => { $("job-" + field).value = job[field] || ""; });
    }
    if (can("manage_contracts")) renderJobContracts(true);
    $("job-dialog").showModal();
    $("job-code").focus();
    if (can("manage_contracts") && !state.contractsReady) loadJobContracts();
  }

  function openEntry(entry = null, jobId = "") {
    if (entry && !requirePermission("edit_time_entries")) return;
    if (busyDialogs.has("entry-dialog")) return;
    if (!state.jobsReady) { showNotice("Attendi il caricamento delle commesse, poi riprova.", true); return; }
    if (state.jobs.length === 0) {
      if (!can("manage_jobs")) { showNotice("Nessuna commessa abilitata. Chiedi a un gestore di abilitarti alla commessa su cui devi registrare il lavoro.", true); return; }
      state.openEntryAfterJob = true;
      openJob();
      $("job-dialog-description").textContent = "Crea prima una commessa: subito dopo potrai registrare le tue ore.";
      return;
    }
    $("entry-form").reset();
    $("entry-hours").setCustomValidity("");
    $("entry-hours").removeAttribute("aria-invalid");
    $("entry-error").hidden = true;
    $("entry-id").value = entry ? entry.id : "";
    $("entry-dialog-title").textContent = entry ? "Modifica registrazione" : "Registra ore";
    $("entry-save").textContent = entry ? "Salva modifiche" : "Salva ore";
    const options = [node("option", "", "Seleziona una commessa")];
    options[0].value = "";
    for (const job of state.jobs) {
      const option = node("option", "", `${job.code} · ${job.name}`);
      option.value = job.id;
      options.push(option);
    }
    $("entry-job").replaceChildren(...options);
    $("entry-job").value = entry ? entry.job_id : jobId;
    $("entry-date").value = entry ? entry.date : state.activeTab === "hours" ? state.selectedDate : localDate(new Date());
    if (entry) {
      $("entry-hours").value = `${Math.floor(entry.minutes / 60)}:${String(entry.minutes % 60).padStart(2, "0")}`;
      $("entry-note").value = entry.note || "";
    }
    $("entry-dialog").showModal();
    if (entry || jobId) $("entry-hours").focus(); else $("entry-job").focus();
  }

  function openDelete(entry) {
    if (!requirePermission("edit_time_entries")) return;
    state.deleteEntry = entry;
    $("delete-error").hidden = true;
    $("delete-description").textContent = `${minutesLabel(entry.minutes)} su “${entry.job_name}”, il ${dateLabel(entry.date)}. La registrazione verrà rimossa dal tuo registro ore.`;
    $("delete-dialog").showModal();
  }

  function openUser(user = null) {
    if (!requirePermission("manage_users") || busyDialogs.has("user-dialog")) return;
    $("user-form").reset();
    $("user-password").setCustomValidity("");
    $("user-error").hidden = true;
    $("user-id").value = user ? user.id : "";
    $("user-dialog-title").textContent = user ? "Modifica utente" : "Nuovo utente";
    $("user-dialog-description").textContent = user ? "Aggiorna il nome, il ruolo e l’accesso dell’utente. Deve rimanere almeno un gestore attivo." : "Crea un account e scegli cosa può fare nello spazio di lavoro.";
    $("user-save").textContent = user ? "Salva modifiche" : "Crea utente";
    $("user-email").readOnly = Boolean(user);
    $("user-email-help").hidden = !user;
    $("user-password-field").hidden = Boolean(user);
    $("user-password").required = !user;
    $("user-active-field").hidden = !user;
    if (user) {
      $("user-name").value = user.name;
      $("user-email").value = user.email;
      $("user-role").value = user.role;
      $("user-active").value = user.active ? "1" : "0";
    }
    $("user-dialog").showModal();
    $("user-name").focus();
  }

  function setDate(value) {
    if (!validDate(value)) return;
    state.selectedDate = value;
    $("entry-date-filter").value = value;
    $("entries-loading").textContent = "Caricamento delle ore…";
    $("load-error").hidden = true;
    refreshEntries();
  }

  function moveDay(direction) {
    const [year, month, day] = state.selectedDate.split("-").map(Number);
    const date = new Date(0);
    date.setFullYear(year, month - 1, day);
    date.setHours(12, 0, 0, 0);
    date.setDate(date.getDate() + direction);
    const value = localDate(date);
    if (validDate(value)) setDate(value);
  }

  $("job-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_jobs")) return;
    if (busyDialogs.has("job-dialog")) return;
    ["code", "name", "client", "description"].forEach((field) => { $("job-" + field).value = $("job-" + field).value.trim(); });
    if (can("manage_contracts") && !state.contractsReady) { dialogError("job-error", new Error("Ricarica l’elenco dei contratti prima di salvare.")); return; }
    if (!$("job-form").reportValidity()) return;
    $("job-error").hidden = true;
    const id = $("job-id").value;
    const fields = Object.fromEntries(["code", "name", "client", "description", "status"].map((field) => [field, $("job-" + field).value]));
    if (can("manage_contracts")) fields.contract_id = $("job-contract").value;
    const followWithEntry = state.openEntryAfterJob && !id;
    dialogBusy("job-dialog", true, "job-save", id ? "Salva modifiche" : "Crea commessa");
    try {
      const result = await post("/api/jobs" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("job-dialog").close();
      showNotice(id ? "Commessa aggiornata. Tutto al punto." : "Commessa creata. Un nuovo punto di partenza.");
      await refreshAll();
      if (followWithEntry && state.jobs.some((job) => String(job.id) === String(result.id))) openEntry(null, result.id);
    } catch (error) {
      dialogError("job-error", error);
    } finally {
      dialogBusy("job-dialog", false, "job-save", id ? "Salva modifiche" : "Crea commessa");
      state.openEntryAfterJob = false;
    }
  });

  $("entry-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if ($("entry-id").value && !requirePermission("edit_time_entries")) return;
    if (busyDialogs.has("entry-dialog")) return;
    $("entry-hours").setCustomValidity("");
    $("entry-hours").removeAttribute("aria-invalid");
    if (!$("entry-form").reportValidity()) return;
    let minutes;
    try { minutes = hoursToMinutes($("entry-hours").value); } catch (error) {
      $("entry-hours").setCustomValidity(error.message);
      $("entry-hours").setAttribute("aria-invalid", "true");
      $("entry-hours").reportValidity();
      return;
    }
    if (!validDate($("entry-date").value)) { dialogError("entry-error", new Error("Scegli una giornata lavorativa valida.")); return; }
    const id = $("entry-id").value;
    const fields = { job_id: $("entry-job").value, date: $("entry-date").value, minutes, note: $("entry-note").value.trim() };
    $("entry-error").hidden = true;
    dialogBusy("entry-dialog", true, "entry-save", id ? "Salva modifiche" : "Salva ore");
    try {
      await post("/api/time-entries" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("entry-dialog").close();
      state.selectedDate = fields.date;
      $("entry-date-filter").value = fields.date;
      selectTab("hours");
      showNotice(id ? "Registrazione aggiornata." : "Ore salvate. Il tuo lavoro ha la sua traccia.");
      await refreshAll();
    } catch (error) {
      dialogError("entry-error", error);
    } finally {
      dialogBusy("entry-dialog", false, "entry-save", id ? "Salva modifiche" : "Salva ore");
    }
  });

  $("delete-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("edit_time_entries")) return;
    if (busyDialogs.has("delete-dialog") || !state.deleteEntry) return;
    $("delete-error").hidden = true;
    dialogBusy("delete-dialog", true, "delete-save", "Elimina registrazione");
    $("delete-save").textContent = "Eliminazione…";
    try {
      await api("/api/time-entries/" + encodeURIComponent(state.deleteEntry.id), { method: "DELETE" });
      state.auditReady = false;
      $("delete-dialog").close();
      state.deleteEntry = null;
      showNotice("Registrazione eliminata. Totali aggiornati.");
      await refreshAll();
    } catch (error) {
      dialogError("delete-error", error);
    } finally {
      dialogBusy("delete-dialog", false, "delete-save", "Elimina registrazione");
    }
  });

  $("user-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_users") || busyDialogs.has("user-dialog")) return;
    $("user-name").value = $("user-name").value.trim();
    $("user-email").value = $("user-email").value.trim();
    $("user-password").setCustomValidity("");
    if (!$("user-form").reportValidity()) return;
    const id = $("user-id").value;
    if (!id) {
      const bytes = new TextEncoder().encode($("user-password").value).length;
      if (bytes < 8 || bytes > 1024) {
        $("user-password").setCustomValidity("La password deve contenere da 8 a 1024 byte. Usa almeno 8 caratteri.");
        $("user-password").reportValidity();
        return;
      }
    }
    const fields = { name: $("user-name").value, role: $("user-role").value };
    if (id) fields.active = $("user-active").value;
    else { fields.email = $("user-email").value; fields.password = $("user-password").value; }
    const editingSelf = id && $("user-email").value === state.session.email;
    $("user-error").hidden = true;
    dialogBusy("user-dialog", true, "user-save", id ? "Salva modifiche" : "Crea utente");
    try {
      await post("/api/users" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("user-dialog").close();
      if (editingSelf && !await refreshSession()) return;
      showNotice(id ? "Utente aggiornato." : "Utente creato. L’account è pronto per accedere.");
      if (can("manage_users")) await refreshUsers();
    } catch (error) {
      dialogError("user-error", error);
    } finally {
      fields.password = "";
      dialogBusy("user-dialog", false, "user-save", id ? "Salva modifiche" : "Crea utente");
    }
  });

  $("assignment-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog") || !state.assignmentJob || !state.assignmentsReady) return;
    $("assignment-end").setCustomValidity("");
    const startsOn = $("assignment-start").value;
    const endsOn = $("assignment-end").value;
    if (startsOn && !validDate(startsOn) || endsOn && !validDate(endsOn)) { dialogError("assignment-error", new Error("Scegli date valide per il periodo di abilitazione.")); return; }
    if (startsOn && endsOn && startsOn > endsOn) $("assignment-end").setCustomValidity("La data finale deve essere uguale o successiva alla data iniziale.");
    if (!$("assignment-form").reportValidity()) return;
    const jobId = state.assignmentJob.id;
    const fields = { user_id: $("assignment-user").value, starts_on: startsOn, ends_on: endsOn };
    $("assignment-error").hidden = true;
    setAssignmentBusy(true);
    try {
      await post(`/api/jobs/${encodeURIComponent(jobId)}/assignments`, fields);
      state.auditReady = false;
      state.revokeAssignment = null;
      $("assignment-revoke").hidden = true;
      showNotice("Abilitazione salvata. Il periodo di accesso alla commessa è aggiornato.");
      await loadAssignments();
      await refreshAll();
    } catch (error) {
      dialogError("assignment-error", error);
    } finally {
      setAssignmentBusy(false);
    }
  });
  $("assignment-revoke-save").addEventListener("click", async () => {
    if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog") || !state.assignmentJob || !state.revokeAssignment) return;
    const jobId = state.assignmentJob.id;
    const userId = state.revokeAssignment.user_id;
    $("assignment-error").hidden = true;
    setAssignmentBusy(true);
    try {
      await api(`/api/jobs/${encodeURIComponent(jobId)}/assignments/${encodeURIComponent(userId)}`, { method: "DELETE" });
      state.auditReady = false;
      state.revokeAssignment = null;
      $("assignment-revoke").hidden = true;
      showNotice("Abilitazione revocata. Le ore già registrate sono conservate.");
      await loadAssignments();
      await refreshAll();
    } catch (error) {
      dialogError("assignment-error", error);
    } finally {
      setAssignmentBusy(false);
    }
  });
  $("assignment-revoke-cancel").addEventListener("click", () => { state.revokeAssignment = null; $("assignment-revoke").hidden = true; $(state.editingAssignment ? "assignment-start" : "assignment-user").focus(); });
  $("assignment-reset").addEventListener("click", () => { resetAssignmentForm(); $("assignment-user").focus(); });
  $("assignment-retry").addEventListener("click", loadAssignments);
  $("assignment-start").addEventListener("input", () => $("assignment-end").setCustomValidity(""));
  $("assignment-end").addEventListener("input", () => $("assignment-end").setCustomValidity(""));
  $("assignment-dialog").addEventListener("close", () => {
    state.assignmentRequest++;
    state.assignmentJob = null;
    state.assignments = [];
    state.assignmentUsers = [];
    state.assignmentsReady = false;
    state.editingAssignment = null;
    state.revokeAssignment = null;
    $("assignment-form").reset();
  });
  $("audit-filter").addEventListener("change", () => refreshAudit());
  $("audit-refresh").addEventListener("click", () => refreshAudit());
  $("audit-more").addEventListener("click", () => refreshAudit(true));
  $("audit-retry").addEventListener("click", () => refreshAudit(state.auditRetryMore));

  $("supplier-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_suppliers") || busyDialogs.has("supplier-dialog")) return;
    const keys = ["code", "name", "vat_number", "email", "phone", "address", "notes"];
    keys.forEach((key) => { $("supplier-" + key).value = $("supplier-" + key).value.trim(); });
    if (!$("supplier-form").reportValidity()) return;
    const id = $("supplier-id").value;
    const fields = Object.fromEntries(keys.map((key) => [key, $("supplier-" + key).value]));
    fields.active = $("supplier-active").value;
    $("supplier-error").hidden = true;
    dialogBusy("supplier-dialog", true, "supplier-save", id ? "Salva modifiche" : "Crea fornitore");
    try {
      await post("/api/suppliers" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("supplier-dialog").close();
      showNotice(id ? "Fornitore aggiornato." : "Fornitore censito. Puoi associarlo a un contratto.");
      await refreshRegistry("suppliers");
      if (can("manage_contracts") && state.contractsReady) await refreshRegistry("contracts");
    } catch (error) {
      dialogError("supplier-error", error);
    } finally {
      dialogBusy("supplier-dialog", false, "supplier-save", id ? "Salva modifiche" : "Crea fornitore");
    }
  });

  $("contract-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_contracts") || busyDialogs.has("contract-dialog")) return;
    if (!state.suppliersReady) { dialogError("contract-error", new Error("Ricarica l’elenco dei fornitori prima di salvare.")); return; }
    ["code", "title", "notes"].forEach((key) => { $("contract-" + key).value = $("contract-" + key).value.trim(); });
    $("contract-value").setCustomValidity("");
    $("contract-ends_on").setCustomValidity("");
    let cents;
    try { cents = valueToCents($("contract-value").value); } catch (error) { $("contract-value").setCustomValidity(error.message); }
    const startsOn = $("contract-starts_on").value;
    const endsOn = $("contract-ends_on").value;
    if (startsOn && !validDate(startsOn) || endsOn && !validDate(endsOn)) { dialogError("contract-error", new Error("Scegli date valide per il contratto.")); return; }
    if (startsOn && endsOn && startsOn > endsOn) $("contract-ends_on").setCustomValidity("La data finale deve essere uguale o successiva alla data iniziale.");
    if (!$("contract-form").reportValidity()) return;
    const id = $("contract-id").value;
    const fields = { code: $("contract-code").value, title: $("contract-title").value, supplier_id: $("contract-supplier").value, status: $("contract-status").value, starts_on: startsOn, ends_on: endsOn, value_cents: cents, currency: "EUR", notes: $("contract-notes").value };
    $("contract-error").hidden = true;
    dialogBusy("contract-dialog", true, "contract-save", id ? "Salva modifiche" : "Crea contratto");
    try {
      await post("/api/contracts" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("contract-dialog").close();
      showNotice(id ? "Contratto aggiornato." : "Contratto censito. Usa il pulsante PDF per archiviare i documenti originali.");
      await refreshRegistry("contracts");
      await refreshAll();
    } catch (error) {
      dialogError("contract-error", error);
    } finally {
      dialogBusy("contract-dialog", false, "contract-save", id ? "Salva modifiche" : "Crea contratto");
    }
  });

  $("document-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_contracts") || busyDialogs.has("documents-dialog") || !state.documentsContract) return;
    $("document-file").setCustomValidity("");
    const file = $("document-file").files[0];
    if (file && !/\.pdf$/i.test(file.name)) $("document-file").setCustomValidity("Seleziona un documento con estensione .pdf.");
    if (file && (file.size === 0 || file.size > 10 * 1024 * 1024)) $("document-file").setCustomValidity("Il documento deve avere una dimensione da 1 byte a 10 MiB.");
    if (!$("document-form").reportValidity() || !file) return;
    const id = state.documentsContract.id;
    $("documents-error").hidden = true;
    dialogBusy("documents-dialog", true, "document-save", "Carica PDF");
    $("document-save").textContent = "Caricamento…";
    try {
      await api(`/api/contracts/${encodeURIComponent(id)}/documents?filename=${encodeURIComponent(file.name)}`, { method: "POST", headers: { "Content-Type": "application/pdf" }, body: file, timeoutMs: 60000 });
      if (!can("manage_contracts") || !state.documentsContract || String(state.documentsContract.id) !== String(id)) return;
      state.auditReady = false;
      $("document-form").reset();
      showNotice("PDF archiviato. Il documento originale è disponibile per il download.");
      await loadDocuments();
      await refreshRegistry("contracts");
    } catch (error) {
      if (state.documentsContract && String(state.documentsContract.id) === String(id)) dialogError("documents-error", error);
    } finally {
      dialogBusy("documents-dialog", false, "document-save", "Carica PDF");
    }
  });

  $("new-supplier").addEventListener("click", () => openSupplier());
  $("new-contract").addEventListener("click", () => openContract());
  $("supplier-search").addEventListener("input", renderSuppliers);
  $("supplier-filter").addEventListener("change", renderSuppliers);
  $("contract-search").addEventListener("input", renderContracts);
  $("contract-filter").addEventListener("change", renderContracts);
  $("suppliers-retry").addEventListener("click", () => refreshRegistry("suppliers"));
  $("contracts-retry").addEventListener("click", () => refreshRegistry("contracts"));
  $("job-contract-retry").addEventListener("click", loadJobContracts);
  $("contract-supplier-retry").addEventListener("click", loadContractSuppliers);
  $("contract-value").addEventListener("input", () => $("contract-value").setCustomValidity(""));
  ["contract-starts_on", "contract-ends_on"].forEach((id) => $(id).addEventListener("input", () => $("contract-ends_on").setCustomValidity("")));
  $("document-file").addEventListener("change", () => $("document-file").setCustomValidity(""));
  $("documents-retry").addEventListener("click", loadDocuments);
  $("supplier-dialog").addEventListener("close", () => { $("supplier-form").reset(); $("supplier-id").value = ""; });
  $("contract-dialog").addEventListener("close", () => { state.editingContract = null; $("contract-form").reset(); $("contract-id").value = ""; $("contract-supplier").replaceChildren(); });
  $("documents-dialog").addEventListener("close", () => {
    state.documentsRequest++;
    state.documentsContract = null;
    state.documents = [];
    state.documentsReady = false;
    $("documents-list").replaceChildren();
    $("documents-dialog-description").textContent = "";
    $("documents-dialog-title").textContent = "Documenti del contratto";
    $("document-form").reset();
  });

  document.querySelectorAll("[data-close-dialog]").forEach((button) => {
    button.addEventListener("click", () => { const id = button.dataset.closeDialog; if (!busyDialogs.has(id)) $(id).close(); });
  });
  document.querySelectorAll(".work-dialog").forEach((dialog) => {
    dialog.addEventListener("cancel", (event) => { if (busyDialogs.has(dialog.id)) event.preventDefault(); });
    dialog.addEventListener("click", (event) => {
      if (event.target !== dialog || busyDialogs.has(dialog.id)) return;
      const rect = dialog.getBoundingClientRect();
      if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) dialog.close();
    });
  });
  $("job-dialog").addEventListener("close", () => {
    if (!busyDialogs.has("job-dialog")) state.openEntryAfterJob = false;
    state.editingJob = null;
    $("job-dialog-description").textContent = "Definisci i dettagli della commessa. Potrai aggiornarli in ogni momento.";
  });
  $("user-dialog").addEventListener("close", () => { $("user-form").reset(); $("user-password").value = ""; $("user-password").setCustomValidity(""); });
  $("user-password").addEventListener("input", () => $("user-password").setCustomValidity(""));
  $("entry-hours").addEventListener("input", () => { $("entry-hours").setCustomValidity(""); $("entry-hours").removeAttribute("aria-invalid"); });
  ["new-job", "empty-new-job"].forEach((id) => $(id).addEventListener("click", () => { state.openEntryAfterJob = false; openJob(); }));
  ["new-entry", "panel-new-entry", "empty-new-entry"].forEach((id) => $(id).addEventListener("click", () => openEntry()));
  $("new-user").addEventListener("click", () => openUser());
  tabs.forEach((tab) => {
    $("tab-" + tab).addEventListener("click", () => selectTab(tab));
    $("tab-" + tab).addEventListener("keydown", (event) => {
      if (["ArrowDown", "ArrowRight", "ArrowUp", "ArrowLeft", "Home", "End"].includes(event.key)) {
        event.preventDefault();
        const available = availableTabs();
        const direction = ["ArrowDown", "ArrowRight"].includes(event.key) ? 1 : -1;
        const next = event.key === "Home" ? available[0] : event.key === "End" ? available[available.length - 1] : available[(available.indexOf(tab) + direction + available.length) % available.length];
        selectTab(next, true);
      }
    });
  });
  $("job-search").addEventListener("input", renderJobs);
  $("job-filter").addEventListener("change", renderJobs);
  $("clear-filters").addEventListener("click", () => { $("job-search").value = ""; $("job-filter").value = ""; renderJobs(); $("job-search").focus(); });
  $("entry-date-filter").value = state.selectedDate;
  $("entry-date-filter").addEventListener("change", () => {
    if (validDate($("entry-date-filter").value)) setDate($("entry-date-filter").value);
    else $("entry-date-filter").value = state.selectedDate;
  });
  $("previous-day").addEventListener("click", () => moveDay(-1));
  $("next-day").addEventListener("click", () => moveDay(1));
  $("go-today").addEventListener("click", () => setDate(localDate(new Date())));
  $("notice-close").addEventListener("click", () => { $("notice").hidden = true; clearTimeout(noticeTimer); });
  $("retry-load").addEventListener("click", refreshAll);
  $("logout").addEventListener("click", async () => {
    $("logout").disabled = true;
    try { await post("/api/logout", {}); window.location.replace("/"); } catch (error) { showNotice(error.message, true); $("logout").disabled = false; }
  });
  $("current-year").textContent = new Date().getFullYear();
  const mobileNavigation = window.matchMedia("(max-width: 760px)");
  const updateTabOrientation = () => document.querySelector(".app-navigation").setAttribute("aria-orientation", mobileNavigation.matches ? "horizontal" : "vertical");
  updateTabOrientation();
  mobileNavigation.addEventListener("change", updateTabOrientation);

  async function initialize() {
    try {
      if (!await refreshSession()) return;
      $("session-status").hidden = true;
      $("workspace").hidden = false;
      await refreshAll();
    } catch (error) {
      $("session-status").textContent = error.message + " Ricarica la pagina per riprovare.";
    }
  }
  initialize();
})();
