"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const states = { planned: "Da avviare", active: "In corso", paused: "In pausa", completed: "Completata", cancelled: "Annullata" };
  const roles = { manager: "Gestore", worker: "Operatore" };
  const contractStates = { draft: "Bozza", active: "Attivo", expired: "Scaduto", closed: "Chiuso", cancelled: "Annullato" };
  const tabs = ["jobs", "work-board", "inventory", "hours", "reports", "notifications", "alerts", "suppliers", "contracts", "users", "organization", "audit"];
  const reportViews = { periodo: { api: "date", label: "Per periodo", title: "Il lavoro nel periodo" }, commesse: { api: "job", label: "Per commessa", title: "Il lavoro per commessa" }, utenti: { api: "user", label: "Per utente", title: "Il lavoro per utente" }, costi: { api: "cost", label: "Valore contratti", title: "Valori contrattuali e lavoro registrato" }, "costo-ore": { api: "labor_cost", label: "Costo delle ore", title: "Il costo stimato delle ore" } };
  const state = { session: null, sessionRequest: 0, jobs: [], entries: [], users: [], todayMinutes: null, todayCount: null, jobsReady: false, entriesReady: false, usersReady: false, selectedDate: localDate(new Date()), entriesRequest: 0, usersRequest: 0, refreshRequest: 0, deleteEntry: null, openEntryAfterJob: false, activeTab: "jobs", assignmentJob: null, assignments: [], assignmentUsers: [], assignmentsReady: false, assignmentRequest: 0, editingAssignment: null, revokeAssignment: null, auditEvents: [], auditReady: false, auditRequest: 0, auditLoading: false, auditCursor: null, auditRetryMore: false };
  Object.assign(state, { suppliers: [], contracts: [], suppliersReady: false, contractsReady: false, suppliersRequest: 0, contractsRequest: 0, suppliersLoading: false, contractsLoading: false, editingJob: null, deleteJob: null, editingContract: null, documentsContract: null, documents: [], documentsReady: false, documentsRequest: 0 });
  Object.assign(state, { organizationGroups: [], organizationRoles: [], organizationUsers: [], organizationReady: false, organizationLoading: false, organizationRequest: 0, selectedGroup: null, groupMembers: [], groupMembersReady: false, groupMembersRequest: 0, organizationBusy: false, organizationConfirm: null, assignmentGroups: [], assignmentRoles: [], groupGrants: [], editingGroupGrant: null, revokeGroupGrant: null });
  Object.assign(state, { reportView: "periodo", reportFilters: defaultReportFilters(), reportToday: null, reportDefaultsPending: true, reportData: null, reportReady: false, reportLoading: false, reportRequest: 0 });
  Object.assign(state, { laborRates: [], laborRateUsers: [], laborRatesReady: false, laborRatesLoading: false, laborRatesRequest: 0, editingLaborRate: null, routeHash: null });
  Object.assign(state, { alertRules: [], alertUsers: [], alertRulesReady: false, alertRulesLoading: false, alertRulesRequest: 0, editingAlertRule: null, runningAlertRule: null, alertSmtpConfigured: false, notifications: [], notificationsReady: false, notificationsLoading: false, notificationsRequest: 0, notificationsUnread: 0, readingNotifications: new Set() });
  const alertTypes = {
    invalid_job: { label: "Commessa inesistente", description: "Controlla i tentativi rifiutati di registrare ore su commesse inesistenti o eliminate. Le ore di questi tentativi non sono state salvate." },
    missing_hours: { label: "Ore giornaliere mancanti", description: "Controlla le giornate da lunedì a venerdì con meno di 8 ore, dopo 48 ore reali dalla fine della giornata in Europe/Rome. Il monitoraggio parte dalla creazione della regola." },
    inventory_due: { label: "Scadenze inventario", description: "Controlla garanzie, manutenzioni, scadenze e rinnovi delle licenze. Include le date già superate; esclude beni dismessi o smarriti. Ogni destinatario riceve un solo avviso per asset, tipo di scadenza, data e anticipo impostato." }
  };
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

  function redirectToLogin() {
    const hash = window.location.hash;
    const retained = hash.length <= 2048 && /^#analisi\/(?:periodo|commesse|utenti|costi|costo-ore)(?:\?|$)/.test(hash) ? hash : "";
    window.location.replace("/" + retained);
  }

  async function api(path, options = {}) {
    const controller = new AbortController();
    const { timeoutMs = 15000, ...fetchOptions } = options;
    const timeout = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const response = await fetch(path, { credentials: "same-origin", cache: "no-store", ...fetchOptions, signal: controller.signal });
      if (response.status === 401) {
        if (/^\/api\/inventory(?:\/|$|\?)/.test(path)) resetInventory();
        redirectToLogin();
        throw new Error("La sessione è scaduta. Accedi di nuovo.");
      }
      let data;
      try { data = await response.json(); } catch (_) { throw new Error("Il server ha restituito una risposta non valida. Riprova."); }
      if (!response.ok || data.ok === false) {
        const workResourceUnavailable = [403, 404].includes(response.status) && /^\/api\/(?:work-board|tasks|blockers)(?:\/|$|\?)/.test(path);
        if (workResourceUnavailable) {
          resetWorkBoard(true);
          showNotice(data.error || "La risorsa non è più disponibile. Seleziona una commessa abilitata.", true);
        }
        if (response.status === 403) {
          if (/^\/api\/inventory(?:\/|$|\?)/.test(path)) resetInventory();
          showNotice(data.error || "Il tuo account non è abilitato a questa operazione.", true);
          if (path !== "/api/session") refreshSession().catch(() => {});
        }
        const error = new Error(data.error || (response.status === 403 ? "Il tuo account non è abilitato a questa operazione." : "Non siamo riusciti a completare l’operazione. Riprova."));
        error.status = response.status;
        throw error;
      }
      if (fetchOptions.method && fetchOptions.method !== "GET" && /^\/api\/(?:time-entries|jobs|users|contracts|suppliers|groups|job-roles|labor-rates)(?:\/|$|\?)/.test(path)) invalidateReports();
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
    const permissions = { users: "manage_users", audit: "view_audit", suppliers: "manage_suppliers", contracts: "manage_contracts", organization: "manage_groups" };
    return tabs.filter((tab) => (tab !== "alerts" || isAlertManager()) && (!permissions[tab] || can(permissions[tab])));
  }

  function applySession(session) {
    if (!session.authenticated) { redirectToLogin(); return false; }
    const authorizationChanged = state.session && (state.session.email !== session.email || state.session.role !== session.role || JSON.stringify(state.session.permissions) !== JSON.stringify(session.permissions));
    if (authorizationChanged) {
      // Responses started with the previous permissions must not restore its data.
      state.refreshRequest++;
      state.entriesRequest++;
      resetAlertsAndNotifications();
      resetWorkBoard(true);
      resetInventory();
      state.jobs = [];
      state.entries = [];
      state.jobsReady = false;
      state.entriesReady = false;
      state.todayMinutes = null;
      state.todayCount = null;
      state.deleteEntry = null;
      state.openEntryAfterJob = false;
      ["jobs-body", "entries-body"].forEach((id) => $(id).replaceChildren());
      ["jobs-table-wrap", "jobs-empty", "jobs-table-footer", "entries-table-wrap", "entries-empty"].forEach((id) => { $(id).hidden = true; });
      ["jobs-loading", "entries-loading"].forEach((id) => { $(id).hidden = false; });
      ["stat-active", "stat-today", "stat-jobs", "day-total"].forEach((id) => { $(id).textContent = "—"; });
      $("stat-today").removeAttribute("aria-label");
      ["nav-jobs-count", "jobs-count"].forEach((id) => { $(id).textContent = "0"; });
      ["stat-today-caption", "day-entries-count", "jobs-visible-count"].forEach((id) => { $(id).textContent = ""; });
      ["new-entry", "panel-new-entry", "empty-new-entry"].forEach((id) => { $(id).hidden = true; });
      document.querySelectorAll(".work-dialog[open]").forEach((dialog) => dialog.close());
      $("entry-form").reset();
      $("entry-id").value = "";
      invalidateReports(true);
      state.laborRatesRequest++;
      state.laborRates = [];
      state.laborRateUsers = [];
      state.laborRatesReady = false;
      state.laborRatesLoading = false;
      state.editingLaborRate = null;
      $("report-rates-body").replaceChildren();
      $("labor-rate-form").reset();
      $("labor-rate-user").replaceChildren();
      $("report-rates-table-wrap").hidden = true;
      $("report-rates-empty").hidden = true;
      $("report-rates-error").hidden = true;
      $("report-rates-error-text").textContent = "";
    }
    state.session = session;
    if (!isReportManager()) state.reportFilters.userId = "";
    if (!availableReportViews().includes(state.reportView)) state.reportView = "periodo";
    renderReportControls();
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
    $("tab-organization").hidden = !can("manage_groups");
    $("tab-audit").hidden = !can("view_audit");
    $("tab-alerts").hidden = !isAlertManager();
    renderInventory();
    if (!isAlertManager()) {
      state.alertRulesRequest++;
      state.alertRules = [];
      state.alertUsers = [];
      state.alertRulesReady = false;
      state.alertRulesLoading = false;
      state.editingAlertRule = null;
      state.runningAlertRule = null;
      $("alert-rules-list").replaceChildren();
      $("alert-rule-users").replaceChildren();
      $("alert-rule-emails").value = "";
      if ($("alert-rule-dialog").open) $("alert-rule-dialog").close();
    }
    $("new-user").hidden = !can("manage_users");
    document.querySelector(".jobs-table").classList.toggle("jobs-with-assignments", can("manage_assignments"));
    $("entries-actions-heading").hidden = !can("edit_time_entries");
    document.querySelector(".entries-table").classList.toggle("entries-readonly", !can("edit_time_entries"));
    $("permission-hint").textContent = can("manage_jobs") ? "Gestore · Gestisci commesse, contratti, fornitori e persone. Ogni modifica ha la sua traccia." : "Operatore · Registra il lavoro sulle commesse abilitate per te. Le ore inserite restano nel tuo registro.";
    $("jobs-panel-description").textContent = can("manage_jobs") ? "Dal primo passo all’ultimo dettaglio, tutto sotto controllo." : "Le commesse abilitate per te, pronte per registrare il lavoro svolto.";
    if (!can("manage_jobs")) {
      state.openEntryAfterJob = false;
      if ($("job-dialog").open) $("job-dialog").close();
      if ($("job-delete-dialog").open) $("job-delete-dialog").close();
      state.deleteJob = null;
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
    if (!can("manage_groups")) {
      state.organizationRequest++;
      state.groupMembersRequest++;
      state.organizationGroups = [];
      state.organizationUsers = [];
      state.organizationRoles = [];
      state.groupMembers = [];
      state.selectedGroup = null;
      state.organizationReady = false;
      state.groupMembersReady = false;
      state.organizationLoading = false;
      state.organizationConfirm = null;
      $("groups-tree").replaceChildren();
      $("group-members").replaceChildren();
      $("job-roles-list").replaceChildren();
      $("organization-content").hidden = true;
      $("organization-confirm").hidden = true;
      if ($("group-dialog").open) $("group-dialog").close();
      if ($("job-role-dialog").open) $("job-role-dialog").close();
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
    selectTab(availableTabs().includes(state.activeTab) ? state.activeTab : "jobs", false, false);
    if (authorizationChanged && state.activeTab === "reports") writeReportRoute(true);
    renderJobs();
    renderEntries();
    return true;
  }

  async function refreshSession() {
    const request = ++state.sessionRequest;
    const hadSession = state.session !== null;
    const session = await api("/api/session");
    if (request !== state.sessionRequest) return Boolean(state.session?.authenticated);
    if (!applySession(session)) return false;
    if (hadSession) await refreshAll();
    return true;
  }

  function selectTab(name, moveFocus = false, updateRoute = true) {
    if (!availableTabs().includes(name)) return;
    const entering = state.activeTab !== name;
    if (name !== "reports" && state.activeTab === "reports") state.reportRequest++;
    state.activeTab = name;
    tabs.forEach((tab) => {
      const selected = tab === name;
      $("tab-" + tab).classList.toggle("is-active", selected);
      $("tab-" + tab).setAttribute("aria-selected", String(selected));
      $("tab-" + tab).tabIndex = selected ? 0 : -1;
      $("panel-" + tab).hidden = !selected;
    });
    $("workspace-summary").hidden = ["reports", "alerts", "notifications", "work-board", "inventory"].includes(name);
    const title = name === "reports" ? "Analisi · " + reportViews[state.reportView].label : $("tab-" + name).querySelector("span").textContent;
    $("workspace-current-view").textContent = title;
    document.title = title + " · punto";
    if (updateRoute) {
      if (name === "reports") writeReportRoute();
      else if (window.location.hash !== "#" + name) window.history.pushState(null, "", "#" + name);
      state.routeHash = window.location.hash;
    }
    if (moveFocus) $("tab-" + name).focus();
    if (name === "reports" && (entering || !state.reportReady && !state.reportLoading)) refreshReports();
    if (name === "reports" && state.reportView === "costo-ore" && (entering || !state.laborRatesReady && !state.laborRatesLoading)) refreshLaborRates();
    if (name === "work-board") { renderWorkJobs(); if (state.workJobId && !state.workLoading && (entering || !state.workReady)) refreshWorkBoard(); }
    if (name === "inventory" && !state.inventoryReady && !state.inventoryLoading) refreshInventory();
    if (name === "users" && !state.usersReady) refreshUsers();
    if (name === "organization" && !state.organizationReady && !state.organizationLoading) refreshOrganization();
    if (name === "audit" && !state.auditReady && !state.auditLoading) refreshAudit();
    if (name === "alerts" && (entering || !state.alertRulesReady) && !state.alertRulesLoading) refreshAlertRules();
    if (name === "notifications" && (entering || !state.notificationsReady) && !state.notificationsLoading) refreshNotifications();
    if (["suppliers", "contracts"].includes(name) && !state[name + "Ready"] && !state[name + "Loading"]) refreshRegistry(name);
  }

  function isReportManager() {
    return state.session?.role === "manager";
  }

  function availableReportViews() {
    return Object.keys(reportViews).filter((view) => !["costi", "costo-ore"].includes(view) || isReportManager() && can("manage_contracts"));
  }

  function defaultReportFilters(today = localDate(new Date())) {
    return { from: today.slice(0, 7) + "-01", to: today, jobId: "", userId: "" };
  }

  function reportDayNumber(value) {
    const [year, month, day] = value.split("-").map(Number);
    const date = new Date(0);
    date.setUTCFullYear(year, month - 1, day);
    date.setUTCHours(0, 0, 0, 0);
    return date.getTime() / 86400000;
  }

  function reportFilterError(filters) {
    if (!validDate(filters.from) || !validDate(filters.to)) return "Scegli una data iniziale e una data finale valide.";
    if (filters.from > filters.to) return "La data finale deve essere uguale o successiva alla data iniziale.";
    if (reportDayNumber(filters.to) - reportDayNumber(filters.from) + 1 > 366) return "Seleziona un periodo di massimo 366 giorni, comprese le date iniziale e finale.";
    for (const id of [filters.jobId, filters.userId]) {
      if (id && (!/^[1-9]\d*$/.test(id) || !Number.isSafeInteger(Number(id)))) return "Il collegamento contiene un filtro non valido. Azzera i filtri e riprova.";
    }
    return "";
  }

  function showReportError(message) {
    $("report-error-text").textContent = message;
    $("report-error").hidden = false;
  }

  function invalidateReports(clearFilters = false) {
    state.reportRequest++;
    state.reportData = null;
    state.reportReady = false;
    state.reportLoading = false;
    if (clearFilters) { state.reportFilters.jobId = ""; state.reportFilters.userId = ""; }
    $("report-results").hidden = true;
    $("report-results").setAttribute("aria-busy", "false");
    $("report-loading").hidden = true;
    $("report-error").hidden = true;
    $("report-body").replaceChildren();
    $("report-head").replaceChildren();
    ["report-total-hours", "report-total-entries", "report-total-jobs", "report-total-users", "report-known-value", "report-estimated-cost"].forEach((id) => { $(id).textContent = "—"; });
    ["report-caption", "report-table-caption", "report-contract-count", "report-unpriced-count", "report-uncontracted-hours", "report-priced-hours", "report-unpriced-hours"].forEach((id) => { $(id).textContent = ""; });
    renderReportControls();
  }

  function reportSelectOptions(id, items, selected, label, identify) {
    const select = $(id);
    const placeholder = node("option", "", label);
    placeholder.value = "";
    select.replaceChildren(placeholder);
    for (const item of items) {
      const option = node("option", "", identify(item));
      option.value = String(item.id);
      select.append(option);
    }
    if (selected && !items.some((item) => String(item.id) === selected)) {
      const unavailable = node("option", "", `Filtro selezionato (#${selected}) · da verificare`);
      unavailable.value = selected;
      select.append(unavailable);
    }
    select.value = selected;
  }

  function renderReportControls() {
    const manager = isReportManager();
    $("report-user-field").hidden = !manager;
    $("report-user").disabled = !manager;
    $("report-tab-costi").hidden = !availableReportViews().includes("costi");
    $("report-tab-costo-ore").hidden = !availableReportViews().includes("costo-ore");
    for (const view of Object.keys(reportViews)) {
      const selected = view === state.reportView;
      $("report-tab-" + view).classList.toggle("is-active", selected);
      $("report-tab-" + view).setAttribute("aria-selected", String(selected));
      $("report-tab-" + view).tabIndex = selected ? 0 : -1;
    }
    $("report-view").setAttribute("aria-labelledby", "report-tab-" + state.reportView);
    $("report-title").textContent = reportViews[state.reportView].title;
    $("report-scope").textContent = manager ? "Vista dello spazio di lavoro: tutti gli utenti, oppure la persona selezionata. I totali riguardano soltanto i filtri applicati." : "Vista personale: soltanto le ore del tuo account, comprese quelle già registrate sulle commesse non più abilitate.";
    $("report-cost-notice").hidden = state.reportView !== "costi";
    $("report-labor-notice").hidden = state.reportView !== "costo-ore";
    $("report-rates").hidden = state.reportView !== "costo-ore" || !manager;
    document.querySelectorAll("[data-report-preset]").forEach((button) => { button.disabled = !state.reportToday; });
    $("report-from").value = state.reportFilters.from;
    $("report-to").value = state.reportFilters.to;
    const options = state.reportData?.options || {};
    reportSelectOptions("report-job", options.jobs || [], state.reportFilters.jobId, manager ? "Tutte le commesse" : "Tutte le tue commesse con ore", (job) => `${job.code} · ${job.name}`);
    reportSelectOptions("report-user", manager ? options.users || [] : [], manager ? state.reportFilters.userId : "", "Tutti gli utenti", (user) => `${user.name || user.email}${user.active ? "" : " · Disattivato"}`);
  }

  function writeReportRoute(replace = false) {
    const query = new URLSearchParams({ from: state.reportFilters.from, to: state.reportFilters.to });
    if (state.reportFilters.jobId) query.set("job_id", state.reportFilters.jobId);
    if (isReportManager() && state.reportFilters.userId) query.set("user_id", state.reportFilters.userId);
    const hash = "#analisi/" + state.reportView + "?" + query.toString();
    if (window.location.hash !== hash) window.history[replace ? "replaceState" : "pushState"](null, "", hash);
    state.routeHash = window.location.hash;
  }

  function applyReportRoute() {
    if (!state.session) return;
    const hash = window.location.hash.slice(1);
    state.routeHash = window.location.hash;
    if (hash === "main-content") return;
    if (!hash.startsWith("analisi")) {
      const name = availableTabs().includes(hash) ? hash : "jobs";
      selectTab(name, false, false);
      return;
    }
    const [path, query = ""] = hash.split("?", 2);
    const candidate = path.split("/")[1] || "periodo";
    state.reportView = availableReportViews().includes(candidate) ? candidate : "periodo";
    const params = new URLSearchParams(query);
    const defaults = defaultReportFilters(state.reportToday || localDate(new Date()));
    state.reportDefaultsPending = !params.has("from") && !params.has("to");
    state.reportFilters = { from: params.get("from") ?? defaults.from, to: params.get("to") ?? defaults.to, jobId: params.get("job_id") || "", userId: isReportManager() ? params.get("user_id") || "" : "" };
    invalidateReports();
    renderReportControls();
    selectTab("reports", false, false);
    writeReportRoute(true);
  }

  function chooseReportView(view, moveFocus = false) {
    if (!availableReportViews().includes(view)) return;
    state.reportView = view;
    renderReportControls();
    writeReportRoute();
    $("workspace-current-view").textContent = "Analisi · " + reportViews[view].label;
    document.title = "Analisi · " + reportViews[view].label + " · punto";
    if (moveFocus) $("report-tab-" + view).focus();
    refreshReports();
    if (view === "costo-ore") refreshLaborRates();
  }

  function readReportFilters() {
    return { from: $("report-from").value, to: $("report-to").value, jobId: $("report-job").value, userId: isReportManager() ? $("report-user").value : "" };
  }

  function applyReportFilters(filters) {
    const message = reportFilterError(filters);
    if (message) { showReportError(message); return false; }
    state.reportDefaultsPending = false;
    state.reportFilters = filters;
    writeReportRoute();
    renderReportControls();
    refreshReports();
    return true;
  }

  function reportCell(value, numeric = false) {
    return node("td", numeric ? "report-number" : "", value);
  }

  function reportDrilldown(label, accessibleLabel, handler) {
    const button = node("button", "report-row-link", label);
    button.type = "button";
    button.setAttribute("aria-label", accessibleLabel);
    button.addEventListener("click", handler);
    return button;
  }

  function renderReportData() {
    const data = state.reportData;
    if (!data) return;
    const summary = data.summary;
    const cost = state.reportView === "costi";
    const labor = state.reportView === "costo-ore";
    $("report-total-hours").textContent = minutesLabel(summary.minutes);
    $("report-total-entries").textContent = String(summary.entry_count);
    $("report-total-jobs").textContent = String(summary.job_count);
    $("report-total-users").textContent = String(summary.user_count);
    const filters = state.reportFilters;
    const labels = [filters.from === filters.to ? dateLabel(filters.from) : `${dateLabel(filters.from)} – ${dateLabel(filters.to)}`];
    if (filters.jobId) labels.push($("report-job").selectedOptions[0]?.textContent || "Commessa selezionata");
    if (isReportManager() && filters.userId) labels.push($("report-user").selectedOptions[0]?.textContent || "Utente selezionato");
    labels.push(data.scope === "personal" ? "Le tue ore" : "Tutte le ore filtrate");
    $("report-caption").textContent = labels.join(" · ");
    $("report-cost-summary").hidden = !cost;
    $("report-labor-summary").hidden = !labor;
    if (cost) {
      $("report-known-value").textContent = euroLabel(summary.known_value_cents);
      $("report-contract-count").textContent = `${summary.contract_count} ${Number(summary.contract_count) === 1 ? "contratto distinto collegato" : "contratti distinti collegati"} alle ore filtrate. Ogni contratto viene contato una sola volta.`;
      $("report-unpriced-count").textContent = Number(summary.unpriced_contract_count) > 0 ? `${summary.unpriced_contract_count} ${Number(summary.unpriced_contract_count) === 1 ? "contratto senza valore indicato" : "contratti senza valore indicato"}: importo sconosciuto, escluso dal totale noto.` : "Tutti i contratti elencati hanno un valore indicato.";
      $("report-uncontracted-hours").textContent = `${minutesLabel(summary.uncontracted_minutes)} su commesse senza contratto. Queste ore sono comprese nei totali del periodo.`;
    }
    if (labor) {
      const estimated = Number(summary.priced_minutes) > 0 ? euroLabel(summary.estimated_cost_cents) : "Non disponibile";
      $("report-estimated-cost").textContent = estimated;
      $("report-priced-hours").textContent = `${minutesLabel(summary.priced_minutes)} coperte da tariffa.`;
      $("report-unpriced-hours").textContent = Number(summary.unpriced_minutes) > 0 ? `${minutesLabel(summary.unpriced_minutes)} senza tariffa valida: la stima è ${Number(summary.priced_minutes) > 0 ? "parziale" : "non disponibile"}. Le ore senza tariffa non vengono considerate a costo zero.` : "Tutte le ore filtrate sono coperte da tariffa.";
    }
    const headers = {
      periodo: ["Giorno", "Ore", "Registrazioni", "Commesse", "Utenti"],
      commesse: ["Commessa", "Cliente", "Stato", "Ore", "Registrazioni", "Utenti"],
      utenti: ["Utente", "Accesso", "Ore", "Registrazioni", "Commesse"],
      costi: ["Contratto", "Fornitore", "Validità contrattuale", "Valore contrattuale", "Stato", "Commesse con ore", "Ore nel periodo"],
      "costo-ore": ["Utente", "Commessa", "Ore", "Registrazioni", "Copertura tariffa", "Costo stimato (EUR)"]
    };
    const heading = node("tr");
    for (const label of headers[state.reportView]) {
      const th = node("th", "", label);
      th.scope = "col";
      heading.append(th);
    }
    $("report-head").replaceChildren(heading);
    $("report-body").replaceChildren();
    const rows = cost ? [...new Map(data.rows.map((row) => [String(row.contract_id), row])).values()] : data.rows;
    for (const item of rows) {
      const row = node("tr");
      if (state.reportView === "periodo") {
        const day = reportCell();
        day.append(reportDrilldown(dateLabel(item.date), `Vedi le commesse del ${dateLabel(item.date)}`, () => {
          state.reportFilters = { ...state.reportFilters, from: item.date, to: item.date };
          state.reportDefaultsPending = false;
          chooseReportView("commesse");
        }));
        row.append(day, reportCell(minutesLabel(item.minutes), true), reportCell(String(item.entry_count), true), reportCell(String(item.job_count), true), reportCell(String(item.user_count), true));
      } else if (state.reportView === "commesse") {
        const identity = reportCell();
        identity.append(reportDrilldown(item.name, `Vedi il periodo per la commessa ${item.code}`, () => {
          state.reportFilters.jobId = String(item.job_id);
          chooseReportView("periodo");
        }), node("span", "report-secondary", item.code));
        const status = reportCell();
        status.append(node("span", "status-badge status-" + (Object.hasOwn(states, item.status) ? item.status : "planned"), states[item.status] || item.status));
        row.append(identity, reportCell(item.client || "—"), status, reportCell(minutesLabel(item.minutes), true), reportCell(String(item.entry_count), true), reportCell(String(item.user_count), true));
      } else if (state.reportView === "utenti") {
        const identity = reportCell();
        if (isReportManager()) identity.append(reportDrilldown(item.name || item.email, `Vedi le commesse di ${item.name || item.email}`, () => {
          state.reportFilters.userId = String(item.user_id);
          chooseReportView("commesse");
        }));
        else identity.append(node("strong", "", item.name || item.email));
        identity.append(node("span", "report-secondary", item.email));
        row.append(identity, reportCell(item.active ? "Attivo" : "Disattivato"), reportCell(minutesLabel(item.minutes), true), reportCell(String(item.entry_count), true), reportCell(String(item.job_count), true));
      } else if (cost) {
        const identity = reportCell();
        identity.append(node("strong", "", item.title), node("span", "report-secondary", item.code));
        const value = item.value_cents === null || item.value_cents === undefined ? "Non indicato" : euroLabel(item.value_cents);
        row.append(identity, reportCell(item.supplier_name || "—"), reportCell(contractPeriod(item)), reportCell(value, true), reportCell(contractStates[item.status] || item.status), reportCell(String(item.job_count), true), reportCell(minutesLabel(item.minutes), true));
      } else {
        const identity = reportCell();
        identity.append(node("strong", "", item.name || item.email), node("span", "report-secondary", item.email));
        const job = reportCell();
        job.append(node("strong", "", item.job_name), node("span", "report-secondary", item.code));
        const coverage = Number(item.priced_minutes) > 0 ? `${minutesLabel(item.priced_minutes)} coperte${Number(item.unpriced_minutes) > 0 ? " · " + minutesLabel(item.unpriced_minutes) + " scoperte" : ""}` : "Nessuna tariffa valida";
        const amount = Number(item.priced_minutes) > 0 && item.cost_cents !== null ? euroLabel(item.cost_cents) + (Number(item.unpriced_minutes) > 0 ? " · Parziale" : "") : "Non disponibile";
        row.append(identity, job, reportCell(minutesLabel(item.minutes), true), reportCell(String(item.entry_count), true), reportCell(coverage), reportCell(amount, true));
      }
      $("report-body").append(row);
    }
    $("report-table-caption").textContent = `${reportViews[state.reportView].title}. ${$("report-caption").textContent}`;
    $("report-table-wrap").hidden = rows.length === 0;
    $("report-empty").hidden = rows.length !== 0;
    $("report-empty-text").textContent = cost && Number(summary.uncontracted_minutes) > 0 ? "Le ore filtrate appartengono a commesse senza contratto: non è disponibile un valore contrattuale da mostrare." : "Non ci sono registrazioni per questi filtri. Prova un altro periodo oppure azzera i filtri.";
    $("report-results").hidden = false;
  }

  async function refreshReports() {
    if (state.activeTab !== "reports" || !availableReportViews().includes(state.reportView)) return;
    const request = ++state.reportRequest;
    const view = state.reportView;
    const filters = { ...state.reportFilters };
    const message = reportFilterError(filters);
    state.reportReady = false;
    state.reportData = null;
    state.reportLoading = false;
    $("report-loading").hidden = true;
    $("report-results").setAttribute("aria-busy", "false");
    $("report-results").hidden = true;
    $("report-body").replaceChildren();
    $("report-error").hidden = true;
    if (message) { showReportError(message); return; }
    state.reportLoading = true;
    $("report-loading").hidden = false;
    $("report-results").setAttribute("aria-busy", "true");
    const query = new URLSearchParams({ view: reportViews[view].api, from: filters.from, to: filters.to });
    if (filters.jobId) query.set("job_id", filters.jobId);
    if (isReportManager() && filters.userId) query.set("user_id", filters.userId);
    try {
      const data = await api("/api/reports?" + query.toString());
      if (request !== state.reportRequest || state.activeTab !== "reports" || view !== state.reportView) return;
      if (!data.summary || !Array.isArray(data.rows) || !data.options || data.view !== reportViews[view].api || data.from !== filters.from || data.to !== filters.to || !isReportManager() && data.scope !== "personal") throw new Error("Il server ha restituito un’analisi non valida. Riprova.");
      if (validDate(data.today)) {
        state.reportToday = data.today;
        if (state.reportDefaultsPending) {
          state.reportDefaultsPending = false;
          const defaults = defaultReportFilters(data.today);
          if (filters.from !== defaults.from || filters.to !== defaults.to) {
            state.reportFilters = { ...filters, from: defaults.from, to: defaults.to };
            renderReportControls();
            writeReportRoute(true);
            refreshReports();
            return;
          }
        }
      }
      state.reportData = data;
      state.reportReady = true;
      renderReportControls();
      renderReportData();
    } catch (error) {
      if (request === state.reportRequest) showReportError(error.message);
    } finally {
      if (request === state.reportRequest) {
        state.reportLoading = false;
        $("report-loading").hidden = true;
        $("report-results").setAttribute("aria-busy", "false");
      }
    }
  }

  function renderLaborRates() {
    if (!isReportManager() || !state.laborRatesReady) return;
    $("report-rates-body").replaceChildren();
    for (const rate of state.laborRates) {
      const row = node("tr");
      const identity = reportCell();
      identity.append(node("strong", "", rate.user_name || rate.user_email), node("span", "report-secondary", rate.user_email));
      const actions = reportCell();
      actions.append(action("Modifica", `Modifica tariffa di ${rate.user_name || rate.user_email} dal ${dateLabel(rate.starts_on)}`, () => openLaborRate(rate)));
      row.append(identity, reportCell(euroLabel(rate.hourly_cents) + " / ora", true), reportCell(dateLabel(rate.starts_on)), reportCell(rate.ends_on ? dateLabel(rate.ends_on) : "Senza scadenza"), actions);
      $("report-rates-body").append(row);
    }
    $("report-rates-table-wrap").hidden = state.laborRates.length === 0;
    $("report-rates-empty").hidden = state.laborRates.length !== 0;
  }

  async function refreshLaborRates() {
    if (!isReportManager()) return;
    const request = ++state.laborRatesRequest;
    state.laborRatesLoading = true;
    state.laborRatesReady = false;
    $("report-rates-loading").hidden = false;
    $("report-rates-error").hidden = true;
    $("report-rates-table-wrap").hidden = true;
    $("report-rates-empty").hidden = true;
    $("report-new-rate").disabled = true;
    try {
      const [data, users] = await Promise.all([api("/api/labor-rates"), api("/api/users")]);
      if (request !== state.laborRatesRequest || !isReportManager()) return;
      if (!Array.isArray(data.rates) || !Array.isArray(users.users)) throw new Error("Il server ha restituito un elenco tariffe non valido. Riprova.");
      state.laborRates = data.rates;
      state.laborRateUsers = users.users;
      state.laborRatesReady = true;
      renderLaborRates();
    } catch (error) {
      if (request === state.laborRatesRequest && isReportManager()) {
        $("report-rates-error-text").textContent = error.message;
        $("report-rates-error").hidden = false;
      }
    } finally {
      if (request === state.laborRatesRequest) {
        state.laborRatesLoading = false;
        $("report-rates-loading").hidden = true;
        $("report-new-rate").disabled = !state.laborRatesReady;
      }
    }
  }

  function openLaborRate(rate = null) {
    if (!isReportManager() || !state.laborRatesReady || busyDialogs.has("labor-rate-dialog")) return;
    state.editingLaborRate = rate;
    $("labor-rate-form").reset();
    $("labor-rate-hourly").setCustomValidity("");
    $("labor-rate-to").setCustomValidity("");
    $("labor-rate-error").hidden = true;
    $("labor-rate-dialog-title").textContent = rate ? "Modifica tariffa oraria" : "Nuova tariffa oraria";
    $("labor-rate-save").textContent = rate ? "Salva modifiche" : "Crea tariffa";
    reportSelectOptions("labor-rate-user", state.laborRateUsers, rate ? String(rate.user_id) : "", "Seleziona un utente", (user) => `${user.name || user.email}${user.active ? "" : " · Disattivato"}`);
    $("labor-rate-user").disabled = Boolean(rate);
    $("labor-rate-from").value = rate?.starts_on || state.reportToday || localDate(new Date());
    $("labor-rate-to").value = rate?.ends_on || "";
    if (rate) {
      const cents = BigInt(rate.hourly_cents);
      $("labor-rate-hourly").value = `${cents / 100n},${String(cents % 100n).padStart(2, "0")}`;
    }
    $("labor-rate-dialog").showModal();
    $(rate ? "labor-rate-hourly" : "labor-rate-user").focus();
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
      if (isCommonJob(job)) nameCell.querySelector(".job-identity").append(node("span", "job-kind-badge job-kind-common", "Comune · tutti gli utenti"));
      if (can("manage_contracts") && job.contract_id) nameCell.querySelector(".job-identity").append(node("span", "job-contract-label", `Contratto ${job.contract_code || job.contract_id}${job.supplier_name ? " · " + job.supplier_name : ""}`));
      const clientCell = node("td", job.client ? "" : "no-value", job.client || "—");
      const statusCell = node("td");
      statusCell.append(node("span", "status-badge status-" + (Object.hasOwn(states, job.status) ? job.status : "planned"), states[job.status] || "Da avviare"));
      const hoursCell = node("td", "hours-cell", minutesLabel(job.total_minutes));
      const actionsCell = node("td");
      const actions = node("div", "row-actions");
      actions.append(action("Attività", "Apri attività e blocchi di " + job.name, () => { if (state.workBusy) return; resetWorkBoard(true); state.workJobId = String(job.id); selectTab("work-board"); }), node("span", "row-divider"));
      if (can("manage_jobs")) actions.append(action("Modifica", "Modifica la commessa " + job.name, () => openJob(job)), node("span", "row-divider"));
      if (can("manage_assignments")) {
        const count = Number(job.assigned_users) || 0;
        const groupCount = Number(job.assigned_groups) || 0;
        actions.append(action("Abilitazioni · " + count + (groupCount ? " + " + groupCount + " gruppi" : ""), "Gestisci le abilitazioni della commessa " + job.name + ": " + count + " utenti e " + groupCount + " gruppi", () => openAssignments(job)), node("span", "row-divider"));
      }
      if (canLogTime(job)) actions.append(action("+ Ore", "Registra ore per " + job.name, () => openEntry(null, job.id)));
      else actions.append(node("span", "readonly-job-label", "Sola consultazione"));
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
    ["new-entry", "panel-new-entry", "empty-new-entry"].forEach((id) => { $(id).hidden = !can("manage_jobs") && !state.jobs.some(canLogTime); });
    if ($("entry-dialog").open) renderEntryJobs($("entry-job").value, true);
    renderStats();
  }

  function canLogTime(job) {
    return can("manage_jobs") || job.can_log_time === true;
  }

  function isCommonJob(job) {
    return job?.is_common === true || job?.is_common === 1;
  }

  function renderEntryJobs(selectedId = "", notifyUnavailable = false) {
    const writableJobs = state.jobs.filter(canLogTime);
    const options = [node("option", "", writableJobs.length ? "Seleziona una commessa" : "Nessuna commessa abilitata")];
    options[0].value = "";
    for (const job of writableJobs) {
      const option = node("option", "", `${job.code} · ${job.name}`);
      option.value = job.id;
      options.push(option);
    }
    const selectionAvailable = writableJobs.some((job) => String(job.id) === String(selectedId));
    $("entry-job").replaceChildren(...options);
    $("entry-job").value = selectionAvailable ? String(selectedId) : "";
    if (notifyUnavailable && selectedId && !selectionAvailable) {
      const nextStep = writableJobs.length ? "Scegli una commessa abilitata." : "Chiedi a un gestore di abilitarti a una commessa.";
      dialogError("entry-error", new Error(`La commessa selezionata non è più abilitata alla registrazione delle ore. ${nextStep} Data, ore e nota sono conservate.`));
    }
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
    $("group-grant-group").disabled = busy || Boolean(state.editingGroupGrant) || !state.assignmentsReady;
    $("group-grant-save").disabled = busy || !state.assignmentsReady || state.assignmentGroups.length === 0;
    $("group-grant-save").textContent = busy ? "Salvataggio…" : state.editingGroupGrant ? "Salva abilitazione gruppo" : "Abilita gruppo";
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
    fillRoleSelect("assignment-role", state.assignmentRoles, assignment?.role_id || 2);
    if (assignment) {
      $("assignment-user").value = assignment.user_id;
      $("assignment-start").value = assignment.starts_on || "";
      $("assignment-end").value = assignment.ends_on || "";
    }
    $("assignment-form-title").textContent = assignment ? "Modifica abilitazione utente" : "Abilita un utente";
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
      meta.append(node("span", "job-access-role", assignment.role_name || "Collaboratore"), node("span", "status-badge status-" + status, label), node("span", "assignment-period", assignmentPeriod(assignment)));
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
        state.revokeGroupGrant = null;
        $("group-grant-revoke").hidden = true;
        $("assignment-revoke-description").textContent = `Revocare l’abilitazione di ${assignment.name}? ${isCommonJob(state.assignmentJob) ? "La commessa è comune: l’utente attivo manterrà il diritto di consultarla e registrare le proprie ore." : assignment.role === "manager" ? "Come gestore manterrà l’accesso alla commessa." : "L’accesso diretto sarà revocato; eventuali abilitazioni ricevute tramite gruppi resteranno valide. Le ore già registrate saranno conservate."}`;
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
      const [data, users, groups, jobRoles, grants] = await Promise.all([api(`/api/jobs/${encodeURIComponent(jobId)}/assignments`), api("/api/users"), api("/api/groups"), api("/api/job-roles"), api(`/api/jobs/${encodeURIComponent(jobId)}/group-grants`)]);
      if (request !== state.assignmentRequest || !state.assignmentJob || !can("manage_assignments")) return;
      state.assignments = data.assignments;
      state.assignmentUsers = users.users;
      state.assignmentGroups = groups.groups;
      state.assignmentRoles = jobRoles.roles;
      state.groupGrants = grants.grants;
      state.assignmentsReady = true;
      renderAssignments();
      renderGroupGrants();
      resetAssignmentForm();
      resetGroupGrantForm();
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
    state.assignmentGroups = [];
    state.assignmentRoles = [];
    state.groupGrants = [];
    state.assignmentsReady = false;
    state.revokeAssignment = null;
    $("assignment-dialog-title").textContent = "Abilitazioni · " + job.code;
    $("assignment-dialog-description").textContent = isCommonJob(job) ? `${job.name}. Commessa comune: tutti gli utenti attivi possono consultarla e registrare le proprie ore. Le abilitazioni specifiche saranno applicate se la commessa smette di essere comune.` : `${job.name}. Scegli chi può utilizzare questa commessa. Una revoca conserva tutte le ore già registrate.`;
    $("assignment-empty").textContent = isCommonJob(job) ? "Nessuna abilitazione specifica. Questa commessa è già disponibile a tutti gli utenti attivi." : "Nessun utente abilitato. Aggiungi una persona dal modulo qui sotto. I gestori hanno comunque accesso a tutte le commesse.";
    $("assignment-list").replaceChildren();
    $("assignment-empty").hidden = true;
    $("assignment-revoke").hidden = true;
    $("group-grants-list").replaceChildren();
    $("group-grants-empty").hidden = true;
    $("group-grant-revoke").hidden = true;
    state.revokeGroupGrant = null;
    resetAssignmentForm();
    resetGroupGrantForm();
    $("assignment-dialog").showModal();
    loadAssignments();
  }

  function fillRoleSelect(id, catalogue, selected = 2) {
    const options = catalogue.map((role) => {
      const option = node("option", "", `${role.name} · ${role.can_log_time ? "consulta e registra ore" : "sola consultazione"}`);
      option.value = role.id;
      return option;
    });
    $(id).replaceChildren(...options);
    $(id).value = String(selected);
  }

  function groupPath(group, groups = state.organizationGroups) {
    const parts = [];
    const seen = new Set();
    let current = group;
    while (current && !seen.has(Number(current.id)) && parts.length < 16) {
      seen.add(Number(current.id));
      parts.unshift(current.name);
      current = groups.find((item) => Number(item.id) === Number(current.parent_id));
    }
    return parts.join(" / ");
  }

  function orderedGroups(groups = state.organizationGroups) {
    return [...groups].sort((left, right) => groupPath(left, groups).localeCompare(groupPath(right, groups), "it-IT"));
  }

  function groupDescendsFrom(group, ancestorId) {
    const seen = new Set();
    let current = group;
    while (current && !seen.has(Number(current.id))) {
      if (Number(current.id) === Number(ancestorId)) return true;
      seen.add(Number(current.id));
      current = state.organizationGroups.find((item) => Number(item.id) === Number(current.parent_id));
    }
    return false;
  }

  function setOrganizationBusy(busy) {
    state.organizationBusy = busy;
    $("panel-organization").setAttribute("aria-busy", String(busy));
    $("panel-organization").querySelectorAll("button, input, select").forEach((element) => { element.disabled = busy; });
    $("group-member-user").disabled = busy || !state.groupMembersReady;
    $("group-member-save").disabled = busy || !state.groupMembersReady || !$("group-member-user").options.length || $("group-member-user").options.length === 1;
  }

  function renderOrganization() {
    if (!state.organizationReady || !can("manage_groups")) return;
    $("organization-content").hidden = false;
    $("groups-count").textContent = state.organizationGroups.length;
    $("groups-empty").hidden = state.organizationGroups.length > 0;
    const seen = new Set();
    const appendBranch = (parentId, parentList) => {
      const children = state.organizationGroups.filter((group) => (group.parent_id === null || group.parent_id === "" ? null : Number(group.parent_id)) === parentId).sort((a, b) => a.name.localeCompare(b.name, "it-IT"));
      for (const group of children) {
        if (seen.has(Number(group.id))) continue;
        seen.add(Number(group.id));
        const item = node("li");
        const button = node("button", "group-tree-button");
        button.type = "button";
        button.setAttribute("aria-pressed", String(Number(state.selectedGroup?.id) === Number(group.id)));
        button.append(node("span", "group-tree-name", group.name), node("span", "group-tree-count", String(Number(group.members_count) || 0)));
        button.setAttribute("aria-label", `${group.name}, ${Number(group.members_count) || 0} membri diretti`);
        button.addEventListener("click", () => {
          if (state.organizationBusy) return;
          state.selectedGroup = group;
          state.organizationConfirm = null;
          $("organization-confirm").hidden = true;
          renderOrganization();
          loadGroupMembers();
        });
        item.append(button);
        if (state.organizationGroups.some((child) => Number(child.parent_id) === Number(group.id))) {
          const subtree = node("ul");
          appendBranch(Number(group.id), subtree);
          item.append(subtree);
        }
        parentList.append(item);
      }
    };
    $("groups-tree").replaceChildren();
    appendBranch(null, $("groups-tree"));
    const group = state.selectedGroup;
    $("group-detail-actions").hidden = !group;
    $("group-members-content").hidden = !group;
    $("group-detail-title").textContent = group ? group.name : "Seleziona un gruppo";
    $("group-detail-path").textContent = group ? groupPath(group) : "Seleziona un gruppo nell’albero per gestire i suoi membri diretti.";
    $("job-roles-list").replaceChildren();
    for (const role of state.organizationRoles) {
      const item = node("li", "job-role-card");
      const identity = node("div");
      identity.append(node("strong", "", role.name), node("span", "assignment-email", role.code), node("p", "field-help", role.can_log_time ? "Consulta e registra le proprie ore" : "Sola consultazione"));
      item.append(identity);
      if (role.builtin) item.append(node("span", "job-access-role", "Predefinito"));
      else if (can("manage_job_roles")) item.append(action("Modifica", "Modifica il ruolo " + role.name, () => openJobRole(role)));
      $("job-roles-list").append(item);
    }
    $("new-job-role").hidden = !can("manage_job_roles");
    setOrganizationBusy(state.organizationBusy);
  }

  async function refreshOrganization(selectedId = state.selectedGroup?.id) {
    if (!can("manage_groups")) return;
    const request = ++state.organizationRequest;
    state.groupMembersRequest++;
    state.organizationLoading = true;
    state.organizationConfirm = null;
    $("organization-confirm").hidden = true;
    state.organizationReady = false;
    state.groupMembersReady = false;
    $("organization-loading").hidden = false;
    $("organization-content").hidden = true;
    $("organization-error").hidden = true;
    try {
      const [groups, catalogue, users] = await Promise.all([api("/api/groups"), api("/api/job-roles"), api("/api/users")]);
      if (request !== state.organizationRequest || !can("manage_groups")) return;
      state.organizationGroups = groups.groups;
      state.organizationRoles = catalogue.roles;
      state.organizationUsers = users.users;
      state.organizationReady = true;
      state.selectedGroup = state.organizationGroups.find((group) => Number(group.id) === Number(selectedId)) || null;
      renderOrganization();
      if (state.selectedGroup) await loadGroupMembers();
    } catch (error) {
      if (request !== state.organizationRequest) return;
      $("organization-error-text").textContent = error.message;
      $("organization-error").hidden = false;
    } finally {
      if (request === state.organizationRequest) {
        state.organizationLoading = false;
        $("organization-loading").hidden = true;
      }
    }
  }

  function renderGroupMembers() {
    if (!state.groupMembersReady || !state.selectedGroup || !can("manage_groups")) return;
    $("group-members").replaceChildren();
    for (const member of state.groupMembers) {
      const item = node("li", "organization-member");
      const identity = node("div", "assignment-identity");
      identity.append(node("strong", "", member.name), node("span", "assignment-email", member.email), node("span", "field-help", `${roles[member.role] || "Operatore"}${member.active ? "" : " · Disattivato"}`));
      item.append(identity, action("Rimuovi", "Rimuovi " + member.name + " dal gruppo", () => {
        const group = state.selectedGroup;
        confirmOrganization(`Rimuovere ${member.name} da ${group.name}? Le abilitazioni ricevute da questo gruppo e dai suoi gruppi padre non saranno più applicate tramite questa appartenenza. Altre appartenenze e abilitazioni dirette restano valide.`, async () => {
          await api(`/api/groups/${encodeURIComponent(group.id)}/members/${encodeURIComponent(member.user_id)}`, { method: "DELETE" });
          showNotice("Utente rimosso dal gruppo. Le ore già registrate sono conservate.");
          await refreshOrganization(group.id);
        });
      }, true));
      $("group-members").append(item);
    }
    $("group-members-empty").hidden = state.groupMembers.length !== 0;
    const first = node("option", "", "Seleziona un utente attivo");
    first.value = "";
    const options = state.organizationUsers.filter((user) => user.active && !state.groupMembers.some((member) => Number(member.user_id) === Number(user.id))).map((user) => {
      const option = node("option", "", `${user.name} · ${user.email}`);
      option.value = user.id;
      return option;
    });
    $("group-member-user").replaceChildren(first, ...options);
    setOrganizationBusy(state.organizationBusy);
  }

  async function loadGroupMembers() {
    if (!state.selectedGroup || !can("manage_groups")) return;
    const groupId = state.selectedGroup.id;
    const request = ++state.groupMembersRequest;
    state.groupMembersReady = false;
    $("group-members-loading").hidden = false;
    $("group-members-error").hidden = true;
    $("group-members-empty").hidden = true;
    $("group-members").replaceChildren();
    setOrganizationBusy(state.organizationBusy);
    try {
      const data = await api(`/api/groups/${encodeURIComponent(groupId)}/members`);
      if (request !== state.groupMembersRequest || Number(state.selectedGroup?.id) !== Number(groupId) || !can("manage_groups")) return;
      state.groupMembers = data.members;
      state.groupMembersReady = true;
      renderGroupMembers();
    } catch (error) {
      if (request === state.groupMembersRequest) dialogError("group-members-error", error);
    } finally {
      if (request === state.groupMembersRequest) {
        $("group-members-loading").hidden = true;
        setOrganizationBusy(state.organizationBusy);
      }
    }
  }

  function openGroup(group = null) {
    if (!requirePermission("manage_groups") || state.organizationBusy || !state.organizationReady || busyDialogs.has("group-dialog")) return;
    $("group-form").reset();
    $("group-error").hidden = true;
    $("group-id").value = group?.id || "";
    $("group-name").value = group?.name || "";
    const first = node("option", "", "Primo livello");
    first.value = "";
    const options = orderedGroups().filter((candidate) => !group || !groupDescendsFrom(candidate, group.id)).map((candidate) => {
      const option = node("option", "", groupPath(candidate));
      option.value = candidate.id;
      return option;
    });
    $("group-parent").replaceChildren(first, ...options);
    $("group-parent").value = group ? group.parent_id || "" : state.selectedGroup?.id || "";
    $("group-dialog-title").textContent = group ? "Modifica gruppo" : "Nuovo gruppo";
    $("group-save").textContent = group ? "Salva modifiche" : "Crea gruppo";
    $("group-dialog").showModal();
    $("group-name").focus();
  }

  function openJobRole(role = null) {
    if (!requirePermission("manage_job_roles") || state.organizationBusy || role?.builtin || busyDialogs.has("job-role-dialog")) return;
    $("job-role-form").reset();
    $("job-role-error").hidden = true;
    $("job-role-id").value = role?.id || "";
    $("job-role-code").value = role?.code || "";
    $("job-role-name").value = role?.name || "";
    $("job-role-log-time").value = role?.can_log_time ? "1" : "0";
    $("job-role-dialog-title").textContent = role ? "Modifica ruolo commessa" : "Nuovo ruolo commessa";
    $("job-role-save").textContent = role ? "Salva modifiche" : "Crea ruolo";
    $("job-role-dialog").showModal();
    $("job-role-code").focus();
  }

  function confirmOrganization(message, operation) {
    if (!requirePermission("manage_groups") || state.organizationBusy) return;
    state.organizationConfirm = operation;
    $("organization-confirm-text").textContent = message;
    $("organization-confirm").hidden = false;
    $("organization-confirm-cancel").focus();
  }

  function resetGroupGrantForm(grant = null) {
    state.editingGroupGrant = grant;
    $("group-grant-form").reset();
    $("group-grant-end").setCustomValidity("");
    const first = node("option", "", "Seleziona un gruppo");
    first.value = "";
    const options = orderedGroups(state.assignmentGroups).map((group) => {
      const option = node("option", "", groupPath(group, state.assignmentGroups));
      option.value = group.id;
      return option;
    });
    $("group-grant-group").replaceChildren(first, ...options);
    fillRoleSelect("group-grant-role", state.assignmentRoles, grant?.role_id || 2);
    if (grant) {
      $("group-grant-group").value = grant.group_id;
      $("group-grant-start").value = grant.starts_on || "";
      $("group-grant-end").value = grant.ends_on || "";
    }
    $("group-grant-form-title").textContent = grant ? "Modifica abilitazione gruppo" : "Abilita un gruppo";
    $("group-grant-reset").hidden = !grant;
    setAssignmentBusy(busyDialogs.has("assignment-dialog"));
  }

  function renderGroupGrants() {
    if (!state.assignmentsReady || !state.assignmentJob || !can("manage_assignments")) return;
    $("group-grants-list").replaceChildren();
    for (const grant of state.groupGrants) {
      const item = node("li", "assignment-item");
      const identity = node("div", "assignment-identity");
      const group = state.assignmentGroups.find((candidate) => Number(candidate.id) === Number(grant.group_id));
      identity.append(node("strong", "", group ? groupPath(group, state.assignmentGroups) : grant.group_name));
      const [label, status] = assignmentStatus({ ...grant, active: true });
      const meta = node("div", "assignment-meta");
      meta.append(node("span", "job-access-role", grant.role_name), node("span", "status-badge status-" + status, label), node("span", "assignment-period", assignmentPeriod(grant)));
      identity.append(meta);
      const controls = node("div", "row-actions");
      controls.append(action("Modifica", "Modifica abilitazione del gruppo " + grant.group_name, () => {
        if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
        state.revokeGroupGrant = null;
        $("group-grant-revoke").hidden = true;
        resetGroupGrantForm(grant);
        $("group-grant-role").focus();
      }), action("Revoca", "Revoca abilitazione del gruppo " + grant.group_name, () => {
        if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
        state.revokeGroupGrant = grant;
        state.revokeAssignment = null;
        $("assignment-revoke").hidden = true;
        $("group-grant-revoke-text").textContent = `Revocare l’abilitazione di ${grant.group_name} e dei suoi sottogruppi? ${isCommonJob(state.assignmentJob) ? "La commessa è comune: tutti gli utenti attivi manterranno il diritto di consultarla e registrare le proprie ore." : "Eventuali abilitazioni dirette o ricevute da altri gruppi restano valide. Le ore già registrate sono conservate."}`;
        $("group-grant-revoke").hidden = false;
        $("group-grant-revoke-cancel").focus();
      }, true));
      item.append(identity, controls);
      $("group-grants-list").append(item);
    }
    $("group-grants-empty").hidden = state.groupGrants.length > 0;
    setAssignmentBusy(busyDialogs.has("assignment-dialog"));
  }

  function isAlertManager() {
    return state.session?.role === "manager";
  }

  function resetAlertsAndNotifications() {
    state.alertRulesRequest++;
    state.notificationsRequest++;
    state.alertRules = [];
    state.alertUsers = [];
    state.alertRulesReady = false;
    state.alertRulesLoading = false;
    state.editingAlertRule = null;
    state.runningAlertRule = null;
    state.alertSmtpConfigured = false;
    state.notifications = [];
    state.notificationsReady = false;
    state.notificationsLoading = false;
    state.notificationsUnread = 0;
    state.readingNotifications = new Set();
    ["alert-rules-list", "alert-rule-users", "notifications-list"].forEach((id) => $(id).replaceChildren());
    ["alert-rules-count", "notifications-count", "nav-notifications-count"].forEach((id) => { $(id).textContent = "0"; });
    $("nav-notifications-count").hidden = true;
    $("alert-rule-form").reset();
    $("notifications-summary").textContent = "Caricamento delle notifiche…";
    $("notifications-empty").hidden = true;
    $("notifications-error").hidden = true;
  }

  function alertDateTime(value, fallback = "Non ancora eseguita") {
    if (!value) return fallback;
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString("it-IT", { timeZone: "Europe/Rome", day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" });
  }

  function setAlertRulesBusy() {
    const busy = !isAlertManager() || !state.alertRulesReady || state.alertRulesLoading || state.runningAlertRule !== null;
    $("new-alert-rule").disabled = busy;
    document.querySelectorAll("[data-new-alert-type]").forEach((button) => { button.disabled = busy; });
    $("alert-rules-list").querySelectorAll("button").forEach((button) => { button.disabled = busy; });
    $("alert-rules-refresh").disabled = state.alertRulesLoading || state.runningAlertRule !== null;
    $("alert-rules-retry").disabled = state.alertRulesLoading;
  }

  function renderAlertRules() {
    if (!isAlertManager()) return;
    $("alert-rules-list").replaceChildren();
    for (const rule of state.alertRules) {
      const item = node("li", "alert-rule-card");
      const heading = node("div", "alert-rule-heading");
      const enabled = rule.enabled === true || rule.enabled === 1;
      heading.append(node("h4", "", rule.name), node("span", "status-badge " + (enabled ? "status-active" : "status-paused"), enabled ? "Attiva" : "Sospesa"));
      const recipients = (rule.recipient_user_ids || []).map((id) => {
        const user = state.alertUsers.find((candidate) => String(candidate.id) === String(id));
        return user ? user.name || user.email : "Utente #" + id;
      });
      const emails = rule.recipient_emails || [];
      const meta = node("dl", "alert-rule-details");
      const fields = [
        ["Controllo", alertTypes[rule.type]?.label || rule.type],
        ["Esecuzione", `Ogni giorno alle ${rule.run_time} · Europe/Rome`],
        ["Notifiche nell’app", recipients.length ? recipients.join(", ") : "Nessun destinatario nell’app"],
        ["Email", emails.length ? emails.join(", ") + (state.alertSmtpConfigured ? "" : " · invii in attesa") : "Nessun destinatario email"],
        ["Ultima esecuzione", alertDateTime(rule.last_run_at)],
        ["Prossima esecuzione", enabled ? alertDateTime(rule.next_run_at, "In programmazione") : "Sospesa"]
      ];
      if (rule.type === "inventory_due") fields.splice(1,0,["Anticipo",`${rule.days_before} giorni · comprese scadenze superate`]);
      for (const [label, value] of fields) meta.append(node("dt", "", label), node("dd", "", value));
      const controls = node("div", "row-actions");
      controls.append(action("Modifica", "Modifica la regola " + rule.name, () => openAlertRule(rule)));
      controls.append(action(state.runningAlertRule === rule.id ? "Controllo in corso…" : "Esegui ora", "Esegui ora la regola " + rule.name, () => runAlertRule(rule)));
      item.append(heading, meta, controls);
      $("alert-rules-list").append(item);
    }
    $("alert-rules-count").textContent = state.alertRules.length;
    $("alert-rules-empty").hidden = !state.alertRulesReady || state.alertRules.length > 0;
    $("alerts-mail-status").textContent = state.alertSmtpConfigured ? "Il canale email è configurato. Le notifiche nell’app restano disponibili nella sezione Notifiche." : "Email non configurata: gli invii vengono messi in attesa e non vengono spediti. Le notifiche nell’app sono disponibili subito.";
    setAlertRulesBusy();
  }

  async function refreshAlertRules() {
    if (!isAlertManager() || state.alertRulesLoading) return;
    const request = ++state.alertRulesRequest;
    state.alertRulesLoading = true;
    $("alert-rules-loading").hidden = state.alertRulesReady;
    $("alert-rules-error").hidden = true;
    $("alert-rules-empty").hidden = true;
    setAlertRulesBusy();
    try {
      const data = await api("/api/alert-rules");
      if (request !== state.alertRulesRequest || !isAlertManager()) return;
      if (!Array.isArray(data.rules) || !Array.isArray(data.users)) throw new Error("Il server ha restituito un elenco regole non valido. Riprova.");
      state.alertRules = data.rules;
      state.alertUsers = data.users;
      state.alertSmtpConfigured = data.smtp_configured === true;
      state.alertRulesReady = true;
      renderAlertRules();
    } catch (error) {
      if (request === state.alertRulesRequest && isAlertManager()) {
        $("alert-rules-error-text").textContent = error.message;
        $("alert-rules-error").hidden = false;
      }
    } finally {
      if (request === state.alertRulesRequest) {
        state.alertRulesLoading = false;
        $("alert-rules-loading").hidden = true;
        setAlertRulesBusy();
      }
    }
  }

  function renderAlertRuleTypeHelp() {
    $("alert-rule-type-help").textContent = alertTypes[$("alert-rule-type").value]?.description || "";
    const inventory = $("alert-rule-type").value === "inventory_due";
    $("alert-rule-days-field").hidden = !inventory;
    $("alert-rule-days").disabled = !inventory;
    $("alert-rule-days").required = inventory;
    $("alert-recipients-help").textContent = inventory ? "Seleziona fino a 50 utenti attivi. Tutti i destinatari selezionati ricevono le scadenze dell’inventario aziendale." : "Seleziona fino a 50 utenti attivi. Gli operatori ricevono soltanto avvisi che li riguardano; i gestori possono ricevere gli avvisi di tutti.";
  }

  function openAlertRule(rule = null, type = "missing_hours") {
    if (!isAlertManager() || !state.alertRulesReady || state.alertRulesLoading || state.runningAlertRule !== null || busyDialogs.has("alert-rule-dialog")) return;
    state.editingAlertRule = rule;
    $("alert-rule-form").reset();
    $("alert-rule-error").hidden = true;
    $("alert-rule-emails").setCustomValidity("");
    $("alert-rule-dialog-title").textContent = rule ? "Modifica regola" : "Nuova regola";
    $("alert-rule-save").textContent = rule ? "Salva modifiche" : "Crea regola";
    $("alert-rule-type").value = rule?.type || type;
    $("alert-rule-name").value = rule?.name || alertTypes[type]?.label || "";
    $("alert-rule-time").value = rule?.run_time || "09:00";
    $("alert-rule-days").value = rule?.days_before || 30;
    $("alert-rule-enabled").checked = rule ? rule.enabled === true || rule.enabled === 1 : true;
    $("alert-rule-emails").value = (rule?.recipient_emails || []).join("\n");
    $("alert-rule-emails-help").textContent = state.alertSmtpConfigured ? "Fino a 50 indirizzi separati da virgole o righe." : "Fino a 50 indirizzi separati da virgole o righe. Email non configurata: gli invii resteranno in attesa, senza spedizione.";
    const selected = new Set((rule?.recipient_user_ids || []).map(String));
    const users = [...state.alertUsers];
    for (const id of selected) if (!users.some((user) => String(user.id) === id)) users.push({ id, name: "Utente #" + id, unavailable: true });
    $("alert-rule-users").replaceChildren();
    for (const user of users) {
      const label = node("label", "alert-recipient-option");
      const checkbox = node("input");
      checkbox.type = "checkbox";
      checkbox.value = user.id;
      checkbox.checked = selected.has(String(user.id));
      checkbox.dataset.unavailable = user.unavailable ? "1" : "0";
      const identity = node("span");
      identity.append(node("strong", "", user.name || user.email));
      identity.append(node("span", "", user.unavailable ? "Non più attivo: deselezionalo per salvare" : `${user.email} · ${roles[user.role] || "Operatore"}`));
      label.append(checkbox, identity);
      $("alert-rule-users").append(label);
    }
    if (!users.length) $("alert-rule-users").append(node("p", "field-help", "Nessun utente attivo disponibile. Puoi impostare destinatari email in attesa di configurazione."));
    renderAlertRuleTypeHelp();
    $("alert-rule-dialog").showModal();
    $("alert-rule-name").focus();
  }

  function alertRecipientEmails() {
    const emails = [...new Set($("alert-rule-emails").value.split(/[,\r\n]+/).map((email) => email.trim().toLowerCase()).filter(Boolean))];
    if (emails.length > 50) throw new Error("Inserisci al massimo 50 indirizzi email.");
    for (const email of emails) {
      if (email.length > 254 || /[^\x21-\x7e]/.test(email) || !/^[^\s@,;<>]+@[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\.[a-z0-9-]{2,}$/i.test(email) || email.includes("..")) throw new Error(`L’indirizzo “${email}” non è valido. Separa gli indirizzi con virgole o righe.`);
    }
    return emails;
  }

  async function runAlertRule(rule) {
    if (!isAlertManager() || state.runningAlertRule !== null || state.alertRulesLoading) return;
    const email = state.session.email;
    state.runningAlertRule = rule.id;
    renderAlertRules();
    try {
      const result = await post(`/api/alert-rules/${encodeURIComponent(rule.id)}/run`, {});
      if (!isAlertManager() || state.session.email !== email) return;
      state.auditReady = false;
      state.alertRulesRequest++;
      state.alertRulesLoading = false;
      state.notificationsRequest++;
      state.notificationsLoading = false;
      const count = Number(result.notifications_created) || 0;
      const queued = Number(result.emails_queued) || 0;
      showNotice(`Controllo completato: ${count} ${count === 1 ? "nuova notifica nell’app" : "nuove notifiche nell’app"}.${queued ? ` ${queued} email in attesa; nessun invio effettuato.` : ""}`);
      await Promise.all([refreshAlertRules(), refreshNotifications()]);
    } catch (error) {
      if (isAlertManager() && state.session.email === email) showNotice(error.message, true);
    } finally {
      if (state.session?.email === email && state.runningAlertRule === rule.id) {
        state.runningAlertRule = null;
        renderAlertRules();
      }
    }
  }

  function renderNotifications() {
    $("notifications-list").replaceChildren();
    for (const notification of state.notifications) {
      const unread = !notification.read_at;
      const item = node("li", "notification-card" + (unread ? " is-unread" : ""));
      const heading = node("div", "notification-heading");
      const title = node("h3", "", notification.title);
      heading.append(title);
      if (unread) heading.append(node("span", "notification-new-label", "Da leggere"));
      const created = node("time", "", alertDateTime(notification.created_at, ""));
      created.dateTime = notification.created_at;
      item.append(heading, node("p", "notification-message", notification.message), created);
      if (notification.work_date && validDate(notification.work_date)) item.append(node("p", "notification-context", "Giornata: " + dateLabel(notification.work_date)));
      if (notification.event_kind === "inventory_due" && Number.isSafeInteger(notification.asset_id) && notification.asset_id > 0) {
        item.append(action("Apri asset", "Apri l’asset relativo alla notifica", () => { selectTab("inventory"); openInventoryDetail(notification.asset_id); }));
      }
      if (unread) {
        const button = action(state.readingNotifications.has(notification.id) ? "Aggiornamento…" : "Segna come letta", "Segna come letta: " + notification.title, () => markNotificationRead(notification));
        button.disabled = state.readingNotifications.has(notification.id);
        item.append(button);
      } else item.append(node("span", "notification-read-label", "Letta il " + alertDateTime(notification.read_at, "")));
      $("notifications-list").append(item);
    }
    $("notifications-count").textContent = state.notifications.length;
    $("nav-notifications-count").textContent = state.notificationsUnread;
    $("nav-notifications-count").setAttribute("aria-label", `${state.notificationsUnread} notifiche non lette`);
    $("nav-notifications-count").hidden = state.notificationsUnread === 0;
    $("notifications-summary").textContent = (state.notifications.length === 100 ? "Sono mostrati gli ultimi 100 avvisi. " : "") + (state.notificationsUnread ? `${state.notificationsUnread} ${state.notificationsUnread === 1 ? "notifica da leggere" : "notifiche da leggere"}.` : "Hai letto tutti gli avvisi ricevuti.");
    $("notifications-empty").hidden = !state.notificationsReady || state.notifications.length > 0;
  }

  async function refreshNotifications() {
    if (!state.session?.authenticated || state.notificationsLoading) return;
    const request = ++state.notificationsRequest;
    state.notificationsLoading = true;
    $("notifications-loading").hidden = state.notificationsReady;
    $("notifications-error").hidden = true;
    $("notifications-refresh").disabled = true;
    $("notifications-retry").disabled = true;
    try {
      const data = await api("/api/notifications");
      if (request !== state.notificationsRequest) return;
      if (!Array.isArray(data.notifications) || !Number.isInteger(data.unread_count) || data.unread_count < 0) throw new Error("Il server ha restituito un elenco notifiche non valido. Riprova.");
      state.notifications = data.notifications;
      state.notificationsUnread = data.unread_count;
      state.notificationsReady = true;
      renderNotifications();
    } catch (error) {
      if (request === state.notificationsRequest) {
        $("notifications-error-text").textContent = error.message;
        $("notifications-error").hidden = false;
        if (!state.notificationsReady) $("notifications-summary").textContent = "Le notifiche non sono disponibili. Riprova a caricarle.";
      }
    } finally {
      if (request === state.notificationsRequest) {
        state.notificationsLoading = false;
        $("notifications-loading").hidden = true;
        $("notifications-refresh").disabled = false;
        $("notifications-retry").disabled = false;
      }
    }
  }

  async function markNotificationRead(notification) {
    if (state.readingNotifications.has(notification.id)) return;
    const email = state.session?.email;
    state.readingNotifications.add(notification.id);
    renderNotifications();
    try {
      await post(`/api/notifications/${encodeURIComponent(notification.id)}/read`, {});
      if (state.session?.email !== email) return;
      // A poll started before the mark must not restore the unread version.
      state.notificationsRequest++;
      state.notificationsLoading = false;
      const current = state.notifications.find((item) => item.id === notification.id);
      if (current && !current.read_at) {
        current.read_at = new Date().toISOString();
        state.notificationsUnread = Math.max(0, state.notificationsUnread - 1);
        renderNotifications();
      }
      await refreshNotifications();
    } catch (error) {
      if (state.session?.email === email) showNotice(error.message, true);
    } finally {
      if (state.session?.email === email) {
        state.readingNotifications.delete(notification.id);
        renderNotifications();
      }
    }
  }

  const auditActions = {
    "tasks.create": "Attività creata", "tasks.update": "Attività aggiornata", "tasks.delete": "Attività eliminata",
    "blockers.create": "Blocco segnalato", "blockers.update": "Blocco aggiornato", "blockers.delete": "Blocco eliminato", "blockers.task_detached": "Blocco scollegato dall’attività eliminata",
    "jobs.create": "Commessa creata", "jobs.update": "Commessa aggiornata", "jobs.delete": "Commessa e ore eliminate",
    "time_entries.create": "Ore registrate", "time_entries.update": "Ore aggiornate", "time_entries.delete": "Ore eliminate",
    "assignments.create": "Utente abilitato", "assignments.update": "Abilitazione aggiornata", "assignments.delete": "Abilitazione revocata",
    "suppliers.create": "Fornitore creato", "suppliers.update": "Fornitore aggiornato",
    "contracts.create": "Contratto creato", "contracts.update": "Contratto aggiornato", "contract_documents.create": "PDF contrattuale archiviato",
    "labor_rates.create": "Tariffa oraria creata", "labor_rates.update": "Tariffa oraria aggiornata",
    "alert_rules.create": "Regola alert creata", "alert_rules.update": "Regola alert aggiornata", "alert_rules.run": "Controllo alert eseguito", "alert_rules.trigger": "Controllo alert automatico eseguito",
    "groups.create": "Gruppo creato", "groups.update": "Gruppo aggiornato", "groups.delete": "Gruppo eliminato",
    "group_members.create": "Utente aggiunto al gruppo", "group_members.delete": "Utente rimosso dal gruppo",
    "job_roles.create": "Ruolo commessa creato", "job_roles.update": "Ruolo commessa aggiornato",
    "group_grants.create": "Gruppo abilitato", "group_grants.update": "Abilitazione gruppo aggiornata", "group_grants.delete": "Abilitazione gruppo revocata",
    "users.create": "Utente creato", "users.update": "Utente aggiornato", "system.migration": "Database aggiornato"
  };
  const auditTypes = { task: "Attività", blocker: "Blocco", job: "Commessa", time_entry: "Registrazione ore", assignment: "Abilitazione", user: "Utente", supplier: "Fornitore", contract: "Contratto", contract_document: "Documento contrattuale", labor_rate: "Tariffa oraria", alert_rule: "Regola alert", group: "Gruppo", group_member: "Membro gruppo", job_role: "Ruolo commessa", group_grant: "Abilitazione gruppo", system: "Sistema" };
  const auditLabels = {
    task_id: "ID attività", assigned_user_id: "ID responsabile", assigned_user_name: "Responsabile", owner_name: "Responsabile", priority: "Priorità", impact: "Impatto", next_action: "Prossima azione", due_date: "Scadenza", revision: "Revisione", created_by: "ID autore", deleted_tasks: "Attività eliminate", deleted_blockers: "Blocchi eliminati",
    parent_id: "ID gruppo padre", group_id: "ID gruppo", group_name: "Gruppo", role_id: "ID ruolo commessa", role_code: "Codice ruolo", role_name: "Ruolo commessa", can_log_time: "Può registrare ore", builtin: "Ruolo predefinito", id: "ID", user_id: "ID utente", job_id: "ID commessa", owner_id: "ID autore", owner: "Account", email: "Email", name: "Nome", role: "Ruolo", active: "Accesso attivo",
    title: "Titolo", supplier_id: "ID fornitore", supplier_name: "Fornitore", contract_id: "ID contratto", contract_code: "Codice contratto", contract_title: "Contratto", vat_number: "Partita IVA", phone: "Telefono", address: "Indirizzo", notes: "Note", value_cents: "Valore", hourly_cents: "Tariffa oraria", currency: "Valuta", file_name: "Nome documento", byte_size: "Dimensione",
    type: "Tipo di controllo", run_time: "Orario giornaliero", enabled: "Regola attiva", recipient_user_ids: "ID destinatari nell’app", recipient_emails: "Destinatari email", recipient_email_count: "Numero destinatari email", recipient_user_count: "Numero destinatari nell’app", notifications_created: "Notifiche create", emails_queued: "Email in attesa", last_run_at: "Ultima esecuzione", last_run_on: "Giorno ultima esecuzione", next_run_at: "Prossima esecuzione", created_at: "Creata il", updated_at: "Aggiornata il",
    code: "Codice", status: "Stato", description: "Descrizione", client: "Cliente", is_common: "Commessa comune", deleted_time_entries: "Registrazioni ore eliminate", deleted_minutes: "Ore eliminate", deleted_assignments: "Abilitazioni utenti eliminate", deleted_group_grants: "Abilitazioni gruppi eliminate", date: "Giornata", minutes: "Ore lavorate", starts_on: "Abilitato dal", ends_on: "Abilitato fino al",
    user_name: "Utente", user_email: "Email utente", job_name: "Commessa", job_code: "Codice commessa", note: "Attività / nota", version: "Versione", schema_version: "Versione database", migrated_entries: "Registrazioni migrate", migrated_users: "Utenti migrati", mapped_entries: "Registrazioni collegate agli utenti", archived_entries: "Registrazioni storiche conservate"
  };

  function auditTime(value) {
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString("it-IT", { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
  }

  function auditValue(key, value, entityType) {
    if (value === null || value === undefined || value === "") return "—";
    if (key === "minutes" || key === "deleted_minutes") return minutesLabel(value);
    if (key === "value_cents") return euroLabel(value);
    if (key === "hourly_cents") return euroLabel(value) + " / ora";
    if (key === "byte_size") return fileSize(value);
    if (key === "role") return roles[value] || String(value);
    if (key === "status") {
      const catalogue = entityType === "task" ? workTaskStates : entityType === "blocker" ? workBlockerStates : entityType === "contract" ? contractStates : entityType === "job" ? states : null;
      return catalogue?.[value] || String(value);
    }
    if (key === "priority" && entityType === "task") return workPriorities[value] || String(value);
    if (key === "impact" && entityType === "blocker") return workImpacts[value] || String(value);
    if (key === "active") return value === true || value === 1 || value === "1" ? "Attivo" : "Disattivato";
    if (key === "is_common") return value === true || value === 1 || value === "1" ? "Sì" : "No";
    if (key === "enabled") return value === true || value === 1 || value === "1" ? "Attiva" : "Sospesa";
    if (key === "type" && entityType === "alert_rule") return alertTypes[value]?.label || String(value);
    if (["last_run_at", "next_run_at", "created_at", "updated_at"].includes(key) && entityType === "alert_rule") return alertDateTime(value, "—");
    if (key === "last_run_on" && validDate(String(value))) return dateLabel(value);
    if (["date", "starts_on", "ends_on", "due_date"].includes(key) && validDate(String(value))) return dateLabel(value);
    if (typeof value === "boolean") return value ? "Sì" : "No";
    return String(value);
  }

  function auditSnapshot(snapshot, title, entityType) {
    const section = node("section", "audit-snapshot");
    section.append(node("h4", "", title));
    if (!snapshot || typeof snapshot !== "object") {
      section.append(node("p", "no-value", title === "Prima" ? "Nessun dato precedente" : "Elemento rimosso"));
      return section;
    }
    const list = node("dl");
    function addFields(fields, prefix = "", depth = 0, fieldEntityType = entityType) {
      for (const [key, value] of Object.entries(fields)) {
        if (/password|hash|salt|token|secret/i.test(key)) continue;
        const contextualLabel = fieldEntityType === "task" && key === "parent_id" ? "ID attività principale" : fieldEntityType === "blocker" && key === "owner_id" ? "ID responsabile" : fieldEntityType === "labor_rate" && key === "starts_on" ? "Valida dal" : fieldEntityType === "labor_rate" && key === "ends_on" ? "Valida al" : null;
        const label = prefix + (contextualLabel || auditLabels[key] || key.replaceAll("_", " "));
        if (value && typeof value === "object" && !Array.isArray(value) && depth < 3) {
          const nestedType = key === "contract" ? "contract" : key === "job" ? "job" : null;
          addFields(value, label + " · ", depth + 1, nestedType);
          continue;
        }
        list.append(node("dt", "", label), node("dd", "", Array.isArray(value) ? value.map((item) => typeof item === "object" ? "Elemento" : auditValue(key, item, fieldEntityType)).join(", ") : auditValue(key, value, fieldEntityType)));
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
      const subject = snapshot.job_name || snapshot.name || snapshot.user_name || snapshot.user_email || snapshot.job_code || snapshot.code;
      const type = auditTypes[event.entity_type] || "Elemento";
      const object = `${type}${event.entity_id ? " #" + event.entity_id : ""}${subject ? " · " + subject : ""}`;
      const actor = event.actor_name || event.actor_email || "Sistema";
      const byline = node("p", "audit-byline", actor + (event.actor_name && event.actor_email ? " · " + event.actor_email : ""));
      const details = node("details", "audit-details");
      details.append(node("summary", "", "Visualizza prima e dopo"));
      const snapshots = node("div", "audit-snapshots");
      snapshots.append(auditSnapshot(event.before, "Prima", event.entity_type), auditSnapshot(event.after, "Dopo", event.entity_type));
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
    invalidateReports();
    const request = ++state.refreshRequest;
    $("load-error").hidden = true;
    $("retry-load").disabled = true;
    if (!state.jobsReady) $("jobs-loading").textContent = "Caricamento delle commesse…";
    const jobsTask = api("/api/jobs").then((data) => {
      if (request !== state.refreshRequest) return;
      state.jobs = data.jobs;
      state.jobsReady = true;
      renderJobs();
      renderWorkJobs();
      if (state.activeTab === "work-board" && state.workJobId && !state.workBusy) return refreshWorkBoard();
    }).catch((error) => {
      if (request !== state.refreshRequest) return;
      if (!state.jobsReady) $("jobs-loading").textContent = "Le commesse non sono disponibili. Usa Riprova per ricaricare i dati.";
      showLoadError(error);
    });
    const tasks = [jobsTask, refreshEntries(), refreshNotifications()];
    if (state.activeTab === "inventory" && !state.inventoryLoading) tasks.push(refreshInventory());
    if (can("manage_users")) tasks.push(refreshUsers());
    if (can("manage_suppliers")) tasks.push(refreshRegistry("suppliers"));
    if (can("manage_contracts")) tasks.push(refreshRegistry("contracts"));
    if (can("manage_groups") && state.activeTab === "organization") tasks.push(refreshOrganization());
    if (can("view_audit") && state.activeTab === "audit") tasks.push(refreshAudit());
    if (isAlertManager() && state.activeTab === "alerts") tasks.push(refreshAlertRules());
    if (state.activeTab === "reports") tasks.push(refreshReports());
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
    $("job-common").checked = isCommonJob(job);
    $("job-delete-field").hidden = !job;
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

  function openDeleteJob() {
    if (!requirePermission("manage_jobs") || !state.editingJob || busyDialogs.has("job-dialog") || busyDialogs.has("job-delete-dialog")) return;
    state.deleteJob = state.editingJob;
    $("job-delete-error").hidden = true;
    $("job-delete-description").textContent = `Stai per eliminare definitivamente “${state.deleteJob.name}” (${state.deleteJob.code}).`;
    $("job-delete-dialog").showModal();
  }

  function openEntry(entry = null, jobId = "") {
    if (entry && !requirePermission("edit_time_entries")) return;
    if (busyDialogs.has("entry-dialog")) return;
    if (!state.jobsReady) { showNotice("Attendi il caricamento delle commesse, poi riprova.", true); return; }
    const writableJobs = state.jobs.filter(canLogTime);
    if (writableJobs.length === 0) {
      if (!can("manage_jobs")) { showNotice("Nessuna commessa abilitata alla registrazione delle ore. Chiedi a un gestore il ruolo adatto alla commessa su cui devi lavorare.", true); return; }
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
    renderEntryJobs(entry ? entry.job_id : jobId);
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

  const workTaskStates = { todo: "Da fare", in_progress: "In corso", blocked: "Bloccato", in_review: "Da verificare", done: "Completato" };
  const workPriorities = { low: "Bassa", normal: "Normale", high: "Alta", urgent: "Urgente" };
  const workBlockerStates = { open: "Aperto", in_progress: "In gestione", resolved: "Risolto" };
  const workImpacts = { low: "Basso", medium: "Medio", high: "Alto", critical: "Critico" };
  Object.assign(state, { inventoryItems: [], inventoryReady: false, inventoryLoading: false, inventoryConfigured: null, inventorySummary: null, inventoryRequest: 0, inventoryEpoch: 0, inventoryCursor: null, inventoryRetryMore: false, inventoryEditorRequest: 0, inventoryEditingId: null, inventoryEditingAsset: null, inventoryConflict: false, inventoryDetailRequest: 0, inventoryDetailId: null, inventoryDetailAsset: null, inventoryHistoryRequest: 0, inventoryHistoryLoading: false, inventoryHistoryReady: false, inventoryHistoryCursor: null, inventoryHistoryRetryMore: false });
  const inventoryStates = { available: "Disponibile", assigned: "Assegnato", maintenance: "In manutenzione", retired: "Dismesso", lost: "Smarrito" };
  const inventoryFields = ["asset_tag", "name", "category", "serial_number", "manufacturer", "model", "status", "location", "assignee", "purchase_date", "warranty_until", "notes"];
  const inventoryTypes = { it_device: "Dispositivo IT", network_device: "Apparato di rete", physical: "Bene fisico", software: "Software", license: "Licenza" };
  const inventoryLicenseModels = { per_device: "Per dispositivo", per_user: "Per utente", concurrent: "Concorrente", site: "Per sede" };
  const inventoryRenewalModes = { manual: "Manuale", automatic: "Automatico", none: "Nessun rinnovo" };
  const inventoryTypeFields = {
    it_device: [["hostname", "Nome host", 253], ["ip_address", "Indirizzo IP", 45], ["mac_address", "Indirizzo MAC", 17], ["operating_system", "Sistema operativo", 120]],
    network_device: [["hostname", "Nome host", 253], ["ip_address", "Indirizzo IP", 45], ["mac_address", "Indirizzo MAC", 17], ["operating_system", "Sistema operativo / firmware", 120], ["network_role", "Ruolo di rete", 80], ["vlan", "VLAN", "number", 1, 4094], ["port_count", "Numero di porte", "number", 0, 65535]],
    physical: [["asset_kind", "Tipo di bene", 80], ["department", "Reparto", 120], ["maintenance_due", "Prossima manutenzione", "date"]],
    software: [["publisher", "Editore", 120], ["software_version", "Versione software", 80], ["edition", "Edizione", 80], ["software_kind", "Tipo di software", 80]],
    license: [["license_model", "Modello di licenza", inventoryLicenseModels, "per_device"], ["seats_total", "Posti acquistati", "number", 1, 1000000, 1], ["expires_on", "Scadenza", "date"], ["renewal_on", "Data di rinnovo", "date"], ["renewal_mode", "Rinnovo", inventoryRenewalModes, "manual"], ["license_reference", "Riferimento della licenza", 120]]
  };
  const inventoryRelationNames = { installed_on: "Software installato", connected_to: "Collegamento di rete", licensed_to: "Assegnazione di licenza" };
  const inventoryAssetCache = new Map();
  const inventoryPickers = new Map();
  Object.assign(state, { inventoryRelationsRequest: 0, inventoryRelationsReady: false, inventoryRelationsLoading: false, inventoryRelationsCursor: null, inventoryRelationsRetryMore: false, inventoryRelations: [], inventoryRelationBusy: false, inventoryRelationConflict: false });

  function inventoryAssetType(asset) { return asset?.asset_type || "it_device"; }
  function inventoryRemember(asset) { if (asset?.id) inventoryAssetCache.set(String(asset.id), asset); return asset; }
  function inventoryAssetLabel(asset) { return asset ? asset.asset_tag + " · " + asset.name : ""; }

  // Both pickers search and paginate the service, so every asset remains reachable.
  function createInventoryPicker(key, hostId, label, types, context) {
    const host = $(hostId);
    const picker = { key, host, types, context, request: 0, cursor: null, loading: false, items: [], selected: null, retryMore: false, enabled: true };
    const field = node("div", "work-field inventory-picker");
    const heading = node("label", "", label + " *");
    heading.htmlFor = key + "-choice";
    const controls = node("div", "inventory-picker-controls");
    picker.search = node("input"); picker.search.type = "search"; picker.search.maxLength = 160; picker.search.autocomplete = "off"; picker.search.placeholder = "Cerca per codice o nome…"; picker.search.setAttribute("aria-label", "Cerca " + label.toLowerCase());
    picker.type = node("select"); picker.type.setAttribute("aria-label", "Tipologia del destinatario");
    picker.find = node("button", "button button-secondary", "Cerca"); picker.find.type = "button";
    picker.choice = node("select"); picker.choice.id = key + "-choice"; picker.choice.required = true;
    picker.status = node("p", "field-help"); picker.status.setAttribute("role", "status");
    picker.error = node("p", "dialog-error"); picker.error.hidden = true; picker.error.setAttribute("role", "alert");
    picker.more = node("button", "row-button", "Carica altri risultati"); picker.more.type = "button"; picker.more.hidden = true;
    controls.append(picker.search, picker.type, picker.find); field.append(heading, controls, picker.choice, picker.status, picker.error, picker.more); host.replaceChildren(field);
    picker.render = () => {
      const selected = picker.selected;
      picker.choice.replaceChildren(new Option("Seleziona un asset…", ""));
      const options = [...(selected ? [selected] : []), ...picker.items.filter((asset) => !selected || String(asset.id) !== String(selected.id))];
      options.forEach((asset) => picker.choice.add(new Option(inventoryAssetLabel(asset), String(asset.id))));
      picker.choice.value = selected ? String(selected.id) : "";
      picker.more.hidden = !picker.cursor && !picker.retryMore;
      picker.more.textContent = picker.retryMore ? "Riprova a caricare altri risultati" : "Carica altri risultati";
      picker.find.disabled = picker.loading || !picker.enabled;
      picker.more.disabled = picker.loading || !picker.enabled;
      picker.choice.disabled = !picker.enabled;
      picker.search.disabled = !picker.enabled;
      picker.type.disabled = !picker.enabled;
      picker.status.textContent = picker.loading ? "Ricerca degli asset…" : picker.items.length ? picker.items.length + " risultati caricati." : "Cerca un asset nell’inventario.";
    };
    picker.setTypes = (allowed, clear = false) => {
      picker.request++; picker.loading = false; picker.cursor = null; picker.items = []; picker.types = allowed;
      const previous = picker.type.value;
      picker.type.replaceChildren(...allowed.map((type) => new Option(inventoryTypes[type], type)));
      if (allowed.includes(previous)) picker.type.value = previous;
      picker.type.hidden = allowed.length < 2;
      if (clear || picker.selected && !allowed.includes(inventoryAssetType(picker.selected))) picker.selected = null;
      picker.error.hidden = true; picker.retryMore = false; picker.render();
    };
    picker.setSelected = async (id) => {
      const request = ++picker.request; const epoch = state.inventoryEpoch;
      picker.selected = id ? inventoryAssetCache.get(String(id)) || { id, asset_tag: "#" + id, name: "Caricamento…", asset_type: types[0] } : null;
      picker.render();
      if (!id || inventoryAssetCache.has(String(id))) return;
      try {
        const data = await api("/api/inventory/assets/" + encodeURIComponent(id));
        if (request !== picker.request || epoch !== state.inventoryEpoch || !context()) return;
        picker.selected = inventoryRemember(data.asset); picker.render();
      } catch (error) {
        if (request !== picker.request || epoch !== state.inventoryEpoch || !context()) return;
        picker.selected = { id, asset_tag: "#" + id, name: "Asset selezionato", asset_type: types[0] }; picker.render();
        picker.error.textContent = "Non siamo riusciti a leggere il nome dell’asset selezionato. Puoi cercarlo di nuovo."; picker.error.hidden = false;
      }
    };
    picker.load = async (more = false) => {
      if (!picker.enabled || !context() || more && (picker.loading || !picker.cursor)) return;
      const queryText = picker.search.value.trim();
      if (new TextEncoder().encode(queryText).length > 160) { picker.search.setCustomValidity("Accorcia la ricerca."); picker.search.reportValidity(); return; }
      const request = ++picker.request; const epoch = state.inventoryEpoch;
      const query = new URLSearchParams({ q: queryText, asset_type: picker.type.value, limit: "30" });
      if (more) query.set("after_id", picker.cursor); else { picker.items = []; picker.cursor = null; }
      picker.loading = true; picker.retryMore = false; picker.error.hidden = true; picker.render();
      const current = () => request === picker.request && epoch === state.inventoryEpoch && context();
      try {
        const data = await api("/api/inventory/assets?" + query);
        if (!current()) return;
        const known = new Set(picker.items.map((asset) => String(asset.id)));
        data.items.forEach((asset) => {
          inventoryRemember(asset);
          if (!known.has(String(asset.id)) && (key !== "inventory-relation-picker" || String(asset.id) !== String(state.inventoryDetailId))) picker.items.push(asset);
        });
        picker.cursor = data.next_after_id || null;
      } catch (error) {
        if (!current()) return;
        picker.retryMore = more; picker.error.textContent = inventoryMessage(error); picker.error.hidden = false;
      } finally { if (current()) { picker.loading = false; picker.render(); } }
    };
    picker.choice.addEventListener("change", () => { picker.choice.setCustomValidity(""); picker.selected = [...picker.items, picker.selected].find((asset) => asset && String(asset.id) === picker.choice.value) || null; });
    picker.search.addEventListener("input", () => { picker.search.setCustomValidity(""); picker.request++; picker.loading = false; picker.cursor = null; picker.items = []; picker.retryMore = false; picker.render(); });
    picker.search.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); picker.load(); } });
    picker.type.addEventListener("change", () => picker.load());
    picker.find.addEventListener("click", () => picker.load()); picker.more.addEventListener("click", () => picker.load(true));
    picker.setTypes(types); inventoryPickers.set(key, picker); return picker;
  }

  function buildInventoryTypedFields() {
    const host = $("inventory-typed-fields");
    Object.entries(inventoryTypeFields).forEach(([type, fields]) => {
      const group = node("fieldset", "inventory-fields inventory-type-group"); group.id = "inventory-type-" + type;
      group.append(node("legend", "", "Dettagli · " + inventoryTypes[type]));
      const grid = node("div", "form-grid");
      if (type === "license") { const pickerHost = node("div", "inventory-picker-wide"); pickerHost.id = "inventory-software-picker-host"; grid.append(pickerHost); }
      fields.forEach(([name, label, kind, min, max, initial]) => {
        const field = node("div", "work-field"); const labelNode = node("label", "", label + (name === "seats_total" ? " *" : "")); labelNode.htmlFor = "inventory-" + type + "-" + name;
        const input = node(typeof kind === "object" ? "select" : "input"); input.id = labelNode.htmlFor; input.dataset.inventoryDetail = name;
        if (typeof kind === "object") Object.entries(kind).forEach(([value, text]) => input.add(new Option(text, value)));
        else { input.type = typeof kind === "number" ? "text" : kind; if (typeof kind === "number") input.maxLength = kind; if (kind === "number") { input.min = min; input.max = max; input.step = "1"; } if (kind === "date") { input.min = "0001-01-01"; input.max = "9999-12-31"; } input.autocomplete = "off"; }
        if (name === "seats_total") input.required = true;
        if (name === "mac_address") { input.pattern = "(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}"; input.placeholder = "00:1A:2B:3C:4D:5E"; }
        input.dataset.initial = initial ?? (typeof kind === "object" ? min : "") ?? "";
        input.addEventListener("input", () => input.setCustomValidity(""));
        field.append(labelNode, input); if (name === "license_reference") field.append(node("p", "field-help", "Inserisci un riferimento amministrativo; conserva le chiavi segrete nel sistema dedicato.")); grid.append(field);
      });
      group.append(grid); host.append(group);
    });
    createInventoryPicker("inventory-software-picker", "inventory-software-picker-host", "Software coperto", ["software"], () => $("inventory-dialog").open && isInventoryManager());
    createInventoryPicker("inventory-relation-picker", "inventory-relation-target", "Dispositivo destinatario", ["it_device", "network_device"], () => $("inventory-detail-dialog").open && !$("inventory-relation-form").hidden && isInventoryManager());
  }

  function inventoryShowTypeFields() {
    const selected = $("inventory-asset_type").value;
    Object.keys(inventoryTypes).forEach((type) => { const group = $("inventory-type-" + type); group.hidden = selected !== type; group.disabled = selected !== type; });
    const picker = inventoryPickers.get("inventory-software-picker"); picker.enabled = selected === "license"; picker.render();
  }

  function inventoryTypedValues() {
    const type = $("inventory-asset_type").value; const details = {};
    $("inventory-type-" + type).querySelectorAll("[data-inventory-detail]").forEach((input) => {
      input.value = input.value.trim(); input.setCustomValidity("");
      if (input.maxLength > 0 && new TextEncoder().encode(input.value).length > input.maxLength) input.setCustomValidity("Accorcia il contenuto di questo campo.");
      if (input.type === "date" && input.value && !validDate(input.value)) input.setCustomValidity("Inserisci una data valida.");
      if (input.value !== "") details[input.dataset.inventoryDetail] = input.type === "number" ? Number(input.value) : input.value;
    });
    if (type === "license") {
      const picker = inventoryPickers.get("inventory-software-picker");
      picker.choice.setCustomValidity(picker.selected ? "" : "Seleziona il software coperto dalla licenza.");
      if (picker.selected) details.software_id = Number(picker.selected.id);
    }
    return details;
  }

  function inventoryTypedDisplay(asset) {
    const type = inventoryAssetType(asset), d = asset.details || {}, fields = [];
    if (type === "license") { const software = inventoryAssetCache.get(String(d.software_id)); fields.push(["Software coperto", software ? inventoryAssetLabel(software) : d.software_id ? "Asset #" + d.software_id : ""]); }
    (inventoryTypeFields[type] || []).forEach(([name, label, kind]) => { const value = d[name]; fields.push([label, typeof kind === "object" ? kind[value] || value : kind === "date" && value && validDate(value) ? dateLabel(value) : value]); });
    if (type === "license" && asset.license_usage) {
      const usage = asset.license_usage;
      fields.push(["Posti assegnati", usage.allocated ?? 0], ["Posti disponibili", usage.available ?? 0], ["Validità della licenza", usage.expired ? "Scaduta" : "Valida"]);
      if (["concurrent", "site"].includes(d.license_model)) fields.push(["Conteggio assegnazioni", "Assegnazioni registrate manualmente; non misura l’utilizzo simultaneo."]);
    }
    return fields;
  }

  let inventorySearchTimer;
  Object.assign(state, { inventoryBackup: null, inventoryExporting: false, inventoryImportRequest: 0, inventoryImportBusy: false, inventoryImportPlan: null });

  function isInventoryManager() { return state.session?.role === "manager"; }

  function inventoryMessage(error) {
    return error.status === 503 ? "Il servizio Asset inventory non è disponibile. Chiedi a un gestore di verificarne la configurazione e l’avvio, poi riprova." : error.message;
  }

  function resetInventory() {
    clearTimeout(inventorySearchTimer);
    state.inventoryEpoch++;
    state.inventoryRequest++;
    state.inventoryEditorRequest++;
    state.inventoryDetailRequest++;
    state.inventoryHistoryRequest++;
    state.inventoryRelationsRequest++;
    state.inventoryImportRequest++;
    state.inventoryImportBusy = false; state.inventoryImportPlan = null; state.inventoryExporting = false; state.inventoryBackup = null;
    busyDialogs.delete("inventory-import-dialog");
    $("inventory-import-form").reset();
    $("inventory-import-preview").hidden = true;
    $("inventory-import-rows").replaceChildren();
    if ($("inventory-import-dialog").open) $("inventory-import-dialog").close();
    renderInventoryImport();
    state.inventoryRelations = []; state.inventoryRelationsReady = false; state.inventoryRelationsCursor = null; state.inventoryRelationsLoading = false; state.inventoryRelationBusy = false; state.inventoryRelationConflict = false;
    inventoryAssetCache.clear(); inventoryPickers.forEach((picker) => { picker.request++; picker.selected = null; picker.items = []; picker.loading = false; picker.cursor = null; picker.retryMore = false; picker.render(); });
    busyDialogs.delete("inventory-detail-dialog");
    $("inventory-relation-form").hidden = true; $("inventory-relations").replaceChildren();
    Object.assign(state, { inventoryItems: [], inventoryReady: false, inventoryLoading: false, inventoryConfigured: null, inventorySummary: null, inventoryCursor: null, inventoryRetryMore: false, inventoryEditingId: null, inventoryEditingAsset: null, inventoryConflict: false, inventoryDetailId: null, inventoryDetailAsset: null, inventoryHistoryLoading: false, inventoryHistoryReady: false, inventoryHistoryCursor: null });
    dialogBusy("inventory-dialog", false, "inventory-save", "Crea asset");
    ["inventory-dialog", "inventory-detail-dialog"].forEach((id) => { if ($(id).open) $(id).close(); });
    $("inventory-body").replaceChildren();
    $("inventory-history").replaceChildren();
    $("inventory-detail-data").replaceChildren();
    $("inventory-search-form").reset();
    $("inventory-search").setCustomValidity("");
    $("inventory-form").reset();
    $("inventory-error").hidden = true;
    renderInventory();
  }

  function renderInventory() {
    const manager = isInventoryManager();
    $("inventory-new").hidden = !manager;
    $("inventory-new").disabled = state.inventoryConfigured !== true;
    $("inventory-import").hidden = !manager;
    $("inventory-import").disabled = state.inventoryConfigured !== true || state.inventoryImportBusy;
    $("inventory-export").disabled = state.inventoryConfigured !== true || state.inventoryExporting;
    $("inventory-export").textContent = state.inventoryExporting ? "Esportazione…" : "Esporta CSV";
    const backup = state.inventoryBackup;
    $("inventory-backup-status").hidden = !manager || state.inventoryConfigured !== true;
    $("inventory-backup-status").textContent = !backup?.enabled ? "Backup automatico non configurato." : backup.last_error ? "Backup automatico: " + backup.last_error + (backup.last_success ? " Ultima copia riuscita: " + inventoryTime(backup.last_success) + "." : "") : "Backup automatico ogni " + backup.interval_hours + " ore · " + backup.keep + " copie conservate. " + (backup.last_success ? "Ultima copia: " + inventoryTime(backup.last_success) + "." : "Prima copia in preparazione.") + (backup.next_due ? " Prossima: " + inventoryTime(backup.next_due) + "." : "");
    $("inventory-refresh").disabled = state.inventoryLoading;
    $("inventory-permission").textContent = manager ? "Gestore · Censisci i beni e aggiorna assegnazioni, stato e garanzie. Per dismettere un bene, scegli Dismesso." : "Consulta i beni aziendali e il loro storico. Un gestore può creare e aggiornare le schede.";
    $("inventory-count").textContent = state.inventorySummary ? state.inventorySummary.total : "—";
    $("inventory-summary").hidden = !state.inventorySummary;
    ["total", ...Object.keys(inventoryStates)].forEach((key) => { $("inventory-" + key).textContent = state.inventorySummary ? state.inventorySummary[key] : "—"; });
    const filtered = Boolean($("inventory-search").value.trim() || $("inventory-filter").value || $("inventory-type-filter").value);
    $("inventory-clear").hidden = !filtered;
    $("inventory-loading").hidden = !state.inventoryLoading;
    $("inventory-loading").textContent = state.inventoryItems.length ? "Aggiornamento dell’inventario…" : "Caricamento dell’inventario…";
    $("inventory-table-wrap").hidden = !state.inventoryItems.length;
    $("inventory-empty").hidden = !state.inventoryReady || Boolean(state.inventoryItems.length) || state.inventoryLoading;
    $("inventory-empty-title").textContent = filtered ? "Nessun bene per questi filtri." : "Il primo bene parte da qui.";
    $("inventory-empty-description").textContent = filtered ? "Prova un’altra ricerca oppure azzera i filtri." : manager ? "Usa Nuovo asset per censire dotazioni, dispositivi e altri beni aziendali." : "Un gestore può censire dotazioni, dispositivi e altri beni aziendali.";
    $("inventory-footer").hidden = !state.inventoryReady;
    $("inventory-visible").textContent = state.inventoryItems.length + (state.inventoryItems.length === 1 ? " bene caricato" : " beni caricati") + (filtered ? " per i filtri selezionati." : " nell’inventario.");
    $("inventory-more").hidden = !state.inventoryCursor;
    $("inventory-more").disabled = state.inventoryLoading;
    $("inventory-more").textContent = state.inventoryLoading ? "Caricamento…" : "Carica altri asset";
    $("inventory-body").replaceChildren();
    for (const asset of state.inventoryItems) {
      const row = node("tr");
      const identity = node("td", "inventory-identity");
      identity.append(node("strong", "", asset.name), node("span", "", asset.asset_tag));
      const category = node("td");
      category.append(node("span", "inventory-type-badge", inventoryTypes[inventoryAssetType(asset)] || asset.asset_type), node("span", "inventory-cell-secondary", asset.category || "hardware"));
      if (asset.serial_number) identity.append(node("span", "", "S/N " + asset.serial_number));
      const status = node("td");
      status.append(node("span", "inventory-status inventory-status-" + asset.status, inventoryStates[asset.status] || asset.status));
      const assigned = node("td");
      assigned.append(node("span", "inventory-cell-line", asset.assignee || "Non assegnato"), node("span", "inventory-cell-secondary", asset.location || "Sede non indicata"));
      const actions = node("td");
      const group = node("div", "inventory-row-actions");
      group.append(action("Scheda", "Apri scheda e storico di " + asset.name, () => openInventoryDetail(asset.id)));
      if (manager) group.append(action("Modifica", "Modifica " + asset.name, () => openInventoryEditor(asset.id)));
      actions.append(group);
      row.append(identity, category, status, assigned, actions);
      $("inventory-body").append(row);
    }
    $("inventory-detail-edit").hidden = !manager || !state.inventoryDetailAsset;
  }

  async function refreshInventory(more = false) {
    if (!state.session?.authenticated || more && (state.inventoryLoading || !state.inventoryCursor)) return;
    clearTimeout(inventorySearchTimer);
    const request = ++state.inventoryRequest;
    const epoch = state.inventoryEpoch;
    const cursor = more ? state.inventoryCursor : null;
    const query = new URLSearchParams({ q: $("inventory-search").value.trim(), limit: "30" });
    if ($("inventory-filter").value) query.set("status", $("inventory-filter").value);
    if ($("inventory-type-filter").value) query.set("asset_type", $("inventory-type-filter").value);
    if (new TextEncoder().encode(query.get("q")).length > 160) {
      state.inventoryLoading = false;
      $("inventory-search").setCustomValidity("La ricerca è troppo lunga. Accorciala e riprova.");
      $("inventory-search").reportValidity();
      renderInventory();
      return;
    }
    if (cursor) query.set("after_id", cursor);
    if (!more) {
      state.inventoryItems = [];
      state.inventoryReady = false;
      state.inventoryCursor = null;
    }
    state.inventoryLoading = true;
    state.inventoryRetryMore = more;
    $("inventory-error").hidden = true;
    renderInventory();
    const current = () => request === state.inventoryRequest && epoch === state.inventoryEpoch;
    try {
      if (!more) {
        const service = await api("/api/inventory/status");
        if (!current()) return;
        if (!service.configured) {
          state.inventoryConfigured = false;
          state.inventorySummary = null;
          $("inventory-error-text").textContent = "Asset inventory non è ancora configurato. Chiedi a un gestore di collegare il servizio di inventario, poi riprova.";
          $("inventory-error").hidden = false;
          return;
        }
        state.inventoryConfigured = true;
        state.inventoryBackup = service.backup || null;
        state.inventorySummary = service.summary || null;
      }
      const data = await api("/api/inventory/assets?" + query);
      if (!current()) return;
      const known = new Set(state.inventoryItems.map((asset) => String(asset.id)));
      state.inventoryItems.push(...data.items.filter((asset) => !known.has(String(asset.id))));
      data.items.forEach(inventoryRemember);
      state.inventorySummary = data.summary || state.inventorySummary;
      state.inventoryCursor = data.next_after_id || null;
      state.inventoryReady = true;
      state.inventoryRetryMore = false;
    } catch (error) {
      if (!current()) return;
      if (error.status === 503) { state.inventoryConfigured = false; state.inventorySummary = null; }
      $("inventory-error-text").textContent = inventoryMessage(error);
      $("inventory-error").hidden = false;
    } finally {
      if (current()) { state.inventoryLoading = false; renderInventory(); }
    }
  }

  function inventoryDownload(content, filename) {
    const url = URL.createObjectURL(new Blob([content], { type: "text/csv;charset=utf-8" }));
    const link = node("a"); link.href = url; link.download = filename;
    document.body.append(link); link.click(); link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  async function exportInventory() {
    if (!state.session?.authenticated || state.inventoryConfigured !== true || state.inventoryExporting) return;
    const epoch = state.inventoryEpoch, email = state.session.email;
    const current = () => epoch === state.inventoryEpoch && state.session?.email === email;
    const query = new URLSearchParams({ limit: "100" });
    if ($("inventory-search").value.trim()) query.set("q", $("inventory-search").value.trim());
    if ($("inventory-filter").value) query.set("status", $("inventory-filter").value);
    if ($("inventory-type-filter").value) query.set("asset_type", $("inventory-type-filter").value);
    state.inventoryExporting = true; renderInventory();
    const parts = []; let previous = null, size = 0, count = 0, complete = false;
    try {
      for (let page = 0; page < 1000; page++) {
        const data = await api("/api/inventory/assets/export?" + query);
        if (!current()) return;
        if (typeof data.csv !== "string" || !Number.isInteger(data.exported) || data.exported < 0) throw new Error("Il server ha restituito un’esportazione non valida.");
        const newline = data.csv.indexOf("\n");
        if (newline < 0) throw new Error("L’esportazione non contiene le intestazioni CSV.");
        const chunk = page ? data.csv.slice(newline + 1) : data.csv;
        size += new TextEncoder().encode(chunk).length;
        if (size > 20 * 1024 * 1024) throw new Error("L’esportazione supera 20 MiB. Restringi i filtri e riprova.");
        parts.push(chunk); count += data.exported;
        if (data.next_after_id === null) { complete = true; break; }
        if (!Number.isSafeInteger(data.next_after_id) || data.next_after_id < 1 || previous !== null && data.next_after_id >= previous) throw new Error("Il server ha restituito una pagina CSV non valida.");
        previous = data.next_after_id; query.set("after_id", String(previous));
      }
      if (!complete) throw new Error("Troppi asset da esportare insieme. Restringi i filtri e riprova.");
      if (!current()) return;
      inventoryDownload("\ufeff" + parts.join(""), "inventario-" + localDate(new Date()) + ".csv");
      showNotice(`Esportazione completata: ${count} asset per i filtri selezionati.`);
    } catch (error) { if (current()) showNotice(inventoryMessage(error), true); }
    finally { if (current()) { state.inventoryExporting = false; renderInventory(); } }
  }

  function renderInventoryImport() {
    const busy = state.inventoryImportBusy;
    ["inventory-import-file", "inventory-import-mode", "inventory-import-template", "inventory-import-preview-button"].forEach((id) => { $(id).disabled = busy; });
    $("inventory-import-preview-button").textContent = busy ? "Operazione in corso…" : "Controlla file";
    $("inventory-import-commit").disabled = busy || !state.inventoryImportPlan?.can_commit;
    $("inventory-import-dialog").setAttribute("aria-busy", String(busy));
    if (busy) busyDialogs.add("inventory-import-dialog"); else busyDialogs.delete("inventory-import-dialog");
  }

  function invalidateInventoryImport() {
    state.inventoryImportRequest++; state.inventoryImportPlan = null;
    $("inventory-import-preview").hidden = true; $("inventory-import-rows").replaceChildren();
    $("inventory-import-error").hidden = true; renderInventoryImport();
  }

  function openInventoryImport() {
    if (!isInventoryManager() || state.inventoryConfigured !== true || state.inventoryImportBusy) return;
    $("inventory-import-form").reset(); invalidateInventoryImport();
    $("inventory-import-dialog").showModal(); $("inventory-import-file").focus();
  }

  async function previewInventoryImport(event) {
    event.preventDefault();
    if (!isInventoryManager() || state.inventoryImportBusy || !$("inventory-import-form").reportValidity()) return;
    const file = $("inventory-import-file").files[0];
    if (!file || file.size > 256 * 1024 || !file.size) { dialogError("inventory-import-error", new Error("Scegli un CSV non vuoto di al massimo 256 KiB.")); return; }
    invalidateInventoryImport();
    const request = ++state.inventoryImportRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryImportRequest && epoch === state.inventoryEpoch && isInventoryManager() && $("inventory-import-dialog").open;
    const mode = $("inventory-import-mode").value;
    state.inventoryImportBusy = true; renderInventoryImport();
    try {
      let csv;
      try { csv = new TextDecoder("utf-8", { fatal: true }).decode(await file.arrayBuffer()); }
      catch { throw new Error("Il file deve essere un CSV con codifica UTF-8."); }
      if (!current()) return;
      const data = await post("/api/inventory/import/preview", { csv, mode });
      if (!current()) return;
      if (!Array.isArray(data.items) || !data.summary || typeof data.can_commit !== "boolean" || data.can_commit && !/^[a-f0-9]{32}$/i.test(data.preview_id || "")) throw new Error("Il server ha restituito un’anteprima non valida.");
      state.inventoryImportPlan = data;
      $("inventory-import-summary").textContent = `${data.summary.total} righe · ${data.summary.create} da creare · ${data.summary.update} da aggiornare · ${data.summary.errors} con errori.`;
      for (const row of data.items) {
        const tr = node("tr");
        tr.append(node("td", "", row.row), node("td", "", [row.asset_tag, row.name].filter(Boolean).join(" · ")), node("td", "", { create: "Crea", update: "Aggiorna", error: "Errore" }[row.action] || row.action), node("td", "", row.errors?.length ? row.errors.join(" · ") : "Pronto"));
        $("inventory-import-rows").append(tr);
      }
      $("inventory-import-preview").hidden = false;
    } catch (error) { if (current()) dialogError("inventory-import-error", new Error(inventoryMessage(error))); }
    finally { if (current()) { state.inventoryImportBusy = false; renderInventoryImport(); } }
  }

  async function commitInventoryImport() {
    const plan = state.inventoryImportPlan;
    if (!isInventoryManager() || state.inventoryImportBusy || !plan?.can_commit) return;
    const request = ++state.inventoryImportRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryImportRequest && epoch === state.inventoryEpoch && isInventoryManager() && $("inventory-import-dialog").open;
    state.inventoryImportBusy = true; renderInventoryImport(); $("inventory-import-error").hidden = true;
    try {
      await post("/api/inventory/import/commit", { preview_id: plan.preview_id });
      if (!current()) return;
      state.inventoryImportBusy = false; renderInventoryImport();
      $("inventory-import-dialog").close(); state.auditReady = false;
      inventoryAssetCache.clear();
      showNotice(`Importazione completata: ${plan.summary.create} asset creati e ${plan.summary.update} aggiornati.`);
      refreshInventory();
    } catch (error) {
      if (!current()) return;
      state.inventoryImportPlan = null;
      dialogError("inventory-import-error", new Error(inventoryMessage(error) + " Controlla nuovamente il file prima di confermare."));
    } finally { if (current()) { state.inventoryImportBusy = false; renderInventoryImport(); } }
  }

  $("inventory-export").addEventListener("click", exportInventory);
  $("inventory-import").addEventListener("click", openInventoryImport);
  $("inventory-import-form").addEventListener("submit", previewInventoryImport);
  $("inventory-import-commit").addEventListener("click", commitInventoryImport);
  ["inventory-import-file", "inventory-import-mode"].forEach((id) => $(id).addEventListener("change", invalidateInventoryImport));
  $("inventory-import-dialog").addEventListener("close", invalidateInventoryImport);
  $("inventory-import-template").addEventListener("click", () => inventoryDownload('\ufeff"asset_tag","name","asset_type","details"\r\n"IT-0001","Computer ufficio","it_device","{}"\r\n', "modello-inventario.csv"));

  function inventoryDatesAndAssignment() {
    const assigned = $("inventory-status").value === "assigned";
    $("inventory-assignee").required = assigned;
    $("inventory-assignee-required").hidden = !assigned;
    $("inventory-assignee").setCustomValidity(assigned && !$("inventory-assignee").value.trim() ? "Indica la persona o il reparto a cui è assegnato il bene." : "");
    const purchase = $("inventory-purchase_date").value;
    const warranty = $("inventory-warranty_until").value;
    $("inventory-warranty_until").min = validDate(purchase) ? purchase : "0001-01-01";
    $("inventory-warranty_until").setCustomValidity(purchase && warranty && warranty < purchase ? "La garanzia deve terminare alla data di acquisto o dopo." : "");
  }

  function fillInventoryEditor(asset = null) {
    $("inventory-form").reset();
    inventoryFields.forEach((field) => { $("inventory-" + field).value = asset ? asset[field] || "" : field === "category" ? "hardware" : field === "status" ? "available" : ""; $("inventory-" + field).setCustomValidity(""); });
    $("inventory-asset_type").value = inventoryAssetType(asset);
    Object.entries(inventoryTypeFields).forEach(([type]) => $("inventory-type-" + type).querySelectorAll("[data-inventory-detail]").forEach((input) => { input.value = asset && inventoryAssetType(asset) === type ? asset.details?.[input.dataset.inventoryDetail] ?? input.dataset.initial : input.dataset.initial; input.setCustomValidity(""); }));
    const softwarePicker = inventoryPickers.get("inventory-software-picker");
    softwarePicker.request++; softwarePicker.items = []; softwarePicker.cursor = null; softwarePicker.loading = false; softwarePicker.retryMore = false; softwarePicker.error.hidden = true;
    softwarePicker.setSelected(asset?.details?.software_id || null);
    inventoryShowTypeFields(); inventoryRemember(asset);
    state.inventoryEditingAsset = asset;
    state.inventoryConflict = false;
    $("inventory-editor-error").hidden = true;
    $("inventory-editor-reload").hidden = true;
    $("inventory-fields").disabled = false;
    $("inventory-save").disabled = false;
    $("inventory-dialog-title").textContent = asset ? "Modifica asset" : "Nuovo asset";
    $("inventory-save").textContent = asset ? "Salva modifiche" : "Crea asset";
    inventoryDatesAndAssignment();
  }

  function openInventoryEditor(id = null) {
    if (!isInventoryManager() || state.inventoryConfigured !== true || busyDialogs.has("inventory-dialog")) return;
    state.inventoryEditorRequest++;
    state.inventoryEditingId = id;
    fillInventoryEditor();
    $("inventory-editor-loading").hidden = true;
    if (!$("inventory-dialog").open) $("inventory-dialog").showModal();
    if (id) loadInventoryEditor(); else $("inventory-asset_tag").focus();
  }

  async function loadInventoryEditor() {
    if (!isInventoryManager() || !state.inventoryEditingId || busyDialogs.has("inventory-dialog")) return;
    const id = state.inventoryEditingId;
    const request = ++state.inventoryEditorRequest;
    const epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryEditorRequest && epoch === state.inventoryEpoch && $("inventory-dialog").open && isInventoryManager();
    $("inventory-dialog-title").textContent = "Modifica asset";
    $("inventory-editor-loading").hidden = false;
    $("inventory-editor-error").hidden = true;
    $("inventory-editor-reload").hidden = true;
    $("inventory-fields").disabled = true;
    $("inventory-save").disabled = true;
    try {
      const data = await api("/api/inventory/assets/" + encodeURIComponent(id));
      if (!current()) return;
      fillInventoryEditor(data.asset);
      $("inventory-name").focus();
    } catch (error) {
      if (!current()) return;
      dialogError("inventory-editor-error", new Error(inventoryMessage(error)));
      $("inventory-editor-reload").textContent = "Riprova a caricare la scheda aggiornata";
      $("inventory-editor-reload").hidden = false;
    } finally {
      if (current()) $("inventory-editor-loading").hidden = true;
    }
  }

  function inventoryTime(value) {
    if (!value) return "—";
    // SQLite timestamps without a suffix are UTC.
    const normalized = /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/.test(value) ? value.replace(" ", "T") + "Z" : value;
    const date = new Date(normalized);
    return Number.isNaN(date.getTime()) ? value : date.toLocaleString("it-IT");
  }

  function inventoryDetails(asset, timestamps = true) {
    const details = node("dl", "inventory-detail-grid");
    const fields = [["Codice inventario", asset.asset_tag], ["Nome", asset.name], ["Tipologia", inventoryTypes[inventoryAssetType(asset)] || asset.asset_type], ["Stato", inventoryStates[asset.status] || asset.status], ["Categoria", asset.category], ["Numero di serie", asset.serial_number], ["Produttore", asset.manufacturer], ["Modello", asset.model], ["Assegnato a", asset.assignee], ["Ubicazione", asset.location], ["Acquistato il", asset.purchase_date && validDate(asset.purchase_date) ? dateLabel(asset.purchase_date) : asset.purchase_date], ["Garanzia fino al", asset.warranty_until && validDate(asset.warranty_until) ? dateLabel(asset.warranty_until) : asset.warranty_until], ["Versione", asset.version]];
    fields.push(...inventoryTypedDisplay(asset));
    if (timestamps) fields.push(["Creato il", inventoryTime(asset.created_at)], ["Aggiornato il", inventoryTime(asset.updated_at)]);
    fields.push(["Note", asset.notes]);
    fields.forEach(([label, value]) => {
      const item = node("div", label === "Note" ? "inventory-detail-wide" : "");
      item.append(node("dt", "", label), node("dd", "", value === null || value === undefined || value === "" ? "—" : String(value)));
      details.append(item);
    });
    return details;
  }

  function openInventoryDetail(id) {
    if (state.inventoryRelationBusy) return;
    state.inventoryDetailId = id;
    state.inventoryDetailAsset = null;
    resetInventoryRelations();
    state.inventoryDetailRequest++;
    state.inventoryHistoryRequest++;
    state.inventoryHistoryReady = false;
    state.inventoryHistoryLoading = false;
    state.inventoryHistoryCursor = null;
    $("inventory-detail-title").textContent = "Scheda asset";
    $("inventory-detail-data").replaceChildren();
    $("inventory-history").replaceChildren();
    $("inventory-detail-content").hidden = true;
    $("inventory-detail-edit").hidden = true;
    if (!$("inventory-detail-dialog").open) $("inventory-detail-dialog").showModal();
    loadInventoryDetail();
  }

  async function loadInventoryDetail() {
    const id = state.inventoryDetailId;
    if (!id) return;
    const request = ++state.inventoryDetailRequest;
    const epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryDetailRequest && epoch === state.inventoryEpoch && $("inventory-detail-dialog").open;
    $("inventory-detail-loading").hidden = false;
    $("inventory-detail-error").hidden = true;
    $("inventory-detail-retry").hidden = true;
    try {
      const data = await api("/api/inventory/assets/" + encodeURIComponent(id));
      if (!current()) return;
      state.inventoryDetailAsset = inventoryRemember(data.asset);
      $("inventory-detail-title").textContent = data.asset.name;
      $("inventory-detail-data").replaceChildren(inventoryDetails(data.asset));
      $("inventory-detail-content").hidden = false;
      $("inventory-detail-edit").hidden = !isInventoryManager();
      renderInventoryRelationOptions();
      loadInventoryRelations();
      loadInventoryHistory();
    } catch (error) {
      if (!current()) return;
      dialogError("inventory-detail-error", new Error(inventoryMessage(error)));
      $("inventory-detail-retry").hidden = false;
    } finally {
      if (current()) $("inventory-detail-loading").hidden = true;
    }
  }

  async function loadInventoryHistory(more = false) {
    const id = state.inventoryDetailId;
    if (!id || more && (state.inventoryHistoryLoading || !state.inventoryHistoryCursor)) return;
    const request = ++state.inventoryHistoryRequest;
    const epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryHistoryRequest && epoch === state.inventoryEpoch && id === state.inventoryDetailId && $("inventory-detail-dialog").open;
    const query = new URLSearchParams({ limit: "20" });
    if (more) query.set("after_id", state.inventoryHistoryCursor);
    else { $("inventory-history").replaceChildren(); state.inventoryHistoryReady = false; state.inventoryHistoryCursor = null; }
    state.inventoryHistoryLoading = true;
    state.inventoryHistoryRetryMore = more;
    $("inventory-history-loading").hidden = false;
    $("inventory-history-error").hidden = true;
    $("inventory-history-retry").hidden = true;
    $("inventory-history-empty").hidden = true;
    $("inventory-history-more").disabled = true;
    $("inventory-history-more").hidden = !state.inventoryHistoryCursor;
    try {
      const data = await api("/api/inventory/assets/" + encodeURIComponent(id) + "/history?" + query);
      if (!current()) return;
      data.items.forEach((entry) => {
        const item = node("li");
        item.append(node("h4", "", (entry.action === "created" ? "Creazione" : "Aggiornamento") + " · versione " + entry.version), node("p", "inventory-history-meta", inventoryTime(entry.created_at) + " · utente #" + entry.actor_id));
        if (entry.change?.relation) {
          const operation = entry.change.operation === "relation_removed" ? "Relazione rimossa" : "Relazione aggiunta";
          item.append(node("p", "inventory-history-summary", operation + " · " + inventoryRelationSummary(entry.change.relation)));
        }
        if (entry.snapshot) {
          item.append(node("p", "inventory-history-summary", (inventoryStates[entry.snapshot.status] || entry.snapshot.status) + (entry.snapshot.assignee ? " · " + entry.snapshot.assignee : "") + (entry.snapshot.location ? " · " + entry.snapshot.location : "")));
          const snapshot = node("details");
          snapshot.append(node("summary", "", "Visualizza i dati di questa versione"), inventoryDetails(entry.snapshot, false));
          item.append(snapshot);
        }
        $("inventory-history").append(item);
      });
      state.inventoryHistoryReady = true;
      state.inventoryHistoryCursor = data.next_after_id || null;
      state.inventoryHistoryRetryMore = false;
      $("inventory-history-empty").hidden = Boolean($("inventory-history").children.length);
    } catch (error) {
      if (!current()) return;
      dialogError("inventory-history-error", new Error(inventoryMessage(error)));
      $("inventory-history-retry").hidden = false;
    } finally {
      if (current()) {
        state.inventoryHistoryLoading = false;
        $("inventory-history-loading").hidden = true;
        $("inventory-history-more").disabled = false;
        $("inventory-history-more").hidden = !state.inventoryHistoryCursor;
      }
    }
  }

  function resetInventoryRelations() {
    state.inventoryRelationsRequest++; state.inventoryRelations = []; state.inventoryRelationsReady = false; state.inventoryRelationsCursor = null; state.inventoryRelationsLoading = false; state.inventoryRelationsRetryMore = false; state.inventoryRelationConflict = false;
    $("inventory-relations").replaceChildren(); $("inventory-relation-form").reset(); $("inventory-relation-form").hidden = true;
    $("inventory-relation-error").hidden = true; $("inventory-relation-reload").hidden = true; $("inventory-relations-error").hidden = true; $("inventory-relations-retry").hidden = true; $("inventory-relations-more").hidden = true;
    const picker = inventoryPickers.get("inventory-relation-picker"); if (picker) { picker.request++; picker.selected = null; picker.items = []; picker.loading = false; picker.cursor = null; picker.retryMore = false; picker.render(); }
  }

  function inventoryRelationSummary(relation) {
    const source = relation.source || relation.source_asset || inventoryAssetCache.get(String(relation.source_id));
    const target = relation.target || relation.target_asset || inventoryAssetCache.get(String(relation.target_id));
    const origin = source ? inventoryAssetLabel(source) : "Asset #" + relation.source_id;
    const destination = target ? inventoryAssetLabel(target) : relation.target_id ? "Asset #" + relation.target_id : relation.assignee || "Destinatario non indicato";
    let summary = (inventoryRelationNames[relation.relation_type] || relation.relation_type) + " · " + origin + " → " + destination;
    if (relation.relation_type === "licensed_to") summary += " · " + relation.quantity + (Number(relation.quantity) === 1 ? " posto" : " posti");
    if (relation.installed_version) summary += " · versione " + relation.installed_version;
    if (relation.source_port || relation.target_port) summary += " · porte " + (relation.source_port || "—") + " → " + (relation.target_port || "—");
    return summary;
  }

  function inventoryRelationAllowed(asset) {
    const type = inventoryAssetType(asset);
    return type === "software" ? ["installed_on"] : type === "license" ? ["licensed_to"] : ["it_device", "network_device"].includes(type) ? ["connected_to"] : [];
  }

  function renderInventoryRelationOptions() {
    const asset = state.inventoryDetailAsset; if (!asset) return;
    const allowed = inventoryRelationAllowed(asset), select = $("inventory-relation-type"), previous = select.value;
    select.replaceChildren(...allowed.map((type) => new Option(inventoryRelationNames[type], type)));
    if (allowed.includes(previous)) select.value = previous;
    $("inventory-relation-new").hidden = !isInventoryManager() || !allowed.length;
    $("inventory-relation-new").disabled = state.inventoryRelationBusy;
    $("inventory-relation-help").textContent = inventoryAssetType(asset) === "license" ? "Registra i posti assegnati a dispositivi o persone secondo il modello della licenza. Le assegnazioni sono inserite manualmente." : "Consulta software installato, collegamenti di rete e licenze assegnate a questo bene.";
    if (!allowed.length) $("inventory-relation-form").hidden = true;
    inventoryRelationControls();
  }

  function inventoryRelationControls() {
    const asset = state.inventoryDetailAsset; if (!asset) return;
    const type = $("inventory-relation-type").value, model = asset.details?.license_model || "per_device";
    const license = type === "licensed_to", flexible = license && ["concurrent", "site"].includes(model);
    const user = license && (model === "per_user" || flexible && $("inventory-relation-recipient-kind").value === "user");
    $("inventory-relation-recipient").hidden = !flexible;
    $("inventory-relation-recipient-kind").disabled = !flexible;
    $("inventory-relation-target").hidden = user;
    $("inventory-relation-assignee-field").hidden = !user;
    $("inventory-relation-assignee").disabled = !user; $("inventory-relation-assignee").required = user;
    $("inventory-relation-quantity-field").hidden = !license; $("inventory-relation-quantity").disabled = !license;
    $("inventory-relation-version-field").hidden = type !== "installed_on"; $("inventory-relation-installed-version").disabled = type !== "installed_on";
    $("inventory-relation-ports").hidden = type !== "connected_to";
    $("inventory-relation-source-port").disabled = type !== "connected_to"; $("inventory-relation-target-port").disabled = type !== "connected_to";
    const picker = inventoryPickers.get("inventory-relation-picker"), types = type === "connected_to" ? ["network_device"] : ["it_device", "network_device"];
    if (picker.types.join() !== types.join()) picker.setTypes(types);
    picker.enabled = !user; picker.choice.required = !user; picker.render();
    $("inventory-relation-fields").disabled = state.inventoryRelationBusy || state.inventoryRelationConflict;
    $("inventory-relation-save").disabled = state.inventoryRelationBusy || state.inventoryRelationConflict || !isInventoryManager();
    $("inventory-relation-cancel").disabled = state.inventoryRelationBusy;
    $("inventory-detail-edit").disabled = state.inventoryRelationBusy;
    $("inventory-relation-reload").disabled = state.inventoryRelationBusy;
  }

  function renderInventoryRelations() {
    $("inventory-relations").replaceChildren();
    state.inventoryRelations.forEach((relation) => {
      const item = node("li", "inventory-relation-card"), heading = node("div", "inventory-relation-heading");
      const outgoing = String(relation.source_id) === String(state.inventoryDetailId);
      const source = relation.source || relation.source_asset, target = relation.target || relation.target_asset;
      if (source) inventoryRemember(source); if (target) inventoryRemember(target);
      const counterpart = outgoing ? target : source;
      heading.append(node("strong", "", inventoryRelationNames[relation.relation_type] || relation.relation_type), node("span", "inventory-cell-secondary", outgoing ? "In uscita" : "In entrata"));
      item.append(heading, node("p", "inventory-relation-summary", inventoryRelationSummary(relation)));
      if (relation.notes) item.append(node("p", "inventory-history-summary", relation.notes));
      const actions = node("div", "inventory-row-actions");
      if (counterpart) { const open = action("Apri scheda collegata", "Apri " + counterpart.name, () => { if (!state.inventoryRelationBusy) openInventoryDetail(counterpart.id); }); open.disabled = state.inventoryRelationBusy; actions.append(open); }
      if (outgoing && isInventoryManager()) {
        const remove = action("Rimuovi relazione", "Rimuovi " + inventoryRelationNames[relation.relation_type], () => {
          if (state.inventoryRelationBusy || state.inventoryRelationConflict) return;
          const confirmation = node("div", "inventory-relation-confirm");
          confirmation.append(node("p", "", "Rimuovere questa relazione? La rimozione resta nello storico. Potrai registrare una nuova relazione in seguito."));
          const cancel = node("button", "button button-secondary", "Mantieni relazione"); cancel.type = "button"; cancel.addEventListener("click", () => { confirmation.remove(); remove.hidden = false; });
          const confirm = node("button", "button button-danger", "Conferma rimozione"); confirm.type = "button"; confirm.addEventListener("click", () => mutateInventoryRelation(null, relation));
          confirmation.append(cancel, confirm); item.append(confirmation); remove.hidden = true; confirm.focus();
        }, true);
        remove.disabled = state.inventoryRelationBusy || state.inventoryRelationConflict; actions.append(remove);
      }
      item.append(actions); $("inventory-relations").append(item);
    });
    $("inventory-relations-empty").hidden = !state.inventoryRelationsReady || Boolean(state.inventoryRelations.length) || state.inventoryRelationsLoading;
    $("inventory-relations-more").hidden = !state.inventoryRelationsCursor;
    $("inventory-relations-more").disabled = state.inventoryRelationsLoading || state.inventoryRelationBusy;
  }

  async function loadInventoryRelations(more = false) {
    const id = state.inventoryDetailId;
    if (!id || more && (state.inventoryRelationsLoading || !state.inventoryRelationsCursor)) return;
    const request = ++state.inventoryRelationsRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryRelationsRequest && epoch === state.inventoryEpoch && String(id) === String(state.inventoryDetailId) && $("inventory-detail-dialog").open;
    const query = new URLSearchParams({ limit: "30" }); if (more) query.set("after_id", state.inventoryRelationsCursor); else { state.inventoryRelations = []; state.inventoryRelationsReady = false; state.inventoryRelationsCursor = null; }
    state.inventoryRelationsLoading = true; state.inventoryRelationsRetryMore = more;
    $("inventory-relations-loading").hidden = false; $("inventory-relations-error").hidden = true; $("inventory-relations-retry").hidden = true; renderInventoryRelations();
    try {
      const data = await api("/api/inventory/assets/" + encodeURIComponent(id) + "/relations?" + query);
      if (!current()) return;
      const known = new Set(state.inventoryRelations.map((relation) => String(relation.id)));
      state.inventoryRelations.push(...data.items.filter((relation) => !known.has(String(relation.id))));
      state.inventoryRelationsCursor = data.next_after_id || null; state.inventoryRelationsRetryMore = false; state.inventoryRelationsReady = true;
    } catch (error) {
      if (!current()) return;
      dialogError("inventory-relations-error", new Error(inventoryMessage(error))); $("inventory-relations-retry").hidden = false;
    } finally { if (current()) { state.inventoryRelationsLoading = false; $("inventory-relations-loading").hidden = true; renderInventoryRelations(); } }
  }

  async function reloadInventoryRelationSource() {
    if (state.inventoryRelationBusy || !state.inventoryDetailId) return;
    const id = state.inventoryDetailId, epoch = state.inventoryEpoch, request = ++state.inventoryDetailRequest;
    const current = () => epoch === state.inventoryEpoch && request === state.inventoryDetailRequest && $("inventory-detail-dialog").open && String(id) === String(state.inventoryDetailId);
    $("inventory-relation-reload").disabled = true;
    try {
      const data = await api("/api/inventory/assets/" + encodeURIComponent(id)); if (!current()) return;
      state.inventoryDetailAsset = inventoryRemember(data.asset); state.inventoryRelationConflict = false;
      $("inventory-detail-data").replaceChildren(inventoryDetails(data.asset)); $("inventory-detail-title").textContent = data.asset.name;
      $("inventory-relation-reload").hidden = true; $("inventory-relation-error").hidden = true; $("inventory-relations-error").hidden = true;
      renderInventoryRelationOptions(); loadInventoryRelations(); loadInventoryHistory();
      showNotice("Scheda aggiornata. La relazione non è stata salvata: controlla i dati e riprova.");
    } catch (error) { if (current()) dialogError("inventory-relations-error", new Error(inventoryMessage(error))); }
    finally { if (current()) $("inventory-relation-reload").disabled = false; }
  }

  async function mutateInventoryRelation(fields, removing = null) {
    const source = state.inventoryDetailAsset;
    if (!source || state.inventoryRelationBusy || state.inventoryRelationConflict || !isInventoryManager()) return;
    const id = source.id, epoch = state.inventoryEpoch, request = state.inventoryDetailRequest;
    const current = () => epoch === state.inventoryEpoch && request === state.inventoryDetailRequest && String(id) === String(state.inventoryDetailId) && $("inventory-detail-dialog").open && isInventoryManager();
    state.inventoryRelationBusy = true; busyDialogs.add("inventory-detail-dialog");
    $("inventory-relation-error").hidden = true; $("inventory-relations-error").hidden = true; inventoryRelationControls(); renderInventoryRelations();
    $("inventory-relations").querySelectorAll("button").forEach((button) => button.disabled = true);
    const payload = { ...(fields || {}), version: String(source.version) };
    const path = "/api/inventory/assets/" + encodeURIComponent(id) + "/relations" + (removing ? "/" + encodeURIComponent(removing.id) + "/remove" : "");
    try {
      const data = await post(path, payload); if (!current()) return;
      state.inventoryDetailAsset = inventoryRemember(data.asset);
      $("inventory-detail-data").replaceChildren(inventoryDetails(data.asset));
      $("inventory-relation-form").hidden = true; $("inventory-relation-form").reset();
      const picker = inventoryPickers.get("inventory-relation-picker"); picker.setSelected(null);
      showNotice(removing ? "Relazione rimossa e conservata nello storico." : "Relazione aggiunta all’inventario.");
      loadInventoryRelations(); loadInventoryHistory(); refreshInventory();
    } catch (error) {
      if (!current()) return;
      const errorId = removing ? "inventory-relations-error" : "inventory-relation-error";
      if (error.status === 409) {
        state.inventoryRelationConflict = true;
        dialogError(errorId, new Error(inventoryMessage(error) + " I dati inseriti restano disponibili. Ricarica la scheda prima di riprovare."));
        $("inventory-relation-reload").hidden = false;
      } else dialogError(errorId, new Error(inventoryMessage(error)));
    } finally {
      if (epoch === state.inventoryEpoch) { state.inventoryRelationBusy = false; busyDialogs.delete("inventory-detail-dialog"); if (current()) { inventoryRelationControls(); renderInventoryRelations(); } }
    }
  }

  buildInventoryTypedFields();
  inventoryShowTypeFields();
  $("inventory-relation-new").addEventListener("click", () => {
    if (!isInventoryManager() || state.inventoryRelationBusy || !state.inventoryDetailAsset) return;
    $("inventory-relation-form").hidden = false; inventoryRelationControls();
    inventoryPickers.get("inventory-relation-picker").load(); $("inventory-relation-type").focus();
  });
  $("inventory-relation-cancel").addEventListener("click", () => { if (!state.inventoryRelationBusy) $("inventory-relation-form").hidden = true; });
  $("inventory-relation-type").addEventListener("change", inventoryRelationControls);
  $("inventory-relation-recipient-kind").addEventListener("change", inventoryRelationControls);
  $("inventory-relations-retry").addEventListener("click", () => loadInventoryRelations(state.inventoryRelationsRetryMore));
  $("inventory-relations-more").addEventListener("click", () => loadInventoryRelations(true));
  $("inventory-relation-reload").addEventListener("click", reloadInventoryRelationSource);
  $("inventory-relation-form").addEventListener("submit", (event) => {
    event.preventDefault(); if (!isInventoryManager() || state.inventoryRelationBusy || state.inventoryRelationConflict) return;
    const type = $("inventory-relation-type").value, picker = inventoryPickers.get("inventory-relation-picker"), user = !$("inventory-relation-assignee").disabled;
    picker.choice.setCustomValidity(!user && !picker.selected ? "Seleziona un dispositivo destinatario." : "");
    const inputs = ["assignee", "installed-version", "source-port", "target-port", "notes"];
    inputs.forEach((name) => { const input = $("inventory-relation-" + name); if (!input.disabled) { input.value = input.value.trim(); input.setCustomValidity(input.maxLength > 0 && new TextEncoder().encode(input.value).length > input.maxLength ? "Accorcia il contenuto di questo campo." : ""); } });
    if (!$("inventory-relation-form").reportValidity()) return;
    const fields = { relation_type: type, notes: $("inventory-relation-notes").value };
    if (user) fields.assignee = $("inventory-relation-assignee").value; else fields.target_id = String(picker.selected.id);
    if (type === "licensed_to") fields.quantity = $("inventory-relation-quantity").value;
    if (type === "installed_on") fields.installed_version = $("inventory-relation-installed-version").value;
    if (type === "connected_to") { fields.source_port = $("inventory-relation-source-port").value; fields.target_port = $("inventory-relation-target-port").value; }
    mutateInventoryRelation(fields);
  });

  $("inventory-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!isInventoryManager() || busyDialogs.has("inventory-dialog") || state.inventoryConflict || $("inventory-fields").disabled) return;
    inventoryFields.forEach((field) => { if (field !== "notes") $("inventory-" + field).value = $("inventory-" + field).value.trim(); });
    inventoryDatesAndAssignment();
    inventoryFields.forEach((field) => {
      const input = $("inventory-" + field);
      if (input.maxLength > 0 && new TextEncoder().encode(input.value).length > input.maxLength) input.setCustomValidity("Il contenuto è troppo lungo. Accorcialo: i caratteri accentati e i simboli possono occupare più spazio.");
    });
    const typedDetails = inventoryTypedValues();
    if (!$("inventory-form").reportValidity()) return;
    const fields = Object.fromEntries(inventoryFields.map((field) => [field, $("inventory-" + field).value]));
    fields.category = fields.category || "hardware";
    fields.asset_type = $("inventory-asset_type").value;
    fields.details = JSON.stringify(typedDetails);
    for (const field of ["purchase_date", "warranty_until"]) {
      if (fields[field] && !validDate(fields[field])) { dialogError("inventory-editor-error", new Error("Inserisci date valide.")); return; }
    }
    const editing = state.inventoryEditingAsset;
    if (state.inventoryEditingId && !editing) return;
    if (editing) fields.version = String(editing.version);
    const epoch = state.inventoryEpoch;
    const request = state.inventoryEditorRequest;
    const current = () => epoch === state.inventoryEpoch && request === state.inventoryEditorRequest && isInventoryManager();
    $("inventory-editor-error").hidden = true;
    dialogBusy("inventory-dialog", true, "inventory-save", editing ? "Salva modifiche" : "Crea asset");
    try {
      await post("/api/inventory/assets" + (editing ? "/" + encodeURIComponent(editing.id) : ""), fields);
      if (!current()) return;
      $("inventory-dialog").close();
      showNotice(editing ? "Scheda asset aggiornata. La versione precedente resta nello storico." : "Asset aggiunto all’inventario.");
      refreshInventory();
    } catch (error) {
      if (!current() || !$("inventory-dialog").open) return;
      if (error.status === 409 && editing && !/già presente/i.test(error.message)) {
        state.inventoryConflict = true;
        dialogError("inventory-editor-error", new Error(inventoryMessage(error) + " Le tue modifiche sono ancora visibili. Copiale se vuoi conservarle, poi ricarica la versione aggiornata prima di salvare."));
        $("inventory-editor-reload").textContent = "Ricarica versione aggiornata e scarta le modifiche non salvate";
        $("inventory-editor-reload").hidden = false;
      } else dialogError("inventory-editor-error", new Error(inventoryMessage(error)));
    } finally {
      // Closing after a successful save invalidates editor requests; release its lock.
      if (epoch === state.inventoryEpoch && (request === state.inventoryEditorRequest || !$("inventory-dialog").open)) {
        dialogBusy("inventory-dialog", false, "inventory-save", editing ? "Salva modifiche" : "Crea asset");
        $("inventory-save").disabled = state.inventoryConflict || !isInventoryManager();
      }
    }
  });
  $("inventory-new").addEventListener("click", () => openInventoryEditor());
  $("inventory-refresh").addEventListener("click", () => refreshInventory());
  $("inventory-retry").addEventListener("click", () => refreshInventory(state.inventoryRetryMore));
  $("inventory-more").addEventListener("click", () => refreshInventory(true));
  $("inventory-search-form").addEventListener("submit", (event) => { event.preventDefault(); refreshInventory(); });
  $("inventory-search").addEventListener("input", () => {
    clearTimeout(inventorySearchTimer);
    $("inventory-search").setCustomValidity("");
    // Invalidate immediately, before the debounce, so old results cannot win.
    state.inventoryRequest++;
    state.inventoryLoading = true;
    state.inventoryReady = false;
    state.inventoryItems = [];
    state.inventoryCursor = null;
    $("inventory-error").hidden = true;
    renderInventory();
    inventorySearchTimer = setTimeout(() => refreshInventory(), 300);
  });
  $("inventory-filter").addEventListener("change", () => refreshInventory());
  $("inventory-type-filter").addEventListener("change", () => refreshInventory());
  $("inventory-asset_type").addEventListener("change", () => { inventoryShowTypeFields(); const picker = inventoryPickers.get("inventory-software-picker"); if ($("inventory-asset_type").value === "license" && !picker.items.length && !picker.loading) picker.load(); });
  $("inventory-clear").addEventListener("click", () => { $("inventory-search-form").reset(); $("inventory-search").setCustomValidity(""); refreshInventory(); $("inventory-search").focus(); });
  inventoryFields.forEach((field) => $("inventory-" + field).addEventListener("input", () => { $("inventory-" + field).setCustomValidity(""); inventoryDatesAndAssignment(); }));
  $("inventory-editor-reload").addEventListener("click", loadInventoryEditor);
  $("inventory-dialog").addEventListener("close", () => {
    state.inventoryEditorRequest++;
    state.inventoryEditingId = null;
    state.inventoryEditingAsset = null;
    state.inventoryConflict = false;
    const picker = inventoryPickers.get("inventory-software-picker"); picker.request++; picker.loading = false;
  });
  $("inventory-detail-retry").addEventListener("click", loadInventoryDetail);
  $("inventory-history-retry").addEventListener("click", () => loadInventoryHistory(state.inventoryHistoryRetryMore));
  $("inventory-history-more").addEventListener("click", () => loadInventoryHistory(true));
  $("inventory-detail-edit").addEventListener("click", () => {
    const id = state.inventoryDetailId;
    if (!isInventoryManager() || !id) return;
    $("inventory-detail-dialog").close();
    openInventoryEditor(id);
  });
  $("inventory-detail-dialog").addEventListener("close", () => {
    state.inventoryDetailRequest++;
    state.inventoryHistoryRequest++;
    state.inventoryDetailId = null;
    state.inventoryDetailAsset = null;
    state.inventoryHistoryLoading = false;
    resetInventoryRelations();
    inventoryPickers.forEach((picker) => { picker.request++; picker.loading = false; });
    $("inventory-history").replaceChildren();
    $("inventory-detail-data").replaceChildren();
  });

  Object.assign(state, { workJobId: "", workTasks: [], workBlockers: [], workAssignees: [], workPermissions: {}, workReady: false, workLoading: false, workRequest: 0, workEpoch: 0, workBusy: false, workEditingTask: null, workEditingBlocker: null, workDeleting: null });

  function resetWorkBoard(clearSelection = false) {
    state.workRequest++;
    state.workEpoch++;
    state.workReady = false;
    state.workLoading = false;
    state.workBusy = false;
    state.workTasks = [];
    state.workBlockers = [];
    state.workAssignees = [];
    state.workPermissions = {};
    state.workEditingTask = null;
    state.workEditingBlocker = null;
    state.workDeleting = null;
    if (clearSelection) state.workJobId = "";
    ["work-task-dialog", "work-blocker-dialog", "work-delete-dialog"].forEach((id) => { if ($(id).open) $(id).close(); });
    ["work-task-form", "work-blocker-form"].forEach((id) => $(id).reset());
    ["work-task-assignee", "work-task-parent", "work-blocker-owner", "work-blocker-task", "work-task-board", "work-task-list", "work-blocker-list"].forEach((id) => $(id).replaceChildren());
    ["work-content", "work-loading", "work-error", "work-new-task", "work-new-blocker"].forEach((id) => { $(id).hidden = true; });
    $("work-scope").textContent = "";
    $("work-summary").textContent = "";
    ["work-task-dialog-description", "work-blocker-owner-help", "work-delete-description", "work-task-error", "work-blocker-error", "work-delete-error", "work-error-text"].forEach((id) => { $(id).textContent = ""; });
    ["work-task-error", "work-blocker-error", "work-delete-error", "work-task-reload", "work-blocker-reload"].forEach((id) => { $(id).hidden = true; });
    $("work-search").value = "";
    $("work-no-job").hidden = Boolean(state.workJobId);
    $("work-job").value = state.workJobId;
    ["work-search", "work-status", "work-priority", "work-view", "work-blocker-filter"].forEach((id) => { $(id).disabled = false; });
    $("work-job").disabled = !state.jobsReady;
    $("work-refresh").disabled = !state.jobsReady;
    $("work-content").setAttribute("aria-busy", "false");
  }

  function renderWorkJobs() {
    const selected = state.workJobId;
    const placeholder = node("option", "", state.jobsReady ? state.jobs.length ? "Seleziona una commessa" : "Nessuna commessa abilitata" : "Caricamento delle commesse…");
    placeholder.value = "";
    $("work-job").replaceChildren(placeholder);
    for (const job of state.jobs) {
      const option = node("option", "", `${job.code} · ${job.name}`);
      option.value = String(job.id);
      $("work-job").append(option);
    }
    if (selected && state.jobsReady && !state.jobs.some((job) => String(job.id) === selected)) {
      resetWorkBoard(true);
      if (state.activeTab === "work-board") showNotice("La commessa selezionata non è più disponibile. Scegli una commessa abilitata.", true);
    }
    $("work-job").value = state.workJobId;
    $("work-job").disabled = !state.jobsReady || state.workBusy;
    $("work-refresh").disabled = !state.jobsReady || state.workBusy || state.workLoading;
    $("work-no-job-description").textContent = state.jobsReady && !state.jobs.length ? "Un gestore deve abilitarti a una commessa per consultarne il lavoro." : "Seleziona una commessa per consultarne le attività e i blocchi.";
  }

  function workContextCurrent(epoch, jobId) {
    return epoch === state.workEpoch && jobId === state.workJobId && Boolean(state.session?.authenticated);
  }

  async function refreshWorkBoard() {
    if (!state.workJobId || !state.jobsReady || !state.jobs.some((job) => String(job.id) === state.workJobId)) return;
    const request = ++state.workRequest;
    const jobId = state.workJobId;
    const epoch = state.workEpoch;
    state.workReady = false;
    state.workLoading = true;
    state.workTasks = [];
    state.workBlockers = [];
    state.workAssignees = [];
    state.workPermissions = {};
    $("work-content").hidden = true;
    $("work-content").setAttribute("aria-busy", "true");
    $("work-task-board").replaceChildren();
    $("work-task-list").replaceChildren();
    $("work-blocker-list").replaceChildren();
    $("work-loading").hidden = false;
    $("work-error").hidden = true;
    $("work-no-job").hidden = true;
    $("work-new-task").hidden = true;
    $("work-new-blocker").hidden = true;
    $("work-refresh").disabled = true;
    $("work-scope").textContent = "";
    try {
      const data = await api("/api/work-board?job_id=" + encodeURIComponent(jobId));
      if (request !== state.workRequest || !workContextCurrent(epoch, jobId)) return;
      if (String(data.job_id) !== jobId || !Array.isArray(data.tasks) || !Array.isArray(data.blockers)) throw new Error("La risposta non corrisponde alla commessa selezionata. Riprova.");
      state.workTasks = data.tasks;
      state.workBlockers = data.blockers;
      state.workAssignees = Array.isArray(data.assignees) ? data.assignees : [];
      state.workPermissions = data.permissions || {};
      state.workReady = true;
      renderWorkBoard();
    } catch (error) {
      if (request !== state.workRequest || !workContextCurrent(epoch, jobId)) return;
      $("work-error-text").textContent = error.message;
      $("work-error").hidden = false;
    } finally {
      if (request === state.workRequest && workContextCurrent(epoch, jobId)) {
        state.workLoading = false;
        $("work-loading").hidden = true;
        $("work-refresh").disabled = state.workBusy;
        $("work-content").setAttribute("aria-busy", "false");
      }
    }
  }

  function workDateText(value) {
    return value && validDate(value) ? dateLabel(value) : "Nessuna scadenza";
  }

  function workDetails(text) {
    const details = node("details", "work-card-details");
    details.append(node("summary", "", "Leggi la descrizione"), node("p", "work-card-description", text));
    return details;
  }

  function workTaskCard(task) {
    const card = node("article", "work-task-card");
    card.dataset.taskId = String(task.id);
    card.tabIndex = -1;
    card.setAttribute("aria-label", task.title);
    const tags = node("div", "work-card-tags");
    tags.append(node("span", "work-card-id", `#${task.id}`), node("span", "work-card-tag", workTaskStates[task.status] || task.status), node("span", "work-card-tag priority-" + (Object.hasOwn(workPriorities, task.priority) ? task.priority : "normal"), "Priorità " + (workPriorities[task.priority] || task.priority)));
    card.append(tags, node("h4", "", task.title));
    if (task.parent_id) {
      const parent = state.workTasks.find((item) => String(item.id) === String(task.parent_id));
      card.append(node("p", "work-task-context", "Sottoattività di " + (parent ? parent.title : "#" + task.parent_id)));
    }
    card.append(node("p", "work-card-meta", "Responsabile: " + (task.assigned_user_name || "Da assegnare")), node("p", "work-card-meta", workDateText(task.due_date)));
    if (task.description) card.append(workDetails(task.description));
    const openBlockers = state.workBlockers.filter((item) => String(item.task_id) === String(task.id) && item.status !== "resolved").length;
    if (openBlockers) card.append(node("p", "work-card-meta", `${openBlockers} ${openBlockers === 1 ? "blocco aperto" : "blocchi aperti"}`));
    const actions = node("div", "work-card-actions");
    if (task.can_edit) {
      actions.append(action("Modifica", "Modifica l’attività " + task.title, () => openWorkTask(task)));
      actions.append(action("+ Sottoattività", "Aggiungi una sottoattività a " + task.title, () => openWorkTask(null, task.id)));
      actions.append(action("Elimina", "Elimina l’attività " + task.title, () => openWorkDelete("task", task), true));
    }
    if (state.workPermissions.report_blockers) actions.append(action("Segnala blocco", "Segnala un blocco per " + task.title, () => openWorkBlocker(null, task.id)));
    if (task.can_update_status) {
      const control = node("form", "work-task-status-control");
      const id = "work-status-task-" + task.id + "-" + $("work-view").value;
      const label = node("label", "", "Stato"); label.htmlFor = id;
      const select = node("select"); select.id = id; select.setAttribute("aria-label", "Stato dell’attività " + task.title);
      for (const [value, text] of Object.entries(workTaskStates)) { const option = node("option", "", text); option.value = value; select.append(option); }
      select.value = task.status;
      const save = node("button", "row-button", "Applica"); save.type = "submit"; save.disabled = true; save.setAttribute("aria-label", "Applica lo stato all’attività " + task.title);
      select.disabled = state.workBusy;
      select.addEventListener("change", () => { save.disabled = state.workBusy || select.value === task.status; });
      control.addEventListener("submit", (event) => { event.preventDefault(); if (select.value !== task.status) updateWorkTaskStatus(task, select.value); });
      control.append(label, select, save); actions.append(control);
    }
    if (actions.childElementCount) card.append(actions);
    if (state.workBusy) card.querySelectorAll("button, select").forEach((element) => { element.disabled = true; });
    return card;
  }

  function renderWorkBoard() {
    if (!state.workReady) return;
    $("work-content").hidden = false;
    if (!state.workPermissions.manage_tasks && $("work-task-dialog").open) $("work-task-dialog").close();
    if (!state.workPermissions.manage_tasks && $("work-delete-dialog").open) $("work-delete-dialog").close();
    if ($("work-blocker-dialog").open && (state.workEditingBlocker ? !state.workBlockers.some((item) => item.id === state.workEditingBlocker.id && item.can_edit) : !state.workPermissions.report_blockers)) $("work-blocker-dialog").close();
    $("work-new-task").hidden = !state.workPermissions.manage_tasks;
    $("work-new-blocker").hidden = !state.workPermissions.report_blockers;
    $("work-new-task").disabled = state.workBusy;
    $("work-new-blocker").disabled = state.workBusy;
    $("work-scope").textContent = state.workPermissions.manage_tasks ? "Gestisci attività e responsabili. I collaboratori abilitati aggiornano lo stato delle attività assegnate e segnalano i blocchi." : state.workPermissions.report_blockers ? "Consulta il lavoro della commessa, aggiorna lo stato delle attività assegnate a te e segnala o gestisci i tuoi blocchi." : "Sola consultazione: puoi leggere attività e blocchi della commessa.";
    const query = $("work-search").value.trim().toLocaleLowerCase("it-IT");
    const status = $("work-status").value;
    const priority = $("work-priority").value;
    const tasks = state.workTasks.filter((task) => (!status || task.status === status) && (!priority || task.priority === priority) && [task.title, task.description, task.assigned_user_name].some((value) => String(value || "").toLocaleLowerCase("it-IT").includes(query)));
    const done = state.workTasks.filter((task) => task.status === "done").length;
    $("work-summary").textContent = `${tasks.length} attività visibili su ${state.workTasks.length} · ${done} completate nella commessa`;
    const listView = $("work-view").value === "list";
    $("work-task-board").replaceChildren();
    $("work-task-list").replaceChildren();
    $("work-task-board").hidden = listView || !tasks.length;
    $("work-task-list").hidden = !listView || !tasks.length;
    $("work-tasks-empty").hidden = Boolean(tasks.length);
    $("work-tasks-empty-title").textContent = state.workTasks.length ? "Nessuna attività corrisponde ai filtri." : "Il prossimo passo prende forma qui.";
    $("work-tasks-empty-description").textContent = state.workTasks.length ? "Cambia la ricerca, lo stato o la priorità per ritrovare le attività." : state.workPermissions.manage_tasks ? "Crea la prima attività, assegna un responsabile e scegli una scadenza." : "Il gestore può creare la prima attività per questa commessa.";
    if (listView) {
      const matching = new Set(tasks.map((task) => String(task.id)));
      const rendered = new Set();
      const children = new Map();
      for (const task of tasks) { const key = String(task.parent_id || ""); if (!children.has(key)) children.set(key, []); children.get(key).push(task); }
      const appendBranch = (task, list, depth) => {
        if (rendered.has(String(task.id))) return;
        rendered.add(String(task.id));
        const item = node("li"); item.append(workTaskCard(task)); list.append(item);
        const descendants = children.get(String(task.id)) || [];
        if (descendants.length) {
          const nested = node("ol", depth < 4 ? "work-task-children" : "");
          item.append(nested);
          descendants.forEach((child) => appendBranch(child, nested, depth + 1));
        }
      };
      const list = node("ol");
      tasks.filter((task) => !task.parent_id || !matching.has(String(task.parent_id))).forEach((task) => appendBranch(task, list, 0));
      tasks.forEach((task) => { if (!rendered.has(String(task.id))) appendBranch(task, list, 0); });
      $("work-task-list").append(list);
    } else {
      for (const [value, label] of Object.entries(workTaskStates)) {
        const column = node("section", "work-column"); column.dataset.status = value;
        const title = node("h3", "work-column-heading", label);
        const items = tasks.filter((task) => task.status === value);
        title.append(node("span", "work-column-count", String(items.length)));
        const list = node("ul", "work-column-items"); list.setAttribute("aria-label", label);
        items.forEach((task) => { const item = node("li"); item.append(workTaskCard(task)); list.append(item); });
        column.append(title, list);
        if (!items.length) column.append(node("p", "work-list-empty", "Nessuna attività"));
        $("work-task-board").append(column);
      }
    }
    renderWorkBlockers();
  }

  function renderWorkBlockers() {
    if (!state.workReady) return;
    const filter = $("work-blocker-filter").value;
    const blockers = state.workBlockers.filter((item) => filter === "all" || (filter === "resolved" ? item.status === "resolved" : item.status !== "resolved"));
    $("work-blocker-list").replaceChildren();
    $("work-blockers-empty").hidden = Boolean(blockers.length);
    for (const blocker of blockers) {
      const item = node("li", "work-blocker-card");
      item.dataset.blockerId = String(blocker.id);
      item.tabIndex = -1;
      item.setAttribute("aria-label", blocker.title);
      const tags = node("div", "work-card-tags");
      tags.append(node("span", "work-card-id", "#" + blocker.id), node("span", "work-card-tag", workBlockerStates[blocker.status] || blocker.status), node("span", "work-card-tag impact-" + (Object.hasOwn(workImpacts, blocker.impact) ? blocker.impact : "medium"), "Impatto " + (workImpacts[blocker.impact] || blocker.impact)));
      item.append(tags, node("h4", "", blocker.title));
      const task = state.workTasks.find((task) => String(task.id) === String(blocker.task_id));
      item.append(node("p", "work-card-meta", task ? "Attività: " + task.title : "Blocco della commessa"), node("p", "work-card-meta", "Responsabile: " + (blocker.owner_name || "Da assegnare") + " · " + workDateText(blocker.due_date)));
      if (blocker.description) item.append(workDetails(blocker.description));
      if (blocker.next_action) { const next = node("p", "work-blocker-action"); next.append(node("strong", "", "PROSSIMA AZIONE"), document.createTextNode(blocker.next_action)); item.append(next); }
      const actions = node("div", "work-card-actions");
      if (blocker.can_edit) actions.append(action("Gestisci", "Gestisci il blocco " + blocker.title, () => openWorkBlocker(blocker)));
      if (state.workPermissions.manage_tasks) actions.append(action("Elimina", "Elimina il blocco " + blocker.title, () => openWorkDelete("blocker", blocker), true));
      if (actions.childElementCount) item.append(actions);
      if (state.workBusy) item.querySelectorAll("button").forEach((button) => { button.disabled = true; });
      $("work-blocker-list").append(item);
    }
  }

  function workSelect(id, items, value, emptyLabel, currentLabel = "") {
    const select = $(id);
    const empty = node("option", "", emptyLabel); empty.value = "";
    select.replaceChildren(empty);
    for (const item of items) { const option = node("option", "", item.name || item.title); option.value = String(item.id); select.append(option); }
    if (value && !items.some((item) => String(item.id) === String(value))) {
      const unavailable = node("option", "", (currentLabel || "#" + value) + " · accesso da verificare"); unavailable.value = String(value); select.append(unavailable);
    }
    select.value = value ? String(value) : "";
  }

  function openWorkTask(task = null, parentId = "") {
    if (!state.workReady || !state.workPermissions.manage_tasks || task && !task.can_edit || state.workBusy || busyDialogs.has("work-task-dialog")) return;
    state.workEditingTask = task;
    $("work-task-form").reset();
    $("work-task-error").hidden = true;
    $("work-task-reload").hidden = true;
    $("work-task-save").disabled = false;
    $("work-task-dialog-title").textContent = task ? "Modifica attività" : parentId ? "Nuova sottoattività" : "Nuova attività";
    $("work-task-save").textContent = task ? "Salva modifiche" : "Crea attività";
    const job = state.jobs.find((job) => String(job.id) === state.workJobId);
    $("work-task-dialog-description").textContent = `${job?.code || ""} · ${job?.name || ""}. Ogni attività ha un avanzamento indipendente dalle sue sottoattività.`;
    ["title", "description", "status", "priority"].forEach((field) => { $("work-task-" + field).value = task?.[field] || (field === "status" ? "todo" : field === "priority" ? "normal" : ""); });
    $("work-task-due").value = task?.due_date || "";
    workSelect("work-task-assignee", state.workAssignees, task?.assigned_user_id, "Da assegnare", task?.assigned_user_name);
    const excluded = new Set(task ? [String(task.id)] : []);
    let changed = true;
    while (changed) { changed = false; for (const item of state.workTasks) if (excluded.has(String(item.parent_id)) && !excluded.has(String(item.id))) { excluded.add(String(item.id)); changed = true; } }
    workSelect("work-task-parent", state.workTasks.filter((item) => !excluded.has(String(item.id))), task?.parent_id || parentId, "Nessuna · attività principale");
    $("work-task-dialog").showModal();
    $("work-task-title").focus();
  }

  function openWorkBlocker(blocker = null, taskId = "") {
    if (!state.workReady || state.workBusy || busyDialogs.has("work-blocker-dialog") || (blocker ? !blocker.can_edit : !state.workPermissions.report_blockers)) return;
    state.workEditingBlocker = blocker;
    $("work-blocker-form").reset();
    $("work-blocker-error").hidden = true;
    $("work-blocker-reload").hidden = true;
    $("work-blocker-save").disabled = false;
    $("work-blocker-dialog-title").textContent = blocker ? "Gestisci blocco" : "Segnala blocco";
    $("work-blocker-dialog-description").textContent = "Descrivi l’impedimento e il prossimo passo per risolverlo. La risoluzione del blocco non cambia automaticamente lo stato dell’attività.";
    $("work-blocker-save").textContent = blocker ? "Salva modifiche" : "Segnala blocco";
    ["title", "description", "impact", "status"].forEach((field) => { $("work-blocker-" + field).value = blocker?.[field] || (field === "impact" ? "medium" : field === "status" ? "open" : ""); });
    $("work-blocker-action").value = blocker?.next_action || "";
    $("work-blocker-due").value = blocker?.due_date || "";
    workSelect("work-blocker-task", state.workTasks, blocker?.task_id || taskId, "Intera commessa");
    workSelect("work-blocker-owner", state.workAssignees, blocker?.owner_id, "Da assegnare", blocker?.owner_name);
    $("work-blocker-owner-field").hidden = !state.workPermissions.manage_tasks;
    $("work-blocker-owner-help").hidden = state.workPermissions.manage_tasks;
    $("work-blocker-owner-help").textContent = blocker ? "Responsabile: " + (blocker.owner_name || "Da assegnare") + ". Un gestore può modificare il responsabile." : "Il blocco sarà assegnato a te. Un gestore può modificare il responsabile.";
    $("work-blocker-dialog").showModal();
    $("work-blocker-title").focus();
  }

  function openWorkDelete(kind, item) {
    if (!state.workReady || !state.workPermissions.manage_tasks || state.workBusy || busyDialogs.has("work-delete-dialog")) return;
    if (kind === "task" && state.workTasks.some((task) => String(task.parent_id) === String(item.id))) { showNotice("Questa attività contiene sottoattività. Spostale o eliminale prima di eliminare l’attività principale.", true); return; }
    state.workDeleting = { kind, item };
    $("work-delete-title").textContent = kind === "task" ? "Eliminare questa attività?" : "Eliminare questo blocco?";
    $("work-delete-description").textContent = `“${item.title}” sarà eliminato definitivamente.` + (kind === "task" ? " Gli eventuali blocchi collegati resteranno nel registro della commessa. Le ore registrate non cambiano." : " L’operazione non può essere annullata dall’app.");
    $("work-delete-error").hidden = true;
    $("work-delete-save").disabled = false;
    $("work-delete-dialog").showModal();
  }

  function focusWorkResult(kind, id = null, statusControl = false) {
    if (state.activeTab !== "work-board" || document.querySelector("#work-task-dialog[open], #work-blocker-dialog[open], #work-delete-dialog[open]")) return;
    const visible = (element) => element && !element.disabled && element.getClientRects().length > 0;
    if (kind === "task" && statusControl && id !== null) {
      const select = $("work-status-task-" + id + "-" + $("work-view").value);
      if (visible(select)) { select.focus(); return; }
    }
    const cards = document.querySelectorAll(kind === "task" ? ".work-task-card" : ".work-blocker-card");
    const card = [...cards].find((element) => element.dataset[kind === "task" ? "taskId" : "blockerId"] === String(id));
    if (visible(card)) { card.focus(); return; }
    const fallback = kind === "task" ? ["work-task-board", "work-task-list", "work-new-task", "work-job"] : ["work-blocker-filter", "work-job"];
    for (const name of fallback) {
      const element = $(name);
      if (!visible(element)) continue;
      if (name === "work-task-list") element.tabIndex = -1;
      element.focus();
      return;
    }
  }

  function workSetBusy(busy) {
    state.workBusy = busy;
    $("work-job").disabled = busy || !state.jobsReady;
    $("work-refresh").disabled = busy || state.workLoading || !state.jobsReady;
    $("work-new-task").disabled = busy;
    $("work-new-blocker").disabled = busy;
    ["work-search", "work-status", "work-priority", "work-view", "work-blocker-filter"].forEach((id) => { $(id).disabled = busy; });
    if (state.workReady) renderWorkBoard();
  }

  async function updateWorkTaskStatus(task, status) {
    if (!state.workReady || !task.can_update_status || state.workBusy || !Object.hasOwn(workTaskStates, status)) return;
    const epoch = state.workEpoch, jobId = state.workJobId;
    workSetBusy(true);
    try {
      await post("/api/tasks/" + task.id, { status, revision: task.revision });
      if (!workContextCurrent(epoch, jobId)) return;
      state.auditReady = false;
      showNotice("Stato dell’attività aggiornato.");
      await refreshWorkBoard();
    } catch (error) {
      if (!workContextCurrent(epoch, jobId)) return;
      showNotice(error.message, true);
      await refreshWorkBoard();
    } finally {
      if (workContextCurrent(epoch, jobId)) {
        workSetBusy(false);
        focusWorkResult("task", task.id, true);
      }
    }
  }

  async function saveWorkDialog(kind) {
    const taskKind = kind === "task";
    const prefix = "work-" + kind;
    const dialogId = prefix + "-dialog";
    const edited = taskKind ? state.workEditingTask : state.workEditingBlocker;
    if (state.workBusy || busyDialogs.has(dialogId) || !state.workReady || (taskKind ? !state.workPermissions.manage_tasks : edited ? !edited.can_edit : !state.workPermissions.report_blockers)) return;
    ["title", "description"].forEach((field) => { $(prefix + "-" + field).value = $(prefix + "-" + field).value.trim(); });
    const due = $(prefix + "-due");
    due.setCustomValidity(due.value && !validDate(due.value) ? "Inserisci una data valida." : "");
    if (!$(prefix + "-form").reportValidity()) return;
    const fields = Object.fromEntries(["title", "description", "status"].map((field) => [field, $(prefix + "-" + field).value]));
    fields.due_date = due.value;
    if (edited) fields.revision = edited.revision; else fields.job_id = state.workJobId;
    if (taskKind) {
      fields.priority = $("work-task-priority").value;
      fields.parent_id = $("work-task-parent").value;
      fields.assigned_user_id = $("work-task-assignee").value;
    } else {
      fields.impact = $("work-blocker-impact").value;
      fields.task_id = $("work-blocker-task").value;
      fields.next_action = $("work-blocker-action").value.trim();
      if (state.workPermissions.manage_tasks) fields.owner_id = $("work-blocker-owner").value;
    }
    const epoch = state.workEpoch, jobId = state.workJobId;
    const label = edited ? "Salva modifiche" : taskKind ? "Crea attività" : "Segnala blocco";
    let conflict = false;
    let savedId = null;
    $(prefix + "-error").hidden = true;
    workSetBusy(true);
    dialogBusy(dialogId, true, prefix + "-save", label);
    try {
      const result = await post("/api/" + (taskKind ? "tasks" : "blockers") + (edited ? "/" + edited.id : ""), fields);
      if (!workContextCurrent(epoch, jobId)) return;
      savedId = result.id || edited?.id || null;
      state.auditReady = false;
      $(dialogId).close();
      showNotice(taskKind ? edited ? "Attività aggiornata." : "Attività creata." : edited ? "Blocco aggiornato." : "Blocco segnalato.");
      await refreshWorkBoard();
    } catch (error) {
      if (!workContextCurrent(epoch, jobId)) return;
      conflict = error.status === 409;
      dialogError(prefix + "-error", error);
      $(prefix + "-reload").hidden = !conflict;
    } finally {
      dialogBusy(dialogId, false, prefix + "-save", label);
      if (workContextCurrent(epoch, jobId)) {
        workSetBusy(false);
        $(prefix + "-save").disabled = conflict;
        if (!$(dialogId).open && savedId !== null) focusWorkResult(kind, savedId);
        else if ($(dialogId).open && !$(prefix + "-error").hidden) { $(prefix + "-error").tabIndex = -1; $(prefix + "-error").focus(); }
      }
    }
  }

  $("work-job").addEventListener("change", () => {
    const jobId = $("work-job").value;
    if (state.workBusy) { $("work-job").value = state.workJobId; return; }
    resetWorkBoard(true);
    state.workJobId = jobId;
    $("work-job").value = jobId;
    $("work-no-job").hidden = Boolean(jobId);
    ["work-search", "work-status", "work-priority"].forEach((id) => { $(id).value = ""; });
    if (jobId) refreshWorkBoard();
  });
  ["work-refresh", "work-retry"].forEach((id) => $(id).addEventListener("click", async () => {
    const epoch = state.workEpoch, jobId = state.workJobId;
    if (jobId) await refreshWorkBoard(); else await refreshAll();
    if (workContextCurrent(epoch, jobId) && state.activeTab === "work-board" && !$("work-refresh").disabled) $("work-refresh").focus();
  }));
  $("work-search").addEventListener("input", renderWorkBoard);
  ["work-status", "work-priority", "work-view"].forEach((id) => $(id).addEventListener("change", renderWorkBoard));
  $("work-blocker-filter").addEventListener("change", renderWorkBlockers);
  $("work-new-task").addEventListener("click", () => openWorkTask());
  $("work-new-blocker").addEventListener("click", () => openWorkBlocker());
  ["task", "blocker"].forEach((kind) => {
    $("work-" + kind + "-form").addEventListener("submit", (event) => { event.preventDefault(); saveWorkDialog(kind); });
    $("work-" + kind + "-due").addEventListener("input", () => $("work-" + kind + "-due").setCustomValidity(""));
    $("work-" + kind + "-reload").addEventListener("click", async () => {
      const epoch = state.workEpoch, jobId = state.workJobId;
      const edited = kind === "task" ? state.workEditingTask : state.workEditingBlocker;
      $("work-" + kind + "-dialog").close();
      await refreshWorkBoard();
      if (workContextCurrent(epoch, jobId)) focusWorkResult(kind, edited?.id);
    });
    $("work-" + kind + "-dialog").addEventListener("close", () => { if (kind === "task") state.workEditingTask = null; else state.workEditingBlocker = null; $("work-" + kind + "-form").reset(); });
  });
  $("work-delete-dialog").addEventListener("close", () => { state.workDeleting = null; });
  $("work-delete-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!state.workDeleting || !state.workReady || !state.workPermissions.manage_tasks || state.workBusy || busyDialogs.has("work-delete-dialog")) return;
    const { kind, item } = state.workDeleting;
    const epoch = state.workEpoch, jobId = state.workJobId;
    let reloadFocus = false;
    workSetBusy(true);
    dialogBusy("work-delete-dialog", true, "work-delete-save", "Elimina");
    try {
      await post("/api/" + (kind === "task" ? "tasks" : "blockers") + "/" + item.id + "/delete", { revision: item.revision });
      if (!workContextCurrent(epoch, jobId)) return;
      state.auditReady = false;
      $("work-delete-dialog").close();
      reloadFocus = true;
      showNotice(kind === "task" ? "Attività eliminata." : "Blocco eliminato.");
      await refreshWorkBoard();
    } catch (error) {
      if (!workContextCurrent(epoch, jobId)) return;
      if (error.status === 409) { $("work-delete-dialog").close(); reloadFocus = true; showNotice(error.message, true); await refreshWorkBoard(); }
      else dialogError("work-delete-error", error);
    } finally {
      dialogBusy("work-delete-dialog", false, "work-delete-save", "Elimina");
      if (workContextCurrent(epoch, jobId)) {
        workSetBusy(false);
        if (reloadFocus) focusWorkResult(kind, item.id);
        else if ($("work-delete-dialog").open && !$("work-delete-error").hidden) { $("work-delete-error").tabIndex = -1; $("work-delete-error").focus(); }
      }
    }
  });

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
    fields.is_common = $("job-common").checked ? "1" : "0";
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

  $("new-alert-rule").addEventListener("click", () => openAlertRule());
  document.querySelectorAll("[data-new-alert-type]").forEach((button) => {
    button.addEventListener("click", () => openAlertRule(null, button.dataset.newAlertType));
  });
  $("alert-rules-refresh").addEventListener("click", refreshAlertRules);
  $("alert-rules-retry").addEventListener("click", refreshAlertRules);
  $("notifications-refresh").addEventListener("click", refreshNotifications);
  $("notifications-retry").addEventListener("click", refreshNotifications);
  $("alert-rule-type").addEventListener("change", renderAlertRuleTypeHelp);
  $("alert-rule-emails").addEventListener("input", () => $("alert-rule-emails").setCustomValidity(""));
  $("alert-rule-dialog").addEventListener("close", () => { state.editingAlertRule = null; });
  $("alert-rule-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!isAlertManager() || busyDialogs.has("alert-rule-dialog")) return;
    $("alert-rule-name").value = $("alert-rule-name").value.trim();
    $("alert-rule-emails").setCustomValidity("");
    if (!$("alert-rule-form").reportValidity()) return;
    if (!/^([01]\d|2[0-3]):[0-5]\d$/.test($("alert-rule-time").value)) { dialogError("alert-rule-error", new Error("Scegli un orario valido, con ore e minuti.")); return; }
    let emails;
    try { emails = alertRecipientEmails(); } catch (error) {
      $("alert-rule-emails").setCustomValidity(error.message);
      $("alert-rule-emails").reportValidity();
      return;
    }
    const selected = [...$("alert-rule-users").querySelectorAll("input:checked")];
    if (selected.some((checkbox) => checkbox.dataset.unavailable === "1")) { dialogError("alert-rule-error", new Error("Deseleziona gli utenti non più attivi prima di salvare la regola.")); return; }
    if (selected.length > 50) { dialogError("alert-rule-error", new Error("Seleziona al massimo 50 utenti destinatari.")); return; }
    if (!selected.length && !emails.length) { dialogError("alert-rule-error", new Error("Scegli almeno un utente destinatario oppure inserisci un indirizzo email.")); return; }
    const id = state.editingAlertRule?.id;
    const email = state.session.email;
    const fields = { name: $("alert-rule-name").value, type: $("alert-rule-type").value, run_time: $("alert-rule-time").value, enabled: $("alert-rule-enabled").checked ? "1" : "0", recipient_user_ids: selected.map((checkbox) => checkbox.value).join(","), recipient_emails: emails.join(",") };
    if (fields.type === "inventory_due") fields.days_before = $("alert-rule-days").value;
    $("alert-rule-error").hidden = true;
    dialogBusy("alert-rule-dialog", true, "alert-rule-save", id ? "Salva modifiche" : "Crea regola");
    try {
      await post("/api/alert-rules" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      if (!isAlertManager() || state.session.email !== email) return;
      state.auditReady = false;
      $("alert-rule-dialog").close();
      state.alertRulesRequest++;
      state.alertRulesLoading = false;
      showNotice(id ? "Regola aggiornata." : fields.enabled === "1" ? "Regola creata. La prossima esecuzione è indicata nell’elenco." : "Regola creata e sospesa. Puoi attivarla o eseguire un controllo manuale.");
      await refreshAlertRules();
    } catch (error) {
      if (isAlertManager() && state.session.email === email) dialogError("alert-rule-error", error);
    } finally {
      dialogBusy("alert-rule-dialog", false, "alert-rule-save", id ? "Salva modifiche" : "Crea regola");
    }
  });

  $("job-delete").addEventListener("click", openDeleteJob);
  $("job-delete-dialog").addEventListener("close", () => { state.deleteJob = null; });
  $("job-delete-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_jobs") || !state.deleteJob || busyDialogs.has("job-delete-dialog")) return;
    const job = state.deleteJob;
    $("job-delete-error").hidden = true;
    dialogBusy("job-delete-dialog", true, "job-delete-save", "Elimina commessa e ore");
    $("job-delete-save").textContent = "Eliminazione…";
    try {
      await api("/api/jobs/" + encodeURIComponent(job.id), { method: "DELETE" });
      state.auditReady = false;
      state.openEntryAfterJob = false;
      $("job-delete-dialog").close();
      $("job-dialog").close();
      if (String(state.assignmentJob?.id) === String(job.id)) {
        state.assignmentRequest++;
        state.assignmentJob = null;
        state.assignmentsReady = false;
        $("assignment-dialog").close();
      }
      showNotice(`Commessa “${job.name}” eliminata insieme a tutte le ore e alle abilitazioni collegate.`);
      await refreshAll();
    } catch (error) {
      dialogError("job-delete-error", error);
    } finally {
      dialogBusy("job-delete-dialog", false, "job-delete-save", "Elimina commessa e ore");
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
      state.organizationRequest++;
      state.groupMembersRequest++;
      state.organizationReady = false;
      state.groupMembersReady = false;
      state.organizationLoading = false;
      state.organizationConfirm = null;
      state.organizationUsers = [];
      state.groupMembers = [];
      $("organization-content").hidden = true;
      $("organization-loading").hidden = true;
      $("organization-confirm").hidden = true;
      $("group-members").replaceChildren();
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
    const fields = { user_id: $("assignment-user").value, role_id: $("assignment-role").value, starts_on: startsOn, ends_on: endsOn };
    $("assignment-error").hidden = true;
    setAssignmentBusy(true);
    try {
      await post(`/api/jobs/${encodeURIComponent(jobId)}/assignments`, fields);
      state.auditReady = false;
      state.revokeAssignment = null;
      $("assignment-revoke").hidden = true;
      showNotice("Abilitazione salvata. Ruolo e periodo di accesso alla commessa sono aggiornati.");
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
    state.assignmentGroups = [];
    state.assignmentRoles = [];
    state.groupGrants = [];
    state.editingGroupGrant = null;
    state.revokeGroupGrant = null;
    $("assignment-form").reset();
    $("group-grant-form").reset();
  });
  $("organization-refresh").addEventListener("click", () => refreshOrganization());
  $("organization-retry").addEventListener("click", () => refreshOrganization());
  $("new-group").addEventListener("click", () => openGroup());
  $("edit-group").addEventListener("click", () => { if (state.selectedGroup) openGroup(state.selectedGroup); });
  $("new-job-role").addEventListener("click", () => openJobRole());
  $("delete-group").addEventListener("click", () => {
    if (!state.selectedGroup) return;
    const group = state.selectedGroup;
    confirmOrganization(`Eliminare il gruppo ${group.name}? È possibile eliminare solo gruppi senza membri, sottogruppi o abilitazioni alle commesse.`, async () => {
      await api(`/api/groups/${encodeURIComponent(group.id)}`, { method: "DELETE" });
      showNotice("Gruppo eliminato.");
      await refreshOrganization(null);
    });
  });
  $("organization-confirm-cancel").addEventListener("click", () => {
    if (state.organizationBusy) return;
    state.organizationConfirm = null;
    $("organization-confirm").hidden = true;
  });
  $("organization-confirm-save").addEventListener("click", async () => {
    if (!requirePermission("manage_groups") || state.organizationBusy || !state.organizationConfirm) return;
    const operation = state.organizationConfirm;
    $("organization-error").hidden = true;
    setOrganizationBusy(true);
    try {
      await operation();
      state.auditReady = false;
      state.organizationConfirm = null;
      $("organization-confirm").hidden = true;
      await refreshAll();
    } catch (error) {
      $("organization-error-text").textContent = error.message;
      $("organization-error").hidden = false;
    } finally { setOrganizationBusy(false); }
  });
  $("group-member-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_groups") || state.organizationBusy || !state.selectedGroup || !state.groupMembersReady || !$("group-member-form").reportValidity()) return;
    const groupId = state.selectedGroup.id;
    const userId = $("group-member-user").value;
    $("group-members-error").hidden = true;
    setOrganizationBusy(true);
    try {
      await post(`/api/groups/${encodeURIComponent(groupId)}/members`, { user_id: userId });
      state.auditReady = false;
      showNotice("Utente aggiunto al gruppo. Riceve anche le abilitazioni dei gruppi padre.");
      await refreshOrganization(groupId);
      await refreshAll();
    } catch (error) { dialogError("group-members-error", error); }
    finally { setOrganizationBusy(false); }
  });
  $("group-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_groups") || busyDialogs.has("group-dialog")) return;
    $("group-name").value = $("group-name").value.trim();
    if (!$("group-form").reportValidity()) return;
    const id = $("group-id").value;
    const fields = { name: $("group-name").value, parent_id: $("group-parent").value };
    $("group-error").hidden = true;
    dialogBusy("group-dialog", true, "group-save", id ? "Salva modifiche" : "Crea gruppo");
    try {
      const data = await post("/api/groups" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("group-dialog").close();
      showNotice(id ? "Gruppo aggiornato. Le abilitazioni ereditate seguono la nuova struttura." : "Gruppo creato. Puoi aggiungere i membri.");
      await refreshOrganization(id || data.id);
      await refreshAll();
    } catch (error) { dialogError("group-error", error); }
    finally { dialogBusy("group-dialog", false, "group-save", id ? "Salva modifiche" : "Crea gruppo"); }
  });
  $("job-role-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_job_roles") || busyDialogs.has("job-role-dialog")) return;
    ["job-role-code", "job-role-name"].forEach((id) => { $(id).value = $(id).value.trim(); });
    if (!$("job-role-form").reportValidity()) return;
    const id = $("job-role-id").value;
    const fields = { code: $("job-role-code").value, name: $("job-role-name").value, can_log_time: $("job-role-log-time").value };
    $("job-role-error").hidden = true;
    dialogBusy("job-role-dialog", true, "job-role-save", id ? "Salva modifiche" : "Crea ruolo");
    try {
      await post("/api/job-roles" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("job-role-dialog").close();
      showNotice(id ? "Ruolo aggiornato. Le abilitazioni che lo utilizzano sono aggiornate." : "Ruolo creato. Puoi assegnarlo agli utenti o ai gruppi nelle commesse.");
      await refreshOrganization();
      await refreshAll();
    } catch (error) { dialogError("job-role-error", error); }
    finally { dialogBusy("job-role-dialog", false, "job-role-save", id ? "Salva modifiche" : "Crea ruolo"); }
  });
  $("group-grant-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog") || !state.assignmentJob || !state.assignmentsReady) return;
    $("group-grant-end").setCustomValidity("");
    const startsOn = $("group-grant-start").value;
    const endsOn = $("group-grant-end").value;
    if (startsOn && !validDate(startsOn) || endsOn && !validDate(endsOn)) { dialogError("assignment-error", new Error("Scegli date valide per il periodo di abilitazione del gruppo.")); return; }
    if (startsOn && endsOn && startsOn > endsOn) $("group-grant-end").setCustomValidity("La data finale deve essere uguale o successiva alla data iniziale.");
    if (!$("group-grant-form").reportValidity()) return;
    const jobId = state.assignmentJob.id;
    const fields = { group_id: $("group-grant-group").value, role_id: $("group-grant-role").value, starts_on: startsOn, ends_on: endsOn };
    $("assignment-error").hidden = true;
    setAssignmentBusy(true);
    try {
      await post(`/api/jobs/${encodeURIComponent(jobId)}/group-grants`, fields);
      state.auditReady = false;
      state.revokeGroupGrant = null;
      $("group-grant-revoke").hidden = true;
      showNotice("Gruppo abilitato. L’accesso vale anche per i membri dei suoi sottogruppi.");
      await loadAssignments();
      await refreshAll();
    } catch (error) { dialogError("assignment-error", error); }
    finally { setAssignmentBusy(false); }
  });
  $("group-grant-revoke-save").addEventListener("click", async () => {
    if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog") || !state.assignmentJob || !state.revokeGroupGrant) return;
    const jobId = state.assignmentJob.id;
    const groupId = state.revokeGroupGrant.group_id;
    $("assignment-error").hidden = true;
    setAssignmentBusy(true);
    try {
      await api(`/api/jobs/${encodeURIComponent(jobId)}/group-grants/${encodeURIComponent(groupId)}`, { method: "DELETE" });
      state.auditReady = false;
      state.revokeGroupGrant = null;
      $("group-grant-revoke").hidden = true;
      showNotice("Abilitazione del gruppo revocata. Le ore già registrate sono conservate.");
      await loadAssignments();
      await refreshAll();
    } catch (error) { dialogError("assignment-error", error); }
    finally { setAssignmentBusy(false); }
  });
  $("group-grant-revoke-cancel").addEventListener("click", () => { state.revokeGroupGrant = null; $("group-grant-revoke").hidden = true; $("group-grant-group").focus(); });
  $("group-grant-reset").addEventListener("click", () => { resetGroupGrantForm(); $("group-grant-group").focus(); });
  ["group-grant-start", "group-grant-end"].forEach((id) => $(id).addEventListener("input", () => $("group-grant-end").setCustomValidity("")));

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

  $("report-form").addEventListener("submit", (event) => {
    event.preventDefault();
    if (!$("report-form").reportValidity()) return;
    applyReportFilters(readReportFilters());
  });
  $("report-reset").addEventListener("click", () => {
    state.reportFilters = defaultReportFilters(state.reportToday || localDate(new Date()));
    state.reportDefaultsPending = !state.reportToday;
    renderReportControls();
    writeReportRoute();
    refreshReports();
  });
  $("report-retry").addEventListener("click", () => {
    if (!$("report-form").reportValidity()) return;
    applyReportFilters(readReportFilters());
  });
  for (const view of Object.keys(reportViews)) {
    $("report-tab-" + view).addEventListener("click", () => chooseReportView(view));
    $("report-tab-" + view).addEventListener("keydown", (event) => {
      if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
      event.preventDefault();
      const available = availableReportViews();
      const direction = event.key === "ArrowRight" ? 1 : -1;
      const next = event.key === "Home" ? available[0] : event.key === "End" ? available[available.length - 1] : available[(available.indexOf(view) + direction + available.length) % available.length];
      chooseReportView(next, true);
    });
  }
  document.querySelectorAll("[data-report-preset]").forEach((button) => {
    button.addEventListener("click", () => {
      if (!state.reportToday) return;
      const today = state.reportToday;
      const filters = { ...readReportFilters(), from: today, to: today };
      if (button.dataset.reportPreset === "month") filters.from = today.slice(0, 7) + "-01";
      if (button.dataset.reportPreset === "week") {
        const date = new Date(reportDayNumber(today) * 86400000);
        date.setUTCDate(date.getUTCDate() - (date.getUTCDay() + 6) % 7);
        filters.from = date.toISOString().slice(0, 10);
      }
      applyReportFilters(filters);
    });
  });
  const onReportRouteChange = () => { if (window.location.hash !== state.routeHash) applyReportRoute(); };
  window.addEventListener("hashchange", onReportRouteChange);
  window.addEventListener("popstate", onReportRouteChange);
  $("report-new-rate").addEventListener("click", () => openLaborRate());
  $("report-rates-retry").addEventListener("click", refreshLaborRates);
  $("labor-rate-hourly").addEventListener("input", () => $("labor-rate-hourly").setCustomValidity(""));
  ["labor-rate-from", "labor-rate-to"].forEach((id) => $(id).addEventListener("input", () => $("labor-rate-to").setCustomValidity("")));
  $("labor-rate-dialog").addEventListener("close", () => {
    state.editingLaborRate = null;
    $("labor-rate-form").reset();
    $("labor-rate-user").replaceChildren();
    $("labor-rate-error").hidden = true;
  });
  $("labor-rate-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!isReportManager() || busyDialogs.has("labor-rate-dialog")) return;
    const editing = state.editingLaborRate;
    $("labor-rate-hourly").setCustomValidity("");
    $("labor-rate-to").setCustomValidity("");
    let cents;
    try {
      cents = valueToCents($("labor-rate-hourly").value);
      if (cents === "" || BigInt(cents) > 99999999n) throw new Error("Inserisci una tariffa da 0 a 999.999,99 EUR, con al massimo due decimali.");
    } catch (error) { $("labor-rate-hourly").setCustomValidity(error.message); }
    const from = $("labor-rate-from").value;
    const to = $("labor-rate-to").value;
    if (!validDate(from) || to && !validDate(to)) { dialogError("labor-rate-error", new Error("Scegli date valide per la tariffa.")); return; }
    if (to && from > to) $("labor-rate-to").setCustomValidity("La data finale deve essere uguale o successiva alla data iniziale.");
    if (!$("labor-rate-form").reportValidity()) return;
    const fields = { user_id: editing ? String(editing.user_id) : $("labor-rate-user").value, hourly_cents: cents, starts_on: from, ends_on: to, currency: "EUR" };
    $("labor-rate-error").hidden = true;
    dialogBusy("labor-rate-dialog", true, "labor-rate-save", editing ? "Salva modifiche" : "Crea tariffa");
    try {
      await post("/api/labor-rates" + (editing ? "/" + encodeURIComponent(editing.id) : ""), fields);
      if (!isReportManager()) return;
      state.auditReady = false;
      $("labor-rate-dialog").close();
      showNotice(editing ? "Tariffa aggiornata. Le stime sono state ricalcolate." : "Tariffa creata. Le ore nel periodo di validità sono ora valorizzate.");
      await Promise.all([refreshReports(), refreshLaborRates()]);
    } catch (error) {
      if (isReportManager() && $("labor-rate-dialog").open) dialogError("labor-rate-error", error);
    } finally {
      dialogBusy("labor-rate-dialog", false, "labor-rate-save", editing ? "Salva modifiche" : "Crea tariffa");
      $("labor-rate-user").disabled = Boolean(editing) || !isReportManager();
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
  window.setInterval(() => { if (state.session?.authenticated && !document.hidden) refreshNotifications(); }, 60000);

  async function initialize() {
    try {
      if (!await refreshSession()) return;
      $("session-status").hidden = true;
      $("workspace").hidden = false;
      applyReportRoute();
      await refreshAll();
    } catch (error) {
      $("session-status").textContent = error.message + " Ricarica la pagina per riprovare.";
    }
  }
  initialize();
})();
