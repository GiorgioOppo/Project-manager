"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const statuses = { open: "Aperto", in_progress: "In corso", waiting: "In attesa", resolved: "Risolto", closed: "Chiuso" };
  const priorities = { low: "Bassa", normal: "Normale", high: "Alta", urgent: "Urgente" };
  const queues = { all: "Tutti i ticket", active: "Da lavorare", mine: "I miei ticket", unassigned: "Non assegnati", overdue: "Scaduti", due_soon: "In scadenza" };
  const queueDescriptions = { all: "Tutte le richieste, comprese quelle risolte e chiuse.", active: "Ticket aperti, in corso o in attesa.", mine: "Ticket da lavorare assegnati a te.", unassigned: "Ticket da lavorare che aspettano un assegnatario.", overdue: "Ticket da lavorare con una scadenza già passata.", due_soon: "Ticket da lavorare in scadenza da oggi ai prossimi 7 giorni." };
  const historyActions = {
    created: "ha aperto il ticket", create: "ha aperto il ticket", updated: "ha aggiornato il ticket",
    update: "ha aggiornato il ticket", commented: "ha aggiunto un commento", comment: "ha aggiunto un commento",
    status_changed: "ha cambiato lo stato", assigned: "ha aggiornato l’assegnazione"
  };
  const state = {
    user: null, csrf: "", users: [], offset: 0, total: 0, limit: 50,
    listRequest: null, detailRequest: null, detailId: null, detail: null,
    editorTicket: null, editorUser: null, searchTimer: null, toastTimer: null, generation: 0,
    view: "board", queue: "active", listCycle: 0, boardCycle: 0, boardParams: "", listControllers: [], board: new Map(), items: [], routeSync: false, suppressDetailClose: false
  };
  const dateTimeFormat = new Intl.DateTimeFormat("it-IT", { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit", timeZone: "Europe/Rome" });
  const shortDateFormat = new Intl.DateTimeFormat("it-IT", { day: "numeric", month: "short", timeZone: "UTC" });
  const fullDateFormat = new Intl.DateTimeFormat("it-IT", { day: "numeric", month: "long", year: "numeric", timeZone: "UTC" });
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
    return String(name || "?").trim().split(/\s+/).filter(Boolean).slice(0, 2).map((part) => Array.from(part)[0]).join("").toLocaleUpperCase("it-IT");
  }

  function time(value, short = false) {
    const date = new Date(value);
    if (!value || !Number.isFinite(date.getTime())) return "—";
    return short ? new Intl.DateTimeFormat("it-IT", { day: "numeric", month: "short", timeZone: "Europe/Rome" }).format(date) : dateTimeFormat.format(date);
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
    if (days < 0) return { label: `Scaduto · ${calendarDate(ticket.due_date)}`, kind: "overdue" };
    if (days === 0) return { label: "Oggi", kind: "today" };
    if (days === 1) return { label: "Domani", kind: "soon" };
    if (days <= 7) return { label: `Tra ${days} giorni`, kind: "soon" };
    return { label: calendarDate(ticket.due_date), kind: "future" };
  }

  function deadlineNode(ticket) {
    const info = deadline(ticket);
    if (!info) return element("span", "no-deadline", "Nessuna scadenza");
    const node = element("span", `deadline-badge deadline-${info.kind}`, info.label);
    node.title = `Scadenza: ${calendarDate(ticket.due_date, true)}`;
    node.setAttribute("aria-label", `Scadenza: ${info.label}, ${calendarDate(ticket.due_date, true)}`);
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
      if (!Array.from($("assignee-filter").options).some((option) => option.value === saved.assignee)) addOption($("assignee-filter"), saved.assignee, `Utente #${saved.assignee} (non disponibile)`);
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
    link.setAttribute("aria-label", `Apri ticket #${ticket.id}: ${ticket.title}`);
    link.addEventListener("click", (event) => {
      if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey || event.button !== 0) return;
      event.preventDefault();
      openDetail(ticket.id);
    });
    return link;
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
      throw new ApiError("Connessione non disponibile. Controlla la rete e riprova.", 0);
    }
    let data;
    try { data = await response.json(); } catch { data = {}; }
    if (!response.ok) {
      if (response.status === 401 && state.user && generation === state.generation) showLogin("La sessione è scaduta. Accedi di nuovo per continuare.");
      throw new ApiError(data.error || "Non è stato possibile completare l’operazione. Riprova.", response.status);
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
    document.title = "Ticket. · Gestione richieste";
    $("login-password").value = "";
    showError("login-error", "");
    $("login-notice").textContent = notice;
    $("login-notice").hidden = !notice;
    if (notice) $("login-email").focus();
  }

  function updateSession(data) {
    state.user = data.user;
    state.csrf = data.csrf_token;
    $("session-name").textContent = state.user.name;
    $("session-avatar").textContent = initials(state.user.name);
    $("session-role").textContent = state.user.role === "admin" ? "Amministratore" : "Operatore";
    $("manage-users").hidden = state.user.role !== "admin";
    if (state.user.role !== "admin") {
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
    document.title = "Le richieste · Ticket.";
    try { await loadUsers(); } catch { /* L’elenco resta utilizzabile quando il team non è disponibile. */ }
    if (!state.user || generation !== state.generation) return;
    restorePreferences();
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
    addOption(select, "", "Non assegnato");
    for (const user of state.users.filter((item) => item.active)) addOption(select, user.id, user.name);
    if (value !== "" && value !== null && !state.users.some((item) => item.active && String(item.id) === String(value))) {
      const user = state.users.find((item) => String(item.id) === String(value));
      addOption(select, value, `${user?.name || existingName || "Utente"} (disattivato)`, true);
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
      addOption(filter, "", "Tutti gli assegnatari");
      addOption(filter, "unassigned", "Non assegnati");
      for (const user of state.users) addOption(filter, user.id, user.active ? user.name : `${user.name} (disattivato)`);
      if (selected && !Array.from(filter.options).some((option) => option.value === selected)) addOption(filter, selected, `Utente #${selected} (non disponibile)`);
      filter.value = selected;
      renderUsers();
    } catch (error) {
      if (state.user && generation === state.generation) {
        showError("users-error", error.message);
        toast("Non è stato possibile caricare il team. Riprova con Aggiorna.", true);
      }
      throw error;
    }
  }

  function filtersActive() {
    return Boolean(state.queue !== "all" || $("search-input").value.trim() || $("status-filter").value || $("priority-filter").value || $("assignee-filter").value || $("sort-filter").value !== "updated");
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
    return params;
  }

  function updateCounts(data) {
    for (const status of Object.keys(statuses)) $("stat-" + status).textContent = (Number(data.stats?.[status]) || 0).toLocaleString("it-IT");
    for (const queue of Object.keys(queues)) {
      const total = queue === "all" ? Object.keys(statuses).reduce((sum, key) => sum + (Number(data.stats?.[key]) || 0), 0) : Number(data.queues?.[queue]) || 0;
      $("queue-count-" + queue).textContent = total.toLocaleString("it-IT");
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
      $("ticket-total").textContent = state.total.toLocaleString("it-IT");
      $("list-announcement").textContent = state.total === 1 ? "1 ticket trovato." : `${state.total} ticket trovati.`;
    } catch (error) {
      if (error.name !== "AbortError" && cycle === state.listCycle && generation === state.generation && state.user) {
        showError("list-error", error.message + " I risultati precedenti restano visibili. Usa Aggiorna per riprovare.");
        $("list-announcement").textContent = "Caricamento non riuscito.";
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
    const footer = element("div", "board-card-footer");
    const person = element("span", "board-person", ticket.assignee_name || "Non assegnato");
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
    heading.append(title, element("span", "count-pill", column.total.toLocaleString("it-IT")));
    section.append(heading);
    const content = element("div", "board-column-cards");
    for (const ticket of column.items) content.append(boardCard(ticket));
    if (!column.items.length) content.append(element("p", "column-empty", "Nessun ticket in questa colonna."));
    section.append(content);
    const footer = element("div", "board-column-footer");
    footer.append(element("span", "column-progress", `${column.items.length} di ${column.total}`));
    if (column.error) {
      const error = element("p", "column-error", column.error);
      error.setAttribute("role", "alert"); footer.append(error);
    }
    if (column.offset < column.total) {
      const button = element("button", "button button-secondary load-column", column.loading ? "Caricamento…" : column.error ? "Riprova" : "Mostra altri");
      button.type = "button"; button.disabled = column.loading || state.boardCycle !== state.listCycle;
      button.setAttribute("aria-label", `Mostra altri ticket: ${statuses[column.status]}`);
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
      $("ticket-total").textContent = state.total.toLocaleString("it-IT");
      updateCounts(data);
      $("list-announcement").textContent = `${column.items.length} ticket visualizzati nella colonna ${statuses[status]}.`;
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
      titleCell.append(title, element("span", "ticket-requester", ticket.requester || "Nessun richiedente indicato"));
      const mobileMeta = element("div", "ticket-mobile-meta");
      mobileMeta.append(element("span", "ticket-id", `#${ticket.id}`), deadlineNode(ticket), element("span", "mobile-assignee", ticket.assignee_name || "Non assegnato"));
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
      } else assignee.append(element("span", "unassigned", "Non assegnato"));
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
    $("empty-heading").textContent = filtered ? "Nessun ticket corrisponde ai filtri" : "Nessun ticket, per ora";
    $("empty-description").textContent = filtered ? "Prova un’altra ricerca o reimposta i filtri." : "Crea il primo ticket e dai il via al lavoro.";
    $("empty-action").textContent = filtered ? "Reimposta i filtri" : "Crea un ticket";
    $("pagination").hidden = items.length === 0;
    $("page-description").textContent = `${state.offset + 1}–${Math.min(state.offset + items.length, state.total)} di ${state.total.toLocaleString("it-IT")} ticket`;
    $("previous-page").disabled = state.offset === 0;
    $("next-page").disabled = state.offset + state.limit >= state.total;
  }

  function resetFilters() {
    clearTimeout(state.searchTimer);
    $("filter-form").reset();
    state.queue = "all";
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
    $("detail-title").textContent = "Apertura del ticket…";
    $("detail-number").textContent = `TICKET #${id}`;
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
    try {
      const data = await api(`/api/tickets/${id}`, { signal: controller.signal });
      if (controller.signal.aborted || state.detailId !== id || !$("ticket-detail").open) return null;
      state.detail = data;
      renderDetail(data);
      return data;
    } catch (error) {
      if (error.name !== "AbortError" && state.user && state.detailId === id && $("ticket-detail").open) {
        $("detail-load-error").textContent = error.message;
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
    $("detail-number").textContent = `TICKET #${ticket.id}`;
    $("detail-description").textContent = ticket.description || "Non è stata aggiunta una descrizione.";
    $("detail-description").classList.toggle("muted", !ticket.description);
    $("detail-requester").textContent = ticket.requester || "Non indicato";
    $("detail-creator").textContent = ticket.creator_name || "—";
    $("detail-created").textContent = time(ticket.created_at);
    $("detail-updated").textContent = time(ticket.updated_at);
    $("detail-status").value = ticket.status;
    $("detail-priority").value = ticket.priority;
    $("detail-due-date").value = ticket.due_date || "";
    $("detail-tools").hidden = false;
    $("take-ticket").hidden = ["resolved", "closed"].includes(ticket.status);
    $("take-ticket").disabled = ticket.assignee_id === state.user.id && ticket.status === "in_progress";
    $("take-ticket").textContent = $("take-ticket").disabled ? "In carico a te" : "Prendi in carico";
    populateAssignees($("detail-assignee"), ticket.assignee_id, ticket.assignee_name);
    const comments = data.comments || [];
    $("comment-count").textContent = comments.length;
    const commentNodes = document.createDocumentFragment();
    if (!comments.length) commentNodes.append(element("p", "comments-empty", "La conversazione inizia qui. Aggiungi il primo aggiornamento."));
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
      description.append(element("strong", "", event.actor_name || "Sistema"), document.createTextNode(" " + (historyActions[event.action] || event.action || "ha aggiornato il ticket")));
      item.append(description);
      if (event.detail) item.append(element("p", "history-detail", typeof event.detail === "string" ? event.detail : JSON.stringify(event.detail)));
      const date = element("time", "history-time", time(event.created_at));
      date.dateTime = event.created_at;
      item.append(date);
      history.append(item);
    }
    if (!data.history?.length) history.append(element("li", "comments-empty", "Nessuna attività registrata."));
    $("history-list").replaceChildren(history);
    $("detail-content").hidden = false;
  }

  function populateEditor(ticket = null) {
    state.editorTicket = ticket ? { ...ticket } : null;
    $("ticket-form").reset();
    $("editor-title").textContent = ticket ? "Modifica ticket" : "Nuovo ticket";
    $("editor-kicker").textContent = ticket ? `TICKET #${ticket.id}` : "UNA NUOVA RICHIESTA";
    $("save-ticket").textContent = ticket ? "Salva modifiche" : "Crea ticket";
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
      due_date: $("ticket-due-date").value || null
    };
  }

  async function saveTicket(event) {
    event.preventDefault();
    const form = $("ticket-form");
    const original = state.editorTicket;
    const values = editorValues();
    if (!values.title) { showError("editor-error", "Aggiungi un titolo per il ticket."); $("ticket-title").focus(); return; }
    let body = values;
    if (original) {
      values.status = $("ticket-status").value;
      body = changes(values, original);
      if (!Object.keys(body).length) { $("ticket-editor").close(); return; }
      body.revision = original.revision;
    }
    showError("editor-error", "");
    busy(form, $("save-ticket"), true, "Salvataggio…");
    try {
      const data = await api(original ? `/api/tickets/${original.id}` : "/api/tickets", { method: original ? "PUT" : "POST", body });
      $("ticket-editor").close();
      toast(original ? "Ticket aggiornato." : `Ticket #${data.ticket.id} creato.`);
      await loadTickets();
      if (original && $("ticket-detail").open) await loadDetail(original.id);
      else if (!original) await openDetail(data.ticket.id);
    } catch (error) {
      if (state.user) {
        showError("editor-error", error.status === 409 ? "Un’altra persona ha aggiornato il ticket. Ricarica l’ultima versione prima di modificarlo; il contenuto di questo modulo sarà sostituito." : error.message);
        $("reload-editor").hidden = error.status !== 409;
      }
    } finally { busy(form, $("save-ticket"), false); }
  }

  async function saveProperties(event) {
    event.preventDefault();
    if (!state.detail) return;
    const ticket = state.detail.ticket;
    const body = changes({ status: $("detail-status").value, priority: $("detail-priority").value, assignee_id: $("detail-assignee").value ? Number($("detail-assignee").value) : null, due_date: $("detail-due-date").value || null }, ticket);
    if (!Object.keys(body).length) { toast("Non ci sono modifiche da salvare."); return; }
    body.revision = ticket.revision;
    showError("properties-error", "");
    busy($("detail-properties-form"), $("save-properties"), true, "Salvataggio…");
    try {
      await api(`/api/tickets/${ticket.id}`, { method: "PUT", body });
      await loadDetail(ticket.id);
      loadTickets();
      toast("Ticket aggiornato.");
    } catch (error) {
      if (state.user) {
        if (error.status === 409) await loadDetail(ticket.id);
        showError("properties-error", error.status === 409 ? "Il ticket era stato aggiornato da un’altra persona. Abbiamo caricato l’ultima versione: verifica e ripeti le modifiche." : error.message);
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
    if (!body) { showError("comment-error", "Scrivi un commento prima di inviarlo."); $("comment-body").focus(); return; }
    showError("comment-error", "");
    busy($("comment-form"), $("send-comment"), true, "Invio…");
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
        } else showError("properties-error", "Le proprietà sono state aggiornate da un’altra persona. Verifica l’ultima versione e ripeti le modifiche non salvate.");
      }
      loadTickets();
      toast("Commento aggiunto.");
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
        } else showError("properties-error", "Il ticket è cambiato. Abbiamo caricato l’ultima versione: verifica e ripeti le modifiche non salvate.");
      }
      if (data) toast("Ticket aggiornato all’ultima versione.");
    } finally { $("refresh-detail").disabled = false; }
  }

  async function takeTicket() {
    if (!state.detail || !state.user) return;
    const ticket = state.detail.ticket, generation = state.generation;
    const draft = { status: $("detail-status").value, priority: $("detail-priority").value, assignee_id: $("detail-assignee").value ? Number($("detail-assignee").value) : null, due_date: $("detail-due-date").value || null };
    if (Object.keys(changes(draft, ticket)).length) {
      showError("properties-error", "Hai modifiche non salvate alle proprietà. Salvale prima di prendere in carico il ticket.");
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
      toast("Ticket preso in carico. Buon lavoro!");
    } catch (error) {
      if (generation !== state.generation || !state.user) return;
      if (error.status === 409) await loadDetail(ticket.id);
      showError("properties-error", error.status === 409 ? "Un’altra persona ha aggiornato il ticket. Verifica l’ultima versione prima di prenderlo in carico." : error.message);
    } finally {
      button.removeAttribute("aria-busy");
      if (state.detail?.ticket.id === ticket.id) button.disabled = state.detail.ticket.assignee_id === state.user?.id && state.detail.ticket.status === "in_progress";
      else button.disabled = false;
    }
  }

  function renderUsers() {
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
      row.append(personCell, element("td", "", user.role === "admin" ? "Amministratore" : "Operatore"));
      const status = element("td");
      status.append(element("span", `user-status${user.active ? "" : " inactive"}`, user.active ? "Attivo" : "Disattivato"));
      row.append(status);
      const action = element("td");
      const edit = element("button", "button button-secondary", "Modifica");
      edit.type = "button";
      edit.setAttribute("aria-label", `Modifica l’utente ${user.name}`);
      edit.addEventListener("click", () => openUserEditor(user));
      action.append(edit);
      row.append(action);
      rows.append(row);
    }
    $("users-rows").replaceChildren(rows);
  }

  function openUserEditor(user = null) {
    state.editorUser = user ? { ...user } : null;
    $("user-form").reset();
    $("user-editor-title").textContent = user ? "Modifica utente" : "Nuovo utente";
    $("save-user").textContent = user ? "Salva modifiche" : "Crea utente";
    $("user-name").value = user?.name || "";
    $("user-email").value = user?.email || "";
    $("user-email").readOnly = Boolean(user);
    $("user-email-hint").hidden = !user;
    $("user-role").value = user?.role || "agent";
    $("user-active-field").hidden = !user;
    $("user-active").value = user ? String(Boolean(user.active)) : "true";
    $("user-password").required = !user;
    $("user-password-label").textContent = user ? "Reimposta password" : "Password iniziale";
    $("user-password-hint").textContent = user ? "Lascia vuoto per mantenere la password attuale, oppure inserisci almeno 12 caratteri." : "Almeno 12 caratteri.";
    showError("user-editor-error", "");
    $("user-editor").showModal();
    $("user-name").focus();
  }

  async function saveUser(event) {
    event.preventDefault();
    const original = state.editorUser;
    const body = { name: $("user-name").value.trim(), role: $("user-role").value };
    if (!body.name) { showError("user-editor-error", "Inserisci il nome dell’utente."); $("user-name").focus(); return; }
    if (original) body.active = $("user-active").value === "true";
    else body.email = $("user-email").value.trim();
    if ($("user-password").value) body.password = $("user-password").value;
    showError("user-editor-error", "");
    busy($("user-form"), $("save-user"), true, "Salvataggio…");
    try {
      await api(original ? `/api/users/${original.id}` : "/api/users", { method: original ? "PUT" : "POST", body });
      $("user-editor").close();
      $("user-password").value = "";
      if (original && original.id === state.user.id) updateSession(await api("/api/session"));
      await loadUsers();
      await loadTickets();
      if ($("ticket-detail").open && state.detailId) await loadDetail(state.detailId);
      toast(original ? "Utente aggiornato." : "Utente creato.");
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
    showError("login-error", "");
    $("login-notice").hidden = true;
    const body = { email: $("login-email").value.trim(), password: $("login-password").value };
    const button = $("login-form").querySelector('[type="submit"]');
    busy($("login-form"), button, true, "Accesso…");
    try {
      const data = await api("/api/login", { method: "POST", body });
      $("login-password").value = "";
      await openApp(data);
    } catch (error) { showError("login-error", error.message); }
    finally { busy($("login-form"), button, false); }
  });
  $("logout-button").addEventListener("click", async () => {
    $("logout-button").disabled = true;
    try { await api("/api/logout", { method: "POST", body: {} }); showLogin(); document.title = "Ticket. · Gestione richieste"; $("login-email").focus(); }
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
  $("refresh-tickets").addEventListener("click", () => { clearTimeout(state.searchTimer); Promise.allSettled([loadUsers(), loadTickets()]); });
  $("previous-page").addEventListener("click", () => { state.offset = Math.max(0, state.offset - state.limit); loadTickets(); });
  $("next-page").addEventListener("click", () => { state.offset += state.limit; loadTickets(); });
  $("new-ticket").addEventListener("click", () => openEditor());
  $("ticket-form").addEventListener("submit", saveTicket);
  $("edit-ticket").addEventListener("click", () => { if (state.detail) openEditor(state.detail.ticket); });
  $("retry-detail").addEventListener("click", () => { if (state.detailId) loadDetail(state.detailId); });
  $("refresh-detail").addEventListener("click", refreshCurrentDetail);
  $("take-ticket").addEventListener("click", takeTicket);
  $("copy-ticket-link").addEventListener("click", async () => {
    if (!state.detailId) return;
    try { await navigator.clipboard.writeText(ticketURL(state.detailId).href); toast("Link al ticket copiato."); }
    catch { toast("Il link è nella barra degli indirizzi: puoi copiarlo da lì.", true); }
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
      toast("Ultima versione caricata. Puoi ripetere le modifiche.");
    } catch (error) { if (state.user) showError("editor-error", error.message); }
    finally { $("reload-editor").disabled = false; }
  });
  $("detail-properties-form").addEventListener("submit", saveProperties);
  $("comment-form").addEventListener("submit", sendComment);
  $("manage-users").addEventListener("click", () => { showError("users-error", ""); $("users-dialog").showModal(); loadUsers().catch(() => {}); });
  $("new-user").addEventListener("click", () => openUserEditor());
  $("user-form").addEventListener("submit", saveUser);

  api("/api/session").then(openApp).catch((error) => {
    showLogin();
    if (error.status !== 401) showError("login-error", error.message);
  });
})();
