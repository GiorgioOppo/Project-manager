"use strict";

(async () => {
  if (window.PuntoI18n) await window.PuntoI18n.ready;
  const { t, locale } = window.PuntoI18n || { t: (source, values = []) => source.replace(/\{(\d+)\}/g, (_, index) => values[index]), locale: "it-IT" };
  const $ = (id) => document.getElementById(id);
  const statuses = { open: t("Aperto"), in_progress: t("In corso"), waiting: t("In attesa"), resolved: t("Risolto"), closed: t("Chiuso") };
  const priorities = { low: t("Bassa"), normal: t("Normale"), high: t("Alta"), urgent: t("Urgente") };
  const queues = { all: t("Tutti i ticket"), active: t("Da lavorare"), mine: t("I miei ticket"), unassigned: t("Non assegnati"), overdue: t("Scaduti"), due_soon: t("In scadenza") };
  const queueDescriptions = { all: t("Tutte le richieste, comprese quelle risolte e chiuse."), active: t("Ticket aperti, in corso o in attesa."), mine: t("Ticket da lavorare assegnati a te."), unassigned: t("Ticket da lavorare che aspettano un assegnatario."), overdue: t("Ticket da lavorare con una scadenza già passata."), due_soon: t("Ticket da lavorare in scadenza da oggi ai prossimi 7 giorni.") };
  const historyActions = {
    created: t("ha aperto il ticket"), create: t("ha aperto il ticket"), updated: t("ha aggiornato il ticket"),
    update: t("ha aggiornato il ticket"), commented: t("ha aggiunto un commento"), comment: t("ha aggiunto un commento"),
    status_changed: t("ha cambiato lo stato"), assigned: t("ha aggiornato l’assegnazione")
  };
  const state = {
    user: null, csrf: "", auth: null, users: [], offset: 0, total: 0, limit: 50,
    listRequest: null, detailRequest: null, detailId: null, detail: null,
    editorTicket: null, editorUser: null, searchTimer: null, toastTimer: null, generation: 0,
    view: "board", queue: "active", listCycle: 0, boardCycle: 0, boardParams: "", listControllers: [], board: new Map(), items: [], routeSync: false, suppressDetailClose: false
  };
  Object.assign(state, { punto: { configured: false, connected: false }, resources: { job: null, asset: null }, resourceFilters: { job: null, asset: null }, picker: null, pickerRequest: 0, resourceRouteVersion: 0 });
  const callbackParams = new URLSearchParams(location.hash.slice(1));
  if (callbackParams.has("punto_grant")) {
    state.puntoCallback = { grant: callbackParams.get("punto_grant"), state: callbackParams.get("state") };
    history.replaceState(null, "", location.pathname + location.search);
  }
  const dateTimeFormat = new Intl.DateTimeFormat(locale, { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit", timeZone: "Europe/Rome" });
  const shortDateFormat = new Intl.DateTimeFormat(locale, { day: "numeric", month: "short", timeZone: "UTC" });
  const fullDateFormat = new Intl.DateTimeFormat(locale, { day: "numeric", month: "long", year: "numeric", timeZone: "UTC" });
  const romeDateFormat = new Intl.DateTimeFormat("en-CA", { year: "numeric", month: "2-digit", day: "2-digit", timeZone: "Europe/Rome" });

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function showError(id, message) {
    $(id).textContent = message || "";
    $(id).hidden = !message;
  }

  function toast(message, error = false) {
    clearTimeout(state.toastTimer);
    $("toast").textContent = message;
    $("toast").classList.toggle("is-error", error);
    $("toast").hidden = false;
    state.toastTimer = setTimeout(() => { $("toast").hidden = true; }, error ? 6500 : 3800);
  }

  function initials(name) {
    return String(name || "?").trim().split(/\s+/).filter(Boolean).slice(0, 2).map((part) => Array.from(part)[0]).join("").toLocaleUpperCase(locale);
  }

  function time(value, short = false) {
    const date = new Date(value);
    if (!value || !Number.isFinite(date.getTime())) return "—";
    return short ? new Intl.DateTimeFormat(locale, { day: "numeric", month: "short", timeZone: "Europe/Rome" }).format(date) : dateTimeFormat.format(date);
  }

  function calendarDate(value, full = false) {
    const date = new Date(`${value}T12:00:00Z`);
    return !value || !Number.isFinite(date.getTime()) ? "—" : (full ? fullDateFormat : shortDateFormat).format(date);
  }

  function todayInRome() {
    const parts = Object.fromEntries(romeDateFormat.formatToParts(new Date()).filter((part) => part.type !== "literal").map((part) => [part.type, part.value]));
    return `${parts.year}-${parts.month}-${parts.day}`;
  }

  function deadline(ticket) {
    if (!ticket.due_date) return null;
    const days = Math.round((Date.parse(`${ticket.due_date}T12:00:00Z`) - Date.parse(`${todayInRome()}T12:00:00Z`)) / 86400000);
    if (["resolved", "closed"].includes(ticket.status)) return { label: calendarDate(ticket.due_date), kind: "complete" };
    if (days < 0) return { label: t("Scaduto · {0}", [calendarDate(ticket.due_date)]), kind: "overdue" };
    if (days === 0) return { label: t("Oggi"), kind: "today" };
    if (days === 1) return { label: t("Domani"), kind: "soon" };
    if (days <= 7) return { label: t("Tra {0} giorni", [days]), kind: "soon" };
    return { label: calendarDate(ticket.due_date), kind: "future" };
  }

  function deadlineNode(ticket) {
    const info = deadline(ticket);
    if (!info) return element("span", "no-deadline", t("Nessuna scadenza"));
    const node = element("span", `deadline-badge deadline-${info.kind}`, info.label);
    node.title = t("Scadenza: {0}", [calendarDate(ticket.due_date, true)]);
    node.setAttribute("aria-label", t("Scadenza: {0}, {1}", [info.label, calendarDate(ticket.due_date, true)]));
    return node;
  }

  function preferenceKey() { return `ticket.preferences.v1.${state.user.id}`; }

  function restorePreferences() {
    let saved = {};
    try { saved = JSON.parse(localStorage.getItem(preferenceKey()) || "{}"); } catch { /* Le preferenze sono facoltative. */ }
    if (!saved || typeof saved !== "object") saved = {};
    state.view = ["board", "list"].includes(saved.view) ? saved.view : "board";
    state.queue = Object.hasOwn(queues, saved.queue) ? saved.queue : "active";
    $("status-filter").value = Object.hasOwn(statuses, saved.status) ? saved.status : "";
    $("priority-filter").value = Object.hasOwn(priorities, saved.priority) ? saved.priority : "";
    $("sort-filter").value = ["updated", "priority", "due"].includes(saved.sort) ? saved.sort : "updated";
    if (typeof saved.assignee === "string" && /^(?:unassigned|[1-9]\d*)$/.test(saved.assignee)) {
      if (!Array.from($("assignee-filter").options).some((option) => option.value === saved.assignee)) addOption($("assignee-filter"), saved.assignee, t("Utente #{0} (non disponibile)", [saved.assignee]));
      $("assignee-filter").value = saved.assignee;
    }
    if (["resolved", "closed"].includes($("status-filter").value)) state.queue = "all";
    if (["mine", "unassigned"].includes(state.queue)) $("assignee-filter").value = "";
  }

  function savePreferences() {
    if (!state.user) return;
    try {
      localStorage.setItem(preferenceKey(), JSON.stringify({ view: state.view, queue: state.queue, sort: $("sort-filter").value, status: $("status-filter").value, priority: $("priority-filter").value, assignee: $("assignee-filter").value }));
    } catch { /* L’app funziona anche senza archivio locale. */ }
  }

  function ticketRoute() {
    const value = new URL(location.href).searchParams.get("ticket");
    return value && /^[1-9]\d*$/.test(value) && Number.isSafeInteger(Number(value)) ? Number(value) : null;
  }

  function ticketURL(id) {
    const url = new URL(location.href);
    url.searchParams.set("ticket", String(id));
    return url;
  }

  function ticketLink(ticket, className) {
    const link = element("a", className, ticket.title);
    link.href = ticketURL(ticket.id).href;
    link.setAttribute("aria-label", t("Apri ticket #{0}: {1}", [ticket.id, ticket.title]));
    link.addEventListener("click", (event) => {
      if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey || event.button !== 0) return;
      event.preventDefault();
      openDetail(ticket.id);
    });
    return link;
  }

  function resourceLabel(kind, item) {
    if (!item) return kind === "job" ? t("Nessuna commessa") : t("Nessun asset");
    return `${kind === "job" ? item.code || `#${item.id}` : item.asset_tag || `#${item.id}`} · ${item.name || ""}`;
  }

  function ticketResource(ticket, kind) {
    return ticket[`${kind}_id`] ? { id: ticket[`${kind}_id`], name: ticket[`${kind}_name`], ...(kind === "job" ? { code: ticket.job_code } : { asset_tag: ticket.asset_tag }) } : null;
  }

  function resourceBadges(ticket, links = false) {
    const content = element("div", links ? "detail-resources" : "resource-badges");
    for (const kind of ["job", "asset"]) {
      const item = ticketResource(ticket, kind);
      if (!item) continue;
      const label = `${kind === "job" ? t("Commessa") : t("Asset")}: ${resourceLabel(kind, item)}`;
      const badge = element(links && state.punto.origin ? "a" : "span", "resource-badge", label + (links && state.punto.origin ? " ↗" : ""));
      badge.title = label;
      if (badge.tagName === "A") {
        const url = new URL(state.punto.origin);
        url.pathname = "/app";
        url.hash = kind === "job" ? `jobs?job_id=${item.id}` : `inventory?asset_id=${item.id}`;
        badge.href = window.PuntoI18n ? window.PuntoI18n.url(url.href) : url.href;
      }
      content.append(badge);
    }
    return content;
  }

  function renderPuntoStatus() {
    const punto = state.punto;
    $("punto-integration").hidden = !punto.configured;
    $("punto-status").textContent = punto.connected ? t("Punto collegato · {0}", [punto.user_name]) : t("Commesse e asset di Punto");
    $("punto-status-description").textContent = punto.connected ? t("Vedi i ticket delle commesse abilitate per il tuo account Punto e dell’inventario.") : t("Collega la tua sessione Punto per vedere i ticket associati e scegliere commesse e asset.");
    $("connect-punto").hidden = punto.connected;
    $("disconnect-punto").hidden = !punto.connected;
    $("open-punto").hidden = !punto.origin;
    if (punto.origin) $("open-punto").href = window.PuntoI18n ? window.PuntoI18n.url(new URL("/app", punto.origin).href) : new URL("/app", punto.origin).href;
    $("resource-filters").hidden = !punto.configured;
    for (const kind of ["job", "asset"]) $("filter-" + kind).disabled = !punto.connected;
    renderResourceChoices();
  }

  async function loadPuntoStatus() {
    const generation = state.generation;
    try {
      const punto = await api("/api/integrations/punto/status");
      if (generation !== state.generation || !state.user) return;
      state.punto = punto;
      renderPuntoStatus();
    } catch (error) { if (generation === state.generation && state.user) showError("integration-error", error.message); }
  }

  async function finishPuntoConnection() {
    if (!state.puntoCallback || !state.user) return;
    const generation = state.generation;
    const callback = state.puntoCallback; state.puntoCallback = null;
    try {
      if (!/^[a-f0-9]{64}$/.test(callback.grant || "") || !/^[a-f0-9]{64}$/.test(callback.state || "")) throw new Error(t("Il collegamento ricevuto non è valido. Ripeti Collega Punto."));
      const punto = await api("/api/integrations/punto/connect", { method: "POST", body: callback });
      if (generation !== state.generation || !state.user) return;
      state.punto = punto;
      renderPuntoStatus();
      toast(t("Punto collegato come {0}.", [state.punto.user_name]));
      try {
        const intent = JSON.parse(sessionStorage.getItem("ticket.punto.intent") || "null");
        sessionStorage.removeItem("ticket.punto.intent");
        if (intent?.userId === state.user.id && typeof intent.query === "string") {
          const params = new URLSearchParams(intent.query), restored = new URL(location.href);
          for (const key of ["job_id", "asset_id", "new", "ticket"]) if (params.has(key)) restored.searchParams.set(key, params.get(key));
          history.replaceState(null, "", restored);
        }
      } catch { /* I collegamenti funzionano anche senza ripristinare il filtro. */ }
    } catch (error) { if (generation === state.generation && state.user) showError("integration-error", error.message); }
  }

  function renderResourceChoices() {
    $("ticket-resources").hidden = !state.punto.configured && !state.resources.job && !state.resources.asset;
    $("editor-resource-hint").textContent = state.punto.connected ? t("Scegli una commessa e un asset, anche separatamente.") : t("Collega Punto dalla pagina iniziale per scegliere commesse e asset.");
    for (const kind of ["job", "asset"]) {
      $("editor-" + kind + "-label").textContent = resourceLabel(kind, state.resources[kind]);
      $("choose-" + kind).disabled = !state.punto.connected;
      $("clear-" + kind).hidden = !state.resources[kind];
      $("filter-" + kind).textContent = state.resourceFilters[kind] ? resourceLabel(kind, state.resourceFilters[kind]) : kind === "job" ? t("Tutte le commesse") : t("Tutti gli asset");
    }
    $("clear-resource-filters").hidden = !state.resourceFilters.job && !state.resourceFilters.asset;
  }

  function setResourceFilter(kind, item) {
    state.resourceRouteVersion += 1;
    state.resourceFilters[kind] = item;
    const url = new URL(location.href);
    if (item) url.searchParams.set(kind + "_id", String(item.id)); else url.searchParams.delete(kind + "_id");
    history.replaceState(null, "", url);
    renderResourceChoices(); state.offset = 0; loadTickets();
  }

  async function applyResourceRoute() {
    if (!state.user) return;
    const generation = state.generation;
    const version = state.resourceRouteVersion;
    const url = new URL(location.href), params = new URLSearchParams();
    for (const kind of ["job", "asset"]) {
      const id = url.searchParams.get(kind + "_id");
      if (id && /^[1-9]\d*$/.test(id) && Number.isSafeInteger(Number(id))) params.set(kind + "_id", id);
    }
    if (!params.size) return;
    state.queue = "all"; $("status-filter").value = "";
    for (const kind of ["job", "asset"]) state.resourceFilters[kind] = params.has(kind + "_id") ? { id: Number(params.get(kind + "_id")), name: "" } : null;
    if (!state.punto.connected) {
      showError("integration-error", t("Collega Punto per consultare i ticket di questa commessa o di questo asset."));
      renderResourceChoices(); return;
    }
    try {
      const data = await api(`/api/integrations/punto/validate?${params}`);
      if (generation !== state.generation || version !== state.resourceRouteVersion || !state.user) return;
      state.resourceFilters = { job: data.job, asset: data.asset };
      if (url.searchParams.get("new") === "1") {
        openEditor(); state.resources = { job: data.job, asset: data.asset }; renderResourceChoices();
        url.searchParams.delete("new"); history.replaceState(null, "", url);
      }
    } catch (error) { if (generation === state.generation && version === state.resourceRouteVersion && state.user) showError("integration-error", error.message); }
    if (generation === state.generation && version === state.resourceRouteVersion && state.user) renderResourceChoices();
  }

  function openResourcePicker(kind, target) {
    state.picker = { kind, target, offset: 0, cursor: null, count: 0, query: "" };
    $("resource-picker-title").textContent = kind === "job" ? t("Scegli la commessa") : t("Scegli l’asset");
    $("resource-search").value = "";
    $("resource-none").textContent = target === "filter" ? kind === "job" ? t("Tutte le commesse") : t("Tutti gli asset") : t("Nessun collegamento");
    $("resource-picker").showModal(); searchResources();
  }

  function chooseResource(item) {
    const { kind, target } = state.picker;
    $("resource-picker").close();
    if (target === "filter") setResourceFilter(kind, item);
    else { state.resources[kind] = item; renderResourceChoices(); }
  }

  async function searchResources(more = false) {
    const picker = state.picker, request = ++state.pickerRequest;
    if (!picker) return;
    if (more && picker.query !== $("resource-search").value.trim()) return;
    if (!more) { picker.query = $("resource-search").value.trim(); picker.offset = 0; picker.cursor = null; picker.count = 0; $("resource-results").replaceChildren(); }
    if (picker.kind === "asset" && new TextEncoder().encode(picker.query).length > 200) { showError("resource-error", t("La ricerca asset è troppo lunga. Usa un codice o un nome più breve.")); $("resource-loading").hidden = true; $("resource-more").hidden = true; return; }
    const query = new URLSearchParams({ limit: "30" });
    query.set(picker.kind === "job" ? "search" : "q", picker.query);
    if (picker.kind === "job") query.set("offset", String(picker.offset));
    else if (picker.cursor) query.set("after_id", String(picker.cursor));
    $("resource-loading").hidden = false; $("resource-more").disabled = true; showError("resource-error", "");
    try {
      const data = await api(`/api/integrations/punto/${picker.kind === "job" ? "jobs" : "assets"}?${query}`);
      if (request !== state.pickerRequest || picker !== state.picker || !$("resource-picker").open) return;
      for (const item of data.items || []) {
        const button = element("button", "resource-result"); button.type = "button";
        const catalogStatuses = picker.kind === "job" ? { planned: t("Da avviare"), active: t("In corso"), paused: t("In pausa"), completed: t("Completata"), cancelled: t("Annullata") } : { available: t("Disponibile"), assigned: t("Assegnato"), maintenance: t("In manutenzione"), retired: t("Dismesso"), lost: t("Smarrito") };
        button.append(element("strong", "", resourceLabel(picker.kind, item)), element("span", "", catalogStatuses[item.status] || item.status || ""));
        button.addEventListener("click", () => chooseResource(item)); $("resource-results").append(button);
      }
      picker.count += (data.items || []).length; picker.offset = picker.count; picker.cursor = data.next_after_id;
      $("resource-more").hidden = picker.kind === "job" ? picker.count >= data.total : !picker.cursor;
      if (!picker.count) $("resource-results").append(element("p", "muted", t("Nessun risultato. Prova un altro codice o nome.")));
    } catch (error) { if (request === state.pickerRequest) { showError("resource-error", error.message); $("resource-more").hidden = true; } }
    finally { if (request === state.pickerRequest) { $("resource-loading").hidden = true; $("resource-more").disabled = picker.query !== $("resource-search").value.trim(); } }
  }

  class ApiError extends Error {
    constructor(message, status) { super(message); this.status = status; }
  }

  async function api(path, options = {}) {
    const generation = state.generation;
    const headers = { Accept: "application/json" };
    if (options.body !== undefined) headers["Content-Type"] = "application/json";
    if (options.method && options.method !== "GET" && state.csrf) headers["X-CSRF-Token"] = state.csrf;
    let response;
    try {
      response = await fetch(path, { credentials: "same-origin", cache: "no-store", ...options, headers: { ...headers, ...options.headers }, body: options.body === undefined ? undefined : JSON.stringify(options.body) });
    } catch (error) {
      if (error.name === "AbortError") throw error;
      throw new ApiError(t("Connessione non disponibile. Controlla la rete e riprova."), 0);
    }
    let data;
    try { data = await response.json(); } catch { data = {}; }
    if (!response.ok) {
      if (response.status === 401 && state.user && generation === state.generation) showLogin(t("La sessione è scaduta. Accedi di nuovo per continuare."));
      throw new ApiError((data.error ? t(data.error) : "") || t("Non è stato possibile completare l’operazione. Riprova."), response.status);
    }
    return data;
  }

  function busy(form, button, active, label) {
    if (active) {
      form.setAttribute("aria-busy", "true");
      for (const control of form.elements) {
        control.dataset.wasDisabled = control.disabled ? "true" : "false";
        control.disabled = true;
      }
      button.dataset.previousLabel = button.textContent;
      button.textContent = label;
    } else {
      form.removeAttribute("aria-busy");
      for (const control of form.elements) {
        control.disabled = control.dataset.wasDisabled === "true";
        delete control.dataset.wasDisabled;
      }
      button.textContent = button.dataset.previousLabel || button.textContent;
      delete button.dataset.previousLabel;
    }
  }

  function closeDialog(dialog) {
    if (dialog.querySelector('[aria-busy="true"]')) return;
    dialog.close();
  }

  function showLogin(notice = "") {
    state.generation += 1;
    state.user = null;
    state.csrf = "";
    state.users = [];
    state.detail = null;
    state.detailId = null;
    state.punto = { configured: false, connected: false };
    state.resources = { job: null, asset: null };
    state.resourceFilters = { job: null, asset: null };
    state.pickerRequest += 1;
    state.listRequest?.abort();
    for (const controller of state.listControllers) controller.abort();
    state.listCycle += 1;
    state.board.clear();
    state.items = [];
    $("ticket-board").replaceChildren();
    $("ticket-rows").replaceChildren();
    $("ticket-total").textContent = "—";
    for (const status of Object.keys(statuses)) $("stat-" + status).textContent = "—";
    for (const queue of Object.keys(queues)) $("queue-count-" + queue).textContent = "—";
    $("board-container").hidden = true;
    $("ticket-table-container").hidden = true;
    $("list-empty").hidden = true;
    $("pagination").hidden = true;
    $("list-loading").hidden = true;
    state.detailRequest?.abort();
    clearTimeout(state.searchTimer);
    if ($("ticket-detail").open) state.suppressDetailClose = true;
    for (const dialog of document.querySelectorAll("dialog[open]")) dialog.close();
    $("boot-view").hidden = true;
    $("app-view").hidden = true;
    $("session-actions").hidden = true;
    $("login-header-note").hidden = false;
    $("login-view").hidden = false;
    $("login-form").hidden = state.auth?.local !== true;
    $("sso-login").hidden = state.auth?.sso !== true;
    $("login-description").textContent = !state.auth ? t("Le modalità di accesso non sono disponibili. Ricarica la pagina per riprovare.") : state.auth.managed_users && state.auth.local ? t("Usa l’account aziendale. L’accesso locale è riservato all’amministratore di emergenza.") : state.auth.sso ? (state.auth.local ? t("Usa l’account aziendale oppure le credenziali Ticket.") : t("Usa il tuo account aziendale.")) : t("Usa le credenziali del tuo account Ticket.");
    document.title = t("Ticket. · Gestione richieste");
    $("login-password").value = "";
    showError("login-error", "");
    $("login-notice").textContent = notice;
    $("login-notice").hidden = !notice;
    if (notice) (state.auth?.local ? $("login-email") : $("sso-login").querySelector("a")).focus();
  }

  function updateSession(data) {
    state.user = data.user;
    state.csrf = data.csrf_token;
    $("session-name").textContent = state.user.name;
    $("session-avatar").textContent = initials(state.user.name);
    $("session-role").textContent = state.user.role === "admin" ? t("Amministratore") : t("Operatore");
    const managed = state.auth?.managed_users === true;
    $("manage-users").hidden = state.user.role !== "admin" || managed;
    $("manage-central-users").hidden = state.user.role !== "admin" || !managed || !state.auth.users_admin_url;
    if (managed && state.auth.users_admin_url) $("manage-central-users").href = state.auth.users_admin_url;
    $("new-user").hidden = managed;
    if (state.user.role !== "admin" || managed) {
      $("users-dialog").close();
      $("user-editor").close();
    }
  }

  async function openApp(data) {
    state.generation += 1;
    const generation = state.generation;
    updateSession(data);
    state.offset = 0;
    $("filter-form").reset();
    $("boot-view").hidden = true;
    $("login-view").hidden = true;
    $("login-header-note").hidden = true;
    $("session-actions").hidden = false;
    $("app-view").hidden = false;
    document.title = t("Le richieste · Ticket.");
    try { await loadUsers(); } catch { /* L’elenco resta utilizzabile quando il team non è disponibile. */ }
    if (!state.user || generation !== state.generation) return;
    restorePreferences();
    showError("integration-error", "");
    await loadPuntoStatus();
    if (!state.user || generation !== state.generation) return;
    await finishPuntoConnection();
    if (!state.user || generation !== state.generation) return;
    await applyResourceRoute();
    if (!state.user || generation !== state.generation) return;
    await loadTickets();
    const requestedTicket = ticketRoute();
    if (requestedTicket && state.user && generation === state.generation) await openDetail(requestedTicket, false);
  }

  function addOption(select, value, label, disabled = false) {
    const option = element("option", "", label);
    option.value = String(value);
    option.disabled = disabled;
    select.append(option);
  }

  function populateAssignees(select, value = "", existingName = "") {
    select.replaceChildren();
    addOption(select, "", t("Non assegnato"));
    for (const user of state.users.filter((item) => item.active)) addOption(select, user.id, user.name);
    if (value !== "" && value !== null && !state.users.some((item) => item.active && String(item.id) === String(value))) {
      const user = state.users.find((item) => String(item.id) === String(value));
      addOption(select, value, t("{0} (disattivato)", [user?.name || existingName || t("Utente")]), true);
    }
    select.value = value === null ? "" : String(value);
  }

  async function loadUsers() {
    const generation = state.generation;
    try {
      const data = await api("/api/users");
      if (generation !== state.generation || !state.user) return;
      state.users = data.items || [];
      const filter = $("assignee-filter");
      const selected = filter.value;
      filter.replaceChildren();
      addOption(filter, "", t("Tutti gli assegnatari"));
      addOption(filter, "unassigned", t("Non assegnati"));
      for (const user of state.users) addOption(filter, user.id, user.active ? user.name : t("{0} (disattivato)", [user.name]));
      if (selected && !Array.from(filter.options).some((option) => option.value === selected)) addOption(filter, selected, t("Utente #{0} (non disponibile)", [selected]));
      filter.value = selected;
      renderUsers();
    } catch (error) {
      if (state.user && generation === state.generation) {
        showError("users-error", error.message);
        toast(t("Non è stato possibile caricare il team. Riprova con Aggiorna."), true);
      }
      throw error;
    }
  }

  function filtersActive() {
    return Boolean(state.resourceFilters.job || state.resourceFilters.asset || state.queue !== "all" || $("search-input").value.trim() || $("status-filter").value || $("priority-filter").value || $("assignee-filter").value || $("sort-filter").value !== "updated");
  }

  function syncFilterDisplay() {
    $("reset-filters").hidden = !filtersActive();
    $("list-heading").textContent = queues[state.queue];
    $("queue-description").textContent = queueDescriptions[state.queue];
    $("assignee-filter").disabled = ["mine", "unassigned"].includes(state.queue);
    for (const button of document.querySelectorAll(".queue-button")) button.setAttribute("aria-pressed", String(button.dataset.queue === state.queue));
    $("view-board").setAttribute("aria-pressed", String(state.view === "board"));
    $("view-list").setAttribute("aria-pressed", String(state.view === "list"));
    $("board-hint").hidden = state.view !== "board";
    for (const button of document.querySelectorAll(".stat-card")) button.setAttribute("aria-pressed", String(button.dataset.status === $("status-filter").value));
  }

  function ticketParams() {
    const params = new URLSearchParams({ queue: state.queue, sort: $("sort-filter").value, today: todayInRome() });
    for (const [id, key] of [["search-input", "search"], ["status-filter", "status"], ["priority-filter", "priority"], ["assignee-filter", "assignee_id"]]) {
      const value = $(id).value.trim();
      if (value) params.set(key, value);
    }
    for (const kind of ["job", "asset"]) if (state.resourceFilters[kind]) params.set(kind + "_id", String(state.resourceFilters[kind].id));
    return params;
  }

  function updateCounts(data) {
    for (const status of Object.keys(statuses)) $("stat-" + status).textContent = (Number(data.stats?.[status]) || 0).toLocaleString(locale);
    for (const queue of Object.keys(queues)) {
      const total = queue === "all" ? Object.keys(statuses).reduce((sum, key) => sum + (Number(data.stats?.[key]) || 0), 0) : Number(data.queues?.[queue]) || 0;
      $("queue-count-" + queue).textContent = total.toLocaleString(locale);
    }
  }

  async function loadTickets() {
    for (const controller of state.listControllers) controller.abort();
    state.listControllers = [];
    const cycle = ++state.listCycle;
    const generation = state.generation;
    const view = state.view;
    syncFilterDisplay();
    savePreferences();
    const params = ticketParams();
    $("list-loading").hidden = false;
    $("ticket-list-section").setAttribute("aria-busy", "true");
    $("refresh-tickets").disabled = true;
    for (const button of $("ticket-board").querySelectorAll(".load-column")) button.disabled = true;
    showError("list-error", "");
    const request = async (query) => {
      const controller = new AbortController();
      state.listControllers.push(controller);
      return api(`/api/tickets?${query}`, { signal: controller.signal });
    };
    try {
      if (view === "board") {
        const selected = $("status-filter").value;
        const shown = selected ? [selected] : Object.keys(statuses).filter((status) => state.queue === "all" || !["resolved", "closed"].includes(status));
        const columns = await Promise.all(shown.map(async (status) => {
          const query = new URLSearchParams(params);
          query.set("status", status); query.set("limit", "20"); query.set("offset", "0");
          const data = await request(query);
          return { status, items: data.items || [], total: Number(data.total) || 0, offset: (data.items || []).length, loading: false, error: "", data };
        }));
        if (cycle !== state.listCycle || generation !== state.generation) return;
        state.board = new Map(columns.map((column) => [column.status, column]));
        state.boardCycle = cycle;
        state.boardParams = params.toString();
        state.total = columns.reduce((sum, column) => sum + column.total, 0);
        if (columns[0]) updateCounts(columns[0].data);
        renderBoard();
      } else {
        params.set("limit", String(state.limit)); params.set("offset", String(state.offset));
        const data = await request(params);
        if (cycle !== state.listCycle || generation !== state.generation) return;
        state.total = Number(data.total) || 0;
        if (state.offset > 0 && state.offset >= state.total) {
          state.offset = Math.max(0, Math.floor((state.total - 1) / state.limit) * state.limit);
          return loadTickets();
        }
        state.items = data.items || [];
        updateCounts(data);
        renderTickets(state.items);
      }
      $("ticket-total").textContent = state.total.toLocaleString(locale);
      $("list-announcement").textContent = state.total === 1 ? t("1 ticket trovato.") : t("{0} ticket trovati.", [state.total]);
    } catch (error) {
      if (error.name !== "AbortError" && cycle === state.listCycle && generation === state.generation && state.user) {
        if (state.punto.configured) {
          state.board.clear(); state.items = []; $("ticket-board").replaceChildren(); $("ticket-rows").replaceChildren();
          $("ticket-total").textContent = "—"; $("pagination").hidden = true;
          for (const status of Object.keys(statuses)) $("stat-" + status).textContent = "—";
          for (const queue of Object.keys(queues)) $("queue-count-" + queue).textContent = "—";
        }
        showError("list-error", error.message + t(" Usa Aggiorna per riprovare."));
        $("list-announcement").textContent = t("Caricamento non riuscito.");
      }
    } finally {
      if (cycle === state.listCycle) {
        $("list-loading").hidden = true;
        $("ticket-list-section").removeAttribute("aria-busy");
        $("refresh-tickets").disabled = false;
        for (const button of $("ticket-board").querySelectorAll(".load-column")) button.disabled = state.boardCycle !== cycle;
      }
    }
  }

  function boardCard(ticket) {
    const card = element("article", `board-card card-priority-${ticket.priority}`);
    card.dataset.ticketId = String(ticket.id);
    const top = element("div", "board-card-top");
    top.append(element("span", "ticket-id", `#${ticket.id}`), element("span", `board-priority priority-${ticket.priority}`, priorities[ticket.priority] || ticket.priority));
    card.append(top, ticketLink(ticket, "board-ticket-title"));
    if (ticket.requester) card.append(element("p", "board-requester", ticket.requester));
    card.append(resourceBadges(ticket));
    const footer = element("div", "board-card-footer");
    const person = element("span", "board-person", ticket.assignee_name || t("Non assegnato"));
    if (ticket.assignee_name) {
      const avatar = element("span", "avatar", initials(ticket.assignee_name));
      avatar.setAttribute("aria-hidden", "true");
      person.prepend(avatar);
    }
    footer.append(person, deadlineNode(ticket));
    card.append(footer);
    return card;
  }

  function columnNode(column) {
    const section = element("section", `board-column column-${column.status}`);
    section.id = `column-${column.status}`;
    const heading = element("div", "board-column-heading");
    const title = element("h3", "", statuses[column.status]);
    title.id = `column-title-${column.status}`;
    section.setAttribute("aria-labelledby", title.id);
    heading.append(title, element("span", "count-pill", column.total.toLocaleString(locale)));
    section.append(heading);
    const content = element("div", "board-column-cards");
    for (const ticket of column.items) content.append(boardCard(ticket));
    if (!column.items.length) content.append(element("p", "column-empty", t("Nessun ticket in questa colonna.")));
    section.append(content);
    const footer = element("div", "board-column-footer");
    footer.append(element("span", "column-progress", `${column.items.length} di ${column.total}`));
    if (column.error) {
      const error = element("p", "column-error", column.error);
      error.setAttribute("role", "alert"); footer.append(error);
    }
    if (column.offset < column.total) {
      const button = element("button", "button button-secondary load-column", column.loading ? t("Caricamento…") : column.error ? t("Riprova") : t("Mostra altri"));
      button.type = "button"; button.disabled = column.loading || state.boardCycle !== state.listCycle;
      button.setAttribute("aria-label", t("Mostra altri ticket: {0}", [statuses[column.status]]));
      button.addEventListener("click", () => loadMore(column.status));
      footer.append(button);
    }
    section.append(footer);
    return section;
  }

  function renderBoard() {
    const fragment = document.createDocumentFragment();
    for (const column of state.board.values()) fragment.append(columnNode(column));
    $("ticket-board").replaceChildren(fragment);
    $("ticket-board").style.setProperty("--columns", String(state.board.size));
    $("board-container").hidden = false;
    $("board-hint").hidden = false;
    $("ticket-table-container").hidden = true;
    $("pagination").hidden = true;
    $("list-empty").hidden = true;
  }

  function replaceColumn(column, restoreFocus = false, firstNewId = null) {
    const previous = $("column-" + column.status);
    if (!previous) return;
    const scroll = previous.querySelector(".board-column-cards").scrollTop;
    const node = columnNode(column);
    previous.replaceWith(node);
    node.querySelector(".board-column-cards").scrollTop = scroll;
    if (restoreFocus) {
      const target = node.querySelector(".load-column:not(:disabled)") || (firstNewId && node.querySelector(`[data-ticket-id="${firstNewId}"] .board-ticket-title`)) || node.querySelector("h3");
      if (target?.tagName === "H3") target.tabIndex = -1;
      target?.focus();
    }
  }

  async function loadMore(status) {
    const column = state.board.get(status);
    if (!column || column.loading || column.offset >= column.total || state.boardCycle !== state.listCycle) return;
    const cycle = state.listCycle, generation = state.generation;
    const query = new URLSearchParams(state.boardParams);
    query.set("status", status); query.set("limit", "20"); query.set("offset", String(column.offset));
    const controller = new AbortController();
    state.listControllers.push(controller);
    const restoreFocus = $("column-" + status)?.querySelector(".load-column") === document.activeElement;
    let firstNewId = null;
    column.loading = true; column.error = "";
    replaceColumn(column);
    try {
      const data = await api(`/api/tickets?${query}`, { signal: controller.signal });
      if (cycle !== state.listCycle || generation !== state.generation || state.board.get(status) !== column) return;
      const existing = new Set(column.items.map((ticket) => ticket.id));
      const additional = (data.items || []).filter((ticket) => !existing.has(ticket.id));
      firstNewId = additional[0]?.id || null;
      column.items.push(...additional);
      column.offset += (data.items || []).length;
      column.total = Number(data.total) || 0;
      if (!data.items?.length) column.offset = column.total;
      state.total = Array.from(state.board.values()).reduce((sum, item) => sum + item.total, 0);
      $("ticket-total").textContent = state.total.toLocaleString(locale);
      updateCounts(data);
      $("list-announcement").textContent = t("{0} ticket visualizzati nella colonna {1}.", [column.items.length, statuses[status]]);
    } catch (error) {
      if (error.name !== "AbortError" && cycle === state.listCycle && generation === state.generation) column.error = error.message;
    } finally {
      column.loading = false;
      if (cycle === state.listCycle && generation === state.generation && state.board.get(status) === column) replaceColumn(column, restoreFocus, firstNewId);
    }
  }

  function renderTickets(items) {
    const rows = document.createDocumentFragment();
    for (const ticket of items) {
      const row = element("tr");
      row.append(element("td", "ticket-id-cell ticket-id", `#${ticket.id}`));
      const titleCell = element("td", "ticket-title-cell");
      const title = ticketLink(ticket, "ticket-title-button");
      titleCell.append(title, element("span", "ticket-requester", ticket.requester || t("Nessun richiedente indicato")));
      titleCell.append(resourceBadges(ticket));
      const mobileMeta = element("div", "ticket-mobile-meta");
      mobileMeta.append(element("span", "ticket-id", `#${ticket.id}`), deadlineNode(ticket), element("span", "mobile-assignee", ticket.assignee_name || t("Non assegnato")));
      titleCell.append(mobileMeta);
      row.append(titleCell);
      const statusCell = element("td");
      statusCell.append(element("span", `badge status-${ticket.status}`, statuses[ticket.status] || ticket.status));
      row.append(statusCell);
      const priorityCell = element("td");
      const priority = element("span", `badge priority-badge priority-${ticket.priority}`);
      const priorityMark = element("span", "priority-mark", ticket.priority === "urgent" ? "↑↑" : ticket.priority === "high" ? "↑" : ticket.priority === "low" ? "↓" : "−");
      priorityMark.setAttribute("aria-hidden", "true");
      priority.append(priorityMark, document.createTextNode(priorities[ticket.priority] || ticket.priority));
      priorityCell.append(priority);
      row.append(priorityCell);
      const assignee = element("td", "assignee-cell");
      if (ticket.assignee_name) {
        const person = element("span", "assignee-name");
        const avatar = element("span", "avatar", initials(ticket.assignee_name));
        avatar.setAttribute("aria-hidden", "true");
        person.append(avatar, document.createTextNode(ticket.assignee_name));
        assignee.append(person);
      } else assignee.append(element("span", "unassigned", t("Non assegnato")));
      row.append(assignee);
      const due = element("td", "due-cell");
      due.append(deadlineNode(ticket));
      row.append(due);
      const updated = element("td", "updated-cell");
      const updatedTime = element("time", "updated-time", time(ticket.updated_at, true));
      updatedTime.dateTime = ticket.updated_at;
      updatedTime.title = time(ticket.updated_at);
      updated.append(updatedTime);
      row.append(updated);
      rows.append(row);
    }
    $("ticket-rows").replaceChildren(rows);
    $("board-container").hidden = true;
    $("board-hint").hidden = true;
    $("ticket-table-container").hidden = items.length === 0;
    $("list-empty").hidden = items.length !== 0;
    const filtered = filtersActive();
    $("empty-heading").textContent = filtered ? t("Nessun ticket corrisponde ai filtri") : t("Nessun ticket, per ora");
    $("empty-description").textContent = filtered ? t("Prova un’altra ricerca o reimposta i filtri.") : t("Crea il primo ticket e dai il via al lavoro.");
    $("empty-action").textContent = filtered ? t("Reimposta i filtri") : t("Crea un ticket");
    $("pagination").hidden = items.length === 0;
    $("page-description").textContent = t("{0}–{1} di {2} ticket", [state.offset + 1, Math.min(state.offset + items.length, state.total), state.total.toLocaleString(locale)]);
    $("previous-page").disabled = state.offset === 0;
    $("next-page").disabled = state.offset + state.limit >= state.total;
  }

  function resetFilters() {
    state.resourceRouteVersion += 1;
    clearTimeout(state.searchTimer);
    $("filter-form").reset();
    state.queue = "all";
    state.resourceFilters = { job: null, asset: null };
    const url = new URL(location.href); url.searchParams.delete("job_id"); url.searchParams.delete("asset_id"); history.replaceState(null, "", url);
    renderResourceChoices();
    state.offset = 0;
    loadTickets();
  }

  async function openDetail(id, syncRoute = true) {
    if (syncRoute && ticketRoute() !== id) history.pushState(null, "", ticketURL(id));
    state.detailId = id;
    state.detail = null;
    $("comment-form").reset();
    showError("comment-error", "");
    showError("properties-error", "");
    $("detail-title").textContent = t("Apertura del ticket…");
    $("detail-number").textContent = t("TICKET #{0}", [id]);
    $("detail-tools").hidden = true;
    if (!$("ticket-detail").open) $("ticket-detail").showModal();
    await loadDetail(id);
  }

  async function loadDetail(id) {
    state.detailRequest?.abort();
    const controller = new AbortController();
    state.detailRequest = controller;
    $("detail-loading").hidden = false;
    $("detail-content").hidden = true;
    $("detail-error-area").hidden = true;
    $("detail-connect-punto").hidden = true;
    try {
      const data = await api(`/api/tickets/${id}`, { signal: controller.signal });
      if (controller.signal.aborted || state.detailId !== id || !$("ticket-detail").open) return null;
      state.detail = data;
      renderDetail(data);
      return data;
    } catch (error) {
      if (error.name !== "AbortError" && state.user && state.detailId === id && $("ticket-detail").open) {
        const canConnect = state.punto.configured && [403, 404].includes(error.status);
        $("detail-load-error").textContent = error.message + (canConnect ? " " + t("Se il ticket è associato a Punto, collega la tua sessione per verificare l’accesso.") : "");
        $("detail-connect-punto").hidden = !canConnect;
        $("detail-error-area").hidden = false;
      }
      return null;
    } finally {
      if (state.detailRequest === controller) $("detail-loading").hidden = true;
    }
  }

  function renderDetail(data) {
    const ticket = data.ticket;
    $("detail-title").textContent = ticket.title;
    $("detail-number").textContent = t("TICKET #{0}", [ticket.id]);
    $("detail-description").textContent = ticket.description || t("Non è stata aggiunta una descrizione.");
    $("detail-description").classList.toggle("muted", !ticket.description);
    $("detail-requester").textContent = ticket.requester || t("Non indicato");
    $("detail-creator").textContent = ticket.creator_name || "—";
    $("detail-created").textContent = time(ticket.created_at);
    $("detail-updated").textContent = time(ticket.updated_at);
    $("detail-resources-section").hidden = !ticket.job_id && !ticket.asset_id;
    $("detail-resources").replaceChildren(resourceBadges(ticket, true));
    $("detail-status").value = ticket.status;
    $("detail-priority").value = ticket.priority;
    $("detail-due-date").value = ticket.due_date || "";
    $("detail-tools").hidden = false;
    $("take-ticket").hidden = ["resolved", "closed"].includes(ticket.status);
    $("take-ticket").disabled = ticket.assignee_id === state.user.id && ticket.status === "in_progress";
    $("take-ticket").textContent = $("take-ticket").disabled ? t("In carico a te") : t("Prendi in carico");
    populateAssignees($("detail-assignee"), ticket.assignee_id, ticket.assignee_name);
    const comments = data.comments || [];
    $("comment-count").textContent = comments.length;
    const commentNodes = document.createDocumentFragment();
    if (!comments.length) commentNodes.append(element("p", "comments-empty", t("La conversazione inizia qui. Aggiungi il primo aggiornamento.")));
    for (const comment of comments) {
      const item = element("article", "comment");
      const avatar = element("span", "avatar", initials(comment.author_name));
      avatar.setAttribute("aria-hidden", "true");
      const content = element("div", "comment-content");
      const header = element("div", "comment-header");
      const date = element("time", "comment-time", time(comment.created_at));
      date.dateTime = comment.created_at;
      header.append(element("span", "comment-author", comment.author_name), date);
      content.append(header, element("p", "comment-body", comment.body));
      item.append(avatar, content);
      commentNodes.append(item);
    }
    $("comments-list").replaceChildren(commentNodes);
    const history = document.createDocumentFragment();
    for (const event of [...(data.history || [])].sort((a, b) => String(b.created_at).localeCompare(String(a.created_at)) || b.id - a.id)) {
      const item = element("li", "history-item");
      const description = element("p", "history-text");
      description.append(element("strong", "", event.actor_name || t("Sistema")), document.createTextNode(" " + (historyActions[event.action] || event.action || t("ha aggiornato il ticket"))));
      item.append(description);
      if (event.detail) item.append(element("p", "history-detail", typeof event.detail === "string" ? event.detail : JSON.stringify(event.detail)));
      const date = element("time", "history-time", time(event.created_at));
      date.dateTime = event.created_at;
      item.append(date);
      history.append(item);
    }
    if (!data.history?.length) history.append(element("li", "comments-empty", t("Nessuna attività registrata.")));
    $("history-list").replaceChildren(history);
    $("detail-content").hidden = false;
  }

  function populateEditor(ticket = null) {
    state.editorTicket = ticket ? { ...ticket } : null;
    state.resources = ticket ? { job: ticketResource(ticket, "job"), asset: ticketResource(ticket, "asset") } : { ...state.resourceFilters };
    $("ticket-form").reset();
    $("editor-title").textContent = ticket ? t("Modifica ticket") : t("Nuovo ticket");
    $("editor-kicker").textContent = ticket ? t("TICKET #{0}", [ticket.id]) : t("UNA NUOVA RICHIESTA");
    $("save-ticket").textContent = ticket ? t("Salva modifiche") : t("Crea ticket");
    $("ticket-status-field").hidden = !ticket;
    $("ticket-title").value = ticket?.title || "";
    $("ticket-description").value = ticket?.description || "";
    $("ticket-requester").value = ticket?.requester || "";
    $("ticket-priority").value = ticket?.priority || "normal";
    $("ticket-status").value = ticket?.status || "open";
    $("ticket-due-date").value = ticket?.due_date || "";
    populateAssignees($("ticket-assignee"), ticket?.assignee_id ?? "", ticket?.assignee_name);
    showError("editor-error", "");
    $("reload-editor").hidden = true;
    renderResourceChoices();
  }

  function openEditor(ticket = null) {
    populateEditor(ticket);
    $("ticket-editor").showModal();
    $("ticket-title").focus();
  }

  function changes(values, original) {
    const result = {};
    for (const [key, value] of Object.entries(values)) {
      const previous = original[key] === undefined || original[key] === null ? (typeof value === "string" ? "" : null) : original[key];
      if (value !== previous) result[key] = value;
    }
    return result;
  }

  function editorValues() {
    return {
      title: $("ticket-title").value.trim(), description: $("ticket-description").value.trim(), requester: $("ticket-requester").value.trim(),
      priority: $("ticket-priority").value, assignee_id: $("ticket-assignee").value ? Number($("ticket-assignee").value) : null,
      due_date: $("ticket-due-date").value || null,
      job_id: state.resources.job?.id || null, asset_id: state.resources.asset?.id || null
    };
  }

  async function saveTicket(event) {
    event.preventDefault();
    const form = $("ticket-form");
    const original = state.editorTicket;
    const values = editorValues();
    if (!values.title) { showError("editor-error", t("Aggiungi un titolo per il ticket.")); $("ticket-title").focus(); return; }
    let body = values;
    if (original) {
      values.status = $("ticket-status").value;
      body = changes(values, original);
      if (!Object.keys(body).length) { $("ticket-editor").close(); return; }
      body.revision = original.revision;
    }
    showError("editor-error", "");
    busy(form, $("save-ticket"), true, t("Salvataggio…"));
    try {
      const data = await api(original ? `/api/tickets/${original.id}` : "/api/tickets", { method: original ? "PUT" : "POST", body });
      $("ticket-editor").close();
      toast(original ? t("Ticket aggiornato.") : t("Ticket #{0} creato.", [data.ticket.id]));
      await loadTickets();
      if (original && $("ticket-detail").open) await loadDetail(original.id);
      else if (!original) await openDetail(data.ticket.id);
    } catch (error) {
      if (state.user) {
        showError("editor-error", error.status === 409 ? t("Un’altra persona ha aggiornato il ticket. Ricarica l’ultima versione prima di modificarlo; il contenuto di questo modulo sarà sostituito.") : error.message);
        $("reload-editor").hidden = error.status !== 409;
      }
    } finally { busy(form, $("save-ticket"), false); }
  }

  async function saveProperties(event) {
    event.preventDefault();
    if (!state.detail) return;
    const ticket = state.detail.ticket;
    const body = changes({ status: $("detail-status").value, priority: $("detail-priority").value, assignee_id: $("detail-assignee").value ? Number($("detail-assignee").value) : null, due_date: $("detail-due-date").value || null }, ticket);
    if (!Object.keys(body).length) { toast(t("Non ci sono modifiche da salvare.")); return; }
    body.revision = ticket.revision;
    showError("properties-error", "");
    busy($("detail-properties-form"), $("save-properties"), true, t("Salvataggio…"));
    try {
      await api(`/api/tickets/${ticket.id}`, { method: "PUT", body });
      window.PuntoI18n?.markSaved($("detail-properties-form"));
      await loadDetail(ticket.id);
      loadTickets();
      toast(t("Ticket aggiornato."));
    } catch (error) {
      if (state.user) {
        if (error.status === 409) await loadDetail(ticket.id);
        showError("properties-error", error.status === 409 ? t("Il ticket era stato aggiornato da un’altra persona. Abbiamo caricato l’ultima versione: verifica e ripeti le modifiche.") : error.message);
      }
    } finally { busy($("detail-properties-form"), $("save-properties"), false); }
  }

  async function sendComment(event) {
    event.preventDefault();
    if (!state.detail) return;
    const id = state.detail.ticket.id;
    const revision = state.detail.ticket.revision;
    const draftProperties = { status: $("detail-status").value, priority: $("detail-priority").value, assignee_id: $("detail-assignee").value ? Number($("detail-assignee").value) : null, due_date: $("detail-due-date").value || null };
    const hasDraftProperties = Object.keys(changes(draftProperties, state.detail.ticket)).length > 0;
    const body = $("comment-body").value.trim();
    if (!body) { showError("comment-error", t("Scrivi un commento prima di inviarlo.")); $("comment-body").focus(); return; }
    showError("comment-error", "");
    busy($("comment-form"), $("send-comment"), true, t("Invio…"));
    try {
      await api(`/api/tickets/${id}/comments`, { method: "POST", body: { body } });
      $("comment-body").value = "";
      const refreshed = await loadDetail(id);
      if (hasDraftProperties && refreshed) {
        if (refreshed.ticket.revision === revision) {
          $("detail-status").value = draftProperties.status;
          $("detail-priority").value = draftProperties.priority;
          $("detail-assignee").value = draftProperties.assignee_id === null ? "" : String(draftProperties.assignee_id);
          $("detail-due-date").value = draftProperties.due_date || "";
        } else showError("properties-error", t("Le proprietà sono state aggiornate da un’altra persona. Verifica l’ultima versione e ripeti le modifiche non salvate."));
      }
      loadTickets();
      toast(t("Commento aggiunto."));
    } catch (error) { if (state.user) showError("comment-error", error.message); }
    finally { busy($("comment-form"), $("send-comment"), false); }
  }

  async function refreshCurrentDetail() {
    if (!state.detailId) return;
    const previous = state.detail?.ticket;
    const draft = previous ? { status: $("detail-status").value, priority: $("detail-priority").value, assignee_id: $("detail-assignee").value ? Number($("detail-assignee").value) : null, due_date: $("detail-due-date").value || null } : null;
    const dirty = previous && Object.keys(changes(draft, previous)).length > 0;
    $("refresh-detail").disabled = true;
    showError("properties-error", "");
    try {
      const data = await loadDetail(state.detailId);
      if (data && dirty) {
        if (data.ticket.revision === previous.revision) {
          $("detail-status").value = draft.status;
          $("detail-priority").value = draft.priority;
          $("detail-assignee").value = draft.assignee_id === null ? "" : String(draft.assignee_id);
          $("detail-due-date").value = draft.due_date || "";
        } else showError("properties-error", t("Il ticket è cambiato. Abbiamo caricato l’ultima versione: verifica e ripeti le modifiche non salvate."));
      }
      if (data) toast(t("Ticket aggiornato all’ultima versione."));
    } finally { $("refresh-detail").disabled = false; }
  }

  async function takeTicket() {
    if (!state.detail || !state.user) return;
    const ticket = state.detail.ticket, generation = state.generation;
    const draft = { status: $("detail-status").value, priority: $("detail-priority").value, assignee_id: $("detail-assignee").value ? Number($("detail-assignee").value) : null, due_date: $("detail-due-date").value || null };
    if (Object.keys(changes(draft, ticket)).length) {
      showError("properties-error", t("Hai modifiche non salvate alle proprietà. Salvale prima di prendere in carico il ticket."));
      $("save-properties").focus();
      return;
    }
    const button = $("take-ticket");
    button.disabled = true; button.setAttribute("aria-busy", "true");
    showError("properties-error", "");
    try {
      await api(`/api/tickets/${ticket.id}`, { method: "PUT", body: { assignee_id: state.user.id, status: "in_progress", revision: ticket.revision } });
      if (generation !== state.generation) return;
      if (state.detailId === ticket.id) await loadDetail(ticket.id);
      loadTickets();
      toast(t("Ticket preso in carico. Buon lavoro!"));
    } catch (error) {
      if (generation !== state.generation || !state.user) return;
      if (error.status === 409) await loadDetail(ticket.id);
      showError("properties-error", error.status === 409 ? t("Un’altra persona ha aggiornato il ticket. Verifica l’ultima versione prima di prenderlo in carico.") : error.message);
    } finally {
      button.removeAttribute("aria-busy");
      if (state.detail?.ticket.id === ticket.id) button.disabled = state.detail.ticket.assignee_id === state.user?.id && state.detail.ticket.status === "in_progress";
      else button.disabled = false;
    }
  }

  function renderUsers() {
    if (state.auth?.managed_users === true) { $("users-rows").replaceChildren(); return; }
    const rows = document.createDocumentFragment();
    for (const user of state.users) {
      const row = element("tr");
      const personCell = element("td");
      const person = element("div", "user-person");
      const avatar = element("span", "avatar", initials(user.name));
      avatar.setAttribute("aria-hidden", "true");
      const info = element("div");
      info.append(element("strong", "", user.name), element("small", "", user.email));
      person.append(avatar, info);
      personCell.append(person);
      row.append(personCell, element("td", "", user.role === "admin" ? t("Amministratore") : t("Operatore")));
      const status = element("td");
      status.append(element("span", `user-status${user.active ? "" : " inactive"}`, user.active ? t("Attivo") : t("Disattivato")));
      row.append(status);
      const action = element("td");
      const edit = element("button", "button button-secondary", t("Modifica"));
      edit.type = "button";
      edit.setAttribute("aria-label", t("Modifica l’utente {0}", [user.name]));
      edit.addEventListener("click", () => openUserEditor(user));
      action.append(edit);
      row.append(action);
      rows.append(row);
    }
    $("users-rows").replaceChildren(rows);
  }

  function openUserEditor(user = null) {
    if (state.auth?.managed_users === true) return;
    state.editorUser = user ? { ...user } : null;
    $("user-form").reset();
    $("user-editor-title").textContent = user ? t("Modifica utente") : t("Nuovo utente");
    $("save-user").textContent = user ? t("Salva modifiche") : t("Crea utente");
    $("user-name").value = user?.name || "";
    $("user-email").value = user?.email || "";
    $("user-email").readOnly = Boolean(user);
    $("user-email-hint").hidden = !user;
    $("user-role").value = user?.role || "agent";
    $("user-active-field").hidden = !user;
    $("user-active").value = user ? String(Boolean(user.active)) : "true";
    $("user-password").required = !user;
    $("user-password-label").textContent = user ? t("Reimposta password") : t("Password iniziale");
    $("user-password-hint").textContent = user ? t("Lascia vuoto per mantenere la password attuale, oppure inserisci almeno 12 caratteri.") : t("Almeno 12 caratteri.");
    showError("user-editor-error", "");
    $("user-editor").showModal();
    $("user-name").focus();
  }

  async function saveUser(event) {
    event.preventDefault();
    if (state.auth?.managed_users === true) return;
    const original = state.editorUser;
    const body = { name: $("user-name").value.trim(), role: $("user-role").value };
    if (!body.name) { showError("user-editor-error", t("Inserisci il nome dell’utente.")); $("user-name").focus(); return; }
    if (original) body.active = $("user-active").value === "true";
    else body.email = $("user-email").value.trim();
    if ($("user-password").value) body.password = $("user-password").value;
    showError("user-editor-error", "");
    busy($("user-form"), $("save-user"), true, t("Salvataggio…"));
    try {
      await api(original ? `/api/users/${original.id}` : "/api/users", { method: original ? "PUT" : "POST", body });
      $("user-editor").close();
      $("user-password").value = "";
      if (original && original.id === state.user.id) updateSession(await api("/api/session"));
      await loadUsers();
      await loadTickets();
      if ($("ticket-detail").open && state.detailId) await loadDetail(state.detailId);
      toast(original ? t("Utente aggiornato.") : t("Utente creato."));
    } catch (error) {
      if (state.user) {
        if ($("user-editor").open) showError("user-editor-error", error.message);
        else toast(error.message, true);
      }
    } finally { busy($("user-form"), $("save-user"), false); }
  }

  for (const button of document.querySelectorAll("[data-close-dialog]")) button.addEventListener("click", () => closeDialog($(button.dataset.closeDialog)));
  for (const dialog of document.querySelectorAll("dialog")) dialog.addEventListener("cancel", (event) => { if (dialog.querySelector('[aria-busy="true"]')) event.preventDefault(); });
  $("ticket-detail").addEventListener("close", () => {
    state.detailRequest?.abort(); state.detailId = null; state.detail = null;
    if (state.suppressDetailClose) { state.suppressDetailClose = false; return; }
    if (state.user && !state.routeSync && ticketRoute()) {
      const url = new URL(location.href); url.searchParams.delete("ticket"); history.replaceState(null, "", url);
    }
  });
  $("user-editor").addEventListener("close", () => { $("user-password").value = ""; });
  $("login-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (state.auth?.local !== true) return;
    showError("login-error", "");
    $("login-notice").hidden = true;
    const body = { email: $("login-email").value.trim(), password: $("login-password").value };
    const button = $("login-form").querySelector('[type="submit"]');
    busy($("login-form"), button, true, t("Accesso…"));
    try {
      const data = await api("/api/login", { method: "POST", body });
      $("login-password").value = "";
      await openApp(data);
    } catch (error) { showError("login-error", error.message); }
    finally { busy($("login-form"), button, false); }
  });
  $("logout-button").addEventListener("click", async () => {
    $("logout-button").disabled = true;
    try { await api("/api/logout", { method: "POST", body: {} }); showLogin(); document.title = t("Ticket. · Gestione richieste"); (state.auth?.local ? $("login-email") : $("sso-login").querySelector("a")).focus(); }
    catch (error) { if (state.user) toast(error.message, true); }
    finally { $("logout-button").disabled = false; }
  });
  $("filter-form").addEventListener("submit", (event) => { event.preventDefault(); clearTimeout(state.searchTimer); state.offset = 0; loadTickets(); });
  $("search-input").addEventListener("input", () => {
    clearTimeout(state.searchTimer); state.offset = 0;
    state.listCycle += 1;
    for (const controller of state.listControllers) controller.abort();
    for (const button of $("ticket-board").querySelectorAll(".load-column")) button.disabled = true;
    $("previous-page").disabled = true; $("next-page").disabled = true;
    $("ticket-list-section").setAttribute("aria-busy", "true"); $("list-loading").hidden = false;
    syncFilterDisplay(); state.searchTimer = setTimeout(loadTickets, 300);
  });
  for (const id of ["status-filter", "priority-filter", "assignee-filter", "sort-filter"]) $(id).addEventListener("change", () => {
    clearTimeout(state.searchTimer);
    if (["resolved", "closed"].includes($("status-filter").value)) state.queue = "all";
    state.offset = 0; loadTickets();
  });
  for (const card of document.querySelectorAll(".stat-card")) card.addEventListener("click", () => { clearTimeout(state.searchTimer); $("status-filter").value = $("status-filter").value === card.dataset.status ? "" : card.dataset.status; state.queue = "all"; state.offset = 0; loadTickets(); });
  for (const button of document.querySelectorAll(".queue-button")) button.addEventListener("click", () => {
    clearTimeout(state.searchTimer); state.queue = button.dataset.queue; state.offset = 0;
    if (["resolved", "closed"].includes($("status-filter").value)) $("status-filter").value = "";
    if (["mine", "unassigned"].includes(state.queue)) $("assignee-filter").value = "";
    loadTickets();
  });
  for (const view of ["board", "list"]) $("view-" + view).addEventListener("click", () => {
    if (state.view === view) return;
    clearTimeout(state.searchTimer); state.view = view; state.offset = 0; loadTickets();
  });
  $("reset-filters").addEventListener("click", resetFilters);
  $("empty-action").addEventListener("click", () => { if (filtersActive()) resetFilters(); else openEditor(); });
  $("refresh-tickets").addEventListener("click", async () => { clearTimeout(state.searchTimer); await loadPuntoStatus(); Promise.allSettled([loadUsers(), loadTickets()]); });
  $("previous-page").addEventListener("click", () => { state.offset = Math.max(0, state.offset - state.limit); loadTickets(); });
  $("next-page").addEventListener("click", () => { state.offset += state.limit; loadTickets(); });
  $("new-ticket").addEventListener("click", () => { state.resourceRouteVersion += 1; openEditor(); });
  $("connect-punto").addEventListener("click", async () => {
    const generation = state.generation;
    const intentQuery = state.puntoIntentQuery ?? location.search;
    state.puntoIntentQuery = null;
    $("connect-punto").disabled = true; showError("integration-error", "");
    try {
      const data = await api("/api/integrations/punto/start", { method: "POST", body: {} });
      if (generation !== state.generation || !state.user) return;
      const target = new URL(data.url);
      if (target.origin !== new URL(state.punto.origin).origin || !/^#ticket-connect\?state=[a-f0-9]{64}$/.test(target.hash)) throw new Error(t("Il collegamento a Punto non è valido."));
      try { sessionStorage.setItem("ticket.punto.intent", JSON.stringify({ userId: state.user.id, query: intentQuery })); } catch { /* Ripristino facoltativo. */ }
      location.assign(window.PuntoI18n ? window.PuntoI18n.url(target.href) : target.href);
    } catch (error) { if (generation === state.generation && state.user) showError("integration-error", error.message); }
    finally { if (generation === state.generation) $("connect-punto").disabled = false; }
  });
  $("disconnect-punto").addEventListener("click", async () => {
    const generation = state.generation;
    $("disconnect-punto").disabled = true;
    try {
      const punto = await api("/api/integrations/punto/disconnect", { method: "POST", body: {} });
      if (generation !== state.generation || !state.user) return;
      state.punto = punto;
      for (const id of ["ticket-detail", "ticket-editor", "resource-picker"]) $(id).close();
      renderPuntoStatus(); resetFilters(); toast(t("Sessione Punto scollegata."));
    } catch (error) { if (generation === state.generation && state.user) showError("integration-error", error.message); }
    finally { if (generation === state.generation) $("disconnect-punto").disabled = false; }
  });
  for (const kind of ["job", "asset"]) {
    $("choose-" + kind).addEventListener("click", () => openResourcePicker(kind, "editor"));
    $("filter-" + kind).addEventListener("click", () => openResourcePicker(kind, "filter"));
    $("clear-" + kind).addEventListener("click", () => { state.resources[kind] = null; renderResourceChoices(); });
  }
  $("clear-resource-filters").addEventListener("click", () => {
    state.resourceRouteVersion += 1;
    state.resourceFilters = { job: null, asset: null };
    const url = new URL(location.href); url.searchParams.delete("job_id"); url.searchParams.delete("asset_id"); history.replaceState(null, "", url);
    renderResourceChoices(); state.offset = 0; loadTickets();
  });
  $("resource-search-form").addEventListener("submit", (event) => { event.preventDefault(); searchResources(); });
  $("resource-search").addEventListener("input", () => { $("resource-more").disabled = !state.picker || state.picker.query !== $("resource-search").value.trim(); });
  $("resource-more").addEventListener("click", () => searchResources(true));
  $("resource-none").addEventListener("click", () => chooseResource(null));
  $("resource-picker").addEventListener("close", () => { state.pickerRequest += 1; state.picker = null; });
  $("ticket-form").addEventListener("submit", saveTicket);
  $("edit-ticket").addEventListener("click", () => { if (state.detail) { state.resourceRouteVersion += 1; openEditor(state.detail.ticket); } });
  $("retry-detail").addEventListener("click", () => { if (state.detailId) loadDetail(state.detailId); });
  $("detail-connect-punto").addEventListener("click", () => { state.puntoIntentQuery = location.search; $("ticket-detail").close(); $("connect-punto").click(); });
  $("refresh-detail").addEventListener("click", refreshCurrentDetail);
  $("take-ticket").addEventListener("click", takeTicket);
  $("copy-ticket-link").addEventListener("click", async () => {
    if (!state.detailId) return;
    try { await navigator.clipboard.writeText(ticketURL(state.detailId).href); toast(t("Link al ticket copiato.")); }
    catch { toast(t("Il link è nella barra degli indirizzi: puoi copiarlo da lì."), true); }
  });
  window.addEventListener("popstate", () => {
    if (!state.user) return;
    const id = ticketRoute();
    if (id && id !== state.detailId) openDetail(id, false);
    else if (!id && $("ticket-detail").open) {
      state.routeSync = true; $("ticket-detail").close(); state.routeSync = false;
    }
  });
  $("reload-editor").addEventListener("click", async () => {
    if (!state.editorTicket) return;
    $("reload-editor").disabled = true;
    try {
      const data = await api(`/api/tickets/${state.editorTicket.id}`);
      populateEditor(data.ticket);
      if (state.detailId === data.ticket.id) { state.detail = data; renderDetail(data); }
      toast(t("Ultima versione caricata. Puoi ripetere le modifiche."));
    } catch (error) { if (state.user) showError("editor-error", error.message); }
    finally { $("reload-editor").disabled = false; }
  });
  $("detail-properties-form").addEventListener("submit", saveProperties);
  $("comment-form").addEventListener("submit", sendComment);
  $("manage-users").addEventListener("click", () => { if (state.auth?.managed_users === true) return; showError("users-error", ""); $("users-dialog").showModal(); loadUsers().catch(() => {}); });
  $("new-user").addEventListener("click", () => openUserEditor());
  $("user-form").addEventListener("submit", saveUser);

  api("/api/auth/config", { signal: AbortSignal.timeout(10000) }).then((config) => {
    if (typeof config.sso !== "boolean" || typeof config.local !== "boolean" || (!config.sso && !config.local)) throw new ApiError(t("Configurazione di accesso non disponibile."), 0);
    if (config.managed_users === true && config.users_admin_url) {
      const url = new URL(config.users_admin_url);
      if (url.protocol !== "https:" && !(url.protocol === "http:" && ["localhost", "127.0.0.1", "[::1]"].includes(url.hostname))) throw new ApiError(t("Configurazione di accesso non disponibile."), 0);
    }
    state.auth = config;
    return api("/api/session");
  }).then(openApp).catch((error) => {
    showLogin();
    if (error.status !== 401) showError("login-error", state.auth ? error.message : t("Impossibile caricare le modalità di accesso. Ricarica la pagina per riprovare."));
  });
})();
