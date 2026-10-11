"use strict";

(async () => {
  if (window.PuntoI18n) await window.PuntoI18n.ready;
  const { t, locale } = window.PuntoI18n || { t: (source, values = []) => source.replace(/\{(\d+)\}/g, (_, index) => values[index]), locale: "it-IT" };
  const $ = (id) => document.getElementById(id);
  const statuses = { open: t("Aperto"), in_progress: t("In corso"), waiting: t("In attesa"), resolved: t("Risolto"), closed: t("Chiuso") };
  const priorities = { low: t("Bassa"), normal: t("Normale"), high: t("Alta"), urgent: t("Urgente") };
  const queues = { all: t("Tutti i ticket"), active: t("Da lavorare"), mine: t("I miei ticket"), unassigned: t("Non assegnati"), overdue: t("Scaduti"), due_soon: t("In scadenza") };
  const operationsQueues = { all: t("Tutte le attività"), needs_reply: t("Da rispondere"), waiting_customer: t("In attesa cliente"), sla_today: t("SLA oggi"), unscheduled: t("Interventi da programmare"), follow_up_due: t("Ricontatti scaduti") };
  const queueDescriptions = { all: t("Tutte le richieste, comprese quelle risolte e chiuse."), active: t("Ticket aperti, in corso o in attesa."), mine: t("Ticket da lavorare assegnati a te."), unassigned: t("Ticket da lavorare che aspettano un assegnatario."), overdue: t("Ticket da lavorare con una scadenza già passata."), due_soon: t("Ticket da lavorare in scadenza da oggi ai prossimi 7 giorni.") };
  const historyActions = {
    created: t("ha aperto il ticket"), create: t("ha aperto il ticket"), updated: t("ha aggiornato il ticket"),
    update: t("ha aggiornato il ticket"), commented: t("ha aggiunto un commento"), comment: t("ha aggiunto un commento"),
    status_changed: t("ha cambiato lo stato"), assigned: t("ha aggiornato l’assegnazione"), raw_linked: t("ha collegato un’email al ticket"),
    calendar_requested: t("ha richiesto una modifica al calendario Google"),
    calendar_reconciled: t("ha verificato il collegamento con l’evento Google"),
    operations_updated: t("ha aggiornato l’operatività del ticket"), customer_draft: t("ha salvato una bozza per il cliente"),
    customer_message: t("ha aggiornato l’invio al cliente"), work_logged: t("ha registrato un’operazione sulle ore"),
    solution_saved: t("ha salvato una soluzione"), bulk_updated: t("ha applicato una modifica multipla")
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

  const browserTimezone = Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC";
  const agendaDateFormat = new Intl.DateTimeFormat(locale, { weekday: "long", day: "numeric", month: "short" });
  const agendaTimeFormat = new Intl.DateTimeFormat(locale, { hour: "2-digit", minute: "2-digit" });
  const slotFormat = new Intl.DateTimeFormat(locale, { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" });
  Object.assign(state, { agendaWeek: weekStart(new Date()), agendaItems: [], calendarRequest: null, calendarCycle: 0, calendar: null });

  function weekStart(date) {
    const result = new Date(date.getFullYear(), date.getMonth(), date.getDate());
    result.setDate(result.getDate() - (result.getDay() + 6) % 7);
    return result;
  }

  function civilDay(date, days) { return new Date(date.getFullYear(), date.getMonth(), date.getDate() + days); }
  function localInput(epoch) {
    if (!epoch) return "";
    const date = new Date(epoch * 1000), pad = (n) => String(n).padStart(2, "0");
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`;
  }
  function offsetLabel(epoch) {
    const minutes = -new Date(epoch * 1000).getTimezoneOffset();
    return `UTC${minutes < 0 ? "−" : "+"}${String(Math.floor(Math.abs(minutes) / 60)).padStart(2, "0")}:${String(Math.abs(minutes) % 60).padStart(2, "0")}`;
  }
  function localCandidates(value) {
    const match = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})$/.exec(value);
    if (!match) return [];
    const [, y, m, d, h, min] = match.map(Number);
    const guess = new Date(y, m - 1, d, h, min).getTime();
    const result = [];
    for (let delta = -180; delta <= 180; delta += 30) {
      const epoch = Math.floor((guess + delta * 60000) / 1000);
      if (localInput(epoch) === value) result.push(epoch);
    }
    return [...new Set(result)].sort((a, b) => a - b);
  }
  function updateScheduleInput(prefix, endpoint, selected = null) {
    const input = $(`${prefix}-scheduled-${endpoint}`), select = $(`${prefix}-${endpoint}-offset`);
    const candidates = localCandidates(input.value), previous = selected ?? select.value;
    const field = $(`${prefix}-${endpoint}-offset-field`);
    select.replaceChildren();
    if (candidates.length > 1) {
      addOption(select, "", t("Scegli il fuso orario"));
      for (const epoch of candidates) addOption(select, epoch, offsetLabel(epoch));
      select.value = candidates.includes(Number(previous)) ? String(previous) : "";
      field.hidden = false;
    } else { field.hidden = true; if (candidates[0]) addOption(select, candidates[0], offsetLabel(candidates[0])); }
    updateScheduleHint(prefix);
  }
  function updateScheduleHint(prefix) {
    const original = prefix === "detail" ? state.detail?.ticket : state.editorTicket;
    $(`${prefix}-schedule-zone`).textContent = original?.scheduled_timezone && original.scheduled_timezone !== browserTimezone ? t("Orari visualizzati in {0}. Fuso salvato: {1}.", [browserTimezone, original.scheduled_timezone]) : t("Orari nel fuso del browser: {0}.", [browserTimezone]);
    const labels = [];
    for (const endpoint of ["start", "end"]) {
      const input = $(`${prefix}-scheduled-${endpoint}`), candidates = localCandidates(input.value);
      const chosen = candidates.length === 1 ? candidates[0] : Number($(`${prefix}-${endpoint}-offset`).value);
      if (input.value && !candidates.length) labels.push(t("Orario inesistente nel fuso scelto."));
      else if (input.value && chosen) labels.push(`${endpoint === "start" ? t("Inizio") : t("Fine")}: ${offsetLabel(chosen)}`);
    }
    $(`${prefix}-schedule-offsets`).textContent = labels.join(" · ");
  }
  function populateSchedule(prefix, ticket = null) {
    for (const endpoint of ["start", "end"]) {
      const epoch = ticket?.[`scheduled_${endpoint}`] || null, input = $(`${prefix}-scheduled-${endpoint}`);
      input.value = localInput(epoch);
      input.dataset.originalValue = input.value;
      input.dataset.originalEpoch = epoch ? String(epoch) : "";
      updateScheduleInput(prefix, endpoint, epoch ? Math.floor(epoch / 60) * 60 : "");
    }
    updateScheduleHint(prefix);
  }
  function scheduleValues(prefix, original = null) {
    const values = {};
    const inputs = ["start", "end"].map((endpoint) => $(`${prefix}-scheduled-${endpoint}`).value);
    if (!inputs[0] && !inputs[1]) return { scheduled_start: null, scheduled_end: null, scheduled_timezone: null };
    if (!inputs[0] || !inputs[1]) throw new Error(t("Indica sia l’inizio sia la fine dell’intervento."));
    for (const endpoint of ["start", "end"]) {
      const input = $(`${prefix}-scheduled-${endpoint}`), candidates = localCandidates(input.value);
      if (!candidates.length) throw new Error(t("Questo orario non esiste nel fuso del browser. Scegli un altro orario."));
      const selected = candidates.length === 1 ? candidates[0] : Number($(`${prefix}-${endpoint}-offset`).value);
      if (!candidates.includes(selected)) throw new Error(t("Questo orario si ripete nel cambio d’ora: scegli il fuso per inizio e fine."));
      const previous = Number(input.dataset.originalEpoch);
      values[`scheduled_${endpoint}`] = previous && input.value === input.dataset.originalValue && Math.floor(previous / 60) * 60 === selected ? previous : selected;
    }
    if (values.scheduled_start <= 0 || values.scheduled_end > 7258118399 || values.scheduled_end <= values.scheduled_start || values.scheduled_end - values.scheduled_start > 366 * 86400) throw new Error(t("La fine deve seguire l’inizio; la durata massima è 366 giorni, entro il 2199."));
    values.scheduled_timezone = original?.scheduled_start === values.scheduled_start && original?.scheduled_end === values.scheduled_end ? original.scheduled_timezone : browserTimezone;
    return values;
  }
  function scheduleChanges(values, original) {
    const body = changes(values, original);
    if (["scheduled_start", "scheduled_end", "scheduled_timezone"].some((key) => Object.hasOwn(body, key))) {
      for (const key of ["scheduled_start", "scheduled_end", "scheduled_timezone"]) body[key] = values[key];
    }
    return body;
  }
  function propertyDraft() {
    return { ...assessmentValues("detail", false), status: $("detail-status").value, priority: $("detail-priority").value, assignee_id: $("detail-assignee").value ? Number($("detail-assignee").value) : null, due_date: $("detail-due-date").value || null,
      slot: { ...Object.fromEntries(["start", "end"].map((endpoint) => [endpoint, { value: $(`detail-scheduled-${endpoint}`).value, offset: $(`detail-${endpoint}-offset`).value }])), followUp: $("detail-follow-up").value } };
  }
  function propertiesDirty(ticket, draft = propertyDraft()) {
    const { slot, ...values } = draft;
    if (Object.keys(changes(values, ticket)).length) return true;
    try { assessmentValues("detail"); return Object.keys(changes(scheduleValues("detail", ticket), ticket)).length > 0; } catch { return true; }
  }
  function restorePropertyDraft(draft) {
    $("detail-status").value = draft.status; $("detail-priority").value = draft.priority;
    $("detail-assignee").value = draft.assignee_id === null ? "" : String(draft.assignee_id);
    $("detail-due-date").value = draft.due_date || "";
    fillAssessment("detail", draft); $("detail-follow-up").value = draft.slot.followUp || ""; refreshAssessmentHint("detail");
    for (const endpoint of ["start", "end"]) { $(`detail-scheduled-${endpoint}`).value = draft.slot[endpoint].value; updateScheduleInput("detail", endpoint, draft.slot[endpoint].offset); }
  }
  function scheduleBadge(ticket) {
    if (!ticket.scheduled_start) return null;
    const badge = element("span", "schedule-badge", t("Programmato · {0}", [slotFormat.format(new Date(ticket.scheduled_start * 1000))]));
    badge.title = `${slotFormat.format(new Date(ticket.scheduled_start * 1000))} – ${slotFormat.format(new Date(ticket.scheduled_end * 1000))} · ${browserTimezone}`;
    return badge;
  }
  function renderAgenda() {
    const start = state.agendaWeek, end = civilDay(start, 6);
    $("agenda-week-label").textContent = `${agendaDateFormat.format(start)} — ${agendaDateFormat.format(end)}`;
    $("agenda-timezone").textContent = t("Orari nel fuso del browser: {0}.", [browserTimezone]);
    const fragment = document.createDocumentFragment(), today = new Date();
    for (let index = 0; index < 7; index += 1) {
      const day = civilDay(start, index), next = civilDay(day, 1), from = day.getTime() / 1000, to = next.getTime() / 1000;
      const isToday = day.toDateString() === today.toDateString();
      const section = element("section", "agenda-day" + (isToday ? " agenda-is-today" : ""));
      const heading = element("div", "agenda-day-heading"), label = element("h4", "", agendaDateFormat.format(day));
      if (isToday) label.append(element("span", "agenda-today-label", t("Oggi")));
      const add = element("button", "button button-quiet agenda-add", "+"); add.type = "button";
      add.setAttribute("aria-label", t("Programma un ticket il {0}", [agendaDateFormat.format(day)]));
      add.addEventListener("click", () => {
        openEditor(); const slot = new Date(day.getFullYear(), day.getMonth(), day.getDate(), 9).getTime() / 1000;
        populateSchedule("ticket", { scheduled_start: slot, scheduled_end: slot + 3600, scheduled_timezone: browserTimezone });
      });
      heading.append(label, add); section.append(heading);
      const items = state.agendaItems.filter((ticket) => ticket.scheduled_start < to && ticket.scheduled_end > from).sort((a, b) => a.scheduled_start - b.scheduled_start || a.id - b.id);
      if (!items.length) section.append(element("p", "agenda-day-empty", t("Nessun intervento")));
      for (const ticket of items) {
        const card = element("article", `agenda-card card-priority-${ticket.priority}`);
        const times = `${ticket.scheduled_start < from ? "← " : ""}${agendaTimeFormat.format(new Date(Math.max(from, ticket.scheduled_start) * 1000))} – ${ticket.scheduled_end > to ? "24:00 →" : agendaTimeFormat.format(new Date(ticket.scheduled_end * 1000))}`;
        card.append(element("div", "agenda-slot", times), ticketLink(ticket, "agenda-ticket-title"), element("span", `badge status-${ticket.status}`, statuses[ticket.status]), resourceBadges(ticket));
        card.append(element("p", "agenda-person", ticket.assignee_name || t("Non assegnato")));
        if (ticket.due_date) card.append(deadlineNode(ticket));
        card.title = `${slotFormat.format(new Date(ticket.scheduled_start * 1000))} – ${slotFormat.format(new Date(ticket.scheduled_end * 1000))} · ${browserTimezone}`;
        section.append(card);
      }
      fragment.append(section);
    }
    $("agenda-days").replaceChildren(fragment);
    $("agenda-container").hidden = false; $("board-container").hidden = true; $("board-hint").hidden = true;
    $("ticket-table-container").hidden = true; $("pagination").hidden = true; $("list-empty").hidden = true;
  }

  async function loadTicketCalendar(ticket) {
    state.calendarRequest?.abort(); const controller = new AbortController(); state.calendarRequest = controller;
    const cycle = ++state.calendarCycle, generation = state.generation;
    state.calendar = null; $("ticket-calendar-panel").removeAttribute("aria-busy"); $("calendar-connect").disabled = false; $("ticket-calendar-state").textContent = ""; $("ticket-calendar-description").textContent = "";
    $("ticket-calendar-loading").hidden = false; $("ticket-calendar-remote").hidden = true; $("ticket-calendar-link").hidden = true;
    for (const action of ["sync", "remove", "refresh"]) $("calendar-" + action).disabled = true;
    $("calendar-connect").hidden = true; $("calendar-retry").hidden = true; $("calendar-setup").hidden = true; showError("ticket-calendar-error", "");
    try {
      const data = await api(`/api/tickets/${ticket.id}/calendar`, { signal: controller.signal });
      if (cycle !== state.calendarCycle || generation !== state.generation || state.detailId !== ticket.id || !$("ticket-detail").open) return;
      renderTicketCalendar(data);
    } catch (error) { if (error.name !== "AbortError" && cycle === state.calendarCycle && generation === state.generation && state.detailId === ticket.id && $("ticket-detail").open) showError("ticket-calendar-error", error.message); }
    finally { if (cycle === state.calendarCycle && generation === state.generation) $("ticket-calendar-loading").hidden = true; }
  }
  function renderTicketCalendar(data) {
    state.calendar = data;
    const labels = { idle: t("Non sincronizzato"), queued: t("In coda"), unknown: t("Esito da verificare"), applied: t("Sincronizzato"), conflict: t("Conflitto"), failed: t("Invio non riuscito"), removed: t("Rimosso da Google"), stale: t("Calendario cambiato"), missing: t("Evento non trovato") };
    const messages = { idle: t("La programmazione è salvata solo nell’agenda interna."), queued: t("La modifica è accodata. Verifica Google per conoscere l’esito."), unknown: t("L’esito dell’invio non è ancora noto. Usa “Verifica Google” per controllare la stessa operazione."), applied: t("La programmazione è stata applicata al calendario Google."), conflict: t("L’evento Google è cambiato. Verifica i dati prima di sincronizzare di nuovo."), failed: t("Google non ha applicato la modifica. Verifica il collegamento e riprova."), removed: t("L’evento è stato rimosso da Google. La programmazione interna è conservata."), stale: t("Il calendario collegato è cambiato. Il vecchio evento non viene modificato automaticamente."), missing: t("L’evento non è più presente su Google. La programmazione interna è conservata.") };
    const needsSync = data.state === "applied" && data.remote_diff;
    $("ticket-calendar-state").textContent = needsSync ? t("Da sincronizzare") : labels[data.state] || labels.idle;
    $("ticket-calendar-state").className = "calendar-state calendar-state-" + (Object.hasOwn(labels, data.state) ? data.state : "idle");
    let message = needsSync ? t("Il ticket e Google contengono dati diversi. Verifica prima di sincronizzare.") : messages[data.state] || messages.idle;
    if (!data.configured) message = t("Google Calendar non è configurato. Puoi usare normalmente l’agenda interna.");
    else if (!state.punto.connected) message = t("Collega il calendario tramite Punto per inviare gli interventi a Google.");
    else if (!data.calendar_access) message = t("Autorizza il calendario in Punto con “Collega calendario”.");
    else if (!data.connected && !data.owner_mismatch) message = t("Configura Google Calendar in Punto e scegli un calendario modificabile.");
    if (data.owner_mismatch) message += " " + t("Solo l’account Punto proprietario può modificare questo evento Google. Le modifiche locali al ticket restano disponibili.");
    if (!state.detail?.ticket.scheduled_start) message += " " + t("Imposta inizio e fine dell’intervento e salva il ticket.");
    $("ticket-calendar-description").textContent = message;
    showError("ticket-calendar-error", data.error ? t(data.error) : "");
    const authorized = data.configured && data.connected && data.calendar_access && !data.owner_mismatch;
    $("calendar-sync").disabled = !data.can_sync;
    $("calendar-sync").textContent = t("Sincronizza Google");
    const canRetry = authorized && data.can_retry === true && ["sync", "remove"].includes(data.retry_action);
    $("calendar-retry").hidden = !canRetry; $("calendar-retry").disabled = !canRetry;
    $("calendar-remove").disabled = !(authorized && data.state === "applied" && data.event_id && !data.pending);
    $("calendar-refresh").disabled = !(state.punto.connected && data.calendar_access && !data.owner_mismatch);
    $("calendar-connect").hidden = !state.punto.configured || (state.punto.connected && data.calendar_access);
    $("calendar-setup").hidden = !state.punto.origin;
    if (state.punto.origin) { const url = new URL("/app#calendar", state.punto.origin); $("calendar-setup").href = window.PuntoI18n ? window.PuntoI18n.url(url.href) : url.href; }
    $("ticket-calendar-link").hidden = true;
    if (data.event_link) { try { const url = new URL(data.event_link); if (url.protocol === "https:") { $("ticket-calendar-link").href = url.href; $("ticket-calendar-link").hidden = false; } } catch { /* Ignora link non validi. */ } }
    const remote = $("ticket-calendar-remote"); remote.replaceChildren(); remote.hidden = !data.remote_diff;
    if (data.remote_diff) {
      remote.append(element("p", "", t("Google contiene dati diversi dal ticket. La verifica non importa né sovrascrive la programmazione locale.")));
      const start = new Date(data.remote_event?.start?.dateTime), end = new Date(data.remote_event?.end?.dateTime);
      if (Number.isFinite(start.getTime()) && Number.isFinite(end.getTime())) remote.append(element("p", "", `${slotFormat.format(start)} – ${slotFormat.format(end)} · ${browserTimezone}`));
      if (data.remote_event?.summary) remote.append(element("p", "", data.remote_event.summary));
    }
  }
  async function calendarAction(action) {
    if (!state.detail || !state.calendar) return;
    const ticket = state.detail.ticket, generation = state.generation;
    if (propertiesDirty(ticket)) { showError("properties-error", t("Salva le proprietà e la programmazione prima di usare Google Calendar.")); $("save-properties").focus(); return; }
    const cycle = ++state.calendarCycle;
    state.calendarRequest?.abort(); const controller = new AbortController(); state.calendarRequest = controller;
    $("ticket-calendar-panel").setAttribute("aria-busy", "true"); showError("ticket-calendar-error", "");
    for (const button of $("ticket-calendar-panel").querySelectorAll("button")) button.disabled = true;
    try {
      const data = await api(`/api/tickets/${ticket.id}/calendar`, { method: "POST", body: { action, revision: ticket.revision }, signal: controller.signal });
      if (cycle !== state.calendarCycle || generation !== state.generation || state.detailId !== ticket.id || !$("ticket-detail").open) return;
      renderTicketCalendar(data);
      toast(action === "refresh" ? t("Stato Google verificato. La programmazione locale non è stata modificata.") : data.pending ? t("Modifica accodata su Google. L’applicazione è ancora da verificare.") : data.state === "applied" && !data.remote_diff ? t("Programmazione applicata su Google.") : data.state === "removed" ? t("Evento rimosso da Google.") : t("Stato Google aggiornato: {0}.", [$("ticket-calendar-state").textContent]), ["conflict", "failed", "unknown"].includes(data.state));
    } catch (error) { if (error.name !== "AbortError" && cycle === state.calendarCycle && generation === state.generation && state.detailId === ticket.id && $("ticket-detail").open) { if (state.calendar) renderTicketCalendar(state.calendar); showError("ticket-calendar-error", error.status === 409 ? t("Il ticket è cambiato. Aggiorna il dettaglio prima di usare Google Calendar.") : error.message); } }
    finally { if (cycle === state.calendarCycle) { $("ticket-calendar-panel").removeAttribute("aria-busy"); $("calendar-connect").disabled = false; } }
  }
  async function startPuntoConnection(calendar = false, timeAccess = false) {
    if (!state.punto.origin || !state.user) return;
    const generation = state.generation, intentQuery = state.puntoIntentQuery ?? location.search; state.puntoIntentQuery = null;
    const errorId = calendar ? "ticket-calendar-error" : "integration-error", button = calendar ? $("calendar-connect") : $("connect-punto");
    button.disabled = true; showError(errorId, "");
    try {
      const data = await api("/api/integrations/punto/start", { method: "POST", body: timeAccess ? { time: true, calendar } : calendar ? { calendar: true } : {} });
      if (generation !== state.generation || !state.user) return;
      const target = new URL(data.url);
      if (target.origin !== new URL(state.punto.origin).origin || !/^#ticket-connect\?state=[a-f0-9]{64}(?:&(?:calendar|time)=1)*$/.test(target.hash)) throw new Error(t("Il collegamento a Punto non è valido."));
      try { sessionStorage.setItem("ticket.punto.intent", JSON.stringify({ userId: state.user.id, query: intentQuery })); } catch { /* Ripristino facoltativo. */ }
      location.assign(window.PuntoI18n ? window.PuntoI18n.url(target.href) : target.href);
    } catch (error) { if (generation === state.generation && state.user) showError(errorId, error.message); }
    finally { if (generation === state.generation) button.disabled = false; }
  }

  const rawStates = { new: t("Nuovo"), reviewing: t("In revisione"), converted: t("Convertito"), linked: t("Collegato"), discarded: t("Scartato"), quarantine: t("In quarantena") };
  const rawHistoryActions = { claim: t("ha preso in carico il messaggio"), release: t("ha restituito il messaggio alla coda"), discard: t("ha scartato il messaggio"), converted: t("ha convertito il messaggio in ticket"), linked: t("ha collegato il messaggio a un ticket") };
  state.raw = { section: "tickets", offset: 0, limit: 50, total: 0, cycle: 0, controller: null, detailCycle: 0, detailController: null, detail: null, convert: null, searchTimer: null, mailboxes: [], mailbox: null, sender: null, adminCycle: 0, senderCycle: 0, mailboxJob: null, senderJob: null, link: null, linkCycle: 0, linkOffset: 0, linkQuery: "", linkItems: [], linkTotal: 0, suppressClose: false };
  function rawRoute() { const value = new URL(location.href).searchParams.get("raw"); return /^[1-9]\d*$/.test(value || "") && Number.isSafeInteger(Number(value)) ? Number(value) : null; }
  function rawURL(id) { const url = new URL(location.href); url.searchParams.delete("ticket"); url.searchParams.set("section", "raw"); url.searchParams.set("raw", String(id)); return url; }
  function rawTime(value) { return typeof value === "number" ? time(new Date(value * 1000).toISOString()) : time(value); }
  function byteSize(value) { const bytes = Number(value) || 0; return bytes < 1024 ? `${bytes} B` : bytes < 1048576 ? `${(bytes / 1024).toLocaleString(locale, { maximumFractionDigits: 1 })} KB` : `${(bytes / 1048576).toLocaleString(locale, { maximumFractionDigits: 1 })} MB`; }
  function rawDownload(path, label) {
    const link = element("a", "button button-quiet raw-download", label);
    try { const url = new URL(path, location.origin); if (url.origin !== location.origin || !url.pathname.startsWith("/api/")) throw new Error(); link.href = url.href; link.download = ""; }
    catch { return element("span", "muted", t("Download non disponibile")); }
    return link;
  }
  function resetRawState() {
    const raw = state.raw; raw.controller?.abort(); raw.detailController?.abort(); clearTimeout(raw.searchTimer);
    raw.cycle += 1; raw.detailCycle += 1; raw.adminCycle += 1; raw.senderCycle += 1; raw.linkCycle += 1;
    $("raw-detail").removeAttribute("aria-busy"); $("raw-ticket-picker").removeAttribute("aria-busy");
    if (raw.convert) $("ticket-form").reset();
    raw.linkItems = []; raw.linkOffset = 0; raw.linkTotal = 0; raw.linkQuery = ""; $("raw-ticket-search").value = ""; $("raw-ticket-selection").textContent = ""; $("raw-link-attachment-list").replaceChildren();
    raw.detail = null; raw.convert = null; raw.mailboxes = []; raw.mailbox = null; raw.sender = null; raw.link = null; raw.offset = 0;
    raw.suppressClose = $("raw-detail").open;
    raw.section = "tickets"; raw.mailboxJob = null; raw.senderJob = null;
    for (const id of ["raw-list", "raw-stats", "raw-detail-meta", "raw-attachments", "raw-history-list", "raw-mailboxes", "raw-senders", "raw-ticket-results", "raw-convert-attachment-list", "ticket-raw-origin-list"]) $(id).replaceChildren();
    $("raw-body").textContent = ""; $("raw-detail-title").textContent = ""; $("raw-quarantine-reason").textContent = ""; $("raw-linked-ticket").replaceChildren(); $("raw-mailbox-filter").replaceChildren(); addOption($("raw-mailbox-filter"), "", t("Tutte le caselle")); $("raw-filter-form").reset(); $("raw-discard-form").reset();
    $("raw-mailbox-form").reset(); $("raw-sender-form").reset(); $("raw-mailbox-form").hidden = true; $("raw-sender-form").hidden = true;
    $("raw-workspace").hidden = true; $("tickets-workspace").hidden = false; $("raw-total").textContent = "—";
    $("nav-tickets").setAttribute("aria-pressed", "true"); $("nav-raw").setAttribute("aria-pressed", "false");
  }
  function showWorkspace(section, syncRoute = true) {
    state.raw.section = ["raw", "dashboard"].includes(section) ? section : "tickets";
    $("tickets-workspace").hidden = state.raw.section !== "tickets"; $("raw-workspace").hidden = state.raw.section !== "raw"; $("operations-dashboard").hidden = state.raw.section !== "dashboard";
    $("nav-tickets").setAttribute("aria-pressed", String(state.raw.section === "tickets")); $("nav-raw").setAttribute("aria-pressed", String(state.raw.section === "raw")); $("nav-dashboard").setAttribute("aria-pressed", String(state.raw.section === "dashboard"));
    $("raw-settings").hidden = state.user?.role !== "admin";
    if (syncRoute) { const url = new URL(location.href); if (["raw", "dashboard"].includes(section)) { url.searchParams.set("section", section); if (section !== "raw") url.searchParams.delete("raw"); } else { url.searchParams.delete("section"); url.searchParams.delete("raw"); } history.replaceState(null, "", url); }
    if (state.raw.section === "raw") loadRawMessages();
    if (state.raw.section === "dashboard") loadDashboard();
  }
  async function openRawRoute() {
    const url = new URL(location.href);
    if (url.searchParams.get("section") === "raw" || rawRoute()) { showWorkspace("raw", false); if (rawRoute()) await openRawDetail(rawRoute(), false); }
    else showWorkspace(url.searchParams.get("section") === "dashboard" ? "dashboard" : "tickets", false);
  }
  function initRawFilters() {
    const selected = $("raw-status-filter").value || "new";
    $("raw-status-filter").replaceChildren(); addOption($("raw-status-filter"), "all", t("Tutti i messaggi"));
    for (const [key, label] of Object.entries(rawStates)) if (key !== "quarantine" || state.user?.role === "admin") addOption($("raw-status-filter"), key, label);
    $("raw-status-filter").value = selected === "quarantine" && state.user?.role !== "admin" ? "new" : selected;
  }
  async function loadRawMessages() {
    if (!state.user) return;
    const raw = state.raw, generation = state.generation, cycle = ++raw.cycle;
    raw.controller?.abort(); const controller = new AbortController(); raw.controller = controller;
    const params = new URLSearchParams({ state: $("raw-status-filter").value || "new", search: $("raw-search").value.trim(), limit: String(raw.limit), offset: String(raw.offset) });
    if ($("raw-mailbox-filter").value) params.set("mailbox_id", $("raw-mailbox-filter").value);
    $("raw-loading").hidden = false; $("raw-refresh").disabled = true; $("raw-previous").disabled = true; $("raw-next").disabled = true; showError("raw-list-error", "");
    try {
      const data = await api(`/api/raw/messages?${params}`, { signal: controller.signal });
      if (generation !== state.generation || cycle !== raw.cycle || !state.user) return;
      raw.total = Number(data.total) || 0;
      if (raw.offset && raw.offset >= raw.total) { raw.offset = Math.max(0, Math.floor((raw.total - 1) / raw.limit) * raw.limit); return loadRawMessages(); }
      $("raw-total").textContent = raw.total.toLocaleString(locale);
      const stats = document.createDocumentFragment();
      for (const [key, label] of Object.entries(rawStates)) {
        if (key === "quarantine" && state.user.role !== "admin") continue;
        const button = element("button", "queue-button", label); button.type = "button"; button.setAttribute("aria-pressed", String(params.get("state") === key)); button.append(element("span", "queue-count", (Number(data.stats?.[key]) || 0).toLocaleString(locale)));
        button.addEventListener("click", () => { clearTimeout(raw.searchTimer); $("raw-status-filter").value = key; raw.offset = 0; loadRawMessages(); }); stats.append(button);
      }
      $("raw-stats").replaceChildren(stats);
      const rows = document.createDocumentFragment();
      for (const item of data.items || []) {
        const article = element("article", "raw-list-item"), content = element("div", "raw-list-content");
        const title = element("a", "raw-subject", item.subject || t("Senza oggetto")); title.href = rawURL(item.id).href;
        title.addEventListener("click", (event) => { if (!event.metaKey && !event.ctrlKey && !event.shiftKey && !event.altKey && event.button === 0) { event.preventDefault(); openRawDetail(item.id); } });
        content.append(title, element("p", "raw-sender", `${item.from_name ? item.from_name + " · " : ""}${item.from_email || "—"}`));
        if (item.text_preview) content.append(element("p", "raw-snippet", item.text_preview));
        const meta = element("div", "raw-list-meta"); meta.append(element("span", `badge raw-state-${item.state}`, rawStates[item.state] || item.state), element("span", "", item.mailbox_name || `#${item.mailbox_id}`), element("time", "", rawTime(item.received_at)));
        if (item.claimed_name) meta.append(element("span", "", item.claimed_name));
        if (item.attachment_count) meta.append(element("span", "", t("{0} allegati", [item.attachment_count])));
        article.append(content, meta); rows.append(article);
      }
      if (!data.items?.length) rows.append(element("p", "raw-empty", t("Nessun messaggio corrisponde ai filtri.")));
      $("raw-list").replaceChildren(rows); $("raw-pagination").hidden = !raw.total;
      $("raw-page-description").textContent = raw.total ? t("{0}–{1} di {2} messaggi", [raw.offset + 1, Math.min(raw.offset + (data.items || []).length, raw.total), raw.total.toLocaleString(locale)]) : "";
      $("raw-previous").disabled = raw.offset === 0; $("raw-next").disabled = raw.offset + raw.limit >= raw.total;
      if (Array.isArray(data.mailboxes)) populateRawMailboxFilter(data.mailboxes);
    } catch (error) { if (error.name !== "AbortError" && generation === state.generation && cycle === raw.cycle && state.user) { $("raw-list").replaceChildren(); $("raw-stats").replaceChildren(); $("raw-total").textContent = "—"; $("raw-pagination").hidden = true; showError("raw-list-error", error.message); } }
    finally { if (generation === state.generation && cycle === raw.cycle) { $("raw-loading").hidden = true; $("raw-refresh").disabled = false; } }
  }
  function populateRawMailboxFilter(items) {
    const selected = $("raw-mailbox-filter").value; $("raw-mailbox-filter").replaceChildren(); addOption($("raw-mailbox-filter"), "", t("Tutte le caselle"));
    for (const item of items) addOption($("raw-mailbox-filter"), item.id, item.name || item.address);
    $("raw-mailbox-filter").value = selected;
  }
  async function openRawDetail(id, syncRoute = true) {
    if (!state.user) return;
    const raw = state.raw, cycle = ++raw.detailCycle, generation = state.generation;
    raw.detailController?.abort(); const controller = new AbortController(); raw.detailController = controller; raw.detail = null;
    if (syncRoute) history.pushState(null, "", rawURL(id));
    $("raw-detail").removeAttribute("aria-busy");
    $("raw-detail-title").textContent = t("Caricamento…"); $("raw-detail-number").textContent = `RAW #${id}`; $("raw-detail-content").hidden = true; $("raw-detail-loading").hidden = false; $("raw-actions").hidden = true; showError("raw-detail-error", "");
    $("raw-discard-form").hidden = true; $("raw-discard-form").reset();
    if (!$("raw-detail").open) $("raw-detail").showModal();
    try {
      const data = await api(`/api/raw/messages/${id}`, { signal: controller.signal });
      if (cycle !== raw.detailCycle || generation !== state.generation || !$("raw-detail").open) return;
      raw.detail = { ...data.raw, history: data.history || [], attachments: data.raw?.attachments || data.attachments || [], eml_url: data.raw?.eml_url || data.eml_url || `/api/raw/messages/${id}/eml` };
      renderRawDetail();
    } catch (error) { if (error.name !== "AbortError" && cycle === raw.detailCycle && generation === state.generation && $("raw-detail").open) showError("raw-detail-error", error.message); }
    finally { if (cycle === raw.detailCycle && generation === state.generation) $("raw-detail-loading").hidden = true; }
  }
  function renderRawDetail() {
    const raw = state.raw.detail; if (!raw) return;
    $("raw-detail-title").textContent = raw.subject || t("Senza oggetto"); $("raw-detail-state").textContent = rawStates[raw.state] || raw.state; $("raw-detail-state").className = `badge raw-state-${raw.state}`;
    $("raw-detail-owner").textContent = raw.claimed_name || t("Non assegnato");
    const meta = document.createDocumentFragment();
    const pairs = [[t("Mittente"), `${raw.from_name || ""} <${raw.from_email || "—"}>`], [t("Casella destinataria"), raw.mailbox_name || `#${raw.mailbox_id}`], [t("Ricevuto"), rawTime(raw.received_at)], [t("Dimensione"), byteSize(raw.size_bytes ?? raw.size)], [t("Mittente autorizzato"), raw.sender_authorized ? t("Sì") : t("No")], [t("Autenticazione email"), [{ pass: t("Dominio verificato"), unverified: t("Non verificata"), fail: t("Non superata") }[raw.auth_status] || raw.auth_status, raw.auth_status === "pass" ? "" : raw.auth_reason].filter(Boolean).join(" · ") || t("Non indicata")], [t("Identificativo messaggio"), raw.message_id || "—"]];
    for (const [label, value] of pairs) { const group = element("div"); group.append(element("dt", "", label), element("dd", "", value)); meta.append(group); }
    $("raw-detail-meta").replaceChildren(meta); $("raw-body").textContent = raw.body_text || raw.text_preview || t("Nessun testo disponibile.");
    const quarantineReasons = { mailbox_inactive: t("Casella disattivata"), local_import_untrusted: t("Importazione locale priva di verifica del provider"), authentication_not_verified: t("Autenticazione del dominio non verificata"), sender_not_authorized: t("Mittente non autorizzato"), sender_authorization_expired: t("Autorizzazione del mittente scaduta"), sender_mailbox_job_mismatch: t("Commessa del mittente diversa da quella della casella") };
    $("raw-quarantine-reason").textContent = quarantineReasons[raw.quarantine_reason] || raw.quarantine_reason || ""; $("raw-quarantine-reason").hidden = !raw.quarantine_reason;
    const attachments = document.createDocumentFragment();
    for (const file of raw.attachments) { const item = element("div", "raw-attachment"); item.append(element("span", "", `${file.filename} · ${byteSize(file.size)} · ${file.content_type || ""}`), rawDownload(file.download_url || `/api/raw/messages/${raw.id}/attachments/${file.id}`, t("Scarica"))); attachments.append(item); }
    if (!raw.attachments.length) attachments.append(element("p", "muted", t("Nessun allegato.")));
    if (raw.attachments.length) attachments.append(element("p", "raw-attachment-warning", t("Allegati non analizzati da antivirus. Scaricali solo se ne riconosci il contenuto.")));
    $("raw-attachments").replaceChildren(attachments);
    const history = document.createDocumentFragment();
    for (const event of [...(raw.history || [])].sort((a, b) => String(b.created_at).localeCompare(String(a.created_at)) || b.id - a.id)) {
      const item = element("li", "history-item"), description = element("p", "history-text");
      description.append(element("strong", "", event.actor_name || t("Sistema")), document.createTextNode(" " + (rawHistoryActions[event.action] || event.action || "")));
      item.append(description);
      if (event.detail) item.append(element("p", "history-detail", readableHistoryDetail(event)));
      const date = element("time", "history-time", time(event.created_at)); date.dateTime = event.created_at; item.append(date); history.append(item);
    }
    if (!raw.history?.length) history.append(element("li", "comments-empty", t("Nessuna attività registrata.")));
    $("raw-history-list").replaceChildren(history);
    const download = rawDownload(raw.eml_url, t("Scarica .eml")); $("raw-download").replaceWith(download); download.id = "raw-download";
    const editable = ["new", "reviewing"].includes(raw.state), owned = raw.claimed_by === state.user.id, pending = $("raw-detail").getAttribute("aria-busy") === "true";
    for (const button of $("raw-actions").querySelectorAll("button")) button.disabled = pending;
    $("raw-claim").hidden = !editable || owned; $("raw-claim").disabled = pending || Boolean(raw.claimed_by && !owned && state.user.role !== "admin");
    $("raw-release").hidden = !editable || !raw.claimed_by || (!owned && state.user.role !== "admin");
    for (const action of ["convert", "link", "discard"]) $("raw-" + action).hidden = !editable || !owned;
    for (const action of ["convert", "link"]) $("raw-" + action).disabled = pending || !(raw.state === "reviewing" && owned && raw.sender_authorized && raw.auth_status === "pass");
    $("raw-eligibility").hidden = !editable || (raw.sender_authorized && raw.auth_status === "pass");
    $("raw-eligibility").textContent = t("La conversione richiede un mittente autorizzato e l’autenticazione del dominio verificata dal provider fidato.");
    $("raw-linked-ticket").replaceChildren();
    if (raw.ticket_id) { const link = element("a", "button button-secondary", t("Apri ticket #{0}", [raw.ticket_id])); link.href = ticketURL(raw.ticket_id); link.addEventListener("click", (event) => { if (!event.metaKey && !event.ctrlKey && !event.shiftKey) { event.preventDefault(); $("raw-detail").close(); showWorkspace("tickets"); openDetail(raw.ticket_id); } }); $("raw-linked-ticket").append(link); }
    $("raw-actions").hidden = false; $("raw-detail-content").hidden = false;
  }
  async function rawMutation(action, extra = {}, errorId = "raw-detail-error") {
    const raw = state.raw.detail; if (!raw || !state.user || $("raw-detail").getAttribute("aria-busy") === "true") return null;
    const generation = state.generation, cycle = state.raw.detailCycle;
    $("raw-detail").setAttribute("aria-busy", "true"); for (const button of $("raw-actions").querySelectorAll("button")) button.disabled = true; showError(errorId, "");
    try {
      const data = await api(`/api/raw/messages/${raw.id}/${action}`, { method: "POST", body: { revision: raw.revision, ...extra } });
      if (generation !== state.generation || cycle !== state.raw.detailCycle || !state.user) return null;
      if (data.raw) state.raw.detail = { ...raw, ...data.raw };
      renderRawDetail(); loadRawMessages();
      await openRawDetail(raw.id, false);
      return generation === state.generation && state.user ? data : null;
    } catch (error) { if (generation === state.generation && state.user && cycle === state.raw.detailCycle) { if (error.status === 409) { state.raw.link = null; state.raw.linkCycle += 1; $("raw-ticket-confirm").disabled = true; await openRawDetail(raw.id, false); } if (generation !== state.generation || !state.user) return null; showError(errorId, error.status === 409 ? t("Il messaggio è cambiato. Verifica l’ultima versione prima di riprovare.") : error.message); } return null; }
    finally { if (generation === state.generation && cycle === state.raw.detailCycle) { $("raw-detail").removeAttribute("aria-busy"); for (const button of $("raw-actions").querySelectorAll("button")) button.disabled = false; if (state.raw.detail) renderRawDetail(); } }
  }
  async function startRawConversion() {
    const raw = state.raw.detail; if (!raw) return;
    openEditor(); state.raw.convert = { id: raw.id, revision: raw.revision };
    $("editor-title").textContent = t("Converti messaggio in ticket"); $("save-ticket").textContent = t("Conferma conversione");
    $("ticket-title").value = Array.from(raw.subject || t("Richiesta ricevuta via email")).slice(0, 160).join("");
    $("ticket-description").value = Array.from(raw.body_text || raw.text_preview || "").slice(0, 8000).join(""); $("ticket-requester").value = Array.from(raw.from_email || "").slice(0, 200).join("");
    state.resources = { job: raw.job_id ? { id: raw.job_id, code: raw.job_code || `#${raw.job_id}`, name: raw.job_name || "" } : null, asset: null }; renderResourceChoices();
    $("raw-convert-attachments").hidden = false; const files = document.createDocumentFragment();
    for (const file of raw.attachments) { const label = element("label", "raw-check"); const checkbox = element("input"); checkbox.type = "checkbox"; checkbox.value = String(file.id); checkbox.checked = true; label.append(checkbox, element("span", "", `${file.filename} · ${byteSize(file.size)}`)); files.append(label); }
    if (!raw.attachments.length) files.append(element("p", "muted", t("Nessun allegato."))); $("raw-convert-attachment-list").replaceChildren(files);
    if (raw.job_id && !state.punto.connected) showError("editor-error", t("Questo messaggio è vincolato a una commessa. Collega Punto prima di convertirlo."));
  }
  function selectedRawAttachmentIds(container) { return Array.from($(container).querySelectorAll("input:checked"), (node) => Number(node.value)); }
  function rawAttachmentIds() { return selectedRawAttachmentIds("raw-convert-attachment-list"); }
  function validateRawAttachments(ids) {
    const files = state.raw.detail?.attachments || [], selected = files.filter((file) => ids.includes(file.id));
    if (ids.length > 20 || selected.some((file) => file.size > 5242880) || selected.reduce((sum, file) => sum + file.size, 0) > 10485760) throw new Error(t("Seleziona al massimo 20 allegati, fino a 5 MB ciascuno e 10 MB complessivi."));
  }
  function openRawTicketPicker() {
    const files = document.createDocumentFragment();
    for (const file of state.raw.detail?.attachments || []) { const label = element("label", "raw-check"), checkbox = element("input"); checkbox.type = "checkbox"; checkbox.value = String(file.id); checkbox.checked = true; label.append(checkbox, element("span", "", `${file.filename} · ${byteSize(file.size)}`)); files.append(label); }
    $("raw-link-attachment-list").replaceChildren(files);
    state.raw.link = null; state.raw.linkOffset = 0; state.raw.linkItems = []; $("raw-ticket-search").value = ""; $("raw-ticket-confirm").disabled = true; $("raw-ticket-selection").hidden = true; $("raw-ticket-picker").showModal(); searchRawTickets();
  }
  async function searchRawTickets(more = false) {
    const raw = state.raw, generation = state.generation, cycle = ++raw.linkCycle;
    if (more && raw.linkQuery !== $("raw-ticket-search").value.trim()) return;
    if (!more) { raw.linkOffset = 0; raw.linkItems = []; raw.linkQuery = $("raw-ticket-search").value.trim(); raw.link = null; $("raw-ticket-confirm").disabled = true; $("raw-ticket-selection").hidden = true; }
    const query = new URLSearchParams({ queue: "all", search: raw.linkQuery, limit: "20", offset: String(raw.linkOffset) });
    if (raw.detail?.job_id) query.set("job_id", String(raw.detail.job_id));
    $("raw-ticket-more").disabled = true; showError("raw-ticket-error", "");
    try {
      const data = await api(`/api/tickets?${query}`); if (generation !== state.generation || cycle !== raw.linkCycle || !$("raw-ticket-picker").open) return;
      raw.linkItems.push(...(data.items || [])); raw.linkOffset += (data.items || []).length; raw.linkTotal = Number(data.total) || 0;
      const fragment = document.createDocumentFragment(); for (const ticket of raw.linkItems) { const button = element("button", "resource-result", `#${ticket.id} · ${ticket.title}`); button.type = "button"; button.addEventListener("click", () => { raw.link = ticket; $("raw-ticket-selection").textContent = t("Collegamento da confermare: #{0} · {1}", [ticket.id, ticket.title]); $("raw-ticket-selection").hidden = false; $("raw-ticket-confirm").disabled = false; }); fragment.append(button); }
      if (!raw.linkItems.length) fragment.append(element("p", "muted", t("Nessun ticket corrisponde ai filtri"))); $("raw-ticket-results").replaceChildren(fragment); $("raw-ticket-more").hidden = raw.linkOffset >= raw.linkTotal;
    } catch (error) { if (generation === state.generation && cycle === raw.linkCycle && $("raw-ticket-picker").open) showError("raw-ticket-error", error.message); }
    finally { if (cycle === raw.linkCycle) $("raw-ticket-more").disabled = raw.linkQuery !== $("raw-ticket-search").value.trim(); }
  }
  function renderRawOrigins(data) {
    const items = data.raw_sources || [], files = data.attachments || [], content = document.createDocumentFragment();
    for (const raw of items) { const link = element("a", "raw-origin", t("Email #{0}: {1}", [raw.id, raw.subject || t("Senza oggetto")])); link.href = rawURL(raw.id); link.addEventListener("click", (event) => { if (!event.metaKey && !event.ctrlKey && !event.shiftKey) { event.preventDefault(); $("ticket-detail").close(); showWorkspace("raw"); openRawDetail(raw.id); } }); content.append(link, element("p", "field-hint", `${raw.from_email || ""} · ${rawTime(raw.received_at)}`)); }
    for (const file of files) content.append(rawDownload(file.download_url, `${file.filename} · ${byteSize(file.size)}`));
    $("ticket-raw-origin-list").replaceChildren(content); $("ticket-raw-origins").hidden = !items.length && !files.length;
  }
  async function openRawSettings() {
    if (state.user?.role !== "admin") return;
    $("raw-settings-dialog").showModal(); $("raw-mailbox-form").hidden = true; $("raw-sender-form").hidden = true;
    await loadRawMailboxes();
  }
  async function loadRawMailboxes() {
    const generation = state.generation, cycle = ++state.raw.adminCycle; showError("raw-settings-error", "");
    try {
      const data = await api("/api/raw/mailboxes"); if (generation !== state.generation || cycle !== state.raw.adminCycle || state.user?.role !== "admin") return;
      state.raw.mailboxes = data.items || []; populateRawMailboxFilter(state.raw.mailboxes);
      $("raw-provider-status").textContent = state.raw.mailboxes.some((item) => item.worker?.last_polled_at) ? t("Ultimo controllo del ricevitore: {0}", [rawTime(Math.max(...state.raw.mailboxes.map((item) => Number(item.worker?.last_polled_at) || 0)))]) : t("Ricezione non configurata. Puoi preparare caselle e mittenti; provider e credenziali saranno configurati sul server.");
      const fragment = document.createDocumentFragment();
      for (const mailbox of state.raw.mailboxes) { const button = element("button", "resource-result"); button.type = "button"; button.append(element("strong", "", `${mailbox.name} · ${mailbox.address}`), element("span", "", mailbox.active ? t("Abilitata") : t("Disattivata"))); if (mailbox.worker?.last_error) button.append(element("span", "inline-error", mailbox.worker.last_error)); button.addEventListener("click", () => { if ($("raw-mailbox-form").getAttribute("aria-busy") === "true" || $("raw-sender-form").getAttribute("aria-busy") === "true") return; editRawMailbox(mailbox); loadRawSenders(mailbox.id); }); fragment.append(button); }
      if (!state.raw.mailboxes.length) fragment.append(element("p", "muted", t("Nessuna casella configurata."))); $("raw-mailboxes").replaceChildren(fragment);
    } catch (error) { if (generation === state.generation && cycle === state.raw.adminCycle && $("raw-settings-dialog").open) showError("raw-settings-error", error.message); }
  }
  function renderRawAdminJob(target) {
    const job = state.raw[target + "Job"]; $("raw-" + target + "-job-label").textContent = resourceLabel("job", job); $("raw-" + target + "-job").disabled = !state.punto.connected; $("raw-" + target + "-job-clear").hidden = !job;
  }
  function editRawMailbox(mailbox = null, saved = false) {
    if (!saved && $("raw-mailbox-form").getAttribute("aria-busy") === "true") return;
    state.raw.mailbox = mailbox ? { ...mailbox } : null; state.raw.mailboxJob = mailbox?.job_id ? { id: mailbox.job_id, code: mailbox.job_code || `#${mailbox.job_id}`, name: mailbox.job_name || "" } : null;
    $("raw-mailbox-form").reset(); $("raw-mailbox-form").hidden = false; $("raw-mailbox-address").value = mailbox?.address || ""; $("raw-mailbox-label").value = mailbox?.name || "";
    for (const [id, key, fallback] of [["host", "host", ""], ["port", "port", 993], ["username", "username", ""], ["folder", "folder", "INBOX"], ["auth-mode", "auth_mode", "password"], ["secret-name", "secret_name", ""], ["authserv", "authserv_id", ""]]) $("raw-mailbox-" + id).value = mailbox?.[key] || fallback;
    $("raw-mailbox-enabled").checked = mailbox?.active === true || mailbox?.active === 1; $("raw-mailbox-trust").checked = mailbox?.trust_auth_results === true || mailbox?.trust_auth_results === 1;
    $("raw-sender-new").disabled = !mailbox; renderRawAdminJob("mailbox"); showError("raw-mailbox-error", "");
  }
  async function saveRawMailbox(event) {
    event.preventDefault(); const original = state.raw.mailbox, generation = state.generation;
    const body = { name: $("raw-mailbox-label").value.trim(), address: $("raw-mailbox-address").value.trim(), host: $("raw-mailbox-host").value.trim(), port: Number($("raw-mailbox-port").value) || 993, username: $("raw-mailbox-username").value.trim(), folder: $("raw-mailbox-folder").value.trim() || "INBOX", secret_name: $("raw-mailbox-secret-name").value.trim(), auth_mode: $("raw-mailbox-auth-mode").value, authserv_id: $("raw-mailbox-authserv").value.trim(), trust_auth_results: $("raw-mailbox-trust").checked, active: $("raw-mailbox-enabled").checked, job_id: state.raw.mailboxJob?.id || null };
    if (original) body.revision = original.revision; showError("raw-mailbox-error", ""); busy($("raw-mailbox-form"), $("raw-mailbox-save"), true, t("Salvataggio…"));
    try { const data = await api(original ? `/api/raw/mailboxes/${original.id}` : "/api/raw/mailboxes", { method: original ? "PUT" : "POST", body }); if (generation !== state.generation || !$("raw-settings-dialog").open) return; await loadRawMailboxes(); if (generation !== state.generation || !$("raw-settings-dialog").open) return; if (data.mailbox) { editRawMailbox(data.mailbox, true); loadRawSenders(data.mailbox.id); } toast(t("Casella salvata.")); }
    catch (error) { if (generation === state.generation && state.user) showError("raw-mailbox-error", error.message); }
    finally { if (generation === state.generation) busy($("raw-mailbox-form"), $("raw-mailbox-save"), false); }
  }
  async function loadRawSenders(mailboxId) {
    const generation = state.generation, cycle = ++state.raw.senderCycle; $("raw-sender-form").hidden = true; $("raw-senders").replaceChildren();
    try { const data = await api(`/api/raw/senders?mailbox_id=${mailboxId}`); if (generation !== state.generation || cycle !== state.raw.senderCycle || state.raw.mailbox?.id !== mailboxId || !$("raw-settings-dialog").open) return;
      const fragment = document.createDocumentFragment(); for (const sender of data.items || []) { const button = element("button", "resource-result"); button.type = "button"; button.append(element("strong", "", sender.email), element("span", "", `${sender.active ? t("Abilitato") : t("Disattivato")}${sender.valid_until ? " · " + rawTime(sender.valid_until) : ""}`)); button.addEventListener("click", () => editRawSender(sender)); fragment.append(button); }
      if (!data.items?.length) fragment.append(element("p", "muted", t("Nessun mittente autorizzato."))); $("raw-senders").replaceChildren(fragment); $("raw-sender-hint").textContent = t("Mittenti della casella {0}", [state.raw.mailbox.name]);
    } catch (error) { if (generation === state.generation && cycle === state.raw.senderCycle) showError("raw-settings-error", error.message); }
  }
  function editRawSender(sender = null) {
    if ($("raw-sender-form").getAttribute("aria-busy") === "true") return;
    if (!state.raw.mailbox) return; state.raw.sender = sender ? { ...sender } : null; state.raw.senderJob = sender?.job_id ? { id: sender.job_id, code: sender.job_code || `#${sender.job_id}`, name: sender.job_name || "" } : null;
    $("raw-sender-form").reset(); $("raw-sender-form").hidden = false; $("raw-sender-address").value = sender?.email || ""; $("raw-sender-address").readOnly = Boolean(sender); $("raw-sender-name").value = sender?.name || "";
    $("raw-sender-enabled").checked = sender ? Boolean(sender.active) : true; $("raw-sender-expiry").value = localInput(sender?.valid_until); $("raw-sender-timezone").textContent = t("Orari nel fuso del browser: {0}.", [browserTimezone]); renderRawAdminJob("sender"); showError("raw-sender-error", "");
  }
  async function saveRawSender(event) {
    event.preventDefault(); if (!state.raw.mailbox) return; const original = state.raw.sender, generation = state.generation;
    const expiry = $("raw-sender-expiry").value, candidates = expiry ? localCandidates(expiry) : [];
    if (expiry && candidates.length !== 1) { showError("raw-sender-error", t("Scegli una scadenza con un orario valido e non ripetuto nel cambio d’ora.")); return; }
    const body = { mailbox_id: state.raw.mailbox.id, email: $("raw-sender-address").value.trim(), name: $("raw-sender-name").value.trim(), active: $("raw-sender-enabled").checked, valid_until: expiry ? (original?.valid_until && localInput(original.valid_until) === expiry ? original.valid_until : candidates[0]) : null, job_id: state.raw.senderJob?.id || null };
    if (original) { body.revision = original.revision; delete body.email; } showError("raw-sender-error", ""); busy($("raw-sender-form"), $("raw-sender-save"), true, t("Salvataggio…"));
    try { await api(original ? `/api/raw/senders/${original.id}` : "/api/raw/senders", { method: original ? "PUT" : "POST", body }); if (generation !== state.generation || !$("raw-settings-dialog").open) return; loadRawSenders(body.mailbox_id); toast(t("Mittente salvato.")); }
    catch (error) { if (generation === state.generation && state.user) showError("raw-sender-error", error.message); }
    finally { if (generation === state.generation) busy($("raw-sender-form"), $("raw-sender-save"), false); }
  }

  function preferenceKey() { return `ticket.preferences.v1.${state.user.id}`; }

  function restorePreferences() {
    let saved = {};
    try { saved = JSON.parse(localStorage.getItem(preferenceKey()) || "{}"); } catch { /* Le preferenze sono facoltative. */ }
    if (!saved || typeof saved !== "object") saved = {};
    state.view = ["board", "list", "agenda", "operator"].includes(saved.view) ? saved.view : "board";
    state.queue = Object.hasOwn(queues, saved.queue) ? saved.queue : "active";
    $("status-filter").value = Object.hasOwn(statuses, saved.status) ? saved.status : "";
    $("priority-filter").value = Object.hasOwn(priorities, saved.priority) ? saved.priority : "";
    $("sort-filter").value = ["updated", "priority", "due"].includes(saved.sort) ? saved.sort : "updated";
    $("operations-queue-filter").value = Object.hasOwn(operationsQueues, saved.operations_queue || "") ? saved.operations_queue : "all";
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
      localStorage.setItem(preferenceKey(), JSON.stringify({ view: state.view, queue: state.queue, operations_queue: $("operations-queue-filter").value, sort: $("sort-filter").value, status: $("status-filter").value, priority: $("priority-filter").value, assignee: $("assignee-filter").value }));
    } catch { /* L’app funziona anche senza archivio locale. */ }
  }

  function ticketRoute() {
    const value = new URL(location.href).searchParams.get("ticket");
    return value && /^[1-9]\d*$/.test(value) && Number.isSafeInteger(Number(value)) ? Number(value) : null;
  }

  function ticketURL(id) {
    const url = new URL(location.href);
    url.searchParams.delete("section");
    url.searchParams.delete("raw");
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
          for (const key of ["job_id", "asset_id", "new", "ticket", "view", "queue", "operations_queue", "section", "raw"]) if (params.has(key)) restored.searchParams.set(key, params.get(key));
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
    const requestedView = url.searchParams.get("view");
    if (["board", "list", "agenda", "operator"].includes(requestedView)) state.view = requestedView;
    const requestedOperationsQueue = url.searchParams.get("operations_queue");
    if (Object.hasOwn(operationsQueues, requestedOperationsQueue || "")) $("operations-queue-filter").value = requestedOperationsQueue;
    const requestedQueue = url.searchParams.get("queue"), hasRequestedQueue = Object.hasOwn(queues, requestedQueue || "");
    if (hasRequestedQueue || params.size) {
      state.queue = hasRequestedQueue ? requestedQueue : "all"; state.offset = 0;
      $("status-filter").value = ""; $("priority-filter").value = ""; $("assignee-filter").value = ""; $("search-input").value = "";
    }
    if (!params.size) return;
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
    else if (target === "raw-mailbox" || target === "raw-sender") { const name = target.slice(4); state.raw[name + "Job"] = item; renderRawAdminJob(name); }
    else if (target === "ops-rule" || target === "ops-sla") { const name = target.slice(4); ops[name + "Job"] = item; renderOperationJob(name); }
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
    if (dialog.getAttribute("aria-busy") === "true" || dialog.querySelector('[aria-busy="true"]')) return;
    dialog.close();
  }

  function showLogin(notice = "") {
    state.generation += 1;
    resetOperations();
    resetRawState();
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
    state.items = []; state.agendaItems = []; state.agendaWeek = weekStart(new Date());
    state.calendarRequest?.abort(); state.calendarCycle += 1; state.calendar = null;
    $("agenda-days").replaceChildren(); $("agenda-container").hidden = true;
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
    initRawFilters();
    loadSavedViews();
    $("operations-settings").hidden = state.user.role !== "admin";
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
    await openRawRoute();
    if (!state.user || generation !== state.generation) return;
    await loadTickets();
    const requestedTicket = ticketRoute();
    if (requestedTicket && state.raw.section === "tickets" && state.user && generation === state.generation) await openDetail(requestedTicket, false);
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
    return Boolean(state.resourceFilters.job || state.resourceFilters.asset || state.queue !== "all" || $("operations-queue-filter").value !== "all" || $("search-input").value.trim() || $("status-filter").value || $("priority-filter").value || $("assignee-filter").value || $("sort-filter").value !== "updated");
  }

  function syncFilterDisplay() {
    $("reset-filters").hidden = !filtersActive();
    $("list-heading").textContent = queues[state.queue];
    $("queue-description").textContent = queueDescriptions[state.queue];
    $("assignee-filter").disabled = ["mine", "unassigned"].includes(state.queue);
    for (const button of document.querySelectorAll(".queue-button")) button.setAttribute("aria-pressed", String(button.dataset.queue === state.queue));
    $("view-board").setAttribute("aria-pressed", String(state.view === "board"));
    $("view-list").setAttribute("aria-pressed", String(state.view === "list"));
    $("view-agenda").setAttribute("aria-pressed", String(state.view === "agenda"));
    $("view-operator").setAttribute("aria-pressed", String(state.view === "operator"));
    $("board-hint").hidden = state.view !== "board";
    for (const button of document.querySelectorAll(".stat-card")) button.setAttribute("aria-pressed", String(button.dataset.status === $("status-filter").value));
  }

  function ticketParams() {
    const params = new URLSearchParams({ queue: state.queue, operations_queue: $("operations-queue-filter").value, sort: $("sort-filter").value, today: todayInRome() });
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
    const request = async (query, path = "/api/tickets") => {
      const controller = new AbortController();
      state.listControllers.push(controller);
      return api(`${path}?${query}`, { signal: controller.signal });
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
      } else if (view === "agenda") {
        const from = Math.floor(state.agendaWeek.getTime() / 1000), to = Math.floor(civilDay(state.agendaWeek, 7).getTime() / 1000);
        const countsQuery = new URLSearchParams(params); countsQuery.set("limit", "1"); countsQuery.set("offset", "0");
        const countsPromise = request(countsQuery).then((data) => ({ data }), (error) => ({ error }));
        const items = [], seen = new Set(); let offset = 0, total = 0;
        do {
          const query = new URLSearchParams(params); query.set("from", String(from)); query.set("to", String(to)); query.set("limit", "100"); query.set("offset", String(offset));
          const page = await request(query, "/api/tickets/agenda");
          if (cycle !== state.listCycle || generation !== state.generation) { await countsPromise.catch(() => {}); return; }
          total = Number(page.total) || 0;
          for (const ticket of page.items || []) if (!seen.has(ticket.id)) { seen.add(ticket.id); items.push(ticket); }
          offset += (page.items || []).length;
          if (!page.items?.length && offset < total) throw new Error(t("L’agenda è cambiata durante il caricamento. Aggiorna per visualizzare tutti gli interventi."));
        } while (offset < total);
        const countsResult = await countsPromise; if (countsResult.error) throw countsResult.error;
        const counts = countsResult.data;
        if (cycle !== state.listCycle || generation !== state.generation) return;
        state.agendaItems = items; state.total = items.length; updateCounts(counts); renderAgenda();
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
      updateSelection();
      if (state.raw.section === "tickets" && state.view === "operator" && state.items.length && !ticketRoute() && !$("ticket-detail").open) await openDetail(state.items[0].id);
      if ($("ticket-detail").open) renderOperatorRail();
      $("ticket-total").textContent = state.total.toLocaleString(locale);
      $("list-announcement").textContent = state.total === 1 ? t("1 ticket trovato.") : t("{0} ticket trovati.", [state.total]);
    } catch (error) {
      if (error.name !== "AbortError" && cycle === state.listCycle && generation === state.generation && state.user) {
        if (state.punto.configured) {
          state.board.clear(); state.items = []; state.agendaItems = []; $("agenda-days").replaceChildren(); $("ticket-board").replaceChildren(); $("ticket-rows").replaceChildren();
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
    top.append(selectionControl(ticket));
    top.append(element("span", "ticket-id", `#${ticket.id}`), element("span", `board-priority priority-${ticket.priority}`, priorities[ticket.priority] || ticket.priority));
    card.append(top, ticketLink(ticket, "board-ticket-title"));
    if (ticket.requester) card.append(element("p", "board-requester", ticket.requester));
    card.append(resourceBadges(ticket));
    const scheduled = scheduleBadge(ticket); if (scheduled) card.append(scheduled);
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
    $("agenda-container").hidden = true;
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
    $("agenda-container").hidden = true;
    const rows = document.createDocumentFragment();
    for (const ticket of items) {
      const row = element("tr");
      const selection = element("td", "selection-column"); selection.append(selectionControl(ticket)); row.append(selection);
      row.append(element("td", "ticket-id-cell ticket-id", `#${ticket.id}`));
      const titleCell = element("td", "ticket-title-cell");
      const title = ticketLink(ticket, "ticket-title-button");
      titleCell.append(title, element("span", "ticket-requester", ticket.requester || t("Nessun richiedente indicato")));
      titleCell.append(resourceBadges(ticket));
      const scheduled = scheduleBadge(ticket); if (scheduled) titleCell.append(scheduled);
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
    const url = new URL(location.href); url.searchParams.delete("job_id"); url.searchParams.delete("asset_id"); url.searchParams.delete("operations_queue"); history.replaceState(null, "", url);
    renderResourceChoices();
    state.offset = 0;
    loadTickets();
  }

  async function openDetail(id, syncRoute = true) {
    if (syncRoute && ticketRoute() !== id) history.pushState(null, "", ticketURL(id));
    if (state.detailId && state.detailId !== id) saveLocalDraft();
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
    renderRawOrigins(data);
    $("detail-status").value = ticket.status;
    $("detail-priority").value = ticket.priority;
    $("detail-due-date").value = ticket.due_date || "";
    populateSchedule("detail", ticket);
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
      if (event.detail) item.append(element("p", "history-detail", readableHistoryDetail(event)));
      const date = element("time", "history-time", time(event.created_at));
      date.dateTime = event.created_at;
      item.append(date);
      history.append(item);
    }
    if (!data.history?.length) history.append(element("li", "comments-empty", t("Nessuna attività registrata.")));
    $("history-list").replaceChildren(history);
    $("detail-content").hidden = false;
    loadTicketCalendar(ticket);
    loadOperationsPanels(ticket);
    startPresence(ticket);
  }

  function populateEditor(ticket = null) {
    state.raw.convert = null; $("raw-convert-attachments").hidden = true;
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
    populateSchedule("ticket", ticket);
    fillAssessment("editor", ticket || {});
    refreshAssessmentHint("editor");
    $("editor-draft-status").textContent = "";
    populateAssignees($("ticket-assignee"), ticket?.assignee_id ?? "", ticket?.assignee_name);
    showError("editor-error", "");
    $("reload-editor").hidden = true;
    renderResourceChoices();
  }

  function openEditor(ticket = null) {
    populateEditor(ticket);
    $("ticket-editor").showModal();
    $("ticket-title").focus();
    if (ticket && state.detailId === ticket.id) refreshPresence(ticket, true);
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
      job_id: state.resources.job?.id || null, asset_id: state.resources.asset?.id || null, ...scheduleValues("ticket", state.editorTicket), ...assessmentValues("editor")
    };
  }

  async function saveTicket(event) {
    event.preventDefault();
    const form = $("ticket-form");
    const original = state.editorTicket, rawConvert = state.raw.convert ? { ...state.raw.convert } : null, generation = state.generation;
    let values;
    try { values = editorValues(); if (rawConvert) validateRawAttachments(rawAttachmentIds()); } catch (error) { showError("editor-error", error.message); return; }
    if (!values.title) { showError("editor-error", t("Aggiungi un titolo per il ticket.")); $("ticket-title").focus(); return; }
    let body = values;
    if (original) {
      values.status = $("ticket-status").value;
      body = scheduleChanges(values, original);
      if (!Object.keys(body).length) { $("ticket-editor").close(); return; }
      body.revision = original.revision;
    }
    showError("editor-error", "");
    busy(form, $("save-ticket"), true, t("Salvataggio…"));
    try {
      const data = await api(rawConvert ? `/api/raw/messages/${rawConvert.id}/convert` : original ? `/api/tickets/${original.id}` : "/api/tickets", { method: original ? "PUT" : "POST", body: rawConvert ? { revision: rawConvert.revision, ticket: body, attachment_ids: rawAttachmentIds() } : body });
      if (generation !== state.generation || !state.user) return;
      clearTimeout(ops.draftTimer);
      if (original && ops.serverDrafts.get(original.id)) { const draft = ops.serverDrafts.get(original.id); opApi(`/api/tickets/${original.id}/draft/discard`, { method: "POST", body: { revision: draft.revision } }).then(() => { if (generation === state.generation) { ops.serverDrafts.delete(original.id); ops.serverDraft = null; } }).catch(() => {}); }
      if (rawConvert) { state.raw.convert = null; $("raw-detail").close(); showWorkspace("tickets"); loadRawMessages(); }
      $("ticket-editor").close();
      toast(original ? t("Ticket aggiornato.") : t("Ticket #{0} creato.", [data.ticket.id]));
      await loadTickets();
      if (original && $("ticket-detail").open) await loadDetail(original.id);
      else if (!original) await openDetail(data.ticket.id);
    } catch (error) {
      if (state.user && generation === state.generation) {
        showError("editor-error", error.status === 409 ? t("Un’altra persona ha aggiornato il ticket. Ricarica l’ultima versione prima di modificarlo; il contenuto di questo modulo sarà sostituito.") : error.message);
        if (rawConvert && error.status === 409) { showError("editor-error", t("Il messaggio è cambiato. Chiudi il modulo e riapri il messaggio prima di convertirlo.")); }
        $("reload-editor").hidden = Boolean(rawConvert) || error.status !== 409;
      }
    } finally { if (generation === state.generation) busy(form, $("save-ticket"), false); }
  }

  async function saveProperties(event) {
    event.preventDefault();
    if (!state.detail) return;
    const ticket = state.detail.ticket;
    let body;
    try { const { slot, ...draft } = propertyDraft(); body = scheduleChanges({ ...draft, ...assessmentValues("detail"), ...scheduleValues("detail", ticket) }, ticket); }
    catch (error) { showError("properties-error", error.message); return; }
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
    const draftProperties = propertyDraft();
    const hasDraftProperties = propertiesDirty(state.detail.ticket, draftProperties);
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
          restorePropertyDraft(draftProperties);
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
    const draft = previous ? propertyDraft() : null;
    const dirty = previous && propertiesDirty(previous, draft);
    $("refresh-detail").disabled = true;
    showError("properties-error", "");
    try {
      const data = await loadDetail(state.detailId);
      if (data && dirty) {
        if (data.ticket.revision === previous.revision) {
          restorePropertyDraft(draft);
        } else showError("properties-error", t("Il ticket è cambiato. Abbiamo caricato l’ultima versione: verifica e ripeti le modifiche non salvate."));
      }
      if (data) toast(t("Ticket aggiornato all’ultima versione."));
    } finally { $("refresh-detail").disabled = false; }
  }

  async function takeTicket() {
    if (!state.detail || !state.user) return;
    const ticket = state.detail.ticket, generation = state.generation;
    if (propertiesDirty(ticket)) {
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

  const ops = { cycle: 0, dashboardCycle: 0, selected: new Map(), templates: [], views: [], conversation: null, message: null, time: null, timeKey: null, timePayload: null, presenceTimer: null, presenceId: null, dialogAction: null, draftTimer: null, drafts: new Map(), serverDrafts: new Map(), draftQueue: Promise.resolve(), rules: [], policies: [], ruleJob: null, slaJob: null };
  async function opApi(path, options) { const generation = state.generation; try { const data = await api(path, options); if (generation !== state.generation || !state.user) throw new DOMException("Session changed", "AbortError"); return data; } catch (error) { if (generation !== state.generation || !state.user) throw new DOMException("Session changed", "AbortError"); throw error; } }
  function opButton(label, action, className = "button button-secondary") { const node = element("button", className, label); node.type = "button"; if (action) node.addEventListener("click", action); return node; }
  function opField(id, label, type = "text", options = {}) {
    const group = element("div", "field"), caption = element("label", "", label); caption.htmlFor = id;
    const input = element(type === "textarea" ? "textarea" : type === "select" ? "select" : "input"); input.id = id; if (!["textarea", "select"].includes(type)) input.type = type;
    for (const [key, value] of Object.entries(options)) { if (key === "items") for (const [code, text] of value) addOption(input, code, text); else input[key] = value; }
    group.append(caption, input); return group;
  }
  function opNotice(text) { return element("p", "field-hint", text); }
  function opError(parent, error) { const node = element("p", "inline-error", error.message || String(error)); node.setAttribute("role", "alert"); parent.replaceChildren(node); }
  function operationDialog(title, content, label, action) {
    const dialog = $("operations-dialog"); $("operations-dialog-title").textContent = title; $("operations-dialog-content").replaceChildren(content); showError("operations-dialog-error", "");
    ops.dialogAction = action; $("operations-dialog-confirm").textContent = label || t("Conferma"); $("operations-dialog-confirm").hidden = !action; $("operations-dialog-confirm").disabled = false;
    if (!dialog.open) dialog.showModal();
  }
  function opCurrent(ticket, cycle, generation) { return generation === state.generation && cycle === ops.cycle && state.detailId === ticket.id && $("ticket-detail").open && state.user; }
  function resetOperations() {
    ops.cycle += 1; ops.dashboardCycle += 1; ops.selected.clear(); ops.conversation = null; ops.message = null; ops.templates = []; ops.views = []; ops.rules = []; ops.policies = []; ops.ruleJob = null; ops.slaJob = null; ops.editTemplate = null; ops.time = null; ops.timeKey = null; ops.timePayload = null; ops.drafts.clear(); ops.serverDrafts.clear(); ops.serverDraft = null; ops.draftQueue = Promise.resolve(); ops.dialogAction = null;
    clearInterval(ops.presenceTimer); clearTimeout(ops.draftTimer); ops.presenceTimer = null; ops.presenceId = null;
    for (const id of ["operations-ticket-panel", "customer-conversation-panel", "ticket-time-panel", "ticket-solution-panel", "dashboard-content", "operations-dialog-content", "operator-ticket-rail"]) $(id)?.replaceChildren();
    $("detail-presence").textContent = ""; $("operations-dashboard").hidden = true; $("nav-dashboard").setAttribute("aria-pressed", "false"); updateSelection();
  }
  function buildAssessment(prefix) {
    const fragment = document.createDocumentFragment(), grid = element("div", "form-grid");
    const levels = [["low", t("Basso")], ["medium", t("Medio")], ["high", t("Alto")]];
    grid.append(opField(prefix + "-impact", t("Impatto"), "select", { items: levels }), opField(prefix + "-urgency", t("Urgenza"), "select", { items: levels }));
    grid.append(opField(prefix + "-category", t("Categoria"), "text", { maxLength: 80 }), opField(prefix + "-group", t("Gruppo di lavoro"), "text", { maxLength: 80 }));
    fragment.append(grid, opNotice(t("La priorità suggerita combina l’impatto del problema e l’urgenza dell’intervento.")), element("p", "field-hint", "")); fragment.lastChild.id = prefix + "-priority-suggestion";
    fragment.append(opField(prefix + "-override", t("Motivo della priorità diversa dal suggerimento"), "textarea", { maxLength: 500, rows: 2 }));
    fragment.append(opField(prefix + "-waiting", t("Motivo dell’attesa"), "select", { items: [["", t("Nessuno")], ["customer", t("Risposta del cliente")], ["supplier", t("Risposta del fornitore")], ["spare_part", t("Ricambio")], ["authorization", t("Autorizzazione")]] }));
    fragment.append(opField(prefix + "-follow-up", t("Ricontatto"), "datetime-local"), opNotice(t("Ricontatto nel fuso del browser: {0}.", [browserTimezone])));
    return fragment;
  }
  function fillAssessment(prefix, ticket = {}) {
    $(prefix + "-impact").value = ticket.impact || "medium"; $(prefix + "-urgency").value = ticket.urgency || "medium";
    $(prefix + "-category").value = ticket.category || ""; $(prefix + "-group").value = ticket.group_name || ""; $(prefix + "-override").value = ticket.priority_override_reason || "";
    $(prefix + "-waiting").value = ticket.waiting_reason || ""; $(prefix + "-follow-up").value = localInput(ticket.follow_up_at);
    $(prefix + "-priority-suggestion").textContent = ticket.proposed_priority ? t("Priorità suggerita: {0}", [priorities[ticket.proposed_priority] || ticket.proposed_priority]) : "";
  }
  function assessmentValues(prefix, strict = true) {
    const follow = $(prefix + "-follow-up").value, candidates = follow ? localCandidates(follow) : [];
    if (strict && follow && candidates.length !== 1) throw new Error(t("Scegli un orario di ricontatto valido e non ambiguo nel fuso del browser."));
    return { impact: $(prefix + "-impact").value, urgency: $(prefix + "-urgency").value, category: $(prefix + "-category").value.trim(), group_name: $(prefix + "-group").value.trim(), priority_override_reason: $(prefix + "-override").value.trim(), waiting_reason: $(prefix + "-waiting").value || null, follow_up_at: follow && candidates.length === 1 ? candidates[0] : null };
  }
  function updateSelection() {
    $("selection-count").textContent = ops.selected.size ? t("{0} ticket selezionati", [ops.selected.size]) : ""; $("bulk-open").disabled = !ops.selected.size || ops.selected.size > 100;
    const visible = visibleTickets(); $("select-page").checked = visible.length > 0 && visible.every((item) => ops.selected.has(item.id)); $("select-page").indeterminate = visible.some((item) => ops.selected.has(item.id)) && !$("select-page").checked;
  }
  function visibleTickets() { return state.view === "board" ? Array.from(state.board.values()).flatMap((column) => column.items) : state.view === "agenda" ? state.agendaItems : state.items; }
  function selectionControl(ticket) {
    const input = element("input", "ticket-selection"); input.type = "checkbox"; input.checked = ops.selected.has(ticket.id); input.setAttribute("aria-label", t("Seleziona ticket #{0}", [ticket.id]));
    input.addEventListener("change", () => { if (input.checked) ops.selected.set(ticket.id, ticket.revision); else ops.selected.delete(ticket.id); updateSelection(); }); return input;
  }
  function setupOperatorDialog() {
    const dialog = $("ticket-detail"), rail = element("aside", "operator-ticket-rail"); rail.id = "operator-ticket-rail"; rail.setAttribute("aria-label", t("Ticket nella vista corrente"));
    const content = element("div", "operator-detail-content"); while (dialog.firstChild) content.append(dialog.firstChild); dialog.append(rail, content);
  }
  function renderOperatorRail() {
    const items = visibleTickets(), rail = $("operator-ticket-rail"), fragment = document.createDocumentFragment();
    for (const ticket of items) { const button = opButton(`#${ticket.id} · ${ticket.title}`, () => navigateDetail(ticket.id), "operator-ticket-choice"); button.setAttribute("aria-current", ticket.id === state.detailId ? "true" : "false"); fragment.append(button); }
    rail.replaceChildren(fragment); $("ticket-detail").classList.toggle("operator-dialog", state.view === "operator");
    $("operator-navigation").hidden = false; const index = items.findIndex((ticket) => ticket.id === state.detailId);
    $("detail-previous").disabled = index <= 0; $("detail-next").disabled = index < 0 || index >= items.length - 1; $("detail-position").textContent = index >= 0 ? `${index + 1} / ${items.length}` : "";
  }
  async function navigateDetail(id) { if (id === state.detailId) return; saveLocalDraft(); await openDetail(id); }
  function stepDetail(direction) { const items = visibleTickets(), index = items.findIndex((ticket) => ticket.id === state.detailId); if (index >= 0 && items[index + direction]) navigateDetail(items[index + direction].id); }
  function saveLocalDraft() { if (!state.user || !state.detailId) return; ops.drafts.set(state.detailId, { comment: $("comment-body").value, message: $("customer-body")?.dataset.ticketId === String(state.detailId) ? $("customer-body").value : ops.drafts.get(state.detailId)?.message || "" }); }
  async function loadOperationsPanels(ticket) {
    if (state.detailId === ticket.id && $("customer-body")?.dataset.ticketId === String(ticket.id)) saveLocalDraft();
    const cycle = ++ops.cycle, generation = state.generation; ops.conversation = null; ops.message = null; ops.time = null; ops.timeKey = null; ops.timePayload = null; ops.serverDraft = ops.serverDrafts.get(ticket.id) || null;
    renderOperatorRail(); fillAssessment("detail", ticket); renderSla(ticket); renderConversationLoading();
    for (const id of ["ticket-time-panel", "ticket-solution-panel"]) $(id).replaceChildren(opNotice(t("Caricamento…")));
    const draft = ops.drafts.get(ticket.id); if (draft?.comment) $("comment-body").value = draft.comment;
    const tasks = [
      opApi(`/api/tickets/${ticket.id}/conversation`).then((data) => { if (opCurrent(ticket, cycle, generation)) { ops.conversation = data; renderCustomerConversation(ticket, data, draft); } }, (error) => { if (opCurrent(ticket, cycle, generation)) opError($("customer-conversation-panel"), error); }),
      opApi(`/api/tickets/${ticket.id}/time`).then((data) => { if (opCurrent(ticket, cycle, generation)) { ops.time = data; renderTicketTime(ticket, data); } }, (error) => { if (opCurrent(ticket, cycle, generation)) opError($("ticket-time-panel"), error); }),
      Promise.all([opApi("/api/response-templates"), opApi("/api/solutions?limit=20")]).then(([templates, solutions]) => { if (opCurrent(ticket, cycle, generation)) { ops.templates = templates.items || []; renderSolutions(ticket, solutions.items || []); populateMessageTemplates(); } }, (error) => { if (opCurrent(ticket, cycle, generation)) opError($("ticket-solution-panel"), error); })
    ];
    await Promise.allSettled(tasks);
  }
  function readableHistoryDetail(event) {
    const detail = typeof event.detail === "string" ? event.detail : JSON.stringify(event.detail);
    const levels = { low: t("Basso"), medium: t("Medio"), high: t("Alto") }, waiting = { customer: t("Risposta del cliente"), supplier: t("Risposta del fornitore"), spare_part: t("Ricambio"), authorization: t("Autorizzazione") };
    if (event.action === "operations_updated") return detail.replace(/(Impatto |urgenza )(low|medium|high)\b/g, (_, label, value) => label + levels[value]).replace(/priorità proposta (low|normal|high|urgent)\b/g, (_, value) => t("Priorità suggerita: {0}", [priorities[value]])).replace(/[Aa]ttesa:? (customer|supplier|spare_part|authorization)\b/g, (_, value) => t("Motivo dell’attesa") + ": " + waiting[value]);
    if (event.action === "bulk_updated") { try { const changes = JSON.parse(detail), names = { status: t("Stato"), priority: t("Priorità"), assignee_id: t("Assegnato a"), category: t("Categoria"), group_name: t("Gruppo di lavoro"), waiting_reason: t("Motivo dell’attesa"), follow_up_at: t("Ricontatto"), priority_override_reason: t("Motivo della priorità diversa dal suggerimento") }; return Object.entries(changes).map(([key, value]) => `${names[key] || key}: ${key === "status" ? statuses[value] : key === "priority" ? priorities[value] : key === "waiting_reason" ? waiting[value] || "—" : key === "follow_up_at" ? rawTime(value) : key === "assignee_id" ? state.users.find((user) => user.id === value)?.name || t("Non assegnato") : value || "—"}`).join(" · "); } catch { return detail; } }
    return detail;
  }
  function renderSla(ticket) {
    const panel = $("operations-ticket-panel"), fragment = document.createDocumentFragment(); fragment.append(element("h3", "", t("Operatività e SLA")));
    const sla = ticket.sla;
    if (sla) {
      fragment.append(opNotice(sla.policy_name || t("Nessuna politica SLA")));
      const grid = element("dl", "ticket-meta");
      const rows = [[t("Prima risposta entro"), sla.response_due_at ? rawTime(sla.response_due_at) : "—"], [t("Risoluzione entro"), sla.resolution_due_at ? rawTime(sla.resolution_due_at) : "—"], [t("Prima risposta"), sla.first_response_at ? rawTime(sla.first_response_at) : "—"], [t("Riaperture"), sla.reopened_count || 0], [t("Attesa RAW"), t("{0} minuti", [sla.raw_wait_minutes || 0])]];
      for (const [label, value] of rows) { const item = element("div"); item.append(element("dt", "", label), element("dd", "", value)); grid.append(item); } fragment.append(grid);
      if (sla.response_breached || sla.resolution_breached) fragment.append(element("p", "inline-error", t("Obiettivo SLA superato")));
      if (sla.paused) fragment.append(opNotice(t("Il conteggio della risoluzione è sospeso durante l’attesa; la prima risposta continua.")));
    } else fragment.append(opNotice(t("Nessuna politica SLA")));
    if (ticket.waiting_reason || ticket.follow_up_at) fragment.append(opNotice(t("Ricontatto: {0}", [ticket.follow_up_at ? rawTime(ticket.follow_up_at) : "—"])));
    fragment.append(opButton(t("Verifica suggerimenti di classificazione"), () => previewClassification(ticket)));
    panel.replaceChildren(fragment);
  }
  function renderConversationLoading() { $("customer-conversation-panel").replaceChildren(element("h3", "", t("Conversazione con il cliente")), opNotice(t("Caricamento…"))); }
  function populateMessageTemplates() {
    const select = $("customer-template"); if (!select) return; const selected = select.value; select.replaceChildren(); addOption(select, "", t("Scegli un modello")); for (const item of ops.templates.filter((item) => item.active !== false)) addOption(select, item.id, item.title); select.value = selected;
  }
  function renderCustomerConversation(ticket, data, draft) {
    const fragment = document.createDocumentFragment(); fragment.append(element("h3", "", t("Conversazione con il cliente")), opNotice(t("Le note interne restano visibili soltanto al team. Qui prepari le comunicazioni al cliente.")));
    const configured = Boolean(data.email?.configured && data.email?.smtp_configured !== false);
    if (!configured) fragment.append(element("p", "inline-notice", t("Invio email non configurato. Puoi preparare e salvare bozze; nessun messaggio sarà inviato.")));
    const messages = element("div", "customer-messages");
    const labels = { draft: t("Bozza"), sending: t("Invio in corso"), sent: t("Inviato"), unknown: t("Esito da verificare"), failed: t("Invio non riuscito"), received: t("Ricevuto") };
    for (const message of data.items || []) {
      const card = element("article", "customer-message"); card.append(element("strong", "", message.subject || t("Senza oggetto")), element("span", "badge", labels[message.state] || message.state), opNotice(`${message.direction === "incoming" ? message.from_email || "" : message.to_email || ""} · ${rawTime(message.created_at)}`), element("p", "preserve-lines", message.body));
      if (["draft", "failed"].includes(message.state)) card.append(opButton(t("Modifica bozza"), () => fillCustomerDraft(message)));
      if (["draft", "failed"].includes(message.state) && configured) card.append(opButton(t("Anteprima e invio"), () => previewCustomerMessage(ticket, message)));
      if (message.state === "unknown") card.append(opNotice(t("L’esito non è noto. Non creare un nuovo invio prima di aver verificato il provider.")));
      messages.append(card);
    }
    if (!data.items?.length) messages.append(opNotice(t("Nessuna comunicazione al cliente."))); fragment.append(messages);
    const form = element("form", "customer-message-form"); form.id = "customer-form";
    form.append(opField("customer-to", t("Destinatario"), "email", { required: true, maxLength: 254 }), opField("customer-subject", t("Oggetto"), "text", { required: true, maxLength: 250 }), opField("customer-template", t("Modello di risposta"), "select"), opField("customer-body", t("Messaggio al cliente"), "textarea", { required: true, maxLength: 8000, rows: 5 }));
    const error = element("p", "inline-error"); error.id = "customer-error"; error.hidden = true; error.setAttribute("role", "alert");
    const save = opButton(t("Salva bozza"), null, "button button-primary"); save.type = "submit"; save.id = "customer-save"; form.append(error, save, opButton(t("Nuova bozza"), () => fillCustomerDraft(null)), opButton(t("Gestisci modelli"), openResponseTemplates));
    form.addEventListener("submit", (event) => { event.preventDefault(); saveCustomerDraft(ticket); }); fragment.append(form); $("customer-conversation-panel").replaceChildren(fragment);
    $("customer-body").dataset.ticketId = String(ticket.id);
    $("customer-to").value = data.recipient?.email || ""; $("customer-subject").value = ticket.title; if (draft?.message) $("customer-body").value = draft.message;
    $("customer-template").addEventListener("change", () => { const item = ops.templates.find((item) => String(item.id) === $("customer-template").value); if (item) { $("customer-subject").value = item.subject || ticket.title; $("customer-body").value = item.body || ""; } }); populateMessageTemplates();
  }
  function fillCustomerDraft(message) { ops.message = message; $("customer-to").value = message?.to_email || ops.conversation?.recipient?.email || ""; $("customer-subject").value = message?.subject || state.detail?.ticket.title || ""; $("customer-body").value = message?.body || ""; $("customer-save").textContent = message ? t("Aggiorna bozza") : t("Salva bozza"); $("customer-body").focus(); }
  async function saveCustomerDraft(ticket) {
    const generation = state.generation, cycle = ops.cycle, message = ops.message, body = { to_email: $("customer-to").value.trim(), subject: $("customer-subject").value.trim(), body: $("customer-body").value.trim() };
    if (new TextEncoder().encode(body.subject).length > 250 || new TextEncoder().encode(body.body).length > 8192) { showError("customer-error", t("Oggetto e testo superano il limite di invio. Accorcia il messaggio, soprattutto se contiene caratteri multibyte.")); return; }
    if (message) body.revision = message.revision; const form = $("customer-form"); busy(form, $("customer-save"), true, t("Salvataggio…")); showError("customer-error", "");
    try { await opApi(`/api/tickets/${ticket.id}/messages${message ? "/" + message.id : ""}`, { method: message ? "PUT" : "POST", body }); if (!opCurrent(ticket, cycle, generation)) return; ops.drafts.delete(ticket.id); await refreshConversation(ticket); toast(t("Bozza salvata. Nessuna email inviata.")); }
    catch (error) { if (opCurrent(ticket, cycle, generation)) showError("customer-error", error.message); }
    finally { if (form.isConnected && generation === state.generation) busy(form, form.querySelector("[type=submit]"), false); }
  }
  async function refreshConversation(ticket) { const cycle = ops.cycle, generation = state.generation, data = await opApi(`/api/tickets/${ticket.id}/conversation`); if (opCurrent(ticket, cycle, generation)) { ops.conversation = data; ops.message = null; renderCustomerConversation(ticket, data); } }
  function previewCustomerMessage(ticket, message) {
    const content = element("div"); content.append(opNotice(t("Controlla destinatario e testo prima dell’invio.")), element("p", "", `${t("Destinatario")}: ${message.to_email}`), element("h3", "", message.subject), element("pre", "message-preview", message.body));
    operationDialog(t("Conferma invio al cliente"), content, t("Invia email"), async () => { const generation = state.generation, cycle = ops.cycle; const data = await opApi(`/api/tickets/${ticket.id}/messages/${message.id}/send`, { method: "POST", body: { revision: message.revision } }); if (!opCurrent(ticket, cycle, generation)) return; $("operations-dialog").close(); await refreshConversation(ticket); toast(data.message?.state === "sent" ? t("Email inviata.") : t("Invio registrato. Verifica lo stato nella conversazione.")); });
  }
  function randomKey() { return Array.from(crypto.getRandomValues(new Uint8Array(32)), (value) => value.toString(16).padStart(2, "0")).join(""); }
  function renderTicketTime(ticket, data) {
    const retryable = (data.items || []).filter((entry) => entry.can_retry && /^(pending|unknown)$/.test(entry.state) && /^[a-f0-9]{64}$/.test(entry.idempotency_key || ""));
    const selected = retryable.find((entry) => entry.idempotency_key === ops.timeKey);
    const attempt = selected || (ops.timeKey && ops.timePayload ? JSON.parse(ops.timePayload) : null);
    const requiresResume = retryable.length > 0 && !attempt;
    const fragment = document.createDocumentFragment(); fragment.append(element("h3", "", t("Ore effettive")), opNotice(t("La programmazione in agenda è una proposta: conferma il tempo realmente lavorato prima di registrarlo in Punto.")));
    if (data.connected && state.punto.user_name) fragment.append(opNotice(t("Ore registrate come: {0}", [state.punto.user_name])));
    fragment.append(opNotice(ticket.job_id ? t("Commessa: {0}", [resourceLabel("job", ticketResource(ticket, "job"))]) : t("Collega una commessa al ticket per registrare le ore.")));
    for (const entry of data.items || []) {
      fragment.append(opNotice(`${entry.date} · ${t("{0} minuti", [entry.minutes])} · ${{ pending: t("In coda"), applied: t("Registrato"), unknown: t("Esito da verificare"), failed: t("Non riuscito") }[entry.state] || entry.state} · ${entry.description}${entry.error ? " · " + entry.error : ""}`));
      if (retryable.includes(entry)) {
        const resume = opButton(t("Riprendi questa registrazione"), () => {
          ops.timeKey = entry.idempotency_key;
          ops.timePayload = JSON.stringify({ date: entry.date, minutes: entry.minutes, description: entry.description, confirmed: true });
          renderTicketTime(ticket, data);
          $("time-confirmed").focus();
        });
        resume.dataset.timeResume = String(entry.id); fragment.append(resume);
      }
    }
    if (!data.configured) fragment.append(element("p", "inline-notice", t("Registrazione ore su Punto non configurata.")));
    if (data.configured && (!data.connected || !data.time_access)) fragment.append(opButton(t("Autorizza registrazione ore"), () => startPuntoConnection(Boolean(state.punto.calendar_access), true)));
    if (data.source_error) fragment.append(opNotice(data.source_error));
    if (requiresResume || attempt) fragment.append(opNotice(t("Riprendi la registrazione precedente per verificarne l’esito, senza creare nuove ore.")));
    const form = element("form"); form.id = "ticket-time-form"; const suggestion = attempt || data.suggestion || {};
    form.append(opField("time-date", t("Data del lavoro"), "date", { required: true, value: suggestion.date || todayInRome() }), opField("time-minutes", t("Minuti effettivi"), "number", { required: true, min: 1, max: 1440, value: suggestion.minutes || 60 }), opField("time-description", t("Descrizione del lavoro svolto"), "textarea", { required: true, rows: 2, maxLength: 1000, value: suggestion.description || ticket.title }));
    for (const input of form.querySelectorAll("input, textarea")) { input.readOnly = Boolean(attempt); input.disabled = requiresResume; }
    const confirmed = element("label", "raw-check"), checkbox = element("input"); checkbox.type = "checkbox"; checkbox.required = true; checkbox.id = "time-confirmed"; checkbox.disabled = requiresResume; confirmed.append(checkbox, element("span", "", t("Confermo data, durata effettiva, descrizione e commessa."))); form.append(confirmed);
    const error = element("p", "inline-error"); error.id = "time-error"; error.hidden = true; error.setAttribute("role", "alert"); const save = opButton(attempt ? t("Verifica e riprendi registrazione") : t("Registra ore in Punto"), null, "button button-primary"); save.id = "time-save"; save.type = "submit"; const allowed = Boolean(ticket.job_id && data.configured && data.connected && data.time_access && !requiresResume); save.disabled = !allowed || !checkbox.checked; checkbox.addEventListener("change", () => { save.disabled = !allowed || !checkbox.checked; }); form.append(error, save); fragment.append(form); $("ticket-time-panel").replaceChildren(fragment);
    form.addEventListener("submit", (event) => { event.preventDefault(); submitTicketTime(ticket); });
  }
  async function submitTicketTime(ticket) {
    const generation = state.generation, cycle = ops.cycle, form = $("ticket-time-form"); const value = { date: $("time-date").value, minutes: Number($("time-minutes").value), description: $("time-description").value.trim(), confirmed: $("time-confirmed").checked };
    if (!value.confirmed) return; const serialized = JSON.stringify(value); if (ops.timeKey && ops.timePayload !== serialized) { showError("time-error", t("Verifica l’esito della precedente registrazione prima di cambiare i dati.")); return; }
    ops.timeKey ||= randomKey(); ops.timePayload = serialized; busy(form, $("time-save"), true, t("Salvataggio…")); showError("time-error", "");
    try { const result = await opApi(`/api/tickets/${ticket.id}/time`, { method: "POST", body: { ...value, idempotency_key: ops.timeKey } }); if (!opCurrent(ticket, cycle, generation)) return; const data = await opApi(`/api/tickets/${ticket.id}/time`); if (!opCurrent(ticket, cycle, generation)) return; ops.time = data; if (result.entry?.state === "applied") { ops.timeKey = null; ops.timePayload = null; } renderTicketTime(ticket, data); toast(result.entry?.state === "applied" ? t("Ore registrate in Punto.") : t("Registrazione salvata. Verifica lo stato prima di riprovare.")); }
    catch (error) {
      if (opCurrent(ticket, cycle, generation)) {
        if (error.status >= 400 && error.status < 500) { ops.timeKey = null; ops.timePayload = null; }
        try { const data = await opApi(`/api/tickets/${ticket.id}/time`); if (!opCurrent(ticket, cycle, generation)) return; ops.time = data; renderTicketTime(ticket, data); $("time-date").value = value.date; $("time-minutes").value = value.minutes; $("time-description").value = value.description; } catch { /* Il tentativo resta disponibile con la stessa chiave. */ }
        if (opCurrent(ticket, cycle, generation)) showError("time-error", error.message);
      }
    }
    finally { if (form.isConnected && generation === state.generation) busy(form, form.querySelector("[type=submit]"), false); }
  }
  function renderSolutions(ticket, items) {
    const fragment = document.createDocumentFragment(); fragment.append(element("h3", "", t("Soluzioni e modelli")));
    const search = opField("solution-search", t("Cerca soluzioni"), "search", { maxLength: 200 }), results = element("div", "solution-results"); results.id = "solution-results";
    const render = (entries) => { results.replaceChildren(); for (const item of entries) { const card = element("article", "customer-message"); card.append(element("strong", "", item.title), element("p", "preserve-lines", item.body), opButton(t("Usa nella risposta"), () => { if ($("customer-body")) { $("customer-body").value = item.body; $("customer-body").focus(); } })); results.append(card); } if (!entries.length) results.append(opNotice(t("Nessuna soluzione trovata."))); };
    render(items); fragment.append(search, opButton(t("Cerca"), async () => { const generation = state.generation, cycle = ops.cycle; try { const data = await opApi(`/api/solutions?limit=20&search=${encodeURIComponent($("solution-search").value.trim())}`); if (opCurrent(ticket, cycle, generation)) render(data.items || []); } catch (error) { if (opCurrent(ticket, cycle, generation)) opError(results, error); } }), results);
    if (["resolved", "closed"].includes(ticket.status)) fragment.append(opButton(t("Salva soluzione da questo ticket"), () => createSolution(ticket))); else fragment.append(opNotice(t("Puoi pubblicare una soluzione dopo la risoluzione del ticket.")));
    $("ticket-solution-panel").replaceChildren(fragment);
  }
  function createSolution(ticket) { const content = element("div"); content.append(opField("solution-title", t("Titolo"), "text", { maxLength: 160, value: ticket.title }), opField("solution-body", t("Soluzione"), "textarea", { maxLength: 8000, rows: 6, value: ticket.description || "" })); operationDialog(t("Salva soluzione"), content, t("Salva"), async () => { await opApi(`/api/tickets/${ticket.id}/solution`, { method: "POST", body: { title: $("solution-title").value.trim(), body: $("solution-body").value.trim() } }); $("operations-dialog").close(); if (state.detailId === ticket.id) loadOperationsPanels(ticket); }); }
  function openResponseTemplates() {
    const content = element("div"), list = element("div");
    const edit = (item) => { $("template-title").value = item?.title || ""; $("template-subject").value = item?.subject || ""; $("template-body").value = item?.body || ""; $("template-active").value = String(item?.active !== false); ops.editTemplate = item || null; };
    for (const item of ops.templates) list.append(opButton(item.title, () => edit(item), "button button-quiet"));
    content.append(list, opButton(t("Nuovo modello"), () => edit(null)), opField("template-title", t("Titolo"), "text", { maxLength: 160 }), opField("template-subject", t("Oggetto"), "text", { maxLength: 250 }), opField("template-body", t("Testo della risposta"), "textarea", { maxLength: 8000, rows: 6 }), opField("template-active", t("Abilitata"), "select", { items: [["true", t("Sì")], ["false", t("No")]] }));
    ops.editTemplate = null; operationDialog(t("Modelli di risposta"), content, t("Salva"), async () => { const item = ops.editTemplate, body = { title: $("template-title").value.trim(), subject: $("template-subject").value.trim(), body: $("template-body").value.trim(), active: $("template-active").value === "true" }; if (item) body.revision = item.revision; await opApi(`/api/response-templates${item ? "/" + item.id : ""}`, { method: item ? "PUT" : "POST", body }); const data = await opApi("/api/response-templates"); ops.templates = data.items || []; populateMessageTemplates(); $("operations-dialog").close(); });
  }
  function proposedPriority(impact, urgency) { return { low: { low: "low", medium: "normal", high: "high" }, medium: { low: "normal", medium: "normal", high: "high" }, high: { low: "high", medium: "high", high: "urgent" } }[impact]?.[urgency] || "normal"; }
  function refreshAssessmentHint(prefix) { const code = proposedPriority($(prefix + "-impact").value, $(prefix + "-urgency").value); $(prefix + "-priority-suggestion").replaceChildren(document.createTextNode(t("Priorità suggerita: {0}", [priorities[code]])), opButton(t("Usa suggerimento"), () => { $(prefix === "editor" ? "ticket-priority" : "detail-priority").value = code; })); }
  async function previewClassification(ticket = null) {
    const generation = state.generation, cycle = ops.cycle, body = ticket ? { title: ticket.title, description: ticket.description, requester: ticket.requester, impact: $("detail-impact").value, urgency: $("detail-urgency").value } : { title: $("ticket-title").value, description: $("ticket-description").value, requester: $("ticket-requester").value, impact: $("editor-impact").value, urgency: $("editor-urgency").value };
    try {
      const data = !ticket && state.raw.convert ? await opApi(`/api/raw/messages/${state.raw.convert.id}/suggestions`) : await opApi("/api/operations/suggest", { method: "POST", body }); if (generation !== state.generation || (ticket && !opCurrent(ticket, cycle, generation))) return;
      const content = element("div"); content.append(opNotice(t("Le regole propongono i valori: applicali soltanto dopo averli verificati.")), opNotice(t("Priorità suggerita: {0}", [priorities[data.proposed_priority] || data.proposed_priority])));
      for (const match of data.matches || []) content.append(opNotice(`${match.name} · ${match.category || ""} · ${match.group_name || ""}${match.job_id ? " · " + resourceLabel("job", { id: match.job_id, code: match.job_code, name: match.job_name }) : ""}`));
      if (!data.suggestion) content.append(opNotice(t("Nessuna regola corrispondente.")));
      operationDialog(t("Suggerimenti di classificazione"), content, t("Applica al modulo"), () => {
        const prefix = ticket ? "detail" : "editor", suggestion = data.suggestion || {}; if (suggestion.category) $(prefix + "-category").value = suggestion.category; if (suggestion.group_name) $(prefix + "-group").value = suggestion.group_name;
        $(prefix === "detail" ? "detail-priority" : "ticket-priority").value = data.proposed_priority;
        const match = (data.matches || []).find((item) => item.job_id === suggestion.job_id); if (match && !ticket) { state.resources.job = { id: match.job_id, code: match.job_code, name: match.job_name }; renderResourceChoices(); }
        $("operations-dialog").close(); if (ticket) toast(t("Suggerimenti applicati al modulo. Salva per confermare."));
      });
    } catch (error) { if (generation === state.generation) toast(error.message, true); }
  }
  async function loadSavedViews() {
    const generation = state.generation;
    try { const data = await opApi("/api/operations/views"); if (generation !== state.generation || !state.user) return; ops.views = data.items || []; const selected = $("saved-view").value; $("saved-view").replaceChildren(); addOption($("saved-view"), "", t("Scegli una vista")); for (const view of ops.views.filter((item) => item.active !== false)) addOption($("saved-view"), view.id, view.name); $("saved-view").value = selected; }
    catch (error) { if (generation === state.generation) toast(error.message, true); }
  }
  async function applySavedView() {
    const view = ops.views.find((item) => String(item.id) === $("saved-view").value); $("delete-view").hidden = !view || (view.owner_id !== state.user.id && state.user.role !== "admin"); if (!view) return;
    const filters = view.filters || {}; state.queue = Object.hasOwn(queues, filters.queue || "") ? filters.queue : "all"; state.view = ["board", "list", "agenda", "operator"].includes(view.view) ? view.view : "list"; state.offset = 0;
    $("operations-queue-filter").value = Object.hasOwn(operationsQueues, filters.operations_queue || "") ? filters.operations_queue : "all";
    for (const [id, key] of [["search-input", "search"], ["status-filter", "status"], ["priority-filter", "priority"], ["assignee-filter", "assignee_id"], ["sort-filter", "sort"]]) $(id).value = filters[key] || (key === "sort" ? "updated" : "");
    state.resourceFilters = { job: filters.job_id ? { id: Number(filters.job_id), name: "" } : null, asset: filters.asset_id ? { id: Number(filters.asset_id), name: "" } : null }; state.resourceRouteVersion += 1;
    const url = new URL(location.href); for (const kind of ["job", "asset"]) { if (filters[kind + "_id"]) url.searchParams.set(kind + "_id", filters[kind + "_id"]); else url.searchParams.delete(kind + "_id"); } url.searchParams.set("view", state.view); url.searchParams.set("operations_queue", $("operations-queue-filter").value); history.replaceState(null, "", url);
    renderResourceChoices(); const generation = state.generation, version = state.resourceRouteVersion;
    if ((filters.job_id || filters.asset_id) && state.punto.connected) {
      const query = new URLSearchParams(); if (filters.job_id) query.set("job_id", filters.job_id); if (filters.asset_id) query.set("asset_id", filters.asset_id);
      try { const data = await opApi(`/api/integrations/punto/validate?${query}`); if (generation !== state.generation || version !== state.resourceRouteVersion) return; state.resourceFilters = { job: data.job, asset: data.asset }; renderResourceChoices(); } catch (error) { if (generation !== state.generation) return; showError("integration-error", error.message); }
    }
    if (generation === state.generation && version === state.resourceRouteVersion) await loadTickets();
  }
  function saveViewDialog() {
    const existing = ops.views.find((item) => String(item.id) === $("saved-view").value && (item.owner_id === state.user.id || state.user.role === "admin"));
    const content = element("div"); content.append(opNotice(t("La vista sarà condivisa con il team. Ogni persona vedrà soltanto i ticket autorizzati.")), opField("view-name", t("Nome della vista"), "text", { maxLength: 100, value: existing?.name || "" }));
    if (existing) content.append(opField("view-update", t("Aggiorna la vista selezionata"), "select", { items: [["true", t("Sì")], ["false", t("No")]] }));
    const filters = Object.fromEntries(ticketParams()); delete filters.today;
    operationDialog(t("Salva vista condivisa"), content, t("Salva"), async () => { const update = existing && $("view-update").value === "true", body = { name: $("view-name").value.trim(), filters, view: state.view }; if (update) body.revision = existing.revision; await opApi(`/api/operations/views${update ? "/" + existing.id : ""}`, { method: update ? "PUT" : "POST", body }); $("operations-dialog").close(); await loadSavedViews(); });
  }
  function deleteViewDialog() { const view = ops.views.find((item) => String(item.id) === $("saved-view").value); if (!view) return; const content = element("p", "", t("Eliminare la vista condivisa «{0}»?", [view.name])); operationDialog(t("Elimina vista"), content, t("Elimina"), async () => { await opApi(`/api/operations/views/${view.id}`, { method: "PUT", body: { name: view.name, filters: view.filters, view: view.view, active: false, revision: view.revision } }); $("operations-dialog").close(); $("saved-view").value = ""; await loadSavedViews(); $("delete-view").hidden = true; }); }
  function openBulkDialog() {
    const content = element("div"); content.append(opNotice(t("L’anteprima verifica accesso, revisione e cambiamenti per ogni ticket selezionato.")));
    const statusItems = [["", t("Mantieni valore attuale")], ...Object.entries(statuses)], priorityItems = [["", t("Mantieni valore attuale")], ...Object.entries(priorities)], users = [["keep", t("Mantieni valore attuale")], ["", t("Non assegnato")], ...state.users.filter((user) => user.active).map((user) => [String(user.id), user.name])];
    content.append(opField("bulk-status", t("Stato"), "select", { items: statusItems }), opField("bulk-priority", t("Priorità"), "select", { items: priorityItems }), opField("bulk-assignee", t("Assegnato a"), "select", { items: users }), opField("bulk-reason", t("Motivo della priorità diversa dal suggerimento"), "textarea", { maxLength: 500, rows: 2 }), opField("bulk-waiting", t("Motivo dell’attesa"), "select", { items: [["", t("Nessuno")], ["customer", t("Risposta del cliente")], ["supplier", t("Risposta del fornitore")], ["spare_part", t("Ricambio")], ["authorization", t("Autorizzazione")]] }), opField("bulk-follow-up", t("Ricontatto"), "datetime-local"));
    content.append(opField("bulk-category", t("Categoria"), "text", { maxLength: 80, placeholder: t("Vuoto: mantieni il valore attuale") }), opField("bulk-group", t("Gruppo di lavoro"), "text", { maxLength: 80, placeholder: t("Vuoto: mantieni il valore attuale") }));
    operationDialog(t("Azioni sui selezionati"), content, t("Calcola anteprima"), async () => {
      const changes = {}; if ($("bulk-status").value) changes.status = $("bulk-status").value; if ($("bulk-priority").value) changes.priority = $("bulk-priority").value; if ($("bulk-reason").value.trim()) changes.priority_override_reason = $("bulk-reason").value.trim(); if ($("bulk-assignee").value !== "keep") changes.assignee_id = $("bulk-assignee").value ? Number($("bulk-assignee").value) : null;
      if ($("bulk-category").value.trim()) changes.category = $("bulk-category").value.trim(); if ($("bulk-group").value.trim()) changes.group_name = $("bulk-group").value.trim();
      if (changes.status === "waiting") { changes.waiting_reason = $("bulk-waiting").value || null; const candidates = localCandidates($("bulk-follow-up").value); if (candidates.length !== 1) throw new Error(t("Scegli un orario di ricontatto valido e non ambiguo nel fuso del browser.")); changes.follow_up_at = candidates[0]; }
      const generation = state.generation, data = await opApi("/api/tickets/bulk/preview", { method: "POST", body: { tickets: Array.from(ops.selected, ([id, revision]) => ({ id, revision })), changes } }); if (generation !== state.generation) return;
      const preview = element("div"); preview.append(element("h3", "", t("Modifiche richieste")), opNotice(readableHistoryDetail({ action: "bulk_updated", detail: JSON.stringify(data.changes || changes) })), element("h3", "", t("Inclusi: {0}", [data.eligible?.length || 0]))); for (const item of data.eligible || []) preview.append(opNotice(`#${item.id} · ${item.title}`)); preview.append(element("h3", "", t("Esclusi: {0}", [data.excluded?.length || 0]))); for (const item of data.excluded || []) preview.append(opNotice(`#${item.id} · ${{ not_found: t("Non disponibile o non autorizzato"), revision_changed: t("Versione modificata"), invalid_change: t("Modifica non valida"), inactive_assignee: t("Assegnatario non attivo") }[item.reason] || item.reason}`));
      operationDialog(t("Conferma modifica multipla"), preview, t("Conferma sui ticket inclusi"), data.eligible?.length ? async () => { const result = await opApi("/api/tickets/bulk/apply", { method: "POST", body: { token: data.token } }); if (generation !== state.generation) return; ops.selected.clear(); updateSelection(); $("operations-dialog").close(); await loadTickets(); toast(t("{0} ticket aggiornati; {1} esclusi.", [result.updated?.length || 0, result.excluded?.length || 0])); } : null);
    });
  }
  async function loadDashboard() {
    const generation = state.generation, cycle = ++ops.dashboardCycle; $("dashboard-refresh").disabled = true; showError("dashboard-error", "");
    try {
      const data = await opApi("/api/reports/operations"); if (generation !== state.generation || cycle !== ops.dashboardCycle || !state.user) return;
      const fragment = document.createDocumentFragment(), metrics = [[t("Ticket attivi"), data.counts?.active], [t("Non assegnati"), data.counts?.unassigned], [t("In attesa"), data.counts?.waiting], [t("Prima risposta oltre SLA"), data.counts?.response_breached], [t("Risoluzione oltre SLA"), data.counts?.resolution_breached], [t("Prima risposta prevista oggi"), data.counts?.response_due_today], [t("Risoluzione prevista oggi"), data.counts?.resolution_due_today], [t("Ricontatti da effettuare"), data.counts?.follow_up_due], [t("Riaperture"), data.counts?.reopened], [t("Messaggi RAW da rivedere"), data.raw?.backlog || 0]];
      for (const [label, value] of metrics) { const card = element("article", "operations-metric"); card.append(element("h3", "", label), element("strong", "", Number(value || 0).toLocaleString(locale))); fragment.append(card); }
      const workload = element("section", "operations-dashboard-wide"); workload.append(element("h3", "", t("Carico del team"))); for (const user of data.workload || []) workload.append(opNotice(t("{0}: {1} assegnati, {2} in attesa, {3} scaduti", [user.name, user.assigned, user.waiting, user.overdue]))); if (!data.workload?.length) workload.append(opNotice(t("Nessun dato disponibile."))); fragment.append(workload);
      if (data.group_workload?.length) { const groups = element("section", "operations-dashboard-wide"); groups.append(element("h3", "", t("Carico per gruppo"))); for (const group of data.group_workload) groups.append(opNotice(t("{0}: {1} attivi, {2} in attesa, {3} scaduti", [group.group_name || t("Nessun gruppo"), group.active, group.waiting, group.overdue]))); fragment.append(groups); }
      const timings = element("section", "operations-dashboard-wide"); timings.append(element("h3", "", t("Tempi e anzianità")), opNotice(t("Prima risposta media: {0} minuti", [data.average_response_minutes == null ? "—" : Math.round(data.average_response_minutes)])), opNotice(t("Risoluzione media: {0} minuti", [data.average_resolution_minutes == null ? "—" : Math.round(data.average_resolution_minutes)])), opNotice(t("Attesa RAW media: {0} minuti", [data.average_raw_wait_minutes == null ? "—" : Math.round(data.average_raw_wait_minutes)])));
      if (data.raw?.oldest_received_at) timings.append(opNotice(t("RAW più vecchio: {0} ({1} minuti)", [rawTime(data.raw.oldest_received_at), Math.round(data.raw.oldest_age_minutes || 0)])));
      const bands = [["under_1_day", t("Meno di un giorno")], ["days_1_7", t("Da 1 a 7 giorni")], ["days_8_30", t("Da 8 a 30 giorni")], ["over_30_days", t("Oltre 30 giorni")]]; for (const [key, label] of bands) timings.append(opNotice(`${label}: ${data.age_bands?.[key] || 0}`)); fragment.append(timings); $("dashboard-content").replaceChildren(fragment);
    } catch (error) { if (generation === state.generation && cycle === ops.dashboardCycle) { $("dashboard-content").replaceChildren(); showError("dashboard-error", error.message); } }
    finally { if (generation === state.generation && cycle === ops.dashboardCycle) $("dashboard-refresh").disabled = false; }
  }
  async function refreshPresence(ticket, editing = false) {
    const generation = state.generation;
    try { const data = await opApi(`/api/tickets/${ticket.id}/presence`, { method: "POST", body: { editing } }); if (generation !== state.generation || state.detailId !== ticket.id || !$("ticket-detail").open) return; if (data.draft && (!ops.serverDrafts.get(ticket.id) || data.draft.revision >= ops.serverDrafts.get(ticket.id).revision)) ops.serverDrafts.set(ticket.id, data.draft); ops.serverDraft = ops.serverDrafts.get(ticket.id) || null; ops.presenceId = ticket.id; const others = (data.items || []).filter((person) => person.user_id !== state.user.id); $("detail-presence").textContent = others.map((person) => person.name + (person.editing ? " · " + t("Sta modificando") : "")).join(", "); renderDraftTools(ticket); }
    catch { /* La presenza non impedisce di lavorare. */ }
  }
  function startPresence(ticket) { clearInterval(ops.presenceTimer); refreshPresence(ticket, false); ops.presenceTimer = setInterval(() => { if (state.detailId === ticket.id && $("ticket-detail").open && !document.hidden) refreshPresence(ticket, $("ticket-editor").open); }, 25000); }
  function renderDraftTools(ticket) {
    $("detail-resume-draft")?.remove(); $("detail-discard-draft")?.remove(); if (!ops.serverDraft) return;
    const button = opButton(t("Riprendi bozza"), () => { const draft = ops.serverDraft; if (!draft) return; openEditor({ ...ticket, ...draft.body }); state.editorTicket = { ...ticket, revision: draft.base_revision }; if (draft.base_revision !== ticket.revision) showError("editor-error", t("La bozza parte da una versione precedente. Ricarica il ticket prima di confermare.")); }); button.id = "detail-resume-draft"; $("operator-navigation").append(button);
    const discard = opButton(t("Elimina bozza"), () => operationDialog(t("Elimina bozza"), opNotice(t("Eliminare la copia della bozza salvata sul server?")), t("Elimina"), async () => { const draft = ops.serverDrafts.get(ticket.id); if (!draft) return; await opApi(`/api/tickets/${ticket.id}/draft/discard`, { method: "POST", body: { revision: draft.revision } }); ops.serverDrafts.delete(ticket.id); ops.serverDraft = null; $("operations-dialog").close(); renderDraftTools(ticket); })); discard.id = "detail-discard-draft"; $("operator-navigation").append(discard);
  }
  function scheduleServerDraft() {
    if (!state.editorTicket || !state.user) return; clearTimeout(ops.draftTimer); const ticket = { ...state.editorTicket }, generation = state.generation; let body; try { body = editorValues(); body.status = $("ticket-status").value; } catch { return; }
    ops.draftTimer = setTimeout(() => { ops.draftQueue = ops.draftQueue.catch(() => {}).then(async () => { if (generation !== state.generation) return; try { const result = await opApi(`/api/tickets/${ticket.id}/draft`, { method: "PUT", body: { revision: ops.serverDrafts.get(ticket.id)?.revision || 0, base_revision: ticket.revision, body } }); if (generation !== state.generation) return; ops.serverDrafts.set(ticket.id, result.draft || result); if (state.detailId === ticket.id) ops.serverDraft = result.draft || result; if (state.editorTicket?.id === ticket.id) $("editor-draft-status").textContent = t("Bozza salvata sul server."); } catch (error) { if (generation === state.generation && state.editorTicket?.id === ticket.id) $("editor-draft-status").textContent = error.message; } }); }, 900);
  }
  async function openOperationsSettings() {
    const generation = state.generation; try { const [rules, policies] = await Promise.all([opApi("/api/operations/rules"), opApi("/api/operations/sla-policies")]); if (generation !== state.generation || state.user?.role !== "admin") return; ops.rules = rules.items || []; ops.policies = policies.items || []; renderOperationsSettings(); } catch (error) { if (generation === state.generation) toast(error.message, true); }
  }
  function renderOperationsSettings() {
    const content = element("div"); content.append(element("h3", "", t("Regole di classificazione")), opNotice(t("Le regole producono suggerimenti; non modificano automaticamente i ticket.")));
    for (const rule of ops.rules) content.append(opButton(`${rule.name}${rule.active === false ? " · " + t("Disattivata") : ""}`, () => editOperationRule(rule), "button button-quiet")); content.append(opButton(t("Nuova regola"), () => editOperationRule(null)));
    content.append(element("h3", "", t("Politiche SLA")), opNotice(t("Gli obiettivi usano giorni, orario e fuso della politica. In attesa si sospende soltanto la risoluzione.")));
    for (const policy of ops.policies) content.append(opButton(`${policy.name} · ${policy.timezone}`, () => editSlaPolicy(policy), "button button-quiet")); content.append(opButton(t("Nuova politica SLA"), () => editSlaPolicy(null)));
    operationDialog(t("Regole e SLA"), content);
  }
  function operationJobChoice(target, record) {
    ops[target + "Job"] = record?.job_id ? { id: record.job_id, code: record.job_code || "", name: record.job_name || "" } : null;
    const group = element("fieldset", "resource-fields"), choice = element("div", "resource-choice"), label = element("span");
    group.append(element("legend", "", t("Commessa"))); label.id = target + "-job-label";
    const choose = opButton(t("Scegli la commessa"), () => openResourcePicker("job", "ops-" + target)); choose.id = target + "-job"; choose.disabled = !state.punto.connected;
    const clear = opButton(t("Rimuovi collegamento"), () => { ops[target + "Job"] = null; renderOperationJob(target); }, "button button-quiet"); clear.id = target + "-job-clear";
    choice.append(label, choose, clear); group.append(choice);
    if (!state.punto.connected) group.append(opNotice(t("Collega Punto dalla pagina iniziale per scegliere commesse e asset.")));
    return group;
  }
  function renderOperationJob(target) {
    const job = ops[target + "Job"], label = $(target + "-job-label"); if (!label) return;
    label.textContent = job ? job.code || job.name ? [job.code, job.name].filter(Boolean).join(" · ") : t("Commessa collegata") : target === "sla" ? t("Tutte le commesse") : t("Nessuna commessa");
    $(target + "-job-clear").hidden = !job;
  }
  async function hydrateOperationJob(target) {
    renderOperationJob(target);
    const job = ops[target + "Job"], label = $(target + "-job-label");
    if (!job || job.code || job.name || !state.punto.connected) return;
    try { const data = await opApi(`/api/integrations/punto/validate?job_id=${job.id}`); if (label.isConnected && $("operations-dialog").open && ops[target + "Job"] === job && data.job) { ops[target + "Job"] = data.job; renderOperationJob(target); } }
    catch (error) { if (error.name !== "AbortError" && label.isConnected && $("operations-dialog").open && ops[target + "Job"] === job) { label.textContent = t("Commessa non disponibile"); label.title = error.message; } }
  }
  async function loadRuleMailboxes() {
    const select = $("rule-mailbox"), notice = $("rule-mailbox-notice"); select.disabled = true;
    try {
      const data = await opApi("/api/raw/mailboxes"); if (!select.isConnected || !$("operations-dialog").open) return;
      const selected = select.value; select.replaceChildren(); addOption(select, "", t("Tutte le caselle"));
      for (const mailbox of data.items || []) addOption(select, String(mailbox.id), `${mailbox.name} · ${mailbox.address}${mailbox.active ? "" : " · " + t("Disattivata")}`);
      if (selected && !Array.from(select.options).some((option) => option.value === selected)) addOption(select, selected, t("Casella non disponibile"));
      select.value = selected; notice.textContent = data.items?.length ? "" : t("Nessuna casella configurata.");
    } catch (error) { if (error.name !== "AbortError" && select.isConnected && $("operations-dialog").open) notice.textContent = error.message; }
    finally { if (select.isConnected && state.user) select.disabled = false; }
  }
  function editOperationRule(rule) {
    const content = element("div"); const fields = [["rule-name", t("Nome"), "text", rule?.name || ""], ["rule-sender", t("Mittente esatto, facoltativo"), "email", rule?.sender_email || ""], ["rule-contains", t("Testo contenuto nel messaggio"), "text", rule?.contains || ""], ["rule-category", t("Categoria"), "text", rule?.category || ""], ["rule-group", t("Gruppo di lavoro"), "text", rule?.group_name || ""], ["rule-order", t("Ordine"), "number", rule?.order || 0]];
    const mailboxField = opField("rule-mailbox", t("Casella destinataria"), "select", { items: [["", t("Tutte le caselle")], ...(rule?.mailbox_id ? [[String(rule.mailbox_id), t("Casella collegata")]] : [])], value: rule?.mailbox_id ? String(rule.mailbox_id) : "" });
    const mailboxNotice = opNotice(t("Caricamento…")); mailboxNotice.id = "rule-mailbox-notice"; mailboxNotice.setAttribute("role", "status"); mailboxField.append(mailboxNotice); content.append(mailboxField, operationJobChoice("rule", rule));
    for (const [id, label, type, value] of fields) content.append(opField(id, label, type, { value, maxLength: 200 })); content.append(opField("rule-active", t("Abilitata"), "select", { items: [["true", t("Sì")], ["false", t("No")]], value: String(rule?.active !== false) }));
    operationDialog(t("Regola di classificazione"), content, t("Salva"), async () => { const body = { name: $("rule-name").value.trim(), mailbox_id: Number($("rule-mailbox").value) || null, sender_email: $("rule-sender").value.trim(), contains: $("rule-contains").value.trim(), category: $("rule-category").value.trim(), group_name: $("rule-group").value.trim(), job_id: ops.ruleJob?.id || null, order: Number($("rule-order").value), active: $("rule-active").value === "true" }; if (rule) body.revision = rule.revision; await opApi(`/api/operations/rules${rule ? "/" + rule.id : ""}`, { method: rule ? "PUT" : "POST", body }); await openOperationsSettings(); });
    hydrateOperationJob("rule"); loadRuleMailboxes();
  }
  function editSlaPolicy(policy) {
    const content = element("div"); content.append(opField("sla-name", t("Nome"), "text", { value: policy?.name || "", maxLength: 100 }), operationJobChoice("sla", policy), opField("sla-priority", t("Priorità"), "select", { items: [["all", t("Tutte le priorità")], ...Object.entries(priorities)], value: policy?.priority || "all" }), opField("sla-response", t("Minuti lavorativi per la prima risposta"), "number", { value: policy?.response_minutes || 240, min: 1 }), opField("sla-resolution", t("Minuti lavorativi per la risoluzione"), "number", { value: policy?.resolution_minutes || 1440, min: 1 }), opField("sla-timezone", t("Fuso orario"), "text", { value: policy?.timezone || "Europe/Rome" }), opField("sla-start", t("Inizio giornata lavorativa"), "time", { value: policy?.day_start || "09:00" }), opField("sla-end", t("Fine giornata lavorativa"), "time", { value: policy?.day_end || "18:00" }));
    const days = element("fieldset"); days.append(element("legend", "", t("Giorni lavorativi"))); const labels = [t("Lunedì"), t("Martedì"), t("Mercoledì"), t("Giovedì"), t("Venerdì"), t("Sabato"), t("Domenica")]; for (let i = 1; i <= 7; i++) { const label = element("label", "raw-check"), input = element("input"); input.type = "checkbox"; input.value = String(i); input.checked = (policy?.weekdays || [1,2,3,4,5]).includes(i); label.append(input, document.createTextNode(labels[i - 1])); days.append(label); } days.id = "sla-weekdays"; content.append(days, opField("sla-active", t("Abilitata"), "select", { items: [["true", t("Sì")], ["false", t("No")]], value: String(policy?.active !== false) }));
    operationDialog(t("Politica SLA"), content, t("Salva"), async () => { const body = { name: $("sla-name").value.trim(), job_id: ops.slaJob?.id || null, priority: $("sla-priority").value, response_minutes: Number($("sla-response").value), resolution_minutes: Number($("sla-resolution").value), timezone: $("sla-timezone").value.trim(), day_start: $("sla-start").value, day_end: $("sla-end").value, weekdays: Array.from($("sla-weekdays").querySelectorAll("input:checked"), (input) => Number(input.value)), active: $("sla-active").value === "true" }; if (policy) body.revision = policy.revision; await opApi(`/api/operations/sla-policies${policy ? "/" + policy.id : ""}`, { method: policy ? "PUT" : "POST", body }); await openOperationsSettings(); });
    hydrateOperationJob("sla");
  }
  function initOperations() {
    $("editor-assessment").append(buildAssessment("editor"), opButton(t("Verifica suggerimenti di classificazione"), () => previewClassification())); $("detail-assessment").append(buildAssessment("detail")); const status = element("p", "field-hint"); status.id = "editor-draft-status"; $("editor-assessment").append(status);
    for (const prefix of ["editor", "detail"]) for (const field of ["impact", "urgency"]) $(prefix + "-" + field).addEventListener("change", () => refreshAssessmentHint(prefix));
    $("operations-dialog-confirm").addEventListener("click", async () => { if (!ops.dialogAction) return; const action = ops.dialogAction, generation = state.generation; $("operations-dialog-confirm").disabled = true; $("operations-dialog").setAttribute("aria-busy", "true"); showError("operations-dialog-error", ""); try { await action(); } catch (error) { if (generation === state.generation) showError("operations-dialog-error", error.message); } finally { if (generation === state.generation) { $("operations-dialog").removeAttribute("aria-busy"); $("operations-dialog-confirm").disabled = false; } } });
    $("operations-dialog").addEventListener("close", () => { ops.dialogAction = null; $("operations-dialog").removeAttribute("aria-busy"); });
    $("nav-dashboard").addEventListener("click", () => showWorkspace("dashboard")); $("dashboard-refresh").addEventListener("click", loadDashboard);
    $("saved-view").addEventListener("change", applySavedView); $("save-view").addEventListener("click", saveViewDialog); $("delete-view").addEventListener("click", deleteViewDialog); $("bulk-open").addEventListener("click", openBulkDialog); $("operations-settings").addEventListener("click", openOperationsSettings);
    $("select-page").addEventListener("change", () => { for (const ticket of visibleTickets()) if ($("select-page").checked) ops.selected.set(ticket.id, ticket.revision); else ops.selected.delete(ticket.id); for (const checkbox of document.querySelectorAll(".ticket-selection")) checkbox.checked = $("select-page").checked; updateSelection(); });
    $("detail-previous").addEventListener("click", () => stepDetail(-1)); $("detail-next").addEventListener("click", () => stepDetail(1)); $("ticket-form").addEventListener("input", scheduleServerDraft);
    document.addEventListener("keydown", (event) => { if (event.ctrlKey || event.metaKey || event.altKey || event.target.closest("input,textarea,select,[contenteditable=true]")) return; if (document.querySelector("dialog[open]:not(#ticket-detail)")) return; if ($("ticket-detail").open && ["j", "k"].includes(event.key)) { event.preventDefault(); stepDetail(event.key === "j" ? 1 : -1); } if (event.key === "/" && state.raw.section === "tickets" && !$("ticket-detail").open) { event.preventDefault(); $("search-input").focus(); } });
    $("selection-count").after(opButton(t("Annulla selezione"), () => { ops.selected.clear(); for (const checkbox of document.querySelectorAll(".ticket-selection")) checkbox.checked = false; updateSelection(); }, "button button-quiet"));
    setupOperatorDialog();
  }

  initOperations();

  for (const button of document.querySelectorAll("[data-close-dialog]")) button.addEventListener("click", () => closeDialog($(button.dataset.closeDialog)));
  for (const dialog of document.querySelectorAll("dialog")) dialog.addEventListener("cancel", (event) => { if (dialog.getAttribute("aria-busy") === "true" || dialog.querySelector('[aria-busy="true"]')) event.preventDefault(); });
  $("ticket-detail").addEventListener("close", () => {
    saveLocalDraft(); clearInterval(ops.presenceTimer); ops.cycle += 1;
    state.detailRequest?.abort(); state.calendarRequest?.abort(); state.calendarCycle += 1; state.detailId = null; state.detail = null;
    if (state.suppressDetailClose) { state.suppressDetailClose = false; return; }
    if (state.user && !state.routeSync && ticketRoute()) {
      const url = new URL(location.href); url.searchParams.delete("ticket"); history.replaceState(null, "", url);
    }
  });
  $("user-editor").addEventListener("close", () => { $("user-password").value = ""; });
  $("nav-tickets").addEventListener("click", () => showWorkspace("tickets"));
  $("nav-raw").addEventListener("click", () => showWorkspace("raw"));
  $("raw-refresh").addEventListener("click", loadRawMessages);
  $("raw-filter-form").addEventListener("submit", (event) => { event.preventDefault(); clearTimeout(state.raw.searchTimer); state.raw.offset = 0; loadRawMessages(); });
  $("raw-filter-form").addEventListener("change", () => { clearTimeout(state.raw.searchTimer); state.raw.offset = 0; loadRawMessages(); });
  $("raw-search").addEventListener("input", () => { state.raw.cycle += 1; state.raw.controller?.abort(); clearTimeout(state.raw.searchTimer); $("raw-previous").disabled = true; $("raw-next").disabled = true; state.raw.searchTimer = setTimeout(() => { state.raw.offset = 0; loadRawMessages(); }, 300); });
  $("raw-previous").addEventListener("click", () => { state.raw.offset = Math.max(0, state.raw.offset - state.raw.limit); loadRawMessages(); });
  $("raw-next").addEventListener("click", () => { state.raw.offset += state.raw.limit; loadRawMessages(); });
  $("raw-detail").addEventListener("close", () => { state.raw.detailController?.abort(); state.raw.detailCycle += 1; state.raw.detail = null; $("raw-detail").removeAttribute("aria-busy"); if (state.raw.suppressClose) { state.raw.suppressClose = false; return; } if (state.user && rawRoute()) { const url = new URL(location.href); url.searchParams.delete("raw"); history.replaceState(null, "", url); } });
  $("ticket-editor").addEventListener("close", () => { state.raw.convert = null; if (state.detail?.ticket && $("ticket-detail").open) refreshPresence(state.detail.ticket, false); });
  $("raw-claim").addEventListener("click", async () => { if (await rawMutation("claim")) toast(t("Messaggio preso in carico.")); });
  $("raw-release").addEventListener("click", async () => { if (await rawMutation("release")) toast(t("Messaggio rilasciato.")); });
  $("raw-discard").addEventListener("click", () => { $("raw-discard-form").hidden = !$("raw-discard-form").hidden; if (!$("raw-discard-form").hidden) $("raw-discard-reason").focus(); });
  $("raw-discard-form").addEventListener("submit", async (event) => { event.preventDefault(); const reason = $("raw-discard-reason").value.trim(); if (!reason) return; if (await rawMutation("discard", { reason })) { $("raw-discard-form").hidden = true; toast(t("Messaggio scartato.")); } });
  $("raw-convert").addEventListener("click", startRawConversion);
  $("raw-link").addEventListener("click", openRawTicketPicker);
  $("raw-ticket-search-form").addEventListener("submit", (event) => { event.preventDefault(); searchRawTickets(); });
  $("raw-ticket-search").addEventListener("input", () => { state.raw.linkCycle += 1; state.raw.link = null; $("raw-ticket-confirm").disabled = true; $("raw-ticket-more").disabled = true; $("raw-ticket-selection").hidden = true; });
  $("raw-ticket-more").addEventListener("click", () => searchRawTickets(true));
  $("raw-ticket-picker").addEventListener("close", () => { state.raw.linkCycle += 1; state.raw.link = null; });
  $("raw-ticket-confirm").addEventListener("click", async () => { const ticket = state.raw.link; if (!ticket || !state.raw.detail) return; let attachmentIds; try { attachmentIds = selectedRawAttachmentIds("raw-link-attachment-list"); validateRawAttachments(attachmentIds); } catch (error) { showError("raw-ticket-error", error.message); return; } const generation = state.generation; $("raw-ticket-confirm").disabled = true; $("raw-ticket-picker").setAttribute("aria-busy", "true"); const data = await rawMutation("link", { ticket_id: ticket.id, attachment_ids: attachmentIds }, "raw-ticket-error"); if (generation !== state.generation) return; $("raw-ticket-picker").removeAttribute("aria-busy"); $("raw-ticket-confirm").disabled = !state.raw.link; if (data) { $("raw-ticket-picker").close(); toast(t("Messaggio collegato al ticket #{0}.", [ticket.id])); } });
  $("raw-settings").addEventListener("click", openRawSettings);
  $("raw-settings-dialog").addEventListener("close", () => { state.raw.adminCycle += 1; state.raw.senderCycle += 1; });
  $("raw-mailbox-new").addEventListener("click", () => { if ($("raw-mailbox-form").getAttribute("aria-busy") === "true" || $("raw-sender-form").getAttribute("aria-busy") === "true") return; state.raw.senderCycle += 1; editRawMailbox(); $("raw-senders").replaceChildren(); $("raw-sender-form").hidden = true; });
  $("raw-mailbox-form").addEventListener("submit", saveRawMailbox);
  $("raw-sender-new").addEventListener("click", () => editRawSender());
  $("raw-sender-form").addEventListener("submit", saveRawSender);
  for (const target of ["mailbox", "sender"]) { $("raw-" + target + "-job").addEventListener("click", () => openResourcePicker("job", "raw-" + target)); $("raw-" + target + "-job-clear").addEventListener("click", () => { state.raw[target + "Job"] = null; renderRawAdminJob(target); }); }

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
  for (const id of ["status-filter", "priority-filter", "assignee-filter", "sort-filter", "operations-queue-filter"]) $(id).addEventListener("change", () => {
    clearTimeout(state.searchTimer);
    if (["resolved", "closed"].includes($("status-filter").value)) state.queue = "all";
    if (id === "operations-queue-filter") { const url = new URL(location.href); url.searchParams.set("operations_queue", $(id).value); history.replaceState(null, "", url); }
    state.offset = 0; loadTickets();
  });
  for (const card of document.querySelectorAll(".stat-card")) card.addEventListener("click", () => { clearTimeout(state.searchTimer); $("status-filter").value = $("status-filter").value === card.dataset.status ? "" : card.dataset.status; state.queue = "all"; state.offset = 0; loadTickets(); });
  for (const button of document.querySelectorAll(".queue-button")) button.addEventListener("click", () => {
    clearTimeout(state.searchTimer); state.queue = button.dataset.queue; state.offset = 0;
    if (["resolved", "closed"].includes($("status-filter").value)) $("status-filter").value = "";
    if (["mine", "unassigned"].includes(state.queue)) $("assignee-filter").value = "";
    loadTickets();
  });
  for (const view of ["board", "list", "agenda", "operator"]) $("view-" + view).addEventListener("click", () => {
    if (state.view === view) return;
    clearTimeout(state.searchTimer); state.view = view; state.offset = 0;
    const url = new URL(location.href); url.searchParams.set("view", view); history.replaceState(null, "", url); loadTickets();
  });
  $("reset-filters").addEventListener("click", resetFilters);
  $("empty-action").addEventListener("click", () => { if (filtersActive()) resetFilters(); else openEditor(); });
  $("refresh-tickets").addEventListener("click", async () => { clearTimeout(state.searchTimer); await loadPuntoStatus(); Promise.allSettled([loadUsers(), loadTickets()]); });
  $("previous-page").addEventListener("click", () => { state.offset = Math.max(0, state.offset - state.limit); loadTickets(); });
  $("next-page").addEventListener("click", () => { state.offset += state.limit; loadTickets(); });
  $("new-ticket").addEventListener("click", () => { state.resourceRouteVersion += 1; openEditor(); });
  $("connect-punto").addEventListener("click", () => startPuntoConnection());
  $("calendar-connect").addEventListener("click", () => startPuntoConnection(true));
  $("calendar-retry").addEventListener("click", () => { if (state.calendar?.can_retry && ["sync", "remove"].includes(state.calendar.retry_action)) calendarAction(state.calendar.retry_action); });
  for (const action of ["sync", "remove", "refresh"]) $("calendar-" + action).addEventListener("click", () => calendarAction(action));
  for (const prefix of ["ticket", "detail"]) {
    for (const endpoint of ["start", "end"]) {
      $(`${prefix}-scheduled-${endpoint}`).addEventListener("input", () => updateScheduleInput(prefix, endpoint, ""));
      $(`${prefix}-${endpoint}-offset`).addEventListener("change", () => updateScheduleHint(prefix));
    }
    $(prefix + "-clear-schedule").addEventListener("click", () => { for (const endpoint of ["start", "end"]) { $(`${prefix}-scheduled-${endpoint}`).value = ""; updateScheduleInput(prefix, endpoint, ""); } });
  }
  for (const [id, days] of [["agenda-previous", -7], ["agenda-next", 7], ["agenda-today", 0]]) $(id).addEventListener("click", () => {
    state.agendaWeek = days ? civilDay(state.agendaWeek, days) : weekStart(new Date()); clearTimeout(state.searchTimer); loadTickets();
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
    const requestedRaw = rawRoute();
    if (new URL(location.href).searchParams.get("section") === "raw" || requestedRaw) { showWorkspace("raw", false); if (requestedRaw && state.raw.detail?.id !== requestedRaw) openRawDetail(requestedRaw, false); }
    else { showWorkspace(new URL(location.href).searchParams.get("section") === "dashboard" ? "dashboard" : "tickets", false); if ($("raw-detail").open) { state.raw.suppressClose = true; $("raw-detail").close(); } }
    const view = new URL(location.href).searchParams.get("view"), operationsQueueParam = new URL(location.href).searchParams.get("operations_queue"), operationsQueue = Object.hasOwn(operationsQueues, operationsQueueParam || "") ? operationsQueueParam : "all";
    let changed = false;
    if (["board", "list", "agenda", "operator"].includes(view) && view !== state.view) { state.view = view; changed = true; }
    if ($("operations-queue-filter").value !== operationsQueue) { $("operations-queue-filter").value = operationsQueue; changed = true; }
    if (changed) loadTickets();
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
