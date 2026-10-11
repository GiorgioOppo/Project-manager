"use strict";

(async () => {
  if (window.PuntoI18n) await window.PuntoI18n.ready;
  const { t, locale } = window.PuntoI18n || { t: (source, values = []) => source.replace(/\{(\d+)\}/g, (_, index) => values[index]), locale: "it-IT" };
  const $ = (id) => document.getElementById(id);
  const states = { planned: t("Da avviare"), active: t("In corso"), paused: t("In pausa"), completed: t("Completata"), cancelled: t("Annullata") };
  const roles = { manager: t("Gestore"), worker: t("Operatore") };
  const contractStates = { draft: t("Bozza"), active: t("Attivo"), expired: t("Scaduto"), closed: t("Chiuso"), cancelled: t("Annullato") };
  const tabs = ["my-work", "jobs", "work-board", "inventory", "calendar", "hours", "work-schedules", "reports", "notifications", "alerts", "suppliers", "contracts", "users", "organization", "audit"];
  const reportViews = { periodo: { api: "date", label: t("Per periodo"), title: t("Il lavoro nel periodo") }, commesse: { api: "job", label: t("Per commessa"), title: t("Il lavoro per commessa") }, utenti: { api: "user", label: t("Per utente"), title: t("Il lavoro per utente") }, costi: { api: "cost", label: t("Valore contratti"), title: t("Valori contrattuali e lavoro registrato") }, "costo-ore": { api: "labor_cost", label: t("Costo delle ore"), title: t("Il costo stimato delle ore") } };
  const state = { session: null, sessionRequest: 0, jobs: [], entries: [], users: [], todayMinutes: null, todayCount: null, jobsReady: false, entriesReady: false, usersReady: false, selectedDate: localDate(new Date()), entriesRequest: 0, usersRequest: 0, refreshRequest: 0, deleteEntry: null, openEntryAfterJob: false, activeTab: "jobs", assignmentJob: null, assignments: [], assignmentUsers: [], assignmentsReady: false, assignmentRequest: 0, editingAssignment: null, revokeAssignment: null, auditEvents: [], auditReady: false, auditRequest: 0, auditLoading: false, auditCursor: null, auditRetryMore: false };
  Object.assign(state, { suppliers: [], contracts: [], suppliersReady: false, contractsReady: false, suppliersRequest: 0, contractsRequest: 0, suppliersLoading: false, contractsLoading: false, editingJob: null, deleteJob: null, editingContract: null, documentsContract: null, documents: [], documentsReady: false, documentsRequest: 0 });
  Object.assign(state, { organizationGroups: [], organizationRoles: [], organizationUsers: [], organizationReady: false, organizationLoading: false, organizationRequest: 0, selectedGroup: null, groupMembers: [], groupMembersReady: false, groupMembersRequest: 0, organizationBusy: false, organizationConfirm: null, assignmentGroups: [], assignmentRoles: [], groupGrants: [], editingGroupGrant: null, revokeGroupGrant: null });
  Object.assign(state, { reportView: "periodo", reportFilters: defaultReportFilters(), reportToday: null, reportDefaultsPending: true, reportData: null, reportReady: false, reportLoading: false, reportRequest: 0 });
  Object.assign(state, { laborRates: [], laborRateUsers: [], laborRatesReady: false, laborRatesLoading: false, laborRatesRequest: 0, editingLaborRate: null, routeHash: null });
  Object.assign(state, { alertRules: [], alertUsers: [], alertRulesReady: false, alertRulesLoading: false, alertRulesRequest: 0, editingAlertRule: null, runningAlertRule: null, alertSmtpConfigured: false, notifications: [], notificationsReady: false, notificationsLoading: false, notificationsRequest: 0, notificationsUnread: 0, readingNotifications: new Set() });
  const alertTypes = {
    invalid_job: { label: t("Commessa inesistente"), description: t("Controlla i tentativi rifiutati di registrare ore su commesse inesistenti o eliminate. Le ore di questi tentativi non sono state salvate.") },
    missing_hours: { label: t("Ore giornaliere mancanti"), description: t("Controlla le giornate da lunedì a venerdì con meno di 8 ore, dopo 48 ore reali dalla fine della giornata in Europe/Rome. Il monitoraggio parte dalla creazione della regola.") },
    inventory_due: { label: t("Scadenze inventario"), description: t("Controlla garanzie, manutenzioni, scadenze e rinnovi delle licenze. Include le date già superate; esclude beni dismessi o smarriti. Ogni destinatario riceve un solo avviso per asset, tipo di scadenza, data e anticipo impostato.") },
    inventory_return: { label: t("Restituzioni inventario scadute"), description: t("Controlla le consegne ancora aperte dal giorno successivo alla restituzione prevista. Esclude beni dismessi o smarriti; ogni nuova consegna può generare un nuovo avviso.") },
    inventory_verification: { label: t("Verifiche inventario"), description: t("Controlla i beni materiali mai verificati e quelli la cui ultima verifica ha raggiunto l’intervallo impostato. Esclude beni dismessi o smarriti; una nuova verifica avvia un nuovo intervallo.") }
  };
  const calendar = { request: 0, loading: false, busy: false, ready: false, status: null, date: localDate(new Date()), events: [], cursor: null, calendars: [], calendarCursor: null, catalogReady: false, consentState: null };
  const timeWorkflow = { view: "day", week: null, copyBusy: false, batchRows: [], batchKey: null, batchPayload: null, batchSequence: 0, entry: null, detailEntry: null, requests: [], requestsReady: false, requestsLoading: false, requestsRequest: 0, decision: null, lifecycleJob: null, lifecycle: null, lifecycleRequest: 0 };
  const workspaceFlows = { epoch: 0, myWork: null, myRequest: 0, myLoading: false, overviewJob: null, overviewRequest: 0, accessJob: null, accessRequest: 0, users: [], schedules: [], exceptions: [], holidays: [], scheduleRequest: 0, scheduleLoading: false, scheduleKind: null, scheduleDelete: null, workNotifications: [], workUnread: 0, notificationRequest: 0, notificationLoading: false, reading: new Set(), wizard: { step: 0, ready: false, request: 0, users: [], groups: [], roles: [], contracts: [], templates: [], team: [], tasks: [], sequence: 0, key: null, payload: null } };
  Object.assign(timeWorkflow, { userId: "", team: [], teamReady: false, teamRequest: 0 });
  const accessProfiles = { request: 0, user: null, catalog: [], data: null };
  const accessProfileLabels = { operator: t("Operatori"), manager: t("Gestori"), sales: t("Commerciali"), finance: t("Finance") };
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
    return t("{0} h {1} min", [Math.floor(value / 60), String(value % 60).padStart(2, "0")]);
  }

  function dateLabel(value) {
    const [year, month, day] = value.split("-").map(Number);
    const date = new Date(0);
    date.setFullYear(year, month - 1, day);
    date.setHours(12, 0, 0, 0);
    return date.toLocaleDateString(locale, { day: "numeric", month: "long", year: "numeric" });
  }

  function hoursToMinutes(value) {
    const normalized = value.trim().replace(",", ".");
    if (/^\d{1,2}:\d{2}$/.test(normalized)) {
      const [hours, minutes] = normalized.split(":").map(Number);
      if (minutes > 59) throw new Error(t("Dopo i due punti inserisci i minuti, da 00 a 59."));
      const total = hours * 60 + minutes;
      if (total < 1 || total > 1440) throw new Error(t("Inserisci una durata da 1 minuto a 24 ore."));
      return total;
    }
    if (!/^(?:\d+(?:\.\d+)?|\.\d+)$/.test(normalized)) throw new Error(t("Inserisci le ore come 1,5 oppure 1:30."));
    const [whole, fraction = ""] = normalized.split(".");
    const denominator = 10n ** BigInt(fraction.length);
    const numerator = BigInt((whole || "0") + fraction) * 60n;
    if (numerator % denominator !== 0n) throw new Error(t("La durata deve corrispondere a minuti interi. Usa ad esempio 0:15 per 15 minuti."));
    const minutes = Number(numerator / denominator);
    if (minutes < 1 || minutes > 1440) throw new Error(t("Inserisci una durata da 1 minuto a 24 ore."));
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
    const retained = hash.length <= 2048 && (/^#analisi\/(?:periodo|commesse|utenti|costi|costo-ore)(?:\?|$)/.test(hash) || /^#ticket-connect\?state=[a-f0-9]{64}(?:&calendar=1)?$/.test(hash) || /^#(?:jobs\?job_id|inventory\?asset_id)=[1-9]\d*$/.test(hash) || hash === "#calendar") ? hash : "";
    window.location.replace(window.PuntoI18n ? window.PuntoI18n.url("/" + retained) : "/" + retained);
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
        throw new Error(t("La sessione è scaduta. Accedi di nuovo."));
      }
      let data;
      try { data = await response.json(); } catch (_) { throw new Error(t("Il server ha restituito una risposta non valida. Riprova.")); }
      if (!response.ok || data.ok === false) {
        const workResourceUnavailable = [403, 404].includes(response.status) && /^\/api\/(?:work-board|tasks|blockers)(?:\/|$|\?)/.test(path);
        if (workResourceUnavailable) {
          resetWorkBoard(true);
          showNotice((data.error ? t(data.error) : "") || t("La risorsa non è più disponibile. Seleziona una commessa abilitata."), true);
        }
        if (response.status === 403) {
          if (/^\/api\/inventory(?:\/|$|\?)/.test(path)) resetInventory();
          showNotice((data.error ? t(data.error) : "") || t("Il tuo account non è abilitato a questa operazione."), true);
          if (path !== "/api/session") refreshSession().catch(() => {});
        }
        const error = new Error((data.error ? t(data.error) : "") || (response.status === 403 ? t("Il tuo account non è abilitato a questa operazione.") : t("Non siamo riusciti a completare l’operazione. Riprova.")));
        error.status = response.status;
        error.sourceMessage = data.error;
        throw error;
      }
      if (fetchOptions.method && fetchOptions.method !== "GET" && /^\/api\/(?:time-entries|jobs|users|contracts|suppliers|groups|job-roles|labor-rates)(?:\/|$|\?)/.test(path)) invalidateReports();
      return data;
    } catch (error) {
      if (error.name === "AbortError") throw new Error(t("Il server sta impiegando troppo tempo. Controlla la connessione e riprova."));
      if (error instanceof TypeError) throw new Error(t("Non riusciamo a raggiungere il server. Controlla la connessione e riprova."));
      throw error;
    } finally {
      clearTimeout(timeout);
    }
  }

  function post(path, fields) {
    return api(path, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8" }, body: new URLSearchParams(fields) });
  }

  function ticketAppURL(kind = "", id = null, create = false, agenda = false) {
    if (!state.session?.tickets?.configured) return null;
    const url = new URL(state.session.tickets.origin);
    if (kind && id) url.searchParams.set(kind + "_id", String(id));
    if (create) url.searchParams.set("new", "1");
    if (agenda) url.searchParams.set("view", "agenda");
    return window.PuntoI18n ? window.PuntoI18n.url(url.href) : url.href;
  }

  function ticketAppLink(label, kind, id, create = false, agenda = false) {
    const link = node("a", "row-button", label);
    link.href = ticketAppURL(kind, id, create, agenda);
    return link;
  }

  async function connectTicketSession(connectionState, calendarAccess = false) {
    if (state.ticketConnecting) return;
    if (!state.session?.tickets?.configured) { showNotice(t("Il collegamento a Ticket non è configurato su Punto."), true); return; }
    state.ticketConnecting = true;
    showNotice(t("Collegamento della sessione Punto a Ticket…"));
    try {
      const data = await post("/api/integrations/tickets/grant", { state: connectionState, ...(calendarAccess ? { calendar: "1" } : {}) });
      const target = new URL(data.url);
      if (target.origin !== new URL(state.session.tickets.origin).origin || target.pathname !== "/" || !/^#punto_grant=[a-f0-9]{64}&state=[a-f0-9]{64}$/.test(target.hash)) throw new Error(t("Il collegamento restituito non è valido."));
      window.location.assign(window.PuntoI18n ? window.PuntoI18n.url(target.href) : target.href);
    } catch (error) {
      if (calendarAccess && $("ticket-calendar-consent").open) { $("ticket-calendar-consent-error").textContent = error.message; $("ticket-calendar-consent-error").hidden = false; }
      else showNotice(error.message + t(" Torna a Ticket e ripeti Collega Punto."), true);
    }
    finally { state.ticketConnecting = false; }
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

  function viewingOwnHours() { return !timeWorkflow.userId || String(timeWorkflow.userId) === String(state.session?.user_id || state.session?.id || ""); }
  function canReadUsers() { return can("manage_users") || can("manage_access_groups"); }
  function financialReport(view = state.reportView) { return ["costi", "costo-ore"].includes(view); }
  function reportCanFilterUsers(view = state.reportView) { return can("view_team_hours") && !financialReport(view); }

  function can(permission) {
    return state.session?.permissions?.[permission] === true;
  }

  function renderIdentityManagement() {
    const link = $("keycloak-users-link");
    link.hidden = state.session?.role !== "manager" || !state.usersAdminURL;
    if (state.usersAdminURL) link.href = state.usersAdminURL;
    else link.removeAttribute("href");
    const groupsLink = $("keycloak-groups-link");
    groupsLink.hidden = !can("manage_groups") || !state.groupsManaged || !state.groupsAdminURL;
    if (state.groupsAdminURL) groupsLink.href = state.groupsAdminURL;
    else groupsLink.removeAttribute("href");
    $("keycloak-groups-note").hidden = !can("manage_groups") || !state.groupsManaged;
    $("new-group").hidden = !canEditGroups();
    $("group-member-form").hidden = !canEditGroups();
    if (!canEditGroups()) {
      $("group-detail-actions").hidden = true;
      $("organization-confirm").hidden = true;
      state.organizationConfirm = null;
      if ($("group-dialog").open) $("group-dialog").close();
    }
  }

  async function loadIdentityManagement() {
    const configuration = await api("/api/auth/config");
    state.usersAdminURL = null;
    state.groupsAdminURL = null;
    state.groupsManaged = configuration.managed_groups === true;
    if (configuration.managed_users && typeof configuration.users_admin_url === "string" && configuration.users_admin_url) {
      const target = new URL(configuration.users_admin_url);
      if (target.protocol !== "https:" || target.username || target.password) throw new Error(t("Configurazione accesso non disponibile. Riprova."));
      state.usersAdminURL = target.href;
    }
    if (state.groupsManaged && typeof configuration.groups_admin_url === "string" && configuration.groups_admin_url) {
      const target = new URL(configuration.groups_admin_url);
      if (target.protocol !== "https:" || target.username || target.password) throw new Error(t("Configurazione accesso non disponibile. Riprova."));
      state.groupsAdminURL = target.href;
    }
    renderIdentityManagement();
  }

  function canEditGroups() {
    return !state.groupsManaged && can("manage_groups") && state.session?.permissions?.edit_groups !== false;
  }

  function requireGroupEditing() {
    if (canEditGroups()) return true;
    showNotice(state.groupsManaged ? t("Gestisci gruppi e appartenenze in Keycloak.") : t("Il tuo account non è abilitato a questa operazione."), true);
    return false;
  }

  function requirePermission(permission) {
    if (can(permission)) return true;
    showNotice(t("Il tuo account non è abilitato a questa operazione."), true);
    return false;
  }

  function resetCalendar() {
    calendar.request++; calendar.loading = calendar.busy = calendar.ready = false;
    calendar.status = null; calendar.events = []; calendar.cursor = null;
    calendar.calendars = []; calendar.calendarCursor = null; calendar.catalogReady = false;
    calendar.consentState = null;
    ["calendar-events", "calendar-series-list", "calendar-changes", "calendar-choice"].forEach((id) => $(id).replaceChildren());
    ["calendar-agenda", "calendar-selection", "calendar-queue", "calendar-error"].forEach((id) => { $(id).hidden = true; });
    if ($("ticket-calendar-consent").open) $("ticket-calendar-consent").close();
  }

  function renderCalendarTickets() {
    $("calendar-ticket-context").hidden = !state.session?.tickets?.configured;
    const selected = $("calendar-ticket-job").value;
    const options = [new Option(t("Tutte le commesse abilitate"), ""), ...state.jobs.map((job) => new Option(`${job.code} · ${job.name}`, String(job.id)))];
    $("calendar-ticket-job").replaceChildren(...options);
    $("calendar-ticket-job").value = options.some((option) => option.value === selected) ? selected : "";
    if (state.session?.tickets?.configured) $("calendar-ticket-link").href = ticketAppURL("job", $("calendar-ticket-job").value, false, true);
  }

  function calendarWeek() {
    const date = new Date(calendar.date + "T12:00:00");
    date.setDate(date.getDate() - (date.getDay() + 6) % 7); date.setHours(0, 0, 0, 0);
    const end = new Date(date); end.setDate(end.getDate() + 7);
    return { start: date, end };
  }

  function calendarError(error) {
    const messages = {
      oauth_not_configured: t("Google Calendar richiede la configurazione dell’amministratore."),
      calendar_not_configured: t("Il servizio calendario non è configurato."),
      pending_changes: t("Ci sono operazioni in attesa. Attendi il loro completamento prima di cambiare calendario."),
      calendar_not_connected: t("Collega Google Calendar per proseguire."),
      calendar_not_selected: t("Seleziona un calendario per proseguire."),
      google_account_mismatch: t("È già collegato un altro account Google. Scollegalo prima di cambiare account."),
      calendar_not_writable: t("Scegli un calendario sul quale hai il permesso di modifica.")
    };
    return messages[error.sourceMessage] || error.message;
  }

  function renderCalendarStatus() {
    const status = calendar.status;
    if (!status) return;
    $("calendar-status").textContent = !status.oauth_configured ? t("Google Calendar richiede la configurazione dell’amministratore. L’agenda Ticket è già disponibile.") : status.reauth_required ? t("Il consenso Google è scaduto. Collega nuovamente il tuo account.") : !status.connected ? t("Collega il tuo account Google per sincronizzare gli interventi.") : status.calendar_id ? t("Calendario collegato: {0}", [calendar.calendars.find((item) => item.id === status.calendar_id)?.summary || status.calendar_id]) : t("Account Google collegato. Scegli un calendario da sincronizzare.");
    $("calendar-connect").hidden = !status.oauth_configured || status.connected && !status.reauth_required;
    $("calendar-sync").hidden = !status.connected || !status.calendar_id || status.reauth_required;
    $("calendar-disconnect").hidden = !status.connected;
    $("calendar-disconnect").disabled = calendar.busy;
    $("calendar-selection").hidden = !status.connected || status.reauth_required;
    $("calendar-agenda").hidden = !status.connected || !status.calendar_id || status.reauth_required;
    $("calendar-queue").hidden = !status.connected;
    const pending = (status.queued_changes || 0) + (status.unknown_changes || 0);
    const parts = [];
    if (status.sync_running || status.sync_requested) parts.push(t("Sincronizzazione in corso…"));
    if (status.last_sync) parts.push(t("Ultima sincronizzazione: {0}", [new Date(status.last_sync * 1000).toLocaleString(locale)]));
    if (pending) parts.push(t("{0} operazioni in attesa", [pending]));
    if (status.conflicts) parts.push(t("{0} conflitti da risolvere nel ticket", [status.conflicts]));
    if (status.last_error) parts.push(t("L’ultima sincronizzazione non è riuscita. Riprova o rinnova il collegamento Google."));
    $("calendar-sync-status").textContent = parts.join(" · ");
    ["calendar-connect", "calendar-sync", "calendar-select", "calendar-calendars-more"].forEach((id) => { $(id).disabled = calendar.busy; });
    $("calendar-select").disabled = calendar.busy || !calendar.catalogReady || !$("calendar-choice").value;
    $("calendar-calendars-more").hidden = !calendar.calendarCursor;
    $("calendar-events-more").hidden = !calendar.cursor;
  }

  function googleEventLink(event) {
    try {
      const url = new URL(event.htmlLink);
      return url.origin === "https://calendar.google.com" && !url.username && !url.password ? url.href : null;
    } catch { return null; }
  }

  function renderCalendarEvents() {
    const week = calendarWeek();
    $("calendar-date").value = calendar.date;
    const last = new Date(week.end); last.setDate(last.getDate() - 1);
    const timezone = Intl.DateTimeFormat().resolvedOptions().timeZone;
    $("calendar-period").textContent = `${week.start.toLocaleDateString(locale, { day: "numeric", month: "short" })} – ${last.toLocaleDateString(locale, { day: "numeric", month: "short", year: "numeric" })} · ${timezone}`;
    const days = [];
    for (let i = 0; i < 7; i++) {
      const date = new Date(week.start); date.setDate(date.getDate() + i);
      const end = new Date(date); end.setDate(end.getDate() + 1);
      const section = node("section", "calendar-day");
      if (localDate(date) === localDate(new Date())) section.classList.add("is-today");
      section.append(node("h3", "", date.toLocaleDateString(locale, { weekday: "short", day: "numeric", month: "short" })));
      const list = node("ul", "calendar-day-events"); section.append(list);
      days.push({ date, end, list, section });
    }
    const series = [];
    const ordered = [...calendar.events].filter((event) => event.status !== "cancelled").sort((a, b) => String(a.start?.dateTime || a.start?.date || "").localeCompare(String(b.start?.dateTime || b.start?.date || "")));
    for (const event of ordered) {
      if (event.recurrence?.length) { series.push(event); continue; }
      const allDay = Boolean(event.start?.date);
      const start = allDay ? new Date(event.start.date + "T00:00:00") : new Date(event.start?.dateTime);
      const end = allDay ? new Date((event.end?.date || event.start.date) + "T00:00:00") : new Date(event.end?.dateTime);
      if (!Number.isFinite(+start) || !Number.isFinite(+end)) continue;
      for (const day of days) {
        if (start >= day.end || end <= day.date) continue;
        const item = node("li", "calendar-event");
        const time = allDay ? t("Tutto il giorno") : `${start.toLocaleTimeString(locale, { hour: "2-digit", minute: "2-digit" })} – ${end.toLocaleTimeString(locale, { hour: "2-digit", minute: "2-digit" })}`;
        item.append(node("span", "calendar-event-time", time));
        const link = googleEventLink(event);
        const title = node(link ? "a" : "strong", "", event.summary || t("Senza titolo"));
        if (link) { title.href = link; title.target = "_blank"; title.rel = "noopener noreferrer"; }
        item.append(title);
        if (event.location) item.append(node("span", "field-help", event.location));
        day.list.append(item);
      }
    }
    days.forEach((day) => { if (!day.list.children.length) day.list.append(node("li", "calendar-day-empty", calendar.cursor ? t("Altri eventi da caricare") : t("Nessun evento"))); });
    $("calendar-events").replaceChildren(...days.map((day) => day.section));
    $("calendar-series").hidden = !series.length;
    $("calendar-series-list").replaceChildren(...series.map((event) => node("li", "", event.summary || t("Senza titolo"))));
    $("calendar-events-more").hidden = !calendar.cursor;
  }

  async function loadCalendarCatalog(request, more = false) {
    const query = new URLSearchParams({ limit: "30" });
    if (more && calendar.calendarCursor) query.set("page_token", calendar.calendarCursor);
    const data = await api("/api/calendar/calendars?" + query, { timeoutMs: 30000 });
    if (request !== calendar.request) return;
    calendar.calendars = more ? [...calendar.calendars, ...data.items] : data.items;
    calendar.calendars = [...new Map(calendar.calendars.map((item) => [item.id, item])).values()];
    calendar.calendarCursor = data.next_page_token;
    calendar.catalogReady = true;
    const selected = calendar.status?.calendar_id;
    const choices = calendar.calendars.filter((item) => ["owner", "writer"].includes(item.accessRole));
    $("calendar-choice").replaceChildren(new Option(t("Seleziona un calendario modificabile"), ""), ...choices.map((item) => new Option(item.summary || item.id, item.id)));
    $("calendar-choice").value = choices.some((item) => item.id === selected) ? selected : "";
    renderCalendarStatus();
  }

  async function loadCalendarEvents(request, more = false) {
    const week = calendarWeek();
    // The API filters inclusive UTC days. Widen both edges, then filter by local
    // instants when rendering so midnight events survive timezone/DST changes.
    const query = new URLSearchParams({ limit: "100", from: new Date(+week.start - 86400000).toISOString().slice(0, 10), to: new Date(+week.end + 86400000).toISOString().slice(0, 10) });
    if (more && calendar.cursor) query.set("after_id", calendar.cursor);
    const data = await api("/api/calendar/events?" + query);
    if (request !== calendar.request) return;
    calendar.events = more ? [...calendar.events, ...data.items] : data.items;
    calendar.events = [...new Map(calendar.events.map((event) => [event.id, event])).values()];
    calendar.cursor = data.next_after_id;
    renderCalendarEvents();
  }

  async function refreshCalendar(catalog = false) {
    if (calendar.loading) return;
    const request = ++calendar.request;
    calendar.loading = true; $("calendar-loading").hidden = false; $("calendar-error").hidden = true;
    try {
      const status = await api("/api/calendar/status");
      if (request !== calendar.request) return;
      if (calendar.status?.calendar_id !== status.calendar_id || calendar.status?.connected !== status.connected) {
        calendar.events = []; calendar.cursor = null; calendar.catalogReady = false;
        calendar.calendars = []; calendar.calendarCursor = null;
        $("calendar-choice").replaceChildren(); renderCalendarEvents();
      }
      calendar.status = status; calendar.ready = true; renderCalendarStatus();
      if (status.connected && !status.reauth_required) {
        const tasks = [];
        if (catalog || !calendar.catalogReady) tasks.push(loadCalendarCatalog(request));
        if (status.calendar_id) tasks.push(loadCalendarEvents(request));
        tasks.push(api("/api/calendar/changes").then((data) => {
          if (request !== calendar.request) return;
          const labels = { queued: t("In attesa"), unknown: t("Esito da verificare"), applied: t("Applicata"), conflict: t("Conflitto"), failed: t("Non riuscita") };
          $("calendar-changes").replaceChildren(...data.items.map((item) => node("li", "calendar-change calendar-change-" + item.state, `${labels[item.state] || t("Da verificare")} · ${item.event_id || item.id}`)));
          $("calendar-changes-limit").hidden = !data.truncated;
        }));
        const results = await Promise.allSettled(tasks);
        const failed = results.find((result) => result.status === "rejected");
        if (failed) throw failed.reason;
      }
    } catch (error) {
      if (request === calendar.request) { $("calendar-error").textContent = calendarError(error); $("calendar-error").hidden = false; }
    } finally {
      if (request === calendar.request) { calendar.loading = false; $("calendar-loading").hidden = true; renderCalendarStatus(); }
    }
  }

  async function calendarAction(action, fields = {}) {
    if (calendar.busy) return;
    calendar.busy = true; renderCalendarStatus(); $("calendar-error").hidden = true;
    try {
      const data = await post("/api/calendar/" + action, fields);
      if (action === "oauth/start") {
        const target = new URL(data.authorization_url);
        if (target.origin !== "https://accounts.google.com" || target.username || target.password) throw new Error(t("Il collegamento Google restituito non è valido."));
        location.assign(target.href); return;
      }
      if (action === "disconnect") {
        $("calendar-disconnect-dialog").close();
        showNotice(data.remote_revoked === false ? t("Google scollegato da Punto. La revoca remota non è stata confermata.") : t("Google scollegato da Punto."));
        resetCalendar();
      } else if (action === "sync") showNotice(t("Sincronizzazione richiesta. Lo stato si aggiornerà al completamento."));
      else if (action === "selection") showNotice(t("Calendario selezionato. Sincronizzazione in attesa."));
      await refreshCalendar(action === "selection");
    } catch (error) { $("calendar-error").textContent = calendarError(error); $("calendar-error").hidden = false; }
    finally { calendar.busy = false; renderCalendarStatus(); }
  }

  function availableTabs() {
    const permissions = { audit: "view_audit", suppliers: "view_suppliers", contracts: "view_contracts", organization: "manage_groups" };
    return tabs.filter((tab) => (tab !== "users" || canReadUsers()) && (tab !== "alerts" || isAlertManager()) && (!permissions[tab] || can(permissions[tab])));
  }

  function applySession(session) {
    if (!session.authenticated) { redirectToLogin(); return false; }
    const authorizationChanged = state.session && (state.session.email !== session.email || state.session.role !== session.role || JSON.stringify(state.session.permissions) !== JSON.stringify(session.permissions));
    if (authorizationChanged) {
      resetTimeWorkflows();
      accessProfiles.request++; accessProfiles.user = null; accessProfiles.data = null; accessProfiles.catalog = [];
      $("access-groups-options").replaceChildren();
      resetWorkspaceFlows();
      // Responses started with the previous permissions must not restore its data.
      state.refreshRequest++;
      state.entriesRequest++;
      resetAlertsAndNotifications();
      resetWorkBoard(true);
      resetInventory();
      resetCalendar();
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
    renderIdentityManagement();
    if (!reportCanFilterUsers()) state.reportFilters.userId = "";
    if (!availableReportViews().includes(state.reportView)) state.reportView = "periodo";
    renderReportControls();
    $("session-name").textContent = session.name || session.email;
    $("session-name").title = session.name || session.email;
    $("session-email").textContent = session.email;
    $("session-email").title = session.email;
    $("tickets-app-link").hidden = !session.tickets?.configured;
    if (session.tickets?.configured) $("tickets-app-link").href = ticketAppURL();
    $("session-role").textContent = (Array.isArray(session.access_groups) ? session.access_groups : ["operator", ...(session.role === "manager" ? ["manager"] : []), ...(can("manage_contracts") ? ["sales"] : []), ...(can("view_costs") ? ["finance"] : [])]).map((code) => accessProfileLabels[code] || code).join(" · ");
    $("session-role").hidden = false;
    $("user-avatar").textContent = (session.name || session.email || "P").charAt(0).toUpperCase();
    $("new-job").hidden = !can("manage_jobs");
    $("tab-suppliers").hidden = !can("view_suppliers");
    $("tab-contracts").hidden = !can("view_contracts");
    $("tab-users").hidden = !canReadUsers();
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
    $("job-wizard-open").hidden = !can("manage_jobs");
    document.querySelector(".jobs-table").classList.toggle("jobs-with-assignments", can("manage_assignments"));
    $("entries-actions-heading").hidden = false;
    document.querySelector(".entries-table").classList.remove("entries-readonly");
    $("permission-hint").textContent = can("manage_jobs") ? t("Gestore · Gestisci il lavoro e le persone della tua squadra. Contratti e costi dipendono dai profili funzionali assegnati.") : t("Operatore · Registra il lavoro sulle commesse abilitate per te. Le ore inserite restano nel tuo registro.");
    $("jobs-panel-description").textContent = can("manage_jobs") ? t("Dal primo passo all’ultimo dettaglio, tutto sotto controllo.") : t("Le commesse abilitate per te, pronte per registrare il lavoro svolto.");
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
    if (!canReadUsers()) {
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
    for (const [kind, permission] of [["suppliers", "view_suppliers"], ["contracts", "view_contracts"]]) {
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
    if (!can("view_contracts")) {
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
    $("new-contract").hidden = !can("manage_contracts");
    $("new-supplier").hidden = !can("manage_suppliers");
    $("document-form").hidden = !can("manage_contracts");
    if (!can("manage_contracts") && $("contract-dialog").open) $("contract-dialog").close();
    if (!can("manage_suppliers") && $("supplier-dialog").open) $("supplier-dialog").close();
    if (!can("manage_labor_rates") && $("labor-rate-dialog").open) $("labor-rate-dialog").close();
    selectTab(availableTabs().includes(state.activeTab) ? state.activeTab : "jobs", false, false);
    if (authorizationChanged && state.activeTab === "reports") writeReportRoute(true);
    if (!can("view_team_hours")) { timeWorkflow.userId = ""; timeWorkflow.team = []; timeWorkflow.teamReady = false; }
    renderHoursTeam();
    $("documents-readonly-close").hidden = can("manage_contracts");
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
    $("workspace-summary").hidden = ["my-work", "work-schedules", "reports", "alerts", "notifications", "work-board", "inventory", "calendar"].includes(name);
    const title = name === "reports" ? t("Analisi · ") + reportViews[state.reportView].label : $("tab-" + name).querySelector("span").textContent;
    $("workspace-current-view").textContent = title;
    document.title = title + t(" · punto");
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
    if (name === "calendar") { renderCalendarTickets(); if (!calendar.loading && (entering || !calendar.ready)) refreshCalendar(true); }
    if (name === "users" && !state.usersReady) refreshUsers();
    if (name === "organization" && !state.organizationReady && !state.organizationLoading) refreshOrganization();
    if (name === "audit" && !state.auditReady && !state.auditLoading) refreshAudit();
    if (name === "alerts" && (entering || !state.alertRulesReady) && !state.alertRulesLoading) refreshAlertRules();
    if (name === "notifications" && (entering || !state.notificationsReady) && !state.notificationsLoading) refreshNotifications();
    if (name === "hours" && can("view_team_hours") && (entering || !timeWorkflow.teamReady)) refreshHoursTeam();
    if (name === "hours" && (entering || !timeWorkflow.requestsReady) && !timeWorkflow.requestsLoading) refreshTimeCorrections();
    if (name === "my-work" && !workspaceFlows.myLoading && (entering || !workspaceFlows.myWork)) refreshMyWork();
    if (name === "work-schedules" && !workspaceFlows.scheduleLoading) refreshWorkSchedules();
    if (name === "notifications" && !workspaceFlows.notificationLoading) refreshWorkNotifications();
    if (["suppliers", "contracts"].includes(name) && !state[name + "Ready"] && !state[name + "Loading"]) refreshRegistry(name);
  }

  function isReportManager() {
    return can("view_team_hours");
  }

  function availableReportViews() {
    return Object.keys(reportViews).filter((view) => !financialReport(view) || can("view_costs"));
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
    if (!validDate(filters.from) || !validDate(filters.to)) return t("Scegli una data iniziale e una data finale valide.");
    if (filters.from > filters.to) return t("La data finale deve essere uguale o successiva alla data iniziale.");
    if (reportDayNumber(filters.to) - reportDayNumber(filters.from) + 1 > 366) return t("Seleziona un periodo di massimo 366 giorni, comprese le date iniziale e finale.");
    for (const id of [filters.jobId, filters.userId]) {
      if (id && (!/^[1-9]\d*$/.test(id) || !Number.isSafeInteger(Number(id)))) return t("Il collegamento contiene un filtro non valido. Azzera i filtri e riprova.");
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
      const unavailable = node("option", "", t("Filtro selezionato (#{0}) · da verificare", [selected]));
      unavailable.value = selected;
      select.append(unavailable);
    }
    select.value = selected;
  }

  function renderReportControls() {
    const manager = reportCanFilterUsers();
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
    $("report-scope").textContent = financialReport() ? t("Vista Finance: costi aggregati per commessa, senza il dettaglio delle ore delle altre persone.") : manager ? t("Vista della squadra: le tue ore e quelle delle persone nei tuoi gruppi organizzativi, inclusi i sottogruppi.") : t("Vista personale: soltanto le ore del tuo account, comprese quelle già registrate sulle commesse non più abilitate.");
    $("report-cost-notice").hidden = state.reportView !== "costi";
    $("report-labor-notice").hidden = state.reportView !== "costo-ore";
    $("report-rates").hidden = state.reportView !== "costo-ore" || !can("view_labor_rates");
    $("report-new-rate").hidden = !can("manage_labor_rates");
    document.querySelectorAll("[data-report-preset]").forEach((button) => { button.disabled = !state.reportToday; });
    $("report-from").value = state.reportFilters.from;
    $("report-to").value = state.reportFilters.to;
    const options = state.reportData?.options || {};
    reportSelectOptions("report-job", options.jobs || [], state.reportFilters.jobId, manager || financialReport() ? t("Tutte le commesse") : t("Tutte le tue commesse con ore"), (job) => `${job.code} · ${job.name}`);
    reportSelectOptions("report-user", manager ? options.users || [] : [], manager ? state.reportFilters.userId : "", t("Tutta la squadra"), (user) => `${user.name || user.email}${user.active ? "" : t(" · Disattivato")}`);
  }

  function writeReportRoute(replace = false) {
    const query = new URLSearchParams({ from: state.reportFilters.from, to: state.reportFilters.to });
    if (state.reportFilters.jobId) query.set("job_id", state.reportFilters.jobId);
    if (reportCanFilterUsers() && state.reportFilters.userId) query.set("user_id", state.reportFilters.userId);
    const hash = "#analisi/" + state.reportView + "?" + query.toString();
    if (window.location.hash !== hash) window.history[replace ? "replaceState" : "pushState"](null, "", hash);
    state.routeHash = window.location.hash;
  }

  function applyReportRoute() {
    if (!state.session) return;
    const hash = window.location.hash.slice(1);
    state.routeHash = window.location.hash;
    if (hash === "main-content") return;
    const connection = /^ticket-connect\?state=([a-f0-9]{64})(&calendar=1)?$/.exec(hash);
    if (connection) {
      selectTab(connection[2] ? "calendar" : "jobs", false, false);
      if (connection[2]) {
        calendar.consentState = connection[1];
        $("ticket-calendar-consent-account").textContent = t("Account Punto: {0}", [state.session.name || state.session.email]);
        $("ticket-calendar-consent-error").hidden = true;
        if (!$("ticket-calendar-consent").open) $("ticket-calendar-consent").showModal();
      } else connectTicketSession(connection[1]);
      return;
    }
    const jobRoute = /^jobs\?job_id=([1-9]\d*)$/.exec(hash);
    if (jobRoute && Number.isSafeInteger(Number(jobRoute[1]))) {
      state.linkedJobId = Number(jobRoute[1]); selectTab("jobs", false, false); renderJobs(); return;
    }
    const assetRoute = /^inventory\?asset_id=([1-9]\d*)$/.exec(hash);
    if (assetRoute && Number.isSafeInteger(Number(assetRoute[1]))) {
      selectTab("inventory", false, false); openInventoryDetail(Number(assetRoute[1])); return;
    }
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
    state.reportFilters = { from: params.get("from") ?? defaults.from, to: params.get("to") ?? defaults.to, jobId: params.get("job_id") || "", userId: reportCanFilterUsers() ? params.get("user_id") || "" : "" };
    invalidateReports();
    renderReportControls();
    selectTab("reports", false, false);
    writeReportRoute(true);
  }

  function chooseReportView(view, moveFocus = false) {
    if (!availableReportViews().includes(view)) return;
    state.reportView = view;
    if (!reportCanFilterUsers()) state.reportFilters.userId = "";
    renderReportControls();
    writeReportRoute();
    $("workspace-current-view").textContent = t("Analisi · ") + reportViews[view].label;
    document.title = t("Analisi · ") + reportViews[view].label + t(" · punto");
    if (moveFocus) $("report-tab-" + view).focus();
    refreshReports();
    if (view === "costo-ore") refreshLaborRates();
  }

  function readReportFilters() {
    return { from: $("report-from").value, to: $("report-to").value, jobId: $("report-job").value, userId: reportCanFilterUsers() ? $("report-user").value : "" };
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
    if (filters.jobId) labels.push($("report-job").selectedOptions[0]?.textContent || t("Commessa selezionata"));
    if (reportCanFilterUsers() && filters.userId) labels.push($("report-user").selectedOptions[0]?.textContent || t("Utente selezionato"));
    labels.push(data.scope === "financial" ? t("Costi aggregati per commessa") : data.scope === "personal" ? t("Le tue ore") : t("Ore della squadra filtrate"));
    $("report-caption").textContent = labels.join(" · ");
    $("report-cost-summary").hidden = !cost;
    $("report-labor-summary").hidden = !labor;
    if (cost) {
      $("report-known-value").textContent = euroLabel(summary.known_value_cents);
      $("report-contract-count").textContent = t("{0} {1} alle ore filtrate. Ogni contratto viene contato una sola volta.", [summary.contract_count, Number(summary.contract_count) === 1 ? t("contratto distinto collegato") : t("contratti distinti collegati")]);
      $("report-unpriced-count").textContent = Number(summary.unpriced_contract_count) > 0 ? t("{0} {1}: importo sconosciuto, escluso dal totale noto.", [summary.unpriced_contract_count, Number(summary.unpriced_contract_count) === 1 ? t("contratto senza valore indicato") : t("contratti senza valore indicato")]) : t("Tutti i contratti elencati hanno un valore indicato.");
      $("report-uncontracted-hours").textContent = t("{0} su commesse senza contratto. Queste ore sono comprese nei totali del periodo.", [minutesLabel(summary.uncontracted_minutes)]);
    }
    if (labor) {
      const estimated = Number(summary.priced_minutes) > 0 ? euroLabel(summary.estimated_cost_cents) : t("Non disponibile");
      $("report-estimated-cost").textContent = estimated;
      $("report-priced-hours").textContent = t("{0} coperte da tariffa.", [minutesLabel(summary.priced_minutes)]);
      $("report-unpriced-hours").textContent = Number(summary.unpriced_minutes) > 0 ? t("{0} senza tariffa valida: la stima è {1}. Le ore senza tariffa non vengono considerate a costo zero.", [minutesLabel(summary.unpriced_minutes), Number(summary.priced_minutes) > 0 ? t("parziale") : t("non disponibile")]) : t("Tutte le ore filtrate sono coperte da tariffa.");
    }
    const headers = {
      periodo: [t("Giorno"), t("Ore"), t("Registrazioni"), t("Commesse"), t("Utenti")],
      commesse: [t("Commessa"), t("Cliente"), t("Stato"), t("Ore"), t("Registrazioni"), t("Utenti")],
      utenti: [t("Utente"), t("Accesso"), t("Ore"), t("Registrazioni"), t("Commesse")],
      costi: [t("Contratto"), t("Fornitore"), t("Validità contrattuale"), t("Valore contrattuale"), t("Stato"), t("Commesse con ore"), t("Ore nel periodo")],
      "costo-ore": [t("Commessa"), t("Ore"), t("Registrazioni"), t("Copertura tariffa"), t("Costo stimato (EUR)")]
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
        day.append(reportDrilldown(dateLabel(item.date), t("Vedi le commesse del {0}", [dateLabel(item.date)]), () => {
          state.reportFilters = { ...state.reportFilters, from: item.date, to: item.date };
          state.reportDefaultsPending = false;
          chooseReportView("commesse");
        }));
        row.append(day, reportCell(minutesLabel(item.minutes), true), reportCell(String(item.entry_count), true), reportCell(String(item.job_count), true), reportCell(String(item.user_count), true));
      } else if (state.reportView === "commesse") {
        const identity = reportCell();
        identity.append(reportDrilldown(item.name, t("Vedi il periodo per la commessa {0}", [item.code]), () => {
          state.reportFilters.jobId = String(item.job_id);
          chooseReportView("periodo");
        }), node("span", "report-secondary", item.code));
        const status = reportCell();
        status.append(node("span", "status-badge status-" + (Object.hasOwn(states, item.status) ? item.status : "planned"), states[item.status] || item.status));
        row.append(identity, reportCell(item.client || "—"), status, reportCell(minutesLabel(item.minutes), true), reportCell(String(item.entry_count), true), reportCell(String(item.user_count), true));
      } else if (state.reportView === "utenti") {
        const identity = reportCell();
        if (isReportManager()) identity.append(reportDrilldown(item.name || item.email, t("Vedi le commesse di {0}", [item.name || item.email]), () => {
          state.reportFilters.userId = String(item.user_id);
          chooseReportView("commesse");
        }));
        else identity.append(node("strong", "", item.name || item.email));
        identity.append(node("span", "report-secondary", item.email));
        row.append(identity, reportCell(item.active ? t("Attivo") : t("Disattivato")), reportCell(minutesLabel(item.minutes), true), reportCell(String(item.entry_count), true), reportCell(String(item.job_count), true));
      } else if (cost) {
        const identity = reportCell();
        identity.append(node("strong", "", item.title), node("span", "report-secondary", item.code));
        const value = item.value_cents === null || item.value_cents === undefined ? t("Non indicato") : euroLabel(item.value_cents);
        row.append(identity, reportCell(item.supplier_name || "—"), reportCell(contractPeriod(item)), reportCell(value, true), reportCell(contractStates[item.status] || item.status), reportCell(String(item.job_count), true), reportCell(minutesLabel(item.minutes), true));
      } else {
        const identity = reportCell();
        identity.append(node("strong", "", item.name || item.email), node("span", "report-secondary", item.email));
        const job = reportCell();
        job.append(node("strong", "", item.job_name), node("span", "report-secondary", item.code));
        const coverage = Number(item.priced_minutes) > 0 ? `${minutesLabel(item.priced_minutes)} coperte${Number(item.unpriced_minutes) > 0 ? " · " + minutesLabel(item.unpriced_minutes) + " scoperte" : ""}` : t("Nessuna tariffa valida");
        const amount = Number(item.priced_minutes) > 0 && item.cost_cents !== null ? euroLabel(item.cost_cents) + (Number(item.unpriced_minutes) > 0 ? t(" · Parziale") : "") : t("Non disponibile");
        row.append(job, reportCell(minutesLabel(item.minutes), true), reportCell(String(item.entry_count), true), reportCell(coverage), reportCell(amount, true));
      }
      $("report-body").append(row);
    }
    $("report-table-caption").textContent = `${reportViews[state.reportView].title}. ${$("report-caption").textContent}`;
    $("report-table-wrap").hidden = rows.length === 0;
    $("report-empty").hidden = rows.length !== 0;
    $("report-empty-text").textContent = cost && Number(summary.uncontracted_minutes) > 0 ? t("Le ore filtrate appartengono a commesse senza contratto: non è disponibile un valore contrattuale da mostrare.") : t("Non ci sono registrazioni per questi filtri. Prova un altro periodo oppure azzera i filtri.");
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
    if (reportCanFilterUsers() && filters.userId) query.set("user_id", filters.userId);
    try {
      const data = await api("/api/reports?" + query.toString());
      if (request !== state.reportRequest || state.activeTab !== "reports" || view !== state.reportView) return;
      if (!data.summary || !Array.isArray(data.rows) || !data.options || data.view !== reportViews[view].api || data.from !== filters.from || data.to !== filters.to || (financialReport(view) ? data.scope !== "financial" : !isReportManager() && data.scope !== "personal")) throw new Error(t("Il server ha restituito un’analisi non valida. Riprova."));
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
      if (request === state.reportRequest) {
        if (error.status === 403 && state.reportFilters.userId) {
          state.reportFilters.userId = "";
          renderReportControls(); writeReportRoute(true);
        }
        showReportError(error.message);
      }
    } finally {
      if (request === state.reportRequest) {
        state.reportLoading = false;
        $("report-loading").hidden = true;
        $("report-results").setAttribute("aria-busy", "false");
      }
    }
  }

  function renderLaborRates() {
    if (!can("view_labor_rates") || !state.laborRatesReady) return;
    $("report-rates-body").replaceChildren();
    for (const rate of state.laborRates) {
      const row = node("tr");
      const identity = reportCell();
      identity.append(node("strong", "", rate.user_name || rate.user_email), node("span", "report-secondary", rate.user_email));
      const actions = reportCell();
      if (can("manage_labor_rates")) actions.append(action(t("Modifica"), t("Modifica tariffa di {0} dal {1}", [rate.user_name || rate.user_email, dateLabel(rate.starts_on)]), () => openLaborRate(rate)));
      row.append(identity, reportCell(euroLabel(rate.hourly_cents) + t(" / ora"), true), reportCell(dateLabel(rate.starts_on)), reportCell(rate.ends_on ? dateLabel(rate.ends_on) : t("Senza scadenza")), actions);
      $("report-rates-body").append(row);
    }
    $("report-rates-table-wrap").hidden = state.laborRates.length === 0;
    $("report-rates-empty").hidden = state.laborRates.length !== 0;
  }

  async function refreshLaborRates() {
    if (!can("view_labor_rates")) return;
    const request = ++state.laborRatesRequest;
    state.laborRatesLoading = true;
    state.laborRatesReady = false;
    $("report-rates-loading").hidden = false;
    $("report-rates-error").hidden = true;
    $("report-rates-table-wrap").hidden = true;
    $("report-rates-empty").hidden = true;
    $("report-new-rate").disabled = true;
    try {
      const [data, users] = await Promise.all([api("/api/labor-rates"), can("manage_labor_rates") ? api("/api/users") : Promise.resolve({ users: [] })]);
      if (request !== state.laborRatesRequest || !can("view_labor_rates")) return;
      if (!Array.isArray(data.rates) || !Array.isArray(users.users)) throw new Error(t("Il server ha restituito un elenco tariffe non valido. Riprova."));
      state.laborRates = data.rates;
      state.laborRateUsers = users.users;
      state.laborRatesReady = true;
      renderLaborRates();
    } catch (error) {
      if (request === state.laborRatesRequest && can("view_labor_rates")) {
        $("report-rates-error-text").textContent = error.message;
        $("report-rates-error").hidden = false;
      }
    } finally {
      if (request === state.laborRatesRequest) {
        state.laborRatesLoading = false;
        $("report-rates-loading").hidden = true;
        $("report-new-rate").disabled = !can("manage_labor_rates") || !state.laborRatesReady;
      }
    }
  }

  function openLaborRate(rate = null) {
    if (!can("manage_labor_rates") || !state.laborRatesReady || busyDialogs.has("labor-rate-dialog")) return;
    state.editingLaborRate = rate;
    $("labor-rate-form").reset();
    $("labor-rate-hourly").setCustomValidity("");
    $("labor-rate-to").setCustomValidity("");
    $("labor-rate-error").hidden = true;
    $("labor-rate-dialog-title").textContent = rate ? t("Modifica tariffa oraria") : t("Nuova tariffa oraria");
    $("labor-rate-save").textContent = rate ? t("Salva modifiche") : t("Crea tariffa");
    reportSelectOptions("labor-rate-user", state.laborRateUsers, rate ? String(rate.user_id) : "", t("Seleziona un utente"), (user) => `${user.name || user.email}${user.active ? "" : t(" · Disattivato")}`);
    $("labor-rate-user").disabled = Boolean(rate);
    $("labor-rate-from").value = rate?.starts_on || state.reportToday || localDate(new Date());
    $("labor-rate-to").value = rate?.ends_on || "";
    if (rate) {
      const cents = BigInt(rate.hourly_cents);
      $("labor-rate-hourly").value = `${cents / 100n}${new Intl.NumberFormat(locale).formatToParts(1.1).find((part) => part.type === "decimal").value}${String(cents % 100n).padStart(2, "0")}`;
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
      $("stat-active-caption").textContent = active === 1 ? t("Una commessa prende forma") : t("Il lavoro che prende forma");
    }
    if (state.todayMinutes !== null) {
      const h = Math.floor(state.todayMinutes / 60);
      const m = state.todayMinutes % 60;
      $("stat-today").replaceChildren(node("span", "", h), node("span", "unit", "h"));
      if (m > 0) $("stat-today").append(node("span", "minute-value", String(m).padStart(2, "0")), node("span", "unit", "m"));
      $("stat-today").setAttribute("aria-label", minutesLabel(state.todayMinutes));
      $("stat-today-caption").textContent = state.todayCount === 1 ? t("1 registrazione nella giornata") : t("{0} registrazioni nella giornata", [state.todayCount]);
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
    renderCalendarTickets();
    if (!state.jobsReady) return;
    if (state.linkedJobId) {
      const linked = state.jobs.find((job) => job.id === state.linkedJobId);
      if (linked) { $("job-search").value = linked.code; $("job-filter").value = ""; }
      else showNotice(t("La commessa collegata non è disponibile o non è abilitata per il tuo account."), true);
      state.focusLinkedJob = linked?.id || null; state.linkedJobId = null;
    }
    $("jobs-loading").hidden = true;
    const search = $("job-search").value.trim().toLocaleLowerCase(locale);
    const filter = $("job-filter").value;
    const jobs = state.jobs.filter((job) => (!isJobArchived(job) || $("jobs-show-archived").checked) && (!filter || job.status === filter) && [job.code, job.name, job.client, ...(can("view_contracts") ? [job.contract_code, job.contract_title, job.supplier_name] : [])].some((value) => (value || "").toLocaleLowerCase(locale).includes(search)));
    $("jobs-body").replaceChildren();
    for (const job of jobs) {
      const row = node("tr");
      row.dataset.jobId = String(job.id);
      const nameCell = node("td");
      nameCell.append(jobIdentity(job.code, job.name));
      if (isJobArchived(job) || isJobTimeClosed(job)) nameCell.querySelector(".job-identity").append(node("span", "job-lifecycle-badge" + (isJobArchived(job) ? "" : " is-closed"), isJobArchived(job) ? t("Archiviata") : t("Ore chiuse")));
      if (isCommonJob(job)) nameCell.querySelector(".job-identity").append(node("span", "job-kind-badge job-kind-common", t("Comune · tutti gli utenti")));
      if (can("view_contracts") && job.contract_id) nameCell.querySelector(".job-identity").append(node("span", "job-contract-label", t("Contratto {0}{1}", [job.contract_code || job.contract_id, job.supplier_name ? " · " + job.supplier_name : ""])));
      const clientCell = node("td", job.client ? "" : "no-value", job.client || "—");
      const statusCell = node("td");
      statusCell.append(node("span", "status-badge status-" + (Object.hasOwn(states, job.status) ? job.status : "planned"), states[job.status] || t("Da avviare")));
      const hoursCell = node("td", "hours-cell", minutesLabel(job.total_minutes));
      const actionsCell = node("td");
      const actions = node("div", "row-actions");
      actions.append(action(t("Riepilogo"), t("Apri riepilogo della commessa ") + job.name, () => openJobOverview(job)));
      actions.append(action(t("Attività"), t("Apri attività e blocchi di ") + job.name, () => { if (state.workBusy) return; resetWorkBoard(true); state.workJobId = String(job.id); selectTab("work-board"); }), node("span", "row-divider"));
      if (state.session?.tickets?.configured) actions.append(ticketAppLink(t("Ticket ↗"), "job", job.id), node("span", "row-divider"), ticketAppLink(t("+ Ticket"), "job", job.id, true), node("span", "row-divider"), ticketAppLink(t("Agenda ↗"), "job", job.id, false, true), node("span", "row-divider"));
      if (can("manage_jobs")) actions.append(action(t("Modifica"), t("Modifica la commessa ") + job.name, () => openJob(job)), node("span", "row-divider"));
      if (can("manage_jobs")) actions.append(action(t("Chiusura / archivio"), t("Gestisci chiusura e archivio di ") + job.name, () => openJobLifecycle(job)), node("span", "row-divider"));
      if (can("manage_assignments")) actions.append(action(t("Spiega accesso"), t("Spiega accesso alla commessa ") + job.name, () => openJobAccess(job)), node("span", "row-divider"));
      if (can("manage_assignments")) {
        const count = Number(job.assigned_users) || 0;
        const groupCount = Number(job.assigned_groups) || 0;
        actions.append(action(t("Abilitazioni · ") + count + (groupCount ? " + " + groupCount + t(" gruppi") : ""), t("Gestisci le abilitazioni della commessa ") + job.name + ": " + count + t(" utenti e ") + groupCount + t(" gruppi"), () => openAssignments(job)), node("span", "row-divider"));
      }
      if (canLogTime(job)) actions.append(action(t("+ Ore"), t("Registra ore per ") + job.name, () => openEntry(null, job.id)));
      else actions.append(node("span", "readonly-job-label", t("Sola consultazione")));
      actionsCell.append(actions);
      row.append(nameCell, clientCell, statusCell, hoursCell, actionsCell);
      $("jobs-body").append(row);
    }
    if (state.focusLinkedJob) {
      const linked = $("jobs-body").querySelector(`[data-job-id="${state.focusLinkedJob}"]`);
      if (linked) { linked.tabIndex = -1; linked.focus(); linked.scrollIntoView({ block: "center" }); }
      state.focusLinkedJob = null;
    }
    const empty = jobs.length === 0;
    const noJobs = state.jobs.length === 0;
    $("jobs-table-wrap").hidden = empty;
    $("jobs-empty").hidden = !empty;
    $("jobs-table-footer").hidden = empty;
    $("jobs-empty-title").textContent = noJobs ? can("manage_jobs") ? t("La prossima commessa parte da qui.") : t("Nessuna commessa abilitata.") : t("Nessuna commessa corrisponde alla ricerca.");
    $("jobs-empty-description").textContent = noJobs ? can("manage_jobs") ? t("Aggiungi la prima commessa, scegli il suo stato e inizia a registrare il lavoro.") : t("Un gestore deve abilitarti a una commessa prima che tu possa registrare il lavoro svolto. Le ore già inserite restano nel tuo registro.") : t("Prova con un altro nome, codice o cliente, oppure cambia il filtro di stato.");
    $("empty-new-job").hidden = !noJobs || !can("manage_jobs");
    $("clear-filters").hidden = noJobs;
    $("jobs-visible-count").textContent = `${jobs.length} ${jobs.length === 1 ? t("commessa") : t("commesse")}${jobs.length !== state.jobs.length ? t(" su ") + state.jobs.length : ""}`;
    ["new-entry", "panel-new-entry", "empty-new-entry"].forEach((id) => { $(id).hidden = !can("manage_jobs") && !state.jobs.some(canLogTime); });
    if ($("entry-dialog").open) renderEntryJobs($("entry-job").value, true);
    updateTimeWorkflowControls();
    renderStats();
  }

  function canLogTime(job) {
    return !isJobArchived(job) && !isJobTimeClosed(job) && (can("manage_jobs") || job.can_log_time === true);
  }

  function isJobArchived(job) { return job?.archived === true || job?.archived === 1; }
  function isJobTimeClosed(job) { return job?.time_closed === true || job?.time_closed === 1; }

  function isCommonJob(job) {
    return job?.is_common === true || job?.is_common === 1;
  }

  function renderEntryJobs(selectedId = "", notifyUnavailable = false) {
    const writableJobs = state.jobs.filter(canLogTime);
    const options = [node("option", "", writableJobs.length ? t("Seleziona una commessa") : t("Nessuna commessa abilitata"))];
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
      const nextStep = writableJobs.length ? t("Scegli una commessa abilitata.") : t("Chiedi a un gestore di abilitarti a una commessa.");
      dialogError("entry-error", new Error(t("La commessa selezionata non è più abilitata alla registrazione delle ore. {0} Data, ore e nota sono conservate.", [nextStep])));
    }
  }

  function renderEntries() {
    if (!state.entriesReady) return;
    updateTimeWorkflowControls();
    if (timeWorkflow.view === "week") { renderWeekEntries(); return; }
    $("entries-loading").hidden = true;
    $("entries-body").replaceChildren();
    for (const entry of state.entries) {
      const row = node("tr");
      const nameCell = node("td");
      nameCell.append(jobIdentity(entry.job_code, entry.job_name));
      const noteCell = node("td", entry.note ? "entry-note" : "no-value", entry.note || "—");
      if (entry.task_title) noteCell.prepend(node("strong", "time-entry-task", entry.task_title));
      noteCell.title = entry.note || t("Nessuna nota");
      const hoursCell = node("td", "hours-cell", minutesLabel(entry.minutes));
      row.append(nameCell, noteCell, hoursCell);
      const actionsCell = node("td");
      const actions = node("div", "row-actions");
      actions.append(action(t("Dettagli"), t("Apri la registrazione per ") + entry.job_name, () => openTimeEntryDetail(entry)));
      if (viewingOwnHours() && can("edit_time_entries")) {
        actions.append(action(t("Modifica"), t("Modifica le ore per ") + entry.job_name, () => openEntry(entry)), node("span", "row-divider"), action(t("Elimina"), t("Elimina le ore per ") + entry.job_name, () => openDelete(entry), true));
      } else if (viewingOwnHours()) actions.append(action(t("Richiedi rettifica"), t("Richiedi rettifica delle ore per ") + entry.job_name, () => openTimeCorrection(entry)));
      actionsCell.append(actions);
      row.append(actionsCell);
      $("entries-body").append(row);
    }
    $("entries-table-wrap").hidden = state.entries.length === 0;
    $("entries-empty").hidden = state.entries.length !== 0;
    $("day-total").textContent = minutesLabel(state.entries.reduce((sum, entry) => sum + Number(entry.minutes), 0));
    $("day-entries-count").textContent = `${state.entries.length} ${state.entries.length === 1 ? t("registrazione") : t("registrazioni")}`;
    $("go-today").disabled = state.selectedDate === localDate(new Date());
    renderStats();
  }

  function renderHoursTeam() {
    $("hours-team-toolbar").hidden = !can("view_team_hours");
    const select = $("hours-team-user");
    select.replaceChildren(new Option(t("Le mie ore"), ""));
    for (const user of timeWorkflow.team) {
      if (String(user.id) === String(state.session?.user_id || state.session?.id || "") || user.email === state.session?.email) continue;
      select.append(new Option((user.name || user.email) + (user.active ? "" : t(" · Disattivato")), user.id));
    }
    select.value = timeWorkflow.userId;
    $("hours-panel-title").textContent = viewingOwnHours() ? t("Il tuo registro ore") : t("Registro ore della squadra");
    $("hours-team-scope").textContent = viewingOwnHours() ? t("Le nuove registrazioni restano associate al tuo account. Puoi consultare le persone della tua squadra.") : t("Stai consultando le ore di {0}. Questa vista è di sola lettura; nuove ore e rettifiche riguardano il tuo account.", [select.selectedOptions[0]?.textContent || t("Persona selezionata")]);
    updateTimeWorkflowControls();
  }

  async function refreshHoursTeam() {
    if (!can("view_team_hours")) return;
    const request = ++timeWorkflow.teamRequest;
    $("hours-team-user").disabled = true; $("hours-team-retry").hidden = true;
    try {
      const data = await api("/api/time-team");
      if (request !== timeWorkflow.teamRequest || !can("view_team_hours")) return;
      if (!Array.isArray(data.users)) throw new Error(t("L’elenco della squadra non è disponibile. Riprova."));
      timeWorkflow.team = data.users; timeWorkflow.teamReady = true;
      if (timeWorkflow.userId && !data.users.some((user) => String(user.id) === timeWorkflow.userId)) { timeWorkflow.userId = ""; refreshEntries(); }
      renderHoursTeam();
    } catch (error) {
      if (request === timeWorkflow.teamRequest) { $("hours-team-retry").hidden = false; $("hours-team-scope").textContent = error.message; }
    } finally { if (request === timeWorkflow.teamRequest) $("hours-team-user").disabled = false; }
  }

  function resetTimeWorkflows() {
    timeWorkflow.userId = ""; timeWorkflow.team = []; timeWorkflow.teamReady = false; timeWorkflow.teamRequest++;
    timeWorkflow.requestsRequest++; timeWorkflow.lifecycleRequest++;
    timeWorkflow.requests = []; timeWorkflow.requestsReady = false; timeWorkflow.requestsLoading = false;
    timeWorkflow.week = null; timeWorkflow.entry = null; timeWorkflow.detailEntry = null;
    timeWorkflow.lifecycle = null; timeWorkflow.lifecycleJob = null; timeWorkflow.decision = null;
    timeWorkflow.batchRows = []; timeWorkflow.batchKey = null; timeWorkflow.batchPayload = null;
    ["hours-week-grid", "time-corrections-list", "hours-entry-detail-content", "job-lifecycle-summary"].forEach((id) => $(id).replaceChildren());
    $("hours-week-panel").hidden = true;
  }

  function shiftedWorkDate(value, offset) {
    const date = new Date(value + "T12:00:00");
    date.setDate(date.getDate() + offset);
    return localDate(date);
  }

  function workWeekStart(value) {
    const date = new Date(value + "T12:00:00");
    return shiftedWorkDate(value, -((date.getDay() + 6) % 7));
  }

  function updateTimeWorkflowControls() {
    const week = timeWorkflow.view === "week";
    $("hours-view-day").setAttribute("aria-pressed", String(!week));
    $("hours-view-week").setAttribute("aria-pressed", String(week));
    document.querySelector('label[for="entry-date-filter"]').textContent = week ? t("Settimana del") : t("Giornata lavorativa");
    $("previous-day").setAttribute("aria-label", week ? t("Settimana precedente") : t("Giorno precedente"));
    $("next-day").setAttribute("aria-label", week ? t("Settimana successiva") : t("Giorno successivo"));
    document.querySelector(".daily-total > span").textContent = week ? t("Totale della settimana") : t("Totale della giornata");
    const writable = state.jobsReady && state.jobs.some(canLogTime);
    $("hours-batch-open").hidden = !viewingOwnHours();
    $("hours-copy-week").hidden = !viewingOwnHours();
    $("hours-batch-open").disabled = !writable;
    $("hours-copy-week").disabled = !writable || timeWorkflow.copyBusy;
    $("hours-copy-week").textContent = timeWorkflow.copyBusy ? t("Preparazione anteprima…") : t("Copia settimana precedente");
    $("hours-week-panel").hidden = !week || !state.entriesReady;
  }

  function renderWeekEntries() {
    const data = timeWorkflow.week;
    $("entries-loading").hidden = true; $("entries-table-wrap").hidden = true; $("entries-empty").hidden = true;
    if (!data) { $("hours-week-panel").hidden = true; return; }
    const start = data.start || workWeekStart(state.selectedDate), end = data.end || shiftedWorkDate(start, 6);
    const days = Array.from({ length: 7 }, (_, index) => shiftedWorkDate(start, index));
    $("hours-week-panel").hidden = false;
    $("hours-week-period").textContent = t("Dal {0} al {1}. Seleziona una registrazione per leggere i dettagli.", [dateLabel(start), dateLabel(end)]);
    const entries = data.entries || [], table = node("table", "hours-week-table"), caption = node("caption", "sr-only", t("Registro settimanale per commessa, da lunedì a domenica"));
    const head = node("thead"), heading = node("tr"); heading.append(node("th", "", t("Commessa")));
    for (const day of days) {
      const cell = node("th"), date = new Date(day + "T12:00:00");
      cell.scope = "col"; cell.append(node("span", "", date.toLocaleDateString(locale, { weekday: "short" })));
      const when = node("time", "", date.toLocaleDateString(locale, { day: "numeric", month: "short" })); when.dateTime = day; cell.append(when);
      if (day === localDate(new Date())) cell.setAttribute("aria-current", "date");
      heading.append(cell);
    }
    heading.append(node("th", "", t("Totale"))); head.append(heading);
    const body = node("tbody"), groups = new Map();
    for (const entry of entries) {
      const key = String(entry.job_id);
      if (!groups.has(key)) groups.set(key, { id: entry.job_id, code: entry.job_code, name: entry.job_name, entries: [] });
      groups.get(key).entries.push(entry);
    }
    for (const group of groups.values()) {
      const row = node("tr"), title = node("th"); title.scope = "row"; title.append(jobIdentity(group.code, group.name)); row.append(title);
      for (const day of days) {
        const cell = node("td"), items = group.entries.filter((entry) => entry.date === day);
        for (const entry of items) {
          const button = node("button", "hours-week-entry"); button.type = "button";
          button.setAttribute("aria-label", t("{0}, {1}, {2}. Apri dettaglio", [group.name, dateLabel(day), minutesLabel(entry.minutes)]));
          button.append(node("strong", "", minutesLabel(entry.minutes)));
          if (entry.note) button.append(node("small", "", entry.note));
          button.addEventListener("click", () => openTimeEntryDetail(entry)); cell.append(button);
        }
        const job = state.jobs.find((item) => String(item.id) === String(group.id));
        if (viewingOwnHours() && job && canLogTime(job)) cell.append(action(t("+ Aggiungi"), t("Aggiungi ore per {0}, {1}", [group.name, dateLabel(day)]), () => openHoursBatch([{ job_id: group.id, date: day, minutes: 60, note: "" }])));
        if (!cell.children.length) cell.textContent = "—";
        row.append(cell);
      }
      row.append(node("td", "hours-cell", minutesLabel(group.entries.reduce((sum, entry) => sum + Number(entry.minutes), 0)))); body.append(row);
    }
    if (!groups.size) { const row = node("tr"), cell = node("td", "hours-week-empty", t("Nessuna ora registrata in questa settimana. Aggiungi righe oppure copia la settimana precedente.")); cell.colSpan = 9; row.append(cell); body.append(row); }
    const foot = node("tfoot"), totals = node("tr"); totals.append(node("th", "", t("Totale")));
    for (const day of days) totals.append(node("td", "hours-cell", minutesLabel(entries.filter((entry) => entry.date === day).reduce((sum, entry) => sum + Number(entry.minutes), 0))));
    totals.append(node("td", "hours-cell", minutesLabel(data.total_minutes))); foot.append(totals);
    table.append(caption, head, body, foot); $("hours-week-grid").replaceChildren(table);
    $("day-total").textContent = minutesLabel(data.total_minutes);
    $("day-entries-count").textContent = t("{0} registrazioni nella settimana", [entries.length]);
    $("go-today").disabled = start === workWeekStart(localDate(new Date())); renderStats();
  }

  function openTimeEntryDetail(entry) {
    timeWorkflow.detailEntry = entry;
    const data = node("dl");
    for (const [label, value] of [[t("Commessa"), entry.job_code + " · " + entry.job_name], [t("Giornata lavorativa"), dateLabel(entry.date)], [t("Ore lavorate"), minutesLabel(entry.minutes)], [t("Attività svolta / nota"), entry.note || t("Nessuna nota")]]) data.append(node("dt", "", label), node("dd", "", value));
    $("hours-entry-detail-content").replaceChildren(data);
    const close = action(t("Chiudi"), t("Chiudi dettaglio"), () => $("hours-entry-detail").close());
    const correction = action(t("Richiedi rettifica"), t("Richiedi rettifica"), () => { $("hours-entry-detail").close(); openTimeCorrection(entry); });
    $("hours-entry-detail-actions").replaceChildren(close);
    if (viewingOwnHours()) $("hours-entry-detail-actions").append(correction);
    if (viewingOwnHours() && can("edit_time_entries")) $("hours-entry-detail-actions").append(action(t("Modifica"), t("Modifica registrazione"), () => { $("hours-entry-detail").close(); openEntry(entry); }));
    $("hours-entry-detail").showModal(); close.focus();
  }

  function batchSelectJobs(select, selected) {
    select.replaceChildren(); const empty = node("option", "", t("Seleziona una commessa")); empty.value = ""; select.append(empty);
    const jobs = state.jobs.filter(canLogTime);
    for (const job of jobs) { const option = node("option", "", job.code + " · " + job.name); option.value = String(job.id); select.append(option); }
    if (selected && !jobs.some((job) => String(job.id) === String(selected))) { const option = node("option", "", t("Commessa non disponibile: scegline un’altra")); option.value = String(selected); option.disabled = true; select.append(option); }
    select.value = selected ? String(selected) : "";
  }

  function openHoursBatch(rows = null) {
    if (busyDialogs.has("hours-batch-dialog") || !state.jobs.some(canLogTime)) return;
    timeWorkflow.batchRows = (rows || [{ job_id: "", date: state.selectedDate, minutes: 60, note: "" }]).map((row) => ({ ...row, key: ++timeWorkflow.batchSequence }));
    timeWorkflow.batchKey = null; timeWorkflow.batchPayload = null;
    $("hours-batch-error").hidden = true; renderHoursBatch();
    $("hours-batch-dialog").showModal(); $("hours-batch-rows").querySelector("select")?.focus();
  }

  function batchField(label, control, className) {
    const field = node("div", "work-field " + className), title = node("label", "", label); title.htmlFor = control.id; field.append(title, control); return field;
  }

  function renderHoursBatch() {
    const parent = $("hours-batch-rows"); parent.replaceChildren();
    for (const row of timeWorkflow.batchRows) {
      const wrapper = node("div", "hours-batch-row"), select = node("select"); select.id = "batch-job-" + row.key; select.required = true; batchSelectJobs(select, row.job_id);
      select.addEventListener("change", () => { row.job_id = select.value; row.task_id = null; row.task_title = ""; renderHoursBatch(); $("batch-job-" + row.key)?.focus(); });
      const date = node("input"); date.type = "date"; date.id = "batch-date-" + row.key; date.required = true; date.min = "0001-01-01"; date.max = "9999-12-31"; date.value = row.date;
      date.addEventListener("input", () => { row.date = date.value; updateHoursBatchTotal(); });
      const minutes = node("input"); minutes.id = "batch-minutes-" + row.key; minutes.type = "number"; minutes.min = "1"; minutes.max = "1440"; minutes.step = "1"; minutes.required = true; minutes.value = row.minutes;
      minutes.addEventListener("input", () => { row.minutes = minutes.value; updateHoursBatchTotal(); });
      const note = node("textarea"); note.id = "batch-note-" + row.key; note.rows = 2; note.maxLength = 1000; note.value = row.note || ""; note.addEventListener("input", () => { row.note = note.value; });
      const remove = action(t("Rimuovi"), t("Rimuovi riga {0}", [timeWorkflow.batchRows.indexOf(row) + 1]), () => { timeWorkflow.batchRows = timeWorkflow.batchRows.filter((item) => item !== row); renderHoursBatch(); $("hours-batch-add").focus(); }); remove.classList.add("batch-remove");
      wrapper.append(batchField(t("Commessa"), select, "batch-job"), batchField(t("Data"), date, "batch-date"), batchField(t("Minuti"), minutes, "batch-minutes"), batchField(t("Nota"), note, "batch-note"), remove);
      if (row.task_id) { const task = node("div", "batch-task workflow-note"); task.append(node("span", "", t("Attività collegata: {0}", [row.task_title || "#" + row.task_id])), action(t("Rimuovi collegamento attività"), t("Rimuovi l’attività da questa riga"), () => { row.task_id = null; row.task_title = ""; renderHoursBatch(); })); wrapper.append(task); }
      parent.append(wrapper);
    }
    $("hours-batch-add").disabled = timeWorkflow.batchRows.length >= 100;
    $("hours-batch-save").disabled = !timeWorkflow.batchRows.length;
    updateHoursBatchTotal();
  }

  function updateHoursBatchTotal() {
    const minutes = timeWorkflow.batchRows.reduce((sum, row) => sum + (Number(row.minutes) || 0), 0);
    $("hours-batch-total").textContent = t("{0} righe da aggiungere · {1}. Nessuna registrazione ancora salvata.", [timeWorkflow.batchRows.length, minutesLabel(minutes)]);
  }

  async function copyPreviousHoursWeek() {
    if (!viewingOwnHours() || timeWorkflow.copyBusy || busyDialogs.has("hours-batch-dialog")) return;
    const target = workWeekStart(state.selectedDate), source = shiftedWorkDate(target, -7), identity = state.session?.email;
    timeWorkflow.copyBusy = true; updateTimeWorkflowControls();
    try {
      const data = await api("/api/time-entries/week?date=" + encodeURIComponent(source));
      if (state.session?.email !== identity) return;
      const rows = (data.entries || []).map((entry) => ({ job_id: entry.job_id, task_id: entry.task_id || null, task_title: entry.task_title || "", date: shiftedWorkDate(entry.date, 7), minutes: entry.minutes, note: entry.note || "" }));
      if (!rows.length) { showNotice(t("La settimana precedente non contiene ore da copiare.")); return; }
      if (rows.length > 100) { showNotice(t("La settimana precedente contiene più di 100 righe. Copia un gruppo più piccolo con Aggiungi righe."), true); return; }
      openHoursBatch(rows);
    } catch (error) { if (state.session?.email === identity) showNotice(error.message, true); }
    finally { timeWorkflow.copyBusy = false; updateTimeWorkflowControls(); }
  }

  function workflowError(id, error) { dialogError(id, error); $(id).focus(); }

  async function saveHoursBatch(event) {
    event.preventDefault(); if (busyDialogs.has("hours-batch-dialog")) return;
    $("hours-batch-error").hidden = true;
    if (!$("hours-batch-form").reportValidity()) return;
    const rows = timeWorkflow.batchRows.map((row) => ({ job_id: Number(row.job_id), task_id: row.task_id ? Number(row.task_id) : null, date: row.date, minutes: Number(row.minutes), note: (row.note || "").trim() }));
    const invalid = rows.some((row) => !validDate(row.date) || !Number.isInteger(row.minutes) || row.minutes < 1 || row.minutes > 1440 || !state.jobs.some((job) => Number(job.id) === row.job_id && canLogTime(job)));
    if (!rows.length || rows.length > 100 || invalid) { workflowError("hours-batch-error", new Error(t("Controlla tutte le righe: servono una commessa abilitata, una data valida e da 1 a 1440 minuti."))); return; }
    const totals = new Map(); rows.forEach((row) => totals.set(row.date, (totals.get(row.date) || 0) + row.minutes));
    if ([...totals.values()].some((minutes) => minutes > 1440)) { workflowError("hours-batch-error", new Error(t("Le nuove righe superano 24 ore nella stessa giornata. Riduci i minuti prima di confermare."))); return; }
    const payload = JSON.stringify(rows);
    if (payload !== timeWorkflow.batchPayload) { timeWorkflow.batchKey = crypto.randomUUID(); timeWorkflow.batchPayload = payload; }
    dialogBusy("hours-batch-dialog", true, "hours-batch-save", t("Conferma e salva tutte le righe"));
    try {
      await post("/api/time-entries/batch", { request_key: timeWorkflow.batchKey, entries: payload });
      $("hours-batch-dialog").close(); state.auditReady = false; timeWorkflow.userId = ""; renderHoursTeam();
      showNotice(t("{0} registrazioni salvate.", [rows.length])); await refreshAll();
    } catch (error) { workflowError("hours-batch-error", error); }
    finally { dialogBusy("hours-batch-dialog", false, "hours-batch-save", t("Conferma e salva tutte le righe")); $("hours-batch-add").disabled = timeWorkflow.batchRows.length >= 100; $("hours-batch-save").disabled = !timeWorkflow.batchRows.length; }
  }

  function openTimeCorrection(entry) {
    if (!viewingOwnHours() || busyDialogs.has("time-correction-dialog")) return;
    timeWorkflow.entry = entry; $("time-correction-form").reset(); $("time-correction-error").hidden = true; $("time-correction-hours").setCustomValidity("");
    $("time-correction-original").textContent = t("Originale: {0} · {1} · {2}", [entry.job_name, dateLabel(entry.date), minutesLabel(entry.minutes)]);
    batchSelectJobs($("time-correction-job"), entry.job_id);
    const selected = $("time-correction-job").querySelector('option[value="' + entry.job_id + '"]'); if (selected) { selected.disabled = false; selected.textContent = entry.job_code + " · " + entry.job_name; }
    $("time-correction-date").value = entry.date; $("time-correction-hours").value = Math.floor(entry.minutes / 60) + ":" + String(entry.minutes % 60).padStart(2, "0"); $("time-correction-note").value = entry.note || "";
    $("time-correction-fields").disabled = false; $("time-correction-dialog").showModal(); $("time-correction-reason").focus();
  }

  async function refreshTimeCorrections() {
    if (!state.session?.authenticated || timeWorkflow.requestsLoading) return;
    const request = ++timeWorkflow.requestsRequest; timeWorkflow.requestsLoading = true;
    $("time-corrections-loading").hidden = false; $("time-corrections-error").hidden = true; $("time-corrections-refresh").disabled = true;
    try {
      const data = await api("/api/time-corrections"); if (request !== timeWorkflow.requestsRequest) return;
      timeWorkflow.requests = data.requests || []; timeWorkflow.requestsReady = true; renderTimeCorrections();
    } catch (error) { if (request === timeWorkflow.requestsRequest) dialogError("time-corrections-error", error); }
    finally { if (request === timeWorkflow.requestsRequest) { timeWorkflow.requestsLoading = false; $("time-corrections-loading").hidden = true; $("time-corrections-refresh").disabled = false; } }
  }

  const correctionStatuses = { pending: t("In attesa"), approved: t("Approvata"), rejected: t("Rifiutata"), cancelled: t("Annullata"), conflict: t("Conflitto: registrazione cambiata") };
  function correctionComparison(request) {
    const comparison = node("div", "time-correction-comparison");
    for (const [title, record, deleted] of [[t("Originale"), request.before || {}, false], [t("Proposta"), request, request.delete_requested]]) {
      const panel = node("div"); panel.append(node("h5", "", title));
      if (deleted) panel.append(node("p", "", t("Cancellazione della registrazione")));
      else {
        panel.append(node("p", "", (record.job_code || "") + " · " + (record.job_name || t("Commessa non disponibile"))));
        panel.append(node("p", "", (record.date && validDate(record.date) ? dateLabel(record.date) : "—") + " · " + minutesLabel(record.minutes)));
        if (record.note) panel.append(node("p", "", record.note));
      }
      comparison.append(panel);
    }
    return comparison;
  }

  function renderTimeCorrections() {
    $("time-corrections-scope").textContent = can("manage_jobs") ? t("Coda delle richieste della tua squadra. Un gestore della squadra diverso dal richiedente può approvare o rifiutare.") : t("Le tue richieste, con originale, proposta e decisione del gestore.");
    $("time-corrections-empty").hidden = timeWorkflow.requests.length !== 0; $("time-corrections-list").replaceChildren();
    for (const request of timeWorkflow.requests) {
      const item = node("li", "time-correction-card"), heading = node("div", "time-correction-meta");
      heading.append(node("h4", "", request.requester_name || request.requester_email || t("Richiesta di rettifica")), node("span", "correction-status correction-status-" + request.status, correctionStatuses[request.status] || request.status)); item.append(heading, correctionComparison(request));
      item.append(node("p", "time-correction-reason", t("Motivo: {0}", [request.reason || "—"])));
      if (request.decision_reason) item.append(node("p", "time-correction-reason", t("Decisione: {0}", [request.decision_reason])));
      const actions = node("div", "row-actions");
      if (request.can_decide) actions.append(action(t("Approva"), t("Approva richiesta {0}", [request.id]), () => openTimeDecision(request, "approved")), action(t("Rifiuta"), t("Rifiuta richiesta {0}", [request.id]), () => openTimeDecision(request, "rejected"), true));
      if (request.can_cancel) actions.append(action(t("Annulla richiesta"), t("Annulla richiesta {0}", [request.id]), () => openTimeDecision(request, "cancelled")));
      if (actions.children.length) item.append(actions); $("time-corrections-list").append(item);
    }
  }

  async function saveTimeCorrection(event) {
    event.preventDefault(); if (!timeWorkflow.entry || busyDialogs.has("time-correction-dialog")) return;
    $("time-correction-hours").setCustomValidity(""); $("time-correction-reason").value = $("time-correction-reason").value.trim();
    if (!$("time-correction-form").reportValidity()) return;
    const entry = timeWorkflow.entry, deleted = $("time-correction-delete").checked;
    let minutes = Number(entry.minutes);
    if (!deleted) { try { minutes = hoursToMinutes($("time-correction-hours").value); } catch (error) { $("time-correction-hours").setCustomValidity(error.message); $("time-correction-hours").reportValidity(); return; } }
    const fields = { entry_id: entry.id, revision: entry.revision, job_id: deleted ? entry.job_id : $("time-correction-job").value, date: deleted ? entry.date : $("time-correction-date").value, minutes, note: deleted ? entry.note || "" : $("time-correction-note").value.trim(), reason: $("time-correction-reason").value, delete_requested: deleted ? "1" : "0" };
    if (!validDate(fields.date)) { workflowError("time-correction-error", new Error(t("Scegli una giornata lavorativa valida."))); return; }
    dialogBusy("time-correction-dialog", true, "time-correction-save", t("Invia richiesta di rettifica"));
    try { await post("/api/time-corrections", fields); $("time-correction-dialog").close(); showNotice(t("Richiesta inviata. La registrazione originale resta invariata fino all’approvazione.")); await refreshTimeCorrections(); }
    catch (error) { workflowError("time-correction-error", error); }
    finally { dialogBusy("time-correction-dialog", false, "time-correction-save", t("Invia richiesta di rettifica")); $("time-correction-fields").disabled = $("time-correction-delete").checked; }
  }

  function openTimeDecision(request, decision) {
    if (busyDialogs.has("time-decision-dialog")) return;
    timeWorkflow.decision = { request, decision }; $("time-decision-form").reset(); $("time-decision-error").hidden = true;
    $("time-decision-title").textContent = decision === "approved" ? t("Approva rettifica") : decision === "rejected" ? t("Rifiuta rettifica") : t("Annulla richiesta");
    $("time-decision-context").replaceChildren(correctionComparison(request), node("p", "workflow-note", t("Motivo della richiesta: {0}", [request.reason])));
    $("time-decision-save").textContent = t("Conferma decisione"); $("time-decision-dialog").showModal(); $("time-decision-reason").focus();
  }

  async function saveTimeDecision(event) {
    event.preventDefault(); if (!timeWorkflow.decision || busyDialogs.has("time-decision-dialog")) return;
    $("time-decision-reason").value = $("time-decision-reason").value.trim(); if (!$("time-decision-form").reportValidity()) return;
    const { request, decision } = timeWorkflow.decision; dialogBusy("time-decision-dialog", true, "time-decision-save", t("Conferma decisione"));
    try { await post("/api/time-corrections/" + encodeURIComponent(request.id) + "/decision", { decision, reason: $("time-decision-reason").value }); $("time-decision-dialog").close(); state.auditReady = false; showNotice(t("Decisione registrata.")); await refreshAll(); }
    catch (error) { workflowError("time-decision-error", error); if (error.status === 409) await refreshTimeCorrections(); }
    finally { dialogBusy("time-decision-dialog", false, "time-decision-save", t("Conferma decisione")); }
  }

  async function openJobLifecycle(job) {
    if (!can("manage_jobs") || busyDialogs.has("job-lifecycle-dialog")) return;
    timeWorkflow.lifecycleJob = job; timeWorkflow.lifecycle = null; $("job-lifecycle-form").reset(); $("job-lifecycle-summary").replaceChildren(); $("job-lifecycle-action").replaceChildren();
    $("job-lifecycle-dialog").showModal(); await loadJobLifecycle();
  }

  async function loadJobLifecycle() {
    const job = timeWorkflow.lifecycleJob; if (!job || !can("manage_jobs")) return;
    const request = ++timeWorkflow.lifecycleRequest; timeWorkflow.lifecycle = null;
    $("job-lifecycle-loading").hidden = false; $("job-lifecycle-error").hidden = true; $("job-lifecycle-retry").hidden = true; $("job-lifecycle-save").disabled = true;
    try {
      const data = await api("/api/jobs/" + encodeURIComponent(job.id) + "/closure");
      if (request !== timeWorkflow.lifecycleRequest || !$("job-lifecycle-dialog").open || !can("manage_jobs")) return;
      timeWorkflow.lifecycle = data; const details = node("dl");
      for (const [label, value] of [[t("Commessa"), data.code + " · " + data.name], [t("Stato ore"), data.archived ? t("Archiviata") : data.time_closed ? t("Ore chiuse") : t("Registrazione ore aperta")], [t("Attività ancora aperte"), data.open_tasks], [t("Blocchi ancora aperti"), data.open_blockers], [t("Registrazioni conservate"), data.time_entries], [t("Ore conservate"), minutesLabel(data.total_minutes)], [t("Persone con ore registrate"), data.users_with_hours]]) details.append(node("dt", "", label), node("dd", "", String(value)));
      $("job-lifecycle-summary").replaceChildren(details);
      if (Number(data.open_tasks) || Number(data.open_blockers)) $("job-lifecycle-summary").append(node("p", "workflow-lifecycle-warning", t("Ci sono attività o blocchi ancora aperti. Controlla il lavoro residuo prima di confermare: la chiusura non li completa automaticamente.")));
      const choices = data.archived ? [["reopen", t("Riapri commessa")]] : data.time_closed ? [["archive", t("Archivia commessa")], ["reopen", t("Riapri registrazione ore")]] : [["close", t("Chiudi registrazione ore")], ["archive", t("Archivia commessa")]];
      $("job-lifecycle-action").replaceChildren(...choices.map(([value, label]) => { const option = node("option", "", label); option.value = value; return option; }));
      $("job-lifecycle-save").disabled = false; updateJobLifecycleHelp();
    } catch (error) { if (request === timeWorkflow.lifecycleRequest) { workflowError("job-lifecycle-error", error); $("job-lifecycle-retry").hidden = false; } }
    finally { if (request === timeWorkflow.lifecycleRequest) $("job-lifecycle-loading").hidden = true; }
  }

  function updateJobLifecycleHelp() {
    const action = $("job-lifecycle-action").value;
    $("job-lifecycle-action-help").textContent = action === "reopen" ? t("La commessa torna disponibile secondo le abilitazioni esistenti. Nessuna nuova abilitazione viene creata.") : action === "archive" ? t("La commessa esce dall’elenco ordinario e non accetta nuove ore. Usa Mostra archiviate per ritrovarla.") : t("La commessa resta consultabile nell’elenco, ma non accetta nuove ore.");
  }

  async function saveJobLifecycle(event) {
    event.preventDefault(); if (!timeWorkflow.lifecycle || !timeWorkflow.lifecycleJob || !can("manage_jobs") || busyDialogs.has("job-lifecycle-dialog")) return;
    $("job-lifecycle-reason").value = $("job-lifecycle-reason").value.trim(); if (!$("job-lifecycle-form").reportValidity()) return;
    dialogBusy("job-lifecycle-dialog", true, "job-lifecycle-save", t("Conferma operazione"));
    try {
      await post("/api/jobs/" + encodeURIComponent(timeWorkflow.lifecycleJob.id) + "/lifecycle", { revision: timeWorkflow.lifecycle.lifecycle_revision, action: $("job-lifecycle-action").value, reason: $("job-lifecycle-reason").value });
      $("job-lifecycle-dialog").close(); state.auditReady = false; showNotice(t("Stato della commessa aggiornato. Dati e storico sono conservati.")); await refreshAll();
    } catch (error) { workflowError("job-lifecycle-error", error); if (error.status === 409) { timeWorkflow.lifecycle = null; $("job-lifecycle-retry").hidden = false; } }
    finally { dialogBusy("job-lifecycle-dialog", false, "job-lifecycle-save", t("Conferma operazione")); $("job-lifecycle-save").disabled = !timeWorkflow.lifecycle; }
  }

  function resetWorkspaceFlows() {
    workspaceFlows.epoch++; workspaceFlows.myRequest++; workspaceFlows.overviewRequest++; workspaceFlows.accessRequest++; workspaceFlows.scheduleRequest++; workspaceFlows.notificationRequest++; workspaceFlows.wizard.request++;
    workspaceFlows.myWork = null; workspaceFlows.myLoading = false; workspaceFlows.scheduleLoading = false; workspaceFlows.notificationLoading = false;
    workspaceFlows.workNotifications = []; workspaceFlows.workUnread = 0; workspaceFlows.users = []; workspaceFlows.schedules = []; workspaceFlows.exceptions = []; workspaceFlows.holidays = [];
    workspaceFlows.overviewJob = null; workspaceFlows.accessJob = null; workspaceFlows.reading.clear();
    ["my-work-summary", "my-work-tasks", "my-work-blockers", "job-overview-content", "job-access-content", "work-schedules-list", "work-exceptions-list", "work-holidays-list", "work-notifications-list"].forEach((id) => $(id).replaceChildren());
  }

  function workflowMetric(label, value) { const card = node("div", "workflow-metric"); card.append(node("span", "", label), node("strong", "", value)); return card; }

  async function openWorkflowWork(jobId, id = null, kind = "task") {
    if (state.workBusy) return;
    resetWorkBoard(true); selectTab("work-board"); state.workJobId = String(jobId); renderWorkJobs(); await refreshWorkBoard();
    if (id) focusWorkResult(kind, id, true);
  }

  async function refreshMyWork() {
    if (!state.session?.authenticated || workspaceFlows.myLoading) return;
    const request = ++workspaceFlows.myRequest; workspaceFlows.myLoading = true;
    $("my-work-loading").hidden = false; $("my-work-error").hidden = true; $("my-work-refresh").disabled = true;
    refreshDashboardInventory(request); refreshWorkNotifications(); renderDashboardNotices();
    try {
      const data = await api("/api/my-work"); if (request !== workspaceFlows.myRequest) return;
      workspaceFlows.myWork = data;
      $("my-work-summary").replaceChildren(workflowMetric(t("Le tue ore di oggi"), minutesLabel(data.today_minutes)), workflowMetric(t("Attività da seguire"), String(data.tasks_count)), workflowMetric(t("Blocchi aperti che segui"), String(data.blockers_count)));
      for (const [kind, items] of [["task", data.tasks || []], ["blocker", data.blockers || []]]) {
        const list = $(kind === "task" ? "my-work-tasks" : "my-work-blockers"); list.replaceChildren();
        if (!items.length) list.append(node("li", "workflow-note", kind === "task" ? t("Nessuna attività aperta assegnata a te.") : t("Nessun blocco aperto da seguire.")));
        for (const item of items) {
          const card = node("li", "workflow-work-item"); card.append(node("h4", "", item.title), node("p", "", item.job_code + " · " + item.job_name), node("p", "", (kind === "task" ? workTaskStates[item.status] : workBlockerStates[item.status]) + " · " + workDateText(item.due_date)));
          if (item.next_action) card.append(node("p", "", t("Prossima azione: {0}", [item.next_action])));
          const actions = node("div", "row-actions"); actions.append(action(t("Apri nella commessa"), t("Apri ") + item.title, () => openWorkflowWork(item.job_id, item.id, kind)));
          const job = state.jobs.find((job) => String(job.id) === String(item.job_id));
          if (kind === "task" && job && canLogTime(job)) actions.append(action(t("Registra ore"), t("Registra ore per ") + item.title, () => openEntry(null, item.job_id, item.id)));
          card.append(actions); list.append(card);
        }
      }
      $("my-work-tasks-limit").hidden = !data.tasks_more; $("my-work-blockers-limit").hidden = !data.blockers_more;
      $("my-work-log").disabled = !state.jobs.some(canLogTime);
      $("my-work-tickets").hidden = !state.session?.tickets?.configured; $("my-work-tickets-help").hidden = !state.session?.tickets?.configured;
      if (state.session?.tickets?.configured) { const url = new URL(ticketAppURL()); url.searchParams.set("queue", "mine"); $("my-work-tickets").href = url.href; }
    } catch (error) { if (request === workspaceFlows.myRequest) dialogError("my-work-error", error); }
    finally { if (request === workspaceFlows.myRequest) { workspaceFlows.myLoading = false; $("my-work-loading").hidden = true; $("my-work-refresh").disabled = false; } }
  }

  async function openJobOverview(job) {
    workspaceFlows.overviewJob = job; $("job-overview-content").replaceChildren(); $("job-overview-dialog").showModal(); await loadJobOverview();
  }

  async function refreshDashboardInventory(request) {
    $("my-work-inventory-summary").textContent = t("Caricamento delle dotazioni…");
    try {
      const data = await api("/api/inventory/dashboard?mine=1");
      if (request !== workspaceFlows.myRequest) return;
      const counts = data.counts || {};
      $("my-work-inventory-summary").textContent = t("{0} beni assegnati a te · {1} restituzioni in ritardo · {2} verifiche da eseguire.", [counts.total || 0, counts.overdue_returns || 0, (Number(counts.never_verified) || 0) + (Number(counts.verification_due) || 0)]);
    } catch (error) { if (request === workspaceFlows.myRequest) $("my-work-inventory-summary").textContent = t("Inventario non disponibile. Puoi aprire I miei asset per riprovare."); }
  }

  function renderDashboardNotices() {
    const assets = $("my-work-inventory-notices"), work = $("my-work-operational-notices"); assets.replaceChildren(); work.replaceChildren();
    for (const notification of state.notifications.filter((item) => item.event_kind?.startsWith("inventory_") && !item.read_at).slice(0, 5)) {
      const card = node("li", "workflow-work-item"); card.append(node("h4", "", t(notification.title)), node("p", "", notificationMessage(notification)));
      const actions = node("div", "row-actions");
      if (notification.asset_id) actions.append(action(t("Apri asset"), t("Apri l’asset relativo alla notifica"), () => { selectTab("inventory"); openInventoryDetail(notification.asset_id); }));
      if (notification.asset_id && state.session?.tickets?.configured) actions.append(ticketAppLink(t("Nuovo ticket ↗"), "asset", notification.asset_id, true));
      card.append(actions); assets.append(card);
    }
    if (!assets.children.length) assets.append(node("li", "workflow-note", t("Nessun avviso inventario da leggere.")));
    for (const notification of workspaceFlows.workNotifications.filter((item) => !item.read_at).slice(0, 5)) {
      const card = node("li", "workflow-work-item"); card.append(node("h4", "", t(notification.title)), node("p", "", workNotificationMessage(notification)), action(t("Apri nella commessa"), t("Apri ") + t(notification.title), () => openWorkflowWork(notification.job_id, notification.task_id || notification.blocker_id, notification.blocker_id ? "blocker" : "task"))); work.append(card);
    }
    if (!work.children.length) work.append(node("li", "workflow-note", t("Nessun avviso operativo da leggere.")));
  }

  async function loadJobOverview() {
    const job = workspaceFlows.overviewJob; if (!job) return;
    const request = ++workspaceFlows.overviewRequest; $("job-overview-loading").hidden = false; $("job-overview-error").hidden = true;
    try {
      const data = await api("/api/jobs/" + encodeURIComponent(job.id) + "/overview"); if (request !== workspaceFlows.overviewRequest || !$("job-overview-dialog").open) return;
      const info = data.job || job, container = $("job-overview-content"); container.replaceChildren();
      $("job-overview-title").textContent = info.code + " · " + info.name;
      container.append(node("p", "workflow-note", ["team", "all"].includes(data.scope) ? t("Ore di tutta la squadra; collaboratori visibili al Gestore.") : t("Le tue ore personali; attività e blocchi della commessa abilitata.")));
      const metrics = node("div", "workflow-metrics"); metrics.append(workflowMetric(t("Ore registrate"), minutesLabel(data.total_minutes)), workflowMetric(t("Attività aperte"), String(data.tasks?.open || 0)), workflowMetric(t("Blocchi aperti"), String(data.blockers?.open || 0))); container.append(metrics);
      if (info.description) container.append(node("p", "workflow-note", info.description));
      const links = node("div", "row-actions"); links.append(action(t("Attività e blocchi"), t("Apri attività e blocchi"), () => { $("job-overview-dialog").close(); openWorkflowWork(job.id); }));
      if (canLogTime(info)) links.append(action(t("Registra ore"), t("Registra ore nella commessa"), () => { $("job-overview-dialog").close(); openEntry(null, job.id); }));
      if (state.session?.tickets?.configured) links.append(ticketAppLink(t("Ticket collegati ↗"), "job", job.id), ticketAppLink(t("Nuovo ticket ↗"), "job", job.id, true));
      if (info.contract_id && can("view_contracts")) links.append(action(t("Contratto e documenti"), t("Apri il contratto collegato"), async () => { $("job-overview-dialog").close(); selectTab("contracts"); if (!state.contractsReady) await refreshRegistry("contracts"); const contract = state.contracts.find((item) => String(item.id) === String(info.contract_id)); if (contract) openDocuments(contract); }));
      container.append(links);
      const details = node("section", "workflow-overview-section"); details.append(node("h3", "", t("Ore per attività")));
      const table = node("table"), head = node("tr"); [t("Attività"), t("Stato"), t("Ore"), t("Registrazioni")].forEach((label) => head.append(node("th", "", label)));
      const thead = node("thead"); thead.append(head); const tbody = node("tbody");
      for (const task of data.task_totals || []) { const row = node("tr"), title = node("td"); title.append(action(task.title || t("Ore senza attività specifica"), t("Apri attività ") + (task.title || ""), () => { $("job-overview-dialog").close(); openWorkflowWork(job.id, task.task_id); })); row.append(title, node("td", "", workTaskStates[task.status] || "—"), node("td", "", minutesLabel(task.total_minutes)), node("td", "", String(task.time_entries))); tbody.append(row); }
      table.append(thead, tbody); const wrap = node("div", "table-wrap"); wrap.append(table); details.append(wrap);
      if (!(data.task_totals || []).length) details.append(node("p", "workflow-note", t("Nessuna registrazione associata alle attività."))); container.append(details);
      if (Array.isArray(data.collaborators)) { const people = node("section", "workflow-overview-section"); people.append(node("h3", "", t("Collaboratori e accesso effettivo"))); const list = node("ul", "workflow-work-list");
        for (const person of data.collaborators) { const item = node("li", "workflow-work-item"); item.append(node("h4", "", person.name), node("p", "", person.email), node("p", "", person.can_log_time ? t("Può consultare e registrare ore") : person.can_view ? t("Può consultare") : t("Nessun accesso effettivo oggi"))); list.append(item); } people.append(list); container.append(people); }
    } catch (error) { if (request === workspaceFlows.overviewRequest) workflowError("job-overview-error", error); }
    finally { if (request === workspaceFlows.overviewRequest) $("job-overview-loading").hidden = true; }
  }

  async function openJobAccess(job) {
    if (!can("manage_assignments")) return; workspaceFlows.accessJob = job; $("job-access-context").textContent = job.code + " · " + job.name;
    $("job-access-content").replaceChildren(); $("job-access-error").hidden = true; $("job-access-dialog").showModal();
    const request = ++workspaceFlows.accessRequest;
    try { const data = await api("/api/users"); if (request !== workspaceFlows.accessRequest || !$("job-access-dialog").open) return;
      workspaceFlows.users = data.users || []; fillWorkflowUsers($("job-access-user"), workspaceFlows.users, state.session.id || state.session.user_id, false); await loadJobAccess();
    } catch (error) { if (request === workspaceFlows.accessRequest) workflowError("job-access-error", error); }
  }

  function fillWorkflowUsers(select, users, selected = "", empty = true) {
    select.replaceChildren(); if (empty) { const option = node("option", "", t("Seleziona una persona")); option.value = ""; select.append(option); }
    for (const user of users) { const option = node("option", "", (user.name || user.email) + (user.active === false || user.active === 0 ? t(" · Disattivato") : "")); option.value = String(user.id); select.append(option); }
    if (selected && users.some((user) => String(user.id) === String(selected))) select.value = String(selected);
  }

  async function loadJobAccess() {
    const job = workspaceFlows.accessJob, user = $("job-access-user").value; if (!job || !user || !can("manage_assignments")) return;
    const request = ++workspaceFlows.accessRequest; $("job-access-loading").hidden = false; $("job-access-error").hidden = true; $("job-access-content").replaceChildren();
    try { const data = await api("/api/jobs/" + encodeURIComponent(job.id) + "/access?user_id=" + encodeURIComponent(user));
      if (request !== workspaceFlows.accessRequest || !$("job-access-dialog").open) return;
      const content = $("job-access-content"), rights = node("div", "workflow-rights"); rights.append(node("span", "", data.effective?.can_view ? t("Consultazione consentita") : t("Consultazione non consentita")), node("span", "", data.effective?.can_log_time ? t("Registrazione ore consentita") : t("Registrazione ore non consentita"))); content.append(rights);
      if (data.archived || data.time_closed) content.append(node("p", "workflow-lifecycle-warning", t("La commessa è chiusa o archiviata: non accetta nuove ore, anche se un’abilitazione consente normalmente di registrarle.")));
      if (!data.groups_available) content.append(node("p", "workflow-lifecycle-warning", t("I gruppi aziendali non sono disponibili. Le relative abilitazioni non vengono applicate.")));
      const list = node("ul", "workflow-work-list"), labels = { direct: t("Abilitazione diretta"), group: t("Abilitazione tramite gruppo"), common: t("Commessa comune"), manager: t("Ruolo Gestore") };
      for (const source of data.sources || []) { const item = node("li", "workflow-work-item" + (source.active ? "" : " workflow-source-inactive")); item.append(node("h4", "", labels[source.type] || source.type));
        if (source.group_name) item.append(node("p", "", source.group_name + (source.inherited ? t(" · ereditata dal gruppo superiore") : "")));
        item.append(node("p", "", (source.role_name || "") + " · " + (source.can_log_time ? t("Consultazione e ore") : t("Consultazione"))), node("p", "", source.active ? t("Attiva oggi") : t("Non attiva oggi")));
        if (source.starts_on || source.ends_on) item.append(node("p", "", t("Validità: {0} — {1}", [source.starts_on ? dateLabel(source.starts_on) : t("Senza inizio"), source.ends_on ? dateLabel(source.ends_on) : t("Senza fine")])));
        list.append(item); }
      if (!list.children.length) list.append(node("li", "workflow-note", t("Nessuna fonte di abilitazione trovata per questo utente."))); content.append(list);
    } catch (error) { if (request === workspaceFlows.accessRequest) workflowError("job-access-error", error); }
    finally { if (request === workspaceFlows.accessRequest) $("job-access-loading").hidden = true; }
  }

  const workflowWeekDays = [["mon", t("Lunedì")], ["tue", t("Martedì")], ["wed", t("Mercoledì")], ["thu", t("Giovedì")], ["fri", t("Venerdì")], ["sat", t("Sabato")], ["sun", t("Domenica")]];

  async function refreshWorkSchedules() {
    if (!state.session?.authenticated || workspaceFlows.scheduleLoading) return;
    const request = ++workspaceFlows.scheduleRequest, manager = can("manage_jobs"); workspaceFlows.scheduleLoading = true;
    $("work-schedules-loading").hidden = false; $("work-schedules-error").hidden = true; $("work-schedules-refresh").disabled = true; $("work-schedules-actions").hidden = !manager;
    $("work-schedules-user").disabled = true;
    try {
      if (!workspaceFlows.users.length) { const users = manager ? await api("/api/users") : { users: [{ id: state.session.id || state.session.user_id || "", name: state.session.name, email: state.session.email, active: true }] }; if (request !== workspaceFlows.scheduleRequest) return;
        workspaceFlows.users = users.users || []; }
      const selected = $("work-schedules-user").value;
      const ownId = state.session.id || state.session.user_id || workspaceFlows.users.find((user) => user.email === state.session.email)?.id;
      fillWorkflowUsers($("work-schedules-user"), workspaceFlows.users, selected || ownId, false);
      const query = $("work-schedules-user").value ? "?user_id=" + encodeURIComponent($("work-schedules-user").value) : "";
      const results = await Promise.all([api("/api/work-schedules" + query), api("/api/work-exceptions" + query), api("/api/business-holidays")]);
      if (request !== workspaceFlows.scheduleRequest) return;
      workspaceFlows.schedules = results[0].schedules || []; workspaceFlows.exceptions = results[1].exceptions || []; workspaceFlows.holidays = results[2].holidays || []; renderWorkSchedules();
    } catch (error) { if (request === workspaceFlows.scheduleRequest) dialogError("work-schedules-error", error); }
    finally { if (request === workspaceFlows.scheduleRequest) { workspaceFlows.scheduleLoading = false; $("work-schedules-loading").hidden = true; $("work-schedules-refresh").disabled = false; $("work-schedules-user").disabled = !manager; } }
  }

  function renderWorkSchedules() {
    for (const [kind, items, target] of [["schedule", workspaceFlows.schedules, "work-schedules-list"], ["exception", workspaceFlows.exceptions, "work-exceptions-list"], ["holiday", workspaceFlows.holidays, "work-holidays-list"]]) {
      const list = $(target); list.replaceChildren(); if (!items.length) list.append(node("li", "workflow-note", t("Nessuna voce registrata.")));
      for (const item of items) {
        const card = node("li", "workflow-work-item"); card.append(node("h4", "", kind === "holiday" ? item.name : item.user_name), node("p", "", kind === "schedule" ? t("In vigore dal {0}", [dateLabel(item.starts_on)]) : dateLabel(item.date)));
        if (kind === "schedule") card.append(node("p", "", workflowWeekDays.map(([key, label]) => label + ": " + minutesLabel(item[key])).join(" · ")));
        if (kind === "exception") card.append(node("p", "", t("Lavoro atteso: {0}", [minutesLabel(item.expected_minutes)])), node("p", "", item.reason));
        if (can("manage_jobs")) { const actions = node("div", "row-actions"); actions.append(action(t("Modifica"), t("Modifica ") + (item.name || item.user_name), () => openWorkSchedule(kind, item)));
          if (kind !== "schedule") actions.append(action(t("Rimuovi"), t("Rimuovi ") + (item.name || item.user_name), () => openWorkScheduleDelete(kind, item), true)); card.append(actions); }
        list.append(card);
      }
    }
  }

  function openWorkSchedule(kind, item = null) {
    if (!can("manage_jobs") || busyDialogs.has("work-schedule-dialog")) return;
    workspaceFlows.scheduleKind = kind; $("work-schedule-form").reset(); $("work-schedule-error").hidden = true;
    const schedule = kind === "schedule", holiday = kind === "holiday";
    $("work-schedule-title").textContent = schedule ? t("Imposta orario settimanale") : holiday ? t("Festività aziendale") : t("Assenza o eccezione personale");
    $("work-schedule-person-field").hidden = holiday; $("work-schedule-person").disabled = holiday; $("work-schedule-person").required = !holiday;
    fillWorkflowUsers($("work-schedule-person"), workspaceFlows.users.filter((user) => user.active !== false && user.active !== 0 || String(user.id) === String(item?.user_id)), item?.user_id || $("work-schedules-user").value);
    $("work-schedule-date-label").textContent = schedule ? t("In vigore dal") : t("Data");
    $("work-schedule-date").value = item?.starts_on || item?.date || localDate(new Date());
    $("work-schedule-week-fields").hidden = !schedule; $("work-schedule-week-fields").replaceChildren();
    if (schedule) for (const [key, label] of workflowWeekDays) { const input = node("input"); input.id = "work-schedule-" + key; input.type = "number"; input.min = "0"; input.max = "1440"; input.step = "1"; input.required = true; input.value = item ? item[key] : ["sat", "sun"].includes(key) ? 0 : 480; $("work-schedule-week-fields").append(batchField(t("{0} · minuti", [label]), input, "")); }
    $("work-schedule-minutes-field").hidden = kind !== "exception"; $("work-schedule-minutes").required = kind === "exception"; $("work-schedule-minutes").disabled = kind !== "exception"; $("work-schedule-minutes").value = item?.expected_minutes || 0;
    $("work-schedule-reason-field").hidden = schedule; $("work-schedule-reason").required = !schedule; $("work-schedule-reason").disabled = schedule; $("work-schedule-reason").maxLength = holiday ? 160 : 1000; $("work-schedule-reason").value = item?.reason || item?.name || "";
    $("work-schedule-help").textContent = schedule ? t("Una nuova data conserva gli orari precedenti. Salvando la stessa persona e data aggiorni quella versione dell’orario.") : holiday ? t("La festività imposta a zero il lavoro atteso, salvo un’eccezione personale per la stessa data.") : t("L’eccezione prevale sull’orario e sulle festività per questa persona e giornata.");
    $("work-schedule-dialog").showModal(); $("work-schedule-date").focus();
  }

  async function saveWorkSchedule(event) {
    event.preventDefault(); if (!can("manage_jobs") || busyDialogs.has("work-schedule-dialog")) return;
    $("work-schedule-reason").value = $("work-schedule-reason").value.trim(); if (!$("work-schedule-form").reportValidity()) return;
    const kind = workspaceFlows.scheduleKind, fields = {};
    if (!validDate($("work-schedule-date").value)) { workflowError("work-schedule-error", new Error(t("Inserisci una data valida."))); return; }
    let endpoint;
    if (kind === "schedule") { endpoint = "/api/work-schedules"; fields.user_id = $("work-schedule-person").value; fields.starts_on = $("work-schedule-date").value; for (const [key] of workflowWeekDays) fields[key] = $("work-schedule-" + key).value; }
    else if (kind === "exception") { endpoint = "/api/work-exceptions"; Object.assign(fields, { user_id: $("work-schedule-person").value, date: $("work-schedule-date").value, expected_minutes: $("work-schedule-minutes").value, reason: $("work-schedule-reason").value }); }
    else { endpoint = "/api/business-holidays"; Object.assign(fields, { date: $("work-schedule-date").value, name: $("work-schedule-reason").value }); }
    dialogBusy("work-schedule-dialog", true, "work-schedule-save", t("Salva"));
    try { await post(endpoint, fields); $("work-schedule-dialog").close(); state.auditReady = false; showNotice(t("Calendario di lavoro aggiornato.")); await refreshWorkSchedules(); }
    catch (error) { workflowError("work-schedule-error", error); }
    finally { dialogBusy("work-schedule-dialog", false, "work-schedule-save", t("Salva")); $("work-schedule-person").disabled = kind === "holiday"; $("work-schedule-minutes").disabled = kind !== "exception"; $("work-schedule-reason").disabled = kind === "schedule"; }
  }

  function openWorkScheduleDelete(kind, item) {
    workspaceFlows.scheduleDelete = { kind, item }; $("work-schedule-delete-error").hidden = true;
    $("work-schedule-delete-description").textContent = t("{0} · {1}. Rimuovendo questa voce, il lavoro atteso viene nuovamente calcolato dalle altre regole del calendario.", [item.name || item.user_name, dateLabel(item.date)]);
    $("work-schedule-delete-dialog").showModal();
  }

  async function deleteWorkSchedule(event) {
    event.preventDefault(); if (!can("manage_jobs") || !workspaceFlows.scheduleDelete || busyDialogs.has("work-schedule-delete-dialog")) return;
    const { kind, item } = workspaceFlows.scheduleDelete; dialogBusy("work-schedule-delete-dialog", true, "work-schedule-delete-save", t("Conferma rimozione"));
    try { await post((kind === "holiday" ? "/api/business-holidays/" + encodeURIComponent(item.date) : "/api/work-exceptions/" + encodeURIComponent(item.id)) + "/delete", {}); $("work-schedule-delete-dialog").close(); state.auditReady = false; await refreshWorkSchedules(); }
    catch (error) { workflowError("work-schedule-delete-error", error); }
    finally { dialogBusy("work-schedule-delete-dialog", false, "work-schedule-delete-save", t("Conferma rimozione")); }
  }

  function workNotificationMessage(notification) {
    if (!notification.entity_title) return notification.message || "";
    if (notification.type === "assigned") return t("{0} · apri la commessa per consultare attività e prossimo passo.", [notification.entity_title]);
    if (notification.work_date && validDate(notification.work_date)) return t("{0} · scadenza {1}. {2}", [notification.entity_title, dateLabel(notification.work_date), t(notification.work_date < localDate(new Date()) ? "La scadenza è trascorsa." : "Controlla il prossimo passo.")]);
    return notification.message || "";
  }

  async function refreshWorkNotifications() {
    if (!state.session?.authenticated || workspaceFlows.notificationLoading) return;
    const request = ++workspaceFlows.notificationRequest; workspaceFlows.notificationLoading = true; $("work-notifications-refresh").disabled = true; $("work-notifications-error").hidden = true;
    try { const data = await api("/api/work-notifications"); if (request !== workspaceFlows.notificationRequest) return;
      workspaceFlows.workNotifications = data.notifications || []; workspaceFlows.workUnread = Number(data.unread_count) || 0; renderWorkNotifications();
    } catch (error) { if (request === workspaceFlows.notificationRequest) dialogError("work-notifications-error", error); }
    finally { if (request === workspaceFlows.notificationRequest) { workspaceFlows.notificationLoading = false; $("work-notifications-refresh").disabled = false; } }
  }

  function renderWorkNotifications() {
    $("work-notifications-list").replaceChildren(); $("work-notifications-summary").textContent = workspaceFlows.workUnread ? t("{0} avvisi operativi da leggere.", [workspaceFlows.workUnread]) : t("Nessun avviso operativo da leggere.");
    for (const notification of workspaceFlows.workNotifications) {
      const item = node("li", "workflow-work-item"); item.append(node("h4", "", t(notification.title)), node("p", "", workNotificationMessage(notification)), node("p", "", alertDateTime(notification.created_at, "")));
      const actions = node("div", "row-actions");
      actions.append(action(t("Apri nella commessa"), t("Apri ") + t(notification.title), () => openWorkflowWork(notification.job_id, notification.task_id || notification.blocker_id, notification.blocker_id ? "blocker" : "task")));
      if (!notification.read_at) { const button = action(t("Segna come letta"), t("Segna come letta: ") + t(notification.title), async () => {
        if (workspaceFlows.reading.has(notification.id)) return; workspaceFlows.reading.add(notification.id); button.disabled = true;
        try { await post("/api/work-notifications/" + encodeURIComponent(notification.id) + "/read", {}); await refreshWorkNotifications(); }
        catch (error) { dialogError("work-notifications-error", error); }
        finally { workspaceFlows.reading.delete(notification.id); button.disabled = false; }
      }); button.disabled = workspaceFlows.reading.has(notification.id); actions.append(button); }
      else item.append(node("p", "", t("Letta il ") + alertDateTime(notification.read_at, "")));
      item.append(actions); $("work-notifications-list").append(item);
    }
    updateCombinedNotificationBadge();
    renderDashboardNotices();
  }

  function updateCombinedNotificationBadge() {
    const unread = state.notificationsUnread + workspaceFlows.workUnread;
    $("nav-notifications-count").textContent = unread; $("nav-notifications-count").hidden = !unread;
    $("nav-notifications-count").setAttribute("aria-label", t("{0} notifiche non lette", [unread]));
  }

  async function openJobWizard() {
    if (!can("manage_jobs") || busyDialogs.has("job-wizard-dialog")) return;
    const wizard = workspaceFlows.wizard; wizard.step = 0; wizard.team = []; wizard.tasks = []; wizard.key = null; wizard.payload = null; wizard.createdId = null; wizard.createdDefinition = null;
    $("job-wizard-form").reset(); $("job-wizard-error").hidden = true; $("job-wizard-dialog").showModal(); renderJobWizardStep(); await loadJobWizardCatalogues();
  }

  async function loadJobWizardCatalogues() {
    const wizard = workspaceFlows.wizard, request = ++wizard.request; wizard.ready = false;
    $("job-wizard-loading").hidden = false; $("job-wizard-retry").hidden = true; $("job-wizard-next").disabled = true;
    try {
      const results = await Promise.allSettled([api("/api/users"), api("/api/groups"), api("/api/job-roles"), can("view_contracts") ? api("/api/contracts") : Promise.resolve({ contracts: [] }), api("/api/job-templates")]);
      if (request !== wizard.request || !$("job-wizard-dialog").open || !can("manage_jobs")) return;
      for (const index of [0, 2, 3, 4]) if (results[index].status === "rejected") throw results[index].reason;
      wizard.users = (results[0].value.users || []).filter((user) => user.active !== false && user.active !== 0);
      wizard.groups = results[1].status === "fulfilled" ? (results[1].value.groups || []).filter((group) => group.active !== false && group.active !== 0) : [];
      wizard.roles = (results[2].value.roles || []).filter((role) => role.active !== false && role.active !== 0);
      wizard.contracts = (results[3].value.contracts || []).filter((contract) => !["closed", "cancelled"].includes(contract.status));
      wizard.templates = results[4].value.templates || []; wizard.ready = true;
      const contract = $("job-wizard-contract").value, template = $("job-wizard-template").value;
      $("job-wizard-contract").replaceChildren(new Option(t("Nessun contratto"), ""), ...wizard.contracts.map((item) => new Option(item.code + " · " + item.title, item.id))); $("job-wizard-contract").value = can("manage_contracts") ? contract : ""; $("job-wizard-contract").disabled = !can("manage_contracts");
      $("job-wizard-template").replaceChildren(new Option(t("Nessun modello"), ""), ...wizard.templates.map((item) => new Option(item.name, item.id))); $("job-wizard-template").value = template;
      $("job-wizard-group-add").disabled = !wizard.groups.length;
      if (results[1].status === "rejected") dialogError("job-wizard-error", new Error(t("I gruppi non sono disponibili. Puoi proseguire con abilitazioni dirette agli utenti.")));
      renderJobWizardTeam(); renderJobWizardTasks(); renderJobWizardStep();
    } catch (error) { if (request === wizard.request) { workflowError("job-wizard-error", error); $("job-wizard-retry").hidden = false; } }
    finally { if (request === wizard.request) { $("job-wizard-loading").hidden = true; $("job-wizard-next").disabled = !wizard.ready; } }
  }

  function wizardValidateStep(step) {
    for (const control of $("job-wizard-step-" + step).querySelectorAll("input,select,textarea")) if (!control.disabled && !control.reportValidity()) return false;
    return true;
  }

  function renderJobWizardStep() {
    const wizard = workspaceFlows.wizard;
    for (let index = 0; index < 4; index++) { $("job-wizard-step-" + index).hidden = index !== wizard.step; const item = $("job-wizard-progress").children[index]; if (index === wizard.step) item.setAttribute("aria-current", "step"); else item.removeAttribute("aria-current"); }
    $("job-wizard-back").hidden = wizard.step === 0 || !!wizard.createdId; $("job-wizard-next").hidden = wizard.step === 3; $("job-wizard-save").hidden = wizard.step !== 3;
    $("job-wizard-next").disabled = !wizard.ready; $("job-wizard-save").disabled = !wizard.ready;
    $("job-wizard-contract").disabled = !can("manage_contracts");
    $("job-wizard-save").textContent = wizard.createdId ? t("Salva modello e termina") : t("Crea commessa, squadra e attività");
    if (wizard.step === 3) renderJobWizardReview();
  }

  function addWizardTeam(kind) {
    const wizard = workspaceFlows.wizard; if (!wizard.ready || wizard.team.filter((item) => item.kind === kind).length >= 200 || busyDialogs.has("job-wizard-dialog")) return;
    wizard.team.push({ key: ++wizard.sequence, kind, target: "", role_id: "", starts_on: "", ends_on: "" }); renderJobWizardTeam();
    $("job-wizard-team").lastElementChild?.querySelector("select")?.focus();
  }

  function wizardCatalogSelect(items, selected, labelFor, emptyLabel) {
    const select = node("select"); select.append(new Option(emptyLabel, ""));
    for (const item of items) select.append(new Option(labelFor(item), item.id));
    if (selected && !items.some((item) => String(item.id) === String(selected))) { const missing = new Option(t("Voce non disponibile: scegline un’altra"), selected); missing.disabled = true; select.append(missing); }
    select.value = String(selected || ""); return select;
  }

  function renderJobWizardTeam() {
    const wizard = workspaceFlows.wizard; $("job-wizard-team").replaceChildren();
    for (const row of wizard.team) {
      const wrapper = node("div", "workflow-editor-row");
      const target = wizardCatalogSelect(row.kind === "user" ? wizard.users : wizard.groups, row.target, (item) => item.name || item.email, row.kind === "user" ? t("Seleziona una persona") : t("Seleziona un gruppo")); target.id = "wizard-team-target-" + row.key; target.required = true; target.addEventListener("change", () => { row.target = target.value; });
      const role = wizardCatalogSelect(wizard.roles, row.role_id, (item) => item.name, t("Seleziona un ruolo")); role.id = "wizard-team-role-" + row.key; role.required = true; role.addEventListener("change", () => { row.role_id = role.value; });
      wrapper.append(batchField(row.kind === "user" ? t("Utente") : t("Gruppo"), target, ""), batchField(t("Ruolo nella commessa"), role, ""));
      for (const [key, label] of [["starts_on", t("Valido dal")], ["ends_on", t("Valido fino al")]]) { const input = node("input"); input.type = "date"; input.id = "wizard-team-" + key + "-" + row.key; input.value = row[key]; input.addEventListener("input", () => { row[key] = input.value; }); wrapper.append(batchField(label, input, "")); }
      wrapper.append(action(t("Rimuovi abilitazione"), t("Rimuovi questa abilitazione dalla bozza"), () => { wizard.team = wizard.team.filter((item) => item !== row); renderJobWizardTeam(); })); $("job-wizard-team").append(wrapper);
    }
    if (!wizard.team.length) $("job-wizard-team").append(node("p", "workflow-note", t("Nessuna abilitazione proposta. I gestori conservano il proprio accesso amministrativo.")));
  }

  function addWizardTask() {
    const wizard = workspaceFlows.wizard; if (!wizard.ready || wizard.tasks.length >= 100 || busyDialogs.has("job-wizard-dialog")) return;
    wizard.tasks.push({ key: ++wizard.sequence, title: "", assigned_user_id: "", priority: "normal", due_date: "" }); renderJobWizardTasks(); $("job-wizard-tasks").lastElementChild?.querySelector("input")?.focus();
  }

  function renderJobWizardTasks() {
    const wizard = workspaceFlows.wizard; $("job-wizard-tasks").replaceChildren();
    for (const row of wizard.tasks) {
      const wrapper = node("div", "workflow-editor-row"), title = node("input"); title.id = "wizard-task-title-" + row.key; title.maxLength = 160; title.required = true; title.value = row.title; title.addEventListener("input", () => { row.title = title.value; });
      const person = wizardCatalogSelect(wizard.users, row.assigned_user_id, (item) => item.name || item.email, t("Da assegnare")); person.id = "wizard-task-person-" + row.key; person.addEventListener("change", () => { row.assigned_user_id = person.value; });
      const priority = node("select"); priority.id = "wizard-task-priority-" + row.key; for (const [value, label] of Object.entries(workPriorities)) priority.append(new Option(label, value)); priority.value = row.priority || "normal"; priority.addEventListener("change", () => { row.priority = priority.value; });
      const due = node("input"); due.type = "date"; due.id = "wizard-task-due-" + row.key; due.value = row.due_date || ""; due.addEventListener("input", () => { row.due_date = due.value; });
      wrapper.append(batchField(t("Titolo attività"), title, "workflow-editor-full"), batchField(t("Responsabile"), person, ""), batchField(t("Priorità"), priority, ""), batchField(t("Scadenza"), due, ""), action(t("Rimuovi attività"), t("Rimuovi questa attività dalla bozza"), () => { wizard.tasks = wizard.tasks.filter((item) => item !== row); renderJobWizardTasks(); })); $("job-wizard-tasks").append(wrapper);
    }
    if (!wizard.tasks.length) $("job-wizard-tasks").append(node("p", "workflow-note", t("Nessuna attività iniziale. Potrai aggiungerne dopo la creazione.")));
  }

  function jobWizardDefinition() {
    const wizard = workspaceFlows.wizard, definition = { name: $("job-wizard-name").value.trim(), client: $("job-wizard-client").value.trim(), description: $("job-wizard-description-field").value.trim(), status: $("job-wizard-status").value, contract_id: can("manage_contracts") ? $("job-wizard-contract").value : "", is_common: $("job-wizard-common").checked ? "1" : "0", assignments: [], groups: [], tasks: [] };
    for (const item of wizard.team) definition[item.kind === "user" ? "assignments" : "groups"].push({ [item.kind === "user" ? "user_id" : "group_id"]: Number(item.target), role_id: Number(item.role_id), starts_on: item.starts_on || "", ends_on: item.ends_on || "" });
    definition.tasks = wizard.tasks.map((item) => ({ title: item.title.trim(), assigned_user_id: item.assigned_user_id ? Number(item.assigned_user_id) : null, priority: item.priority || "normal", due_date: item.due_date || "" })); return definition;
  }

  function renderJobWizardReview() {
    const wizard = workspaceFlows.wizard, definition = wizard.createdDefinition || jobWizardDefinition(), content = $("job-wizard-review"), data = node("dl");
    const contract = wizard.contracts.find((item) => String(item.id) === String(definition.contract_id));
    for (const [label, value] of [[t("Codice commessa"), $("job-wizard-code").value.trim()], [t("Nome commessa"), definition.name], [t("Cliente"), definition.client || "—"], [t("Stato"), states[definition.status] || definition.status], [t("Contratto"), contract ? contract.code + " · " + contract.title : t("Nessun contratto")], [t("Accesso comune"), definition.is_common === "1" ? t("Tutti gli utenti attivi: consultazione e ore") : t("Solo utenti abilitati e gestori")]]) data.append(node("dt", "", label), node("dd", "", value)); content.replaceChildren(data);
    const team = node("ul", "workflow-work-list");
    for (const [kind, rows] of [["user", definition.assignments], ["group", definition.groups]]) for (const row of rows) {
      const person = (kind === "user" ? wizard.users : wizard.groups).find((item) => Number(item.id) === Number(row[kind === "user" ? "user_id" : "group_id"])), role = wizard.roles.find((item) => Number(item.id) === Number(row.role_id));
      const item = node("li", "workflow-work-item"); item.append(node("h4", "", (kind === "user" ? t("Utente: ") : t("Gruppo: ")) + (person?.name || person?.email || t("Voce non disponibile"))), node("p", "", t("Ruolo: {0}", [role?.name || t("Non disponibile")])), node("p", "", t("Validità: {0} — {1}", [row.starts_on ? dateLabel(row.starts_on) : t("Senza inizio"), row.ends_on ? dateLabel(row.ends_on) : t("Senza fine")]))); team.append(item);
    }
    content.append(node("h4", "", t("Abilitazioni che saranno create")), team);
    if (!team.children.length) content.append(node("p", "workflow-note", t("Nessuna abilitazione aggiuntiva.")));
    content.append(node("h4", "", t("Attività che saranno create"))); const tasks = node("ul", "workflow-work-list");
    for (const task of definition.tasks) { const user = wizard.users.find((item) => Number(item.id) === Number(task.assigned_user_id)), item = node("li", "workflow-work-item"); item.append(node("h4", "", task.title), node("p", "", t("Responsabile: {0}", [user?.name || t("Da assegnare")])), node("p", "", (workPriorities[task.priority] || task.priority) + " · " + workDateText(task.due_date))); tasks.append(item); } content.append(tasks);
  }

  function applyJobWizardTemplate() {
    const wizard = workspaceFlows.wizard, selected = wizard.templates.find((item) => String(item.id) === $("job-wizard-template").value); if (!selected || !wizard.ready) return;
    try {
      const definition = JSON.parse(selected.definition_json);
      for (const [key, id] of [["name", "job-wizard-name"], ["client", "job-wizard-client"], ["description", "job-wizard-description-field"], ["status", "job-wizard-status"], ["contract_id", "job-wizard-contract"]]) $(id).value = definition[key] || (key === "status" ? "planned" : "");
      if (!can("manage_contracts")) $("job-wizard-contract").value = "";
      $("job-wizard-common").checked = ["1", 1, true].includes(definition.is_common);
      wizard.team = []; for (const [kind, rows] of [["user", definition.assignments || []], ["group", definition.groups || []]]) for (const row of rows) wizard.team.push({ key: ++wizard.sequence, kind, target: row[kind === "user" ? "user_id" : "group_id"], role_id: row.role_id, starts_on: row.starts_on || "", ends_on: row.ends_on || "" });
      wizard.tasks = (definition.tasks || []).map((row) => ({ ...row, key: ++wizard.sequence })); renderJobWizardTeam(); renderJobWizardTasks();
      showNotice(t("Modello caricato nella bozza. Controlla squadra e attività prima di confermare."));
    } catch (error) { workflowError("job-wizard-error", new Error(t("Il modello non contiene dati validi."))); }
  }

  async function saveJobWizard(event) {
    event.preventDefault(); const wizard = workspaceFlows.wizard; if (!can("manage_jobs") || !wizard.ready || busyDialogs.has("job-wizard-dialog")) return;
    if (wizard.step !== 3) { if (wizardValidateStep(wizard.step)) { wizard.step++; renderJobWizardStep(); } return; }
    if (!wizard.createdId) for (let index = 0; index < 3; index++) { wizard.step = index; renderJobWizardStep(); if (!wizardValidateStep(index)) return; } wizard.step = 3; renderJobWizardStep();
    const definition = wizard.createdDefinition || jobWizardDefinition(), fields = { ...definition, code: $("job-wizard-code").value.trim(), assignments: JSON.stringify(definition.assignments), groups: JSON.stringify(definition.groups), tasks: JSON.stringify(definition.tasks) };
    const payload = JSON.stringify(fields); if (payload !== wizard.payload) { wizard.key = crypto.randomUUID(); wizard.payload = payload; }
    fields.request_key = wizard.key; dialogBusy("job-wizard-dialog", true, "job-wizard-save", t("Crea commessa, squadra e attività"));
    try {
      if (!wizard.createdId) { const result = await post("/api/jobs/wizard", fields); wizard.createdId = result.id; wizard.createdDefinition = definition; }
      const template = $("job-wizard-save-template").value.trim();
      if (template) await post("/api/job-templates", { name: template, definition: JSON.stringify(wizard.createdDefinition) });
      $("job-wizard-dialog").close(); state.auditReady = false; selectTab("jobs"); showNotice(t("Commessa, squadra e attività iniziali create.")); await refreshAll();
      const job = state.jobs.find((item) => String(item.id) === String(wizard.createdId)); if (job) openJobOverview(job);
    } catch (error) { workflowError("job-wizard-error", new Error(wizard.createdId ? t("La commessa è stata creata. Il modello non è stato salvato: {0}. Correggi il nome e riprova; la commessa non verrà creata nuovamente.", [error.message]) : error.message)); }
    finally { dialogBusy("job-wizard-dialog", false, "job-wizard-save", t("Crea commessa, squadra e attività")); renderJobWizardStep(); $("job-wizard-group-add").disabled = !wizard.groups.length; }
  }

  async function openAccessProfiles(user) {
    if (!requirePermission("manage_access_groups") || busyDialogs.has("access-groups-dialog")) return;
    accessProfiles.user = user; accessProfiles.data = null;
    $("access-groups-person").textContent = user.name + " · " + user.email;
    $("access-groups-options").replaceChildren(); $("access-groups-effective").textContent = "";
    $("access-groups-dialog").showModal();
    await loadAccessProfiles();
  }

  async function loadAccessProfiles() {
    if (!can("manage_access_groups") || !accessProfiles.user) return;
    const request = ++accessProfiles.request, userId = accessProfiles.user.id;
    $("access-groups-fields").disabled = true; $("access-groups-save").disabled = true;
    $("access-groups-loading").hidden = false; $("access-groups-error").hidden = true; $("access-groups-retry").hidden = true;
    try {
      const [catalog, data] = await Promise.all([api("/api/access-groups"), api("/api/users/" + encodeURIComponent(userId) + "/access-groups")]);
      if (request !== accessProfiles.request || !can("manage_access_groups") || !$("access-groups-dialog").open) return;
      if (!Array.isArray(catalog.groups) || !Array.isArray(data.selected) || !Array.isArray(data.effective)) throw new Error(t("Il server ha restituito profili non validi. Riprova."));
      accessProfiles.catalog = catalog.groups; accessProfiles.data = data;
      $("access-groups-options").replaceChildren();
      for (const group of catalog.groups) {
        const label = node("label", "access-profile-option"), input = node("input"); input.type = "checkbox"; input.value = group.code;
        input.disabled = group.editable !== true || !["sales", "finance"].includes(group.code);
        input.checked = input.disabled ? data.effective.includes(group.code) : data.selected.includes(group.code);
        const info = node("span"); info.append(node("strong", "", accessProfileLabels[group.code] || t(group.name)));
        if (input.disabled) info.append(node("small", "", t("Automatico dall’identità dell’account")));
        label.append(input, info); $("access-groups-options").append(label);
      }
      $("access-groups-effective").textContent = data.active ? t("Profili attivi: {0}", [data.effective.map((code) => accessProfileLabels[code] || code).join(" · ")]) : t("Account disattivato: nessun profilo è attivo. Le assegnazioni saranno conservate per un’eventuale riattivazione.");
      $("access-groups-fields").disabled = false; $("access-groups-save").disabled = false;
      $("access-groups-options").querySelector("input:not(:disabled)")?.focus();
    } catch (error) { if (request === accessProfiles.request) { workflowError("access-groups-error", error); $("access-groups-retry").hidden = false; } }
    finally { if (request === accessProfiles.request) $("access-groups-loading").hidden = true; }
  }

  async function saveAccessProfiles(event) {
    event.preventDefault();
    if (!can("manage_access_groups") || !accessProfiles.user || !accessProfiles.data || busyDialogs.has("access-groups-dialog")) return;
    const user = accessProfiles.user, identity = state.session?.email;
    const groups = Array.from($("access-groups-options").querySelectorAll("input:checked:not(:disabled)"), (input) => input.value);
    $("access-groups-error").hidden = true; dialogBusy("access-groups-dialog", true, "access-groups-save", t("Salva profili"));
    try {
      await post("/api/users/" + encodeURIComponent(user.id) + "/access-groups", { groups: JSON.stringify(groups) });
      if (state.session?.email !== identity) return;
      $("access-groups-dialog").close(); state.auditReady = false;
      if (user.email === identity) await refreshSession(); else await refreshUsers();
      showNotice(t("Profili funzionali aggiornati. I permessi si applicano alle richieste successive."));
    } catch (error) { if (state.session?.email === identity) workflowError("access-groups-error", error); }
    finally { dialogBusy("access-groups-dialog", false, "access-groups-save", t("Salva profili")); for (const input of $("access-groups-options").querySelectorAll("input")) input.disabled = !["sales", "finance"].includes(input.value); }
  }

  function renderUsers() {
    if (!canReadUsers() || !state.usersReady) return;
    $("users-loading").hidden = true;
    $("users-table-wrap").hidden = state.users.length === 0;
    $("users-body").replaceChildren();
    for (const user of state.users) {
      const row = node("tr");
      const identityCell = node("td");
      const identity = node("div", "job-identity");
      identity.append(node("strong", "", user.name), node("span", "", user.email));
      if (user.email === state.session.email) identity.append(node("span", "user-self", t("Il tuo account")));
      identityCell.append(identity);
      const roleCell = node("td");
      for (const code of (Array.isArray(user.access_groups) ? user.access_groups : ["operator", ...(user.role === "manager" ? ["manager"] : [])])) roleCell.append(node("span", "role-badge", accessProfileLabels[code] || code));
      const accessCell = node("td");
      accessCell.append(node("span", "status-badge " + (user.active ? "status-active" : "status-cancelled"), user.active ? t("Attivo") : t("Disattivato")));
      const actionsCell = node("td");
      const actions = node("div", "row-actions");
      if (can("manage_users")) actions.append(action(t("Identità"), t("Modifica l’identità di {0}", [user.name]), () => openUser(user)));
      else if (state.usersAdminURL) { const link = node("a", "row-button", t("Identità in Keycloak ↗")); link.href = state.usersAdminURL; actions.append(link); }
      if (can("manage_access_groups")) actions.append(action(t("Profili"), t("Gestisci i profili funzionali di {0}", [user.name]), () => openAccessProfiles(user)));
      actionsCell.append(actions);
      row.append(identityCell, roleCell, accessCell, actionsCell);
      $("users-body").append(row);
    }
    $("users-count").textContent = state.users.length;
    const active = state.users.filter((user) => user.active).length;
    $("users-active-count").textContent = `${active} ${active === 1 ? t("utente attivo") : t("utenti attivi")}`;
  }

  function euroLabel(cents) {
    if (cents === null || cents === undefined || cents === "") return "—";
    return new Intl.NumberFormat(locale, { style: "currency", currency: "EUR" }).format(Number(cents) / 100);
  }

  function valueToCents(value) {
    const normalized = value.trim().replace(",", ".");
    if (!normalized) return "";
    if (!/^\d{1,10}(?:\.\d{1,2})?$/.test(normalized)) throw new Error(t("Inserisci un importo positivo, senza separatori delle migliaia e con al massimo due decimali."));
    const [whole, fraction = ""] = normalized.split(".");
    const cents = BigInt(whole) * 100n + BigInt(fraction.padEnd(2, "0"));
    if (cents > 999999999999n) throw new Error(t("Il valore massimo è 9.999.999.999,99 €."));
    return String(cents);
  }

  function activeSupplier(supplier) {
    return supplier.active === true || supplier.active === 1 || supplier.active === "1";
  }

  function renderSuppliers() {
    if (!can("view_suppliers") || !state.suppliersReady) return;
    const search = $("supplier-search").value.trim().toLocaleLowerCase(locale);
    const filter = $("supplier-filter").value;
    const suppliers = state.suppliers.filter((supplier) => (!filter || String(Number(activeSupplier(supplier))) === filter) && [supplier.code, supplier.name, supplier.vat_number].some((value) => (value || "").toLocaleLowerCase(locale).includes(search)));
    $("suppliers-body").replaceChildren();
    for (const supplier of suppliers) {
      const row = node("tr");
      const identity = node("td");
      identity.append(jobIdentity(supplier.code, supplier.name));
      const contact = node("td", "registry-contact");
      contact.append(node("span", "", supplier.email || "—"));
      if (supplier.phone) contact.append(node("span", "", supplier.phone));
      const status = node("td");
      status.append(node("span", "status-badge " + (activeSupplier(supplier) ? "status-active" : "status-cancelled"), activeSupplier(supplier) ? t("Attivo") : t("Disattivato")));
      const actions = node("td");
      if (can("manage_suppliers")) actions.append(action(t("Modifica"), t("Modifica il fornitore ") + supplier.name, () => openSupplier(supplier)));
      row.append(identity, node("td", supplier.vat_number ? "" : "no-value", supplier.vat_number || "—"), contact, status, actions);
      $("suppliers-body").append(row);
    }
    $("suppliers-count").textContent = state.suppliers.length;
    $("suppliers-table-wrap").hidden = suppliers.length === 0;
    $("suppliers-empty").hidden = suppliers.length !== 0;
    $("suppliers-empty-title").textContent = state.suppliers.length ? t("Nessun fornitore corrisponde alla ricerca.") : t("Il primo partner parte da qui.");
    $("suppliers-empty-text").textContent = state.suppliers.length ? t("Cambia la ricerca o il filtro di stato.") : t("Censisci un fornitore per associarlo ai tuoi contratti.");
    $("suppliers-visible-count").textContent = t("{0} di {1} fornitori", [suppliers.length, state.suppliers.length]);
  }

  function contractPeriod(contract) {
    if (contract.starts_on && contract.ends_on) return `${dateLabel(contract.starts_on)} – ${dateLabel(contract.ends_on)}`;
    if (contract.starts_on) return t("Dal {0}", [dateLabel(contract.starts_on)]);
    if (contract.ends_on) return t("Fino al {0}", [dateLabel(contract.ends_on)]);
    return t("Non specificata");
  }

  function renderContracts() {
    if (!can("view_contracts") || !state.contractsReady) return;
    const search = $("contract-search").value.trim().toLocaleLowerCase(locale);
    const filter = $("contract-filter").value;
    const contracts = state.contracts.filter((contract) => (!filter || contract.status === filter) && [contract.code, contract.title, contract.supplier_name].some((value) => (value || "").toLocaleLowerCase(locale).includes(search)));
    $("contracts-body").replaceChildren();
    for (const contract of contracts) {
      const row = node("tr");
      const identity = node("td");
      const info = jobIdentity(contract.code, contract.title);
      info.querySelector(".job-identity").append(node("span", "contract-supplier-label", contract.supplier_name || t("Fornitore non disponibile")));
      identity.append(info);
      const status = node("td");
      const statusClass = { draft: "planned", active: "active", expired: "paused", closed: "completed", cancelled: "cancelled" }[contract.status] || "planned";
      status.append(node("span", "status-badge status-" + statusClass, contractStates[contract.status] || contract.status));
      const actionsCell = node("td");
      const actions = node("div", "row-actions");
      if (can("manage_contracts")) actions.append(action(t("Modifica"), t("Modifica il contratto ") + contract.title, () => openContract(contract)), node("span", "row-divider"));
      actions.append(action(t("PDF · {0}", [Number(contract.documents_count) || 0]), t("Apri i documenti del contratto ") + contract.title, () => openDocuments(contract)));
      actionsCell.append(actions);
      row.append(identity, node("td", "contract-period", contractPeriod(contract)), node("td", "contract-value", euroLabel(contract.value_cents)), status, actionsCell);
      $("contracts-body").append(row);
    }
    $("contracts-count").textContent = state.contracts.length;
    $("contracts-table-wrap").hidden = contracts.length === 0;
    $("contracts-empty").hidden = contracts.length !== 0;
    $("contracts-empty-title").textContent = state.contracts.length ? t("Nessun contratto corrisponde alla ricerca.") : t("Ogni commessa ha un punto di partenza.");
    $("contracts-empty-text").textContent = state.contracts.length ? t("Cambia la ricerca o il filtro di stato.") : t("Crea un contratto e raccogli i documenti del tuo accordo.");
    $("contracts-visible-count").textContent = t("{0} di {1} contratti", [contracts.length, state.contracts.length]);
  }

  async function refreshRegistry(kind) {
    const permission = kind === "suppliers" ? "view_suppliers" : "view_contracts";
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
      if (!Array.isArray(data[kind])) throw new Error(t("Il server ha restituito un elenco non valido. Riprova."));
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
    const first = node("option", "", t("Nessun contratto"));
    first.value = "";
    const options = state.contracts.filter((contract) => !["closed", "cancelled"].includes(contract.status) || String(contract.id) === String(current)).map((contract) => {
      const option = node("option", "", `${contract.code} · ${contract.title} · ${contractStates[contract.status] || contract.status}`);
      option.value = contract.id;
      return option;
    });
    if (current && !options.some((option) => String(option.value) === String(current))) {
      const fallback = node("option", "", `${state.editingJob?.contract_code || t("Contratto")} · ${state.editingJob?.contract_title || t("Collegamento esistente")}`);
      fallback.value = current;
      options.push(fallback);
    }
    $("job-contract").replaceChildren(first, ...options);
    $("job-contract").value = current;
    $("job-contract").disabled = !state.contractsReady || busyDialogs.has("job-dialog");
    $("job-contract-help").textContent = state.contractsReady ? t("Collega la commessa a un contratto censito. I contratti chiusi o annullati conservano i collegamenti esistenti.") : t("Caricamento dell’elenco dei contratti necessario per salvare la commessa.");
    $("job-contract-retry").hidden = state.contractsReady;
    $("job-save").disabled = !state.contractsReady || busyDialogs.has("job-dialog");
  }

  async function loadJobContracts() {
    $("job-contract-retry").disabled = true;
    const loaded = await refreshRegistry("contracts");
    if (!$("job-dialog").open || !can("manage_jobs")) return;
    renderJobContracts();
    $("job-contract-retry").disabled = false;
    if (!loaded && !state.contractsReady) $("job-contract-help").textContent = t("L’elenco dei contratti non è disponibile. Ricaricalo prima di salvare.");
  }

  function openSupplier(supplier = null) {
    if (!requirePermission("manage_suppliers") || busyDialogs.has("supplier-dialog")) return;
    $("supplier-form").reset();
    $("supplier-error").hidden = true;
    $("supplier-id").value = supplier?.id || "";
    $("supplier-dialog-title").textContent = supplier ? t("Modifica fornitore") : t("Nuovo fornitore");
    $("supplier-save").textContent = supplier ? t("Salva modifiche") : t("Crea fornitore");
    if (supplier) {
      ["code", "name", "vat_number", "email", "phone", "address", "notes"].forEach((key) => { $("supplier-" + key).value = supplier[key] || ""; });
      $("supplier-active").value = activeSupplier(supplier) ? "1" : "0";
    }
    $("supplier-dialog").showModal();
    $("supplier-code").focus();
  }

  function renderContractSuppliers(initial = false) {
    const current = initial ? state.editingContract?.supplier_id || "" : $("contract-supplier").value;
    const first = node("option", "", t("Seleziona un fornitore attivo"));
    first.value = "";
    const options = state.suppliers.filter((supplier) => activeSupplier(supplier) || String(supplier.id) === String(state.editingContract?.supplier_id)).map((supplier) => {
      const option = node("option", "", `${supplier.code} · ${supplier.name}${activeSupplier(supplier) ? "" : t(" (disattivato)")}`);
      option.value = supplier.id;
      return option;
    });
    if (current && !options.some((option) => String(option.value) === String(current))) {
      const fallback = node("option", "", state.editingContract?.supplier_name || t("Fornitore del contratto"));
      fallback.value = current;
      options.push(fallback);
    }
    $("contract-supplier").replaceChildren(first, ...options);
    $("contract-supplier").value = current;
    $("contract-supplier").disabled = !state.suppliersReady || busyDialogs.has("contract-dialog");
    $("contract-save").disabled = !state.suppliersReady || busyDialogs.has("contract-dialog");
    $("contract-supplier-retry").hidden = state.suppliersReady;
    $("contract-supplier-help").textContent = !state.suppliersReady ? t("Caricamento dell’elenco dei fornitori necessario per salvare il contratto.") : options.length ? t("Puoi scegliere un fornitore attivo. Il fornitore già collegato resta disponibile in modifica.") : t("Crea prima un fornitore attivo nella sezione Fornitori.");
  }

  async function loadContractSuppliers() {
    $("contract-supplier-retry").disabled = true;
    const loaded = await refreshRegistry("suppliers");
    if (!$("contract-dialog").open || !can("manage_contracts")) return;
    renderContractSuppliers();
    $("contract-supplier-retry").disabled = false;
    if (!loaded && !state.suppliersReady) $("contract-supplier-help").textContent = t("L’elenco dei fornitori non è disponibile. Ricaricalo prima di salvare.");
  }

  function openContract(contract = null) {
    if (!requirePermission("manage_contracts") || busyDialogs.has("contract-dialog")) return;
    state.editingContract = contract;
    $("contract-form").reset();
    $("contract-value").setCustomValidity("");
    $("contract-ends_on").setCustomValidity("");
    $("contract-error").hidden = true;
    $("contract-id").value = contract?.id || "";
    $("contract-dialog-title").textContent = contract ? t("Modifica contratto") : t("Nuovo contratto");
    $("contract-save").textContent = contract ? t("Salva modifiche") : t("Crea contratto");
    if (contract) {
      ["code", "title", "status", "starts_on", "ends_on", "notes"].forEach((key) => { $("contract-" + key).value = contract[key] || ""; });
      if (contract.value_cents !== null && contract.value_cents !== undefined && contract.value_cents !== "") {
        const cents = BigInt(contract.value_cents);
        $("contract-value").value = `${cents / 100n}${new Intl.NumberFormat(locale).formatToParts(1.1).find((part) => part.type === "decimal").value}${String(cents % 100n).padStart(2, "0")}`;
      }
    }
    renderContractSuppliers(true);
    $("contract-dialog").showModal();
    $("contract-code").focus();
    if (!state.suppliersReady) loadContractSuppliers();
  }

  function fileSize(bytes) {
    return Number(bytes) < 1024 * 1024 ? t("{0} KiB", [(Number(bytes) / 1024).toLocaleString(locale, { maximumFractionDigits: 1 })]) : t("{0} MiB", [(Number(bytes) / (1024 * 1024)).toLocaleString(locale, { maximumFractionDigits: 2 })]);
  }

  function renderDocuments() {
    if (!state.documentsReady || !state.documentsContract || !can("view_contracts")) return;
    $("documents-list").replaceChildren();
    for (const document of state.documents) {
      const item = node("li", "document-item");
      const info = node("div", "document-identity");
      info.append(node("strong", "", document.file_name), node("span", "", `${fileSize(document.byte_size)} · ${auditTime(document.created_at)}`));
      const link = node("a", "row-button document-download", t("Scarica PDF"));
      link.href = `/api/contract-documents/${encodeURIComponent(document.id)}/download`;
      link.setAttribute("aria-label", t("Scarica il documento ") + document.file_name);
      item.append(info, link);
      $("documents-list").append(item);
    }
    $("documents-empty").hidden = state.documents.length !== 0;
  }

  async function loadDocuments() {
    if (!state.documentsContract || !can("view_contracts")) return;
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
      if (request !== state.documentsRequest || !can("view_contracts")) return;
      if (!Array.isArray(data.documents)) throw new Error(t("Il server ha restituito un elenco non valido. Riprova."));
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
    if (!requirePermission("view_contracts") || busyDialogs.has("documents-dialog")) return;
    state.documentsContract = contract;
    state.documents = [];
    state.documentsReady = false;
    $("document-form").reset();
    $("document-form").hidden = !can("manage_contracts");
    $("documents-readonly-close").hidden = can("manage_contracts");
    $("document-file").setCustomValidity("");
    $("documents-dialog-title").textContent = t("PDF · ") + contract.code;
    $("documents-dialog-description").textContent = contract.title + ". " + t(can("manage_contracts") ? "Scarica i documenti originali o aggiungi un PDF all’archivio." : "Consulta e scarica i documenti originali del contratto.");
    $("documents-dialog").showModal();
    loadDocuments();
  }

  function assignmentPeriod(assignment) {
    if (assignment.starts_on && assignment.ends_on) return `${dateLabel(assignment.starts_on)} – ${dateLabel(assignment.ends_on)}`;
    if (assignment.starts_on) return t("Dal {0}, senza scadenza", [dateLabel(assignment.starts_on)]);
    if (assignment.ends_on) return t("Fino al {0}", [dateLabel(assignment.ends_on)]);
    return t("Da subito, senza scadenza");
  }

  function assignmentStatus(assignment) {
    const today = localDate(new Date());
    if (!assignment.active) return [t("Utente disattivato"), "cancelled"];
    if (assignment.starts_on && assignment.starts_on > today) return [t("Programmata"), "planned"];
    if (assignment.ends_on && assignment.ends_on < today) return [t("Scaduta"), "completed"];
    return [t("Abilitato"), "active"];
  }

  function setAssignmentBusy(busy) {
    if (busy) busyDialogs.add("assignment-dialog"); else busyDialogs.delete("assignment-dialog");
    $("assignment-dialog").setAttribute("aria-busy", String(busy));
    $("assignment-dialog").querySelectorAll("input, select, button").forEach((element) => { element.disabled = busy || element.dataset.assignmentInactive === "1"; });
    $("assignment-user").disabled = busy || Boolean(state.editingAssignment) || !state.assignmentsReady;
    $("assignment-save").disabled = busy || !state.assignmentsReady;
    $("group-grant-group").disabled = busy || Boolean(state.editingGroupGrant) || !state.assignmentsReady;
    $("group-grant-save").disabled = busy || !state.assignmentsReady || state.assignmentGroups.length === 0;
    $("group-grant-save").textContent = busy ? t("Salvataggio…") : state.editingGroupGrant ? t("Salva abilitazione gruppo") : t("Abilita gruppo");
    $("assignment-save").textContent = busy ? t("Salvataggio…") : state.editingAssignment ? t("Salva abilitazione") : t("Abilita utente");
    $("assignment-revoke-save").textContent = busy ? t("Revoca in corso…") : t("Conferma revoca");
  }

  function resetAssignmentForm(assignment = null) {
    state.editingAssignment = assignment;
    $("assignment-form").reset();
    $("assignment-end").setCustomValidity("");
    const first = node("option", "", t("Seleziona un utente attivo"));
    first.value = "";
    const users = state.assignmentUsers.filter((user) => user.active || assignment && Number(user.id) === Number(assignment.user_id));
    const options = users.map((user) => {
      const option = node("option", "", `${user.name} · ${user.email}${user.active ? "" : t(" (disattivato)")}`);
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
    $("assignment-form-title").textContent = assignment ? t("Modifica abilitazione utente") : t("Abilita un utente");
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
      meta.append(node("span", "job-access-role", (["reader", "contributor"].includes(assignment.role_code) ? t(assignment.role_name) : assignment.role_name) || t("Collaboratore")), node("span", "status-badge status-" + status, label), node("span", "assignment-period", assignmentPeriod(assignment)));
      if (assignment.role === "manager") meta.append(node("span", "field-help", t("Gestore · ha già accesso a tutte le commesse")));
      identity.append(meta);
      const controls = node("div", "row-actions");
      const editButton = action(t("Modifica"), t("Modifica abilitazione di ") + assignment.name, () => {
        if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
        state.revokeAssignment = null;
        $("assignment-revoke").hidden = true;
        resetAssignmentForm(assignment);
        $("assignment-start").focus();
      });
      editButton.disabled = !assignment.active;
      if (!assignment.active) editButton.title = t("Riattiva l’utente nella sezione Utenti per modificare l’abilitazione.");
      editButton.dataset.assignmentInactive = assignment.active ? "0" : "1";
      controls.append(editButton, action(t("Revoca"), t("Revoca abilitazione di ") + assignment.name, () => {
        if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
        state.revokeAssignment = assignment;
        state.revokeGroupGrant = null;
        $("group-grant-revoke").hidden = true;
        $("assignment-revoke-description").textContent = t("Revocare l’abilitazione di {0}? {1}", [assignment.name, isCommonJob(state.assignmentJob) ? t("La commessa è comune: l’utente attivo manterrà il diritto di consultarla e registrare le proprie ore.") : assignment.role === "manager" ? t("Come gestore manterrà l’accesso alla commessa.") : t("L’accesso diretto sarà revocato; eventuali abilitazioni ricevute tramite gruppi resteranno valide. Le ore già registrate saranno conservate.")]);
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
    $("assignment-loading").textContent = t("Caricamento delle abilitazioni…");
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
    $("assignment-dialog-title").textContent = t("Abilitazioni · ") + job.code;
    $("assignment-dialog-description").textContent = isCommonJob(job) ? `${job.name}. Commessa comune: tutti gli utenti attivi possono consultarla e registrare le proprie ore. Le abilitazioni specifiche saranno applicate se la commessa smette di essere comune.` : `${job.name}. Scegli chi può utilizzare questa commessa. Una revoca conserva tutte le ore già registrate.`;
    $("assignment-empty").textContent = isCommonJob(job) ? t("Nessuna abilitazione specifica. Questa commessa è già disponibile a tutti gli utenti attivi.") : t("Nessun utente abilitato. Aggiungi una persona dal modulo qui sotto. I gestori hanno comunque accesso a tutte le commesse.");
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
      const option = node("option", "", `${role.builtin ? t(role.name) : role.name} · ${role.can_log_time ? t("consulta e registra ore") : t("sola consultazione")}`);
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
    return [...groups].sort((left, right) => groupPath(left, groups).localeCompare(groupPath(right, groups), locale));
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
    $("group-member-user").disabled = busy || !canEditGroups() || !state.groupMembersReady;
    $("group-member-save").disabled = busy || !canEditGroups() || !state.groupMembersReady || !$("group-member-user").options.length || $("group-member-user").options.length === 1;
  }

  function renderOrganization() {
    if (!state.organizationReady || !can("manage_groups")) return;
    $("organization-content").hidden = false;
    $("groups-count").textContent = state.organizationGroups.length;
    $("groups-empty").hidden = state.organizationGroups.length > 0;
    const seen = new Set();
    const appendBranch = (parentId, parentList) => {
      const children = state.organizationGroups.filter((group) => (group.parent_id === null || group.parent_id === "" ? null : Number(group.parent_id)) === parentId).sort((a, b) => a.name.localeCompare(b.name, locale));
      for (const group of children) {
        if (seen.has(Number(group.id))) continue;
        seen.add(Number(group.id));
        const item = node("li");
        const button = node("button", "group-tree-button");
        button.type = "button";
        button.setAttribute("aria-pressed", String(Number(state.selectedGroup?.id) === Number(group.id)));
        button.append(node("span", "group-tree-name", group.name), node("span", "group-tree-count", String(Number(group.members_count) || 0)));
        button.setAttribute("aria-label", t("{0}, {1} membri diretti", [group.name, Number(group.members_count) || 0]));
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
    $("group-detail-actions").hidden = !group || !canEditGroups();
    $("new-group").hidden = !canEditGroups();
    $("group-member-form").hidden = !canEditGroups();
    $("groups-empty").textContent = state.groupsManaged ? t("Crea il primo gruppo in Keycloak per organizzare la squadra.") : t("Crea il primo gruppo per organizzare la squadra.");
    $("group-members-content").hidden = !group;
    $("group-detail-title").textContent = group ? group.name : t("Seleziona un gruppo");
    $("group-detail-path").textContent = group ? groupPath(group) : state.groupsManaged ? t("Seleziona un gruppo nell’albero per consultare i suoi membri diretti.") : t("Seleziona un gruppo nell’albero per gestire i suoi membri diretti.");
    $("job-roles-list").replaceChildren();
    for (const role of state.organizationRoles) {
      const item = node("li", "job-role-card");
      const identity = node("div");
      identity.append(node("strong", "", role.builtin ? t(role.name) : role.name), node("span", "assignment-email", role.code), node("p", "field-help", role.can_log_time ? t("Consulta e registra le proprie ore") : t("Sola consultazione")));
      item.append(identity);
      if (role.builtin) item.append(node("span", "job-access-role", t("Predefinito")));
      else if (can("manage_job_roles")) item.append(action(t("Modifica"), t("Modifica il ruolo ") + role.name, () => openJobRole(role)));
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
      identity.append(node("strong", "", member.name), node("span", "assignment-email", member.email), node("span", "field-help", `${roles[member.role] || t("Operatore")}${member.active ? "" : t(" · Disattivato")}`));
      item.append(identity);
      if (canEditGroups()) item.append(action(t("Rimuovi"), t("Rimuovi ") + member.name + t(" dal gruppo"), () => {
        if (!requireGroupEditing()) return;
        const group = state.selectedGroup;
        confirmOrganization(t("Rimuovere {0} da {1}? Le abilitazioni ricevute da questo gruppo e dai suoi gruppi padre non saranno più applicate tramite questa appartenenza. Altre appartenenze e abilitazioni dirette restano valide.", [member.name, group.name]), async () => {
          await api(`/api/groups/${encodeURIComponent(group.id)}/members/${encodeURIComponent(member.user_id)}`, { method: "DELETE" });
          showNotice(t("Utente rimosso dal gruppo. Le ore già registrate sono conservate."));
          await refreshOrganization(group.id);
        });
      }, true));
      $("group-members").append(item);
    }
    $("group-members-empty").hidden = state.groupMembers.length !== 0;
    const first = node("option", "", t("Seleziona un utente attivo"));
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
    if (!requireGroupEditing() || state.organizationBusy || !state.organizationReady || busyDialogs.has("group-dialog")) return;
    $("group-form").reset();
    $("group-error").hidden = true;
    $("group-id").value = group?.id || "";
    $("group-name").value = group?.name || "";
    const first = node("option", "", t("Primo livello"));
    first.value = "";
    const options = orderedGroups().filter((candidate) => !group || !groupDescendsFrom(candidate, group.id)).map((candidate) => {
      const option = node("option", "", groupPath(candidate));
      option.value = candidate.id;
      return option;
    });
    $("group-parent").replaceChildren(first, ...options);
    $("group-parent").value = group ? group.parent_id || "" : state.selectedGroup?.id || "";
    $("group-dialog-title").textContent = group ? t("Modifica gruppo") : t("Nuovo gruppo");
    $("group-save").textContent = group ? t("Salva modifiche") : t("Crea gruppo");
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
    $("job-role-dialog-title").textContent = role ? t("Modifica ruolo commessa") : t("Nuovo ruolo commessa");
    $("job-role-save").textContent = role ? t("Salva modifiche") : t("Crea ruolo");
    $("job-role-dialog").showModal();
    $("job-role-code").focus();
  }

  function confirmOrganization(message, operation) {
    if (!requireGroupEditing() || state.organizationBusy) return;
    state.organizationConfirm = operation;
    $("organization-confirm-text").textContent = message;
    $("organization-confirm").hidden = false;
    $("organization-confirm-cancel").focus();
  }

  function resetGroupGrantForm(grant = null) {
    state.editingGroupGrant = grant;
    $("group-grant-form").reset();
    $("group-grant-end").setCustomValidity("");
    const first = node("option", "", t("Seleziona un gruppo"));
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
    $("group-grant-form-title").textContent = grant ? t("Modifica abilitazione gruppo") : t("Abilita un gruppo");
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
      meta.append(node("span", "job-access-role", ["reader", "contributor"].includes(grant.role_code) ? t(grant.role_name) : grant.role_name), node("span", "status-badge status-" + status, label), node("span", "assignment-period", assignmentPeriod(grant)));
      identity.append(meta);
      const controls = node("div", "row-actions");
      controls.append(action(t("Modifica"), t("Modifica abilitazione del gruppo ") + grant.group_name, () => {
        if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
        state.revokeGroupGrant = null;
        $("group-grant-revoke").hidden = true;
        resetGroupGrantForm(grant);
        $("group-grant-role").focus();
      }), action(t("Revoca"), t("Revoca abilitazione del gruppo ") + grant.group_name, () => {
        if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog")) return;
        state.revokeGroupGrant = grant;
        state.revokeAssignment = null;
        $("assignment-revoke").hidden = true;
        $("group-grant-revoke-text").textContent = t("Revocare l’abilitazione di {0} e dei suoi sottogruppi? {1}", [grant.group_name, isCommonJob(state.assignmentJob) ? t("La commessa è comune: tutti gli utenti attivi manterranno il diritto di consultarla e registrare le proprie ore.") : t("Eventuali abilitazioni dirette o ricevute da altri gruppi restano valide. Le ore già registrate sono conservate.")]);
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
    $("notifications-summary").textContent = t("Caricamento delle notifiche…");
    $("notifications-empty").hidden = true;
    $("notifications-error").hidden = true;
  }

  function alertDateTime(value, fallback = t("Non ancora eseguita")) {
    if (!value) return fallback;
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString(locale, { timeZone: "Europe/Rome", day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" });
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
      heading.append(node("h4", "", rule.name), node("span", "status-badge " + (enabled ? "status-active" : "status-paused"), enabled ? t("Attiva") : t("Sospesa")));
      const recipients = (rule.recipient_user_ids || []).map((id) => {
        const user = state.alertUsers.find((candidate) => String(candidate.id) === String(id));
        return user ? user.name || user.email : t("Utente #") + id;
      });
      const emails = rule.recipient_emails || [];
      const meta = node("dl", "alert-rule-details");
      const fields = [
        [t("Controllo"), alertTypes[rule.type]?.label || rule.type],
        [t("Esecuzione"), t("Ogni giorno alle {0} · Europe/Rome", [rule.run_time])],
        [t("Notifiche nell’app"), recipients.length ? recipients.join(", ") : t("Nessun destinatario nell’app")],
        [t("Email"), emails.length ? emails.join(", ") + (state.alertSmtpConfigured ? "" : t(" · invii in attesa")) : t("Nessun destinatario email")],
        [t("Ultima esecuzione"), alertDateTime(rule.last_run_at)],
        [t("Prossima esecuzione"), enabled ? alertDateTime(rule.next_run_at, t("In programmazione")) : t("Sospesa")]
      ];
      if (rule.type === "inventory_due") fields.splice(1,0,[t("Anticipo"),t("{0} giorni · comprese scadenze superate", [rule.days_before])]);
      if (rule.type === "inventory_verification") fields.splice(1,0,[t("Intervallo verifiche"),t("{0} giorni", [rule.verification_days || 90])]);
      for (const [label, value] of fields) meta.append(node("dt", "", label), node("dd", "", value));
      const controls = node("div", "row-actions");
      controls.append(action(t("Modifica"), t("Modifica la regola ") + rule.name, () => openAlertRule(rule)));
      controls.append(action(state.runningAlertRule === rule.id ? t("Controllo in corso…") : t("Esegui ora"), t("Esegui ora la regola ") + rule.name, () => runAlertRule(rule)));
      item.append(heading, meta, controls);
      $("alert-rules-list").append(item);
    }
    $("alert-rules-count").textContent = state.alertRules.length;
    $("alert-rules-empty").hidden = !state.alertRulesReady || state.alertRules.length > 0;
    $("alerts-mail-status").textContent = state.alertSmtpConfigured ? t("Il canale email è configurato. Le notifiche nell’app restano disponibili nella sezione Notifiche.") : t("Email non configurata: gli invii vengono messi in attesa e non vengono spediti. Le notifiche nell’app sono disponibili subito.");
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
      if (!Array.isArray(data.rules) || !Array.isArray(data.users)) throw new Error(t("Il server ha restituito un elenco regole non valido. Riprova."));
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
    const type = $("alert-rule-type").value, due = type === "inventory_due", verification = type === "inventory_verification";
    $("alert-rule-days-field").hidden = !due;
    $("alert-rule-days").disabled = !due;
    $("alert-rule-days").required = due;
    $("alert-rule-verification-field").hidden = !verification;
    $("alert-rule-verification-days").disabled = !verification;
    $("alert-rule-verification-days").required = verification;
    $("alert-recipients-help").textContent = type.startsWith("inventory_") ? t("Seleziona fino a 50 utenti attivi. Tutti i destinatari selezionati ricevono gli avvisi dell’inventario aziendale.") : t("Seleziona fino a 50 utenti attivi. Gli operatori ricevono soltanto avvisi che li riguardano; i gestori possono ricevere gli avvisi di tutti.");
  }

  function openAlertRule(rule = null, type = "missing_hours") {
    if (!isAlertManager() || !state.alertRulesReady || state.alertRulesLoading || state.runningAlertRule !== null || busyDialogs.has("alert-rule-dialog")) return;
    state.editingAlertRule = rule;
    $("alert-rule-form").reset();
    $("alert-rule-error").hidden = true;
    $("alert-rule-emails").setCustomValidity("");
    $("alert-rule-dialog-title").textContent = rule ? t("Modifica regola") : t("Nuova regola");
    $("alert-rule-save").textContent = rule ? t("Salva modifiche") : t("Crea regola");
    $("alert-rule-type").value = rule?.type || type;
    $("alert-rule-name").value = rule?.name || alertTypes[type]?.label || "";
    $("alert-rule-time").value = rule?.run_time || "09:00";
    $("alert-rule-days").value = rule?.days_before || 30;
    $("alert-rule-verification-days").value = rule?.verification_days || 90;
    $("alert-rule-enabled").checked = rule ? rule.enabled === true || rule.enabled === 1 : true;
    $("alert-rule-emails").value = (rule?.recipient_emails || []).join("\n");
    $("alert-rule-emails-help").textContent = state.alertSmtpConfigured ? t("Fino a 50 indirizzi separati da virgole o righe.") : t("Fino a 50 indirizzi separati da virgole o righe. Email non configurata: gli invii resteranno in attesa, senza spedizione.");
    const selected = new Set((rule?.recipient_user_ids || []).map(String));
    const users = [...state.alertUsers];
    for (const id of selected) if (!users.some((user) => String(user.id) === id)) users.push({ id, name: t("Utente #") + id, unavailable: true });
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
      identity.append(node("span", "", user.unavailable ? t("Non più attivo: deselezionalo per salvare") : `${user.email} · ${roles[user.role] || t("Operatore")}`));
      label.append(checkbox, identity);
      $("alert-rule-users").append(label);
    }
    if (!users.length) $("alert-rule-users").append(node("p", "field-help", t("Nessun utente attivo disponibile. Puoi impostare destinatari email in attesa di configurazione.")));
    renderAlertRuleTypeHelp();
    $("alert-rule-dialog").showModal();
    $("alert-rule-name").focus();
  }

  function alertRecipientEmails() {
    const emails = [...new Set($("alert-rule-emails").value.split(/[,\r\n]+/).map((email) => email.trim().toLowerCase()).filter(Boolean))];
    if (emails.length > 50) throw new Error(t("Inserisci al massimo 50 indirizzi email."));
    for (const email of emails) {
      if (email.length > 254 || /[^\x21-\x7e]/.test(email) || !/^[^\s@,;<>]+@[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\.[a-z0-9-]{2,}$/i.test(email) || email.includes("..")) throw new Error(t("L’indirizzo “{0}” non è valido. Separa gli indirizzi con virgole o righe.", [email]));
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
      showNotice(t("Controllo completato: {0} {1}.{2}", [count, count === 1 ? t("nuova notifica nell’app") : t("nuove notifiche nell’app"), queued ? t(" {0} email in attesa; nessun invio effettuato.", [queued]) : ""]));
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

  function notificationMessage(notification) {
    const message = notification.message || "";
    let match;
    if (notification.event_kind === "missing_hours" && (match = /^(.*): il (\d{4}-\d{2}-\d{2}) risultano registrati (\d+) minuti su (\d+) previsti\. Sono trascorse almeno 48 ore dalla fine della giornata lavorativa\.$/s.exec(message))) {
      return t("{0}: il {1} risultano registrati {2} minuti su {3} previsti. Sono trascorse almeno 48 ore dalla fine della giornata lavorativa.", [match[1], dateLabel(match[2]), Number(match[3]).toLocaleString(locale), Number(match[4]).toLocaleString(locale)]);
    }
    if (notification.event_kind === "invalid_job" && (match = /^(.*) ha tentato di registrare (\d+) minuti il (\d{4}-\d{2}-\d{2}) sulla commessa inesistente #(\d+)\. La registrazione è stata rifiutata\.$/s.exec(message))) {
      return t("{0} ha tentato di registrare {1} minuti il {2} sulla commessa inesistente #{3}. La registrazione è stata rifiutata.", [match[1], Number(match[2]).toLocaleString(locale), dateLabel(match[3]), match[4]]);
    }
    if (notification.event_kind === "inventory_due" && (match = /^(.*) — (.*)\. (Garanzia|Manutenzione|Licenza|Rinnovo): (\d{4}-\d{2}-\d{2}) \((scadenza oggi|scadenza (superata da|tra) (\d+) giorni)\)\.$/s.exec(message))) {
      const timing = match[5] === t("scadenza oggi") ? t("scadenza oggi") : match[6] === "tra" ? t("scadenza tra {0} giorni", [match[7]]) : t("scadenza superata da {0} giorni", [match[7]]);
      return `${match[1]} — ${match[2]}. ${t(match[3])}: ${dateLabel(match[4])} (${timing}).`;
    }
    if (notification.event_kind === "inventory_return" && (match = /^(.*) — (.*)\. Restituzione prevista il (\d{4}-\d{2}-\d{2}): in ritardo da (\d+) giorni\.$/s.exec(message))) return t("{0} — {1}. Restituzione prevista il {2}: in ritardo da {3} giorni.", [match[1], match[2], dateLabel(match[3]), match[4]]);
    if (notification.event_kind === "inventory_verification" && (match = /^(.*) — (.*)\. Il bene non è mai stato verificato: occorre una verifica inventario\.$/s.exec(message))) return t("{0} — {1}. Il bene non è mai stato verificato: occorre una verifica inventario.", [match[1], match[2]]);
    if (notification.event_kind === "inventory_verification" && (match = /^(.*) — (.*)\. Verifica da ripetere entro il (\d{4}-\d{2}-\d{2}): intervallo di (\d+) giorni (superato|raggiunto oggi)\.$/s.exec(message))) return match[5] === "superato" ? t("{0} — {1}. Verifica da ripetere entro il {2}: intervallo di {3} giorni superato.", [match[1], match[2], dateLabel(match[3]), match[4]]) : t("{0} — {1}. Verifica da ripetere entro il {2}: intervallo di {3} giorni raggiunto oggi.", [match[1], match[2], dateLabel(match[3]), match[4]]);
    return message;
  }

  function renderNotifications() {
    $("notifications-list").replaceChildren();
    for (const notification of state.notifications) {
      const unread = !notification.read_at;
      const item = node("li", "notification-card" + (unread ? " is-unread" : ""));
      const heading = node("div", "notification-heading");
      const title = node("h3", "", t(notification.title));
      heading.append(title);
      if (unread) heading.append(node("span", "notification-new-label", t("Da leggere")));
      const created = node("time", "", alertDateTime(notification.created_at, ""));
      created.dateTime = notification.created_at;
      item.append(heading, node("p", "notification-message", notificationMessage(notification)), created);
      if (notification.work_date && validDate(notification.work_date)) item.append(node("p", "notification-context", t("Giornata: ") + dateLabel(notification.work_date)));
      const actions = node("div", "row-actions");
      if (["inventory_due", "inventory_return", "inventory_verification"].includes(notification.event_kind) && Number.isSafeInteger(notification.asset_id) && notification.asset_id > 0) {
        actions.append(action(t("Apri asset"), t("Apri l’asset relativo alla notifica"), () => { selectTab("inventory"); openInventoryDetail(notification.asset_id); }));
        if (state.session?.tickets?.configured) actions.append(ticketAppLink(t("Nuovo ticket ↗"), "asset", notification.asset_id, true), ticketAppLink(t("Ticket collegati ↗"), "asset", notification.asset_id));
      }
      if (unread) {
        const button = action(state.readingNotifications.has(notification.id) ? t("Aggiornamento…") : t("Segna come letta"), t("Segna come letta: ") + t(notification.title), () => markNotificationRead(notification));
        button.disabled = state.readingNotifications.has(notification.id);
        actions.append(button);
      }
      if (actions.childElementCount) item.append(actions);
      if (!unread) item.append(node("span", "notification-read-label", t("Letta il ") + alertDateTime(notification.read_at, "")));
      $("notifications-list").append(item);
    }
    $("notifications-count").textContent = state.notifications.length;
    $("nav-notifications-count").textContent = state.notificationsUnread;
    $("nav-notifications-count").setAttribute("aria-label", t("{0} notifiche non lette", [state.notificationsUnread]));
    $("nav-notifications-count").hidden = state.notificationsUnread === 0;
    $("notifications-summary").textContent = (state.notifications.length === 100 ? t("Sono mostrati gli ultimi 100 avvisi. ") : "") + (state.notificationsUnread ? `${state.notificationsUnread} ${state.notificationsUnread === 1 ? t("notifica da leggere") : t("notifiche da leggere")}.` : t("Hai letto tutti gli avvisi ricevuti."));
    $("notifications-empty").hidden = !state.notificationsReady || state.notifications.length > 0;
    updateCombinedNotificationBadge();
    renderDashboardNotices();
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
      if (!Array.isArray(data.notifications) || !Number.isInteger(data.unread_count) || data.unread_count < 0) throw new Error(t("Il server ha restituito un elenco notifiche non valido. Riprova."));
      state.notifications = data.notifications;
      state.notificationsUnread = data.unread_count;
      state.notificationsReady = true;
      renderNotifications();
    } catch (error) {
      if (request === state.notificationsRequest) {
        $("notifications-error-text").textContent = error.message;
        $("notifications-error").hidden = false;
        if (!state.notificationsReady) $("notifications-summary").textContent = t("Le notifiche non sono disponibili. Riprova a caricarle.");
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
    "tasks.create": t("Attività creata"), "tasks.update": t("Attività aggiornata"), "tasks.delete": t("Attività eliminata"),
    "blockers.create": t("Blocco segnalato"), "blockers.update": t("Blocco aggiornato"), "blockers.delete": t("Blocco eliminato"), "blockers.task_detached": t("Blocco scollegato dall’attività eliminata"),
    "jobs.create": t("Commessa creata"), "jobs.update": t("Commessa aggiornata"), "jobs.delete": t("Commessa e ore eliminate"),
    "time_entries.create": t("Ore registrate"), "time_entries.update": t("Ore aggiornate"), "time_entries.delete": t("Ore eliminate"),
    "assignments.create": t("Utente abilitato"), "assignments.update": t("Abilitazione aggiornata"), "assignments.delete": t("Abilitazione revocata"),
    "suppliers.create": t("Fornitore creato"), "suppliers.update": t("Fornitore aggiornato"),
    "contracts.create": t("Contratto creato"), "contracts.update": t("Contratto aggiornato"), "contract_documents.create": t("PDF contrattuale archiviato"),
    "labor_rates.create": t("Tariffa oraria creata"), "labor_rates.update": t("Tariffa oraria aggiornata"),
    "alert_rules.create": t("Regola alert creata"), "alert_rules.update": t("Regola alert aggiornata"), "alert_rules.run": t("Controllo alert eseguito"), "alert_rules.trigger": t("Controllo alert automatico eseguito"),
    "groups.create": t("Gruppo creato"), "groups.update": t("Gruppo aggiornato"), "groups.delete": t("Gruppo eliminato"),
    "group_members.create": t("Utente aggiunto al gruppo"), "group_members.delete": t("Utente rimosso dal gruppo"),
    "job_roles.create": t("Ruolo commessa creato"), "job_roles.update": t("Ruolo commessa aggiornato"),
    "group_grants.create": t("Gruppo abilitato"), "group_grants.update": t("Abilitazione gruppo aggiornata"), "group_grants.delete": t("Abilitazione gruppo revocata"),
    "users.create": t("Utente creato"), "users.update": t("Utente aggiornato"), "system.migration": t("Database aggiornato")
  };
  const auditTypes = { task: t("Attività"), blocker: t("Blocco"), job: t("Commessa"), time_entry: t("Registrazione ore"), assignment: t("Abilitazione"), user: t("Utente"), supplier: t("Fornitore"), contract: t("Contratto"), contract_document: t("Documento contrattuale"), labor_rate: t("Tariffa oraria"), alert_rule: t("Regola alert"), group: t("Gruppo"), group_member: t("Membro gruppo"), job_role: t("Ruolo commessa"), group_grant: t("Abilitazione gruppo"), system: t("Sistema") };
  const auditLabels = {
    task_id: t("ID attività"), assigned_user_id: t("ID responsabile"), assigned_user_name: t("Responsabile"), owner_name: t("Responsabile"), priority: t("Priorità"), impact: t("Impatto"), next_action: t("Prossima azione"), due_date: t("Scadenza"), revision: t("Revisione"), created_by: t("ID autore"), deleted_tasks: t("Attività eliminate"), deleted_blockers: t("Blocchi eliminati"),
    parent_id: t("ID gruppo padre"), group_id: t("ID gruppo"), group_name: t("Gruppo"), role_id: t("ID ruolo commessa"), role_code: t("Codice ruolo"), role_name: t("Ruolo commessa"), can_log_time: t("Può registrare ore"), builtin: t("Ruolo predefinito"), id: "ID", user_id: t("ID utente"), job_id: t("ID commessa"), owner_id: t("ID autore"), owner: t("Account"), email: t("Email"), name: t("Nome"), role: t("Ruolo"), active: t("Accesso attivo"),
    title: t("Titolo"), supplier_id: t("ID fornitore"), supplier_name: t("Fornitore"), contract_id: t("ID contratto"), contract_code: t("Codice contratto"), contract_title: t("Contratto"), vat_number: t("Partita IVA"), phone: t("Telefono"), address: t("Indirizzo"), notes: t("Note"), value_cents: t("Valore"), hourly_cents: t("Tariffa oraria"), currency: t("Valuta"), file_name: t("Nome documento"), byte_size: t("Dimensione"),
    type: t("Tipo di controllo"), run_time: t("Orario giornaliero"), enabled: t("Regola attiva"), recipient_user_ids: t("ID destinatari nell’app"), recipient_emails: t("Destinatari email"), recipient_email_count: t("Numero destinatari email"), recipient_user_count: t("Numero destinatari nell’app"), notifications_created: t("Notifiche create"), emails_queued: t("Email in attesa"), last_run_at: t("Ultima esecuzione"), last_run_on: t("Giorno ultima esecuzione"), next_run_at: t("Prossima esecuzione"), created_at: t("Creata il"), updated_at: t("Aggiornata il"),
    code: t("Codice"), status: t("Stato"), description: t("Descrizione"), client: t("Cliente"), is_common: t("Commessa comune"), deleted_time_entries: t("Registrazioni ore eliminate"), deleted_minutes: t("Ore eliminate"), deleted_assignments: t("Abilitazioni utenti eliminate"), deleted_group_grants: t("Abilitazioni gruppi eliminate"), date: t("Giornata"), minutes: t("Ore lavorate"), starts_on: t("Abilitato dal"), ends_on: t("Abilitato fino al"),
    user_name: t("Utente"), user_email: t("Email utente"), job_name: t("Commessa"), job_code: t("Codice commessa"), note: t("Attività / nota"), version: t("Versione"), schema_version: t("Versione database"), migrated_entries: t("Registrazioni migrate"), migrated_users: t("Utenti migrati"), mapped_entries: t("Registrazioni collegate agli utenti"), archived_entries: t("Registrazioni storiche conservate")
  };

  function auditTime(value) {
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString(locale, { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
  }

  function auditValue(key, value, entityType) {
    if (value === null || value === undefined || value === "") return "—";
    if (key === "minutes" || key === "deleted_minutes") return minutesLabel(value);
    if (key === "value_cents") return euroLabel(value);
    if (key === "hourly_cents") return euroLabel(value) + t(" / ora");
    if (key === "byte_size") return fileSize(value);
    if (key === "role") return roles[value] || String(value);
    if (key === "status") {
      const catalogue = entityType === "task" ? workTaskStates : entityType === "blocker" ? workBlockerStates : entityType === "contract" ? contractStates : entityType === "job" ? states : null;
      return catalogue?.[value] || String(value);
    }
    if (key === "priority" && entityType === "task") return workPriorities[value] || String(value);
    if (key === "impact" && entityType === "blocker") return workImpacts[value] || String(value);
    if (key === "active") return value === true || value === 1 || value === "1" ? t("Attivo") : t("Disattivato");
    if (key === "is_common") return value === true || value === 1 || value === "1" ? t("Sì") : t("No");
    if (key === "enabled") return value === true || value === 1 || value === "1" ? t("Attiva") : t("Sospesa");
    if (key === "type" && entityType === "alert_rule") return alertTypes[value]?.label || String(value);
    if (["last_run_at", "next_run_at", "created_at", "updated_at"].includes(key) && entityType === "alert_rule") return alertDateTime(value, "—");
    if (key === "last_run_on" && validDate(String(value))) return dateLabel(value);
    if (["date", "starts_on", "ends_on", "due_date"].includes(key) && validDate(String(value))) return dateLabel(value);
    if (typeof value === "boolean") return value ? t("Sì") : t("No");
    return String(value);
  }

  function auditSnapshot(snapshot, title, entityType) {
    const section = node("section", "audit-snapshot");
    section.append(node("h4", "", title));
    if (!snapshot || typeof snapshot !== "object") {
      section.append(node("p", "no-value", title === t("Prima") ? t("Nessun dato precedente") : t("Elemento rimosso")));
      return section;
    }
    const list = node("dl");
    function addFields(fields, prefix = "", depth = 0, fieldEntityType = entityType) {
      for (const [key, value] of Object.entries(fields)) {
        if (/password|hash|salt|token|secret/i.test(key)) continue;
        const contextualLabel = fieldEntityType === "task" && key === "parent_id" ? t("ID attività principale") : fieldEntityType === "blocker" && key === "owner_id" ? t("ID responsabile") : fieldEntityType === "labor_rate" && key === "starts_on" ? t("Valida dal") : fieldEntityType === "labor_rate" && key === "ends_on" ? t("Valida al") : null;
        const label = prefix + (contextualLabel || auditLabels[key] || key.replaceAll("_", " "));
        if (value && typeof value === "object" && !Array.isArray(value) && depth < 3) {
          const nestedType = key === "contract" ? "contract" : key === "job" ? "job" : null;
          addFields(value, label + " · ", depth + 1, nestedType);
          continue;
        }
        list.append(node("dt", "", label), node("dd", "", Array.isArray(value) ? value.map((item) => typeof item === "object" ? t("Elemento") : auditValue(key, item, fieldEntityType)).join(", ") : auditValue(key, value, fieldEntityType)));
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
      const type = auditTypes[event.entity_type] || t("Elemento");
      const object = `${type}${event.entity_id ? " #" + event.entity_id : ""}${subject ? " · " + subject : ""}`;
      const actor = event.actor_name || event.actor_email || t("Sistema");
      const byline = node("p", "audit-byline", actor + (event.actor_name && event.actor_email ? " · " + event.actor_email : ""));
      const details = node("details", "audit-details");
      details.append(node("summary", "", t("Visualizza prima e dopo")));
      const snapshots = node("div", "audit-snapshots");
      snapshots.append(auditSnapshot(event.before, t("Prima"), event.entity_type), auditSnapshot(event.after, t("Dopo"), event.entity_type));
      details.append(snapshots);
      item.append(top, node("p", "audit-object", object), byline, details);
      $("audit-list").append(item);
    }
    $("audit-empty").hidden = !state.auditReady || state.auditEvents.length > 0;
    $("audit-more").hidden = !state.auditCursor;
    $("audit-count").textContent = `${state.auditEvents.length} ${state.auditEvents.length === 1 ? t("modifica caricata") : t("modifiche caricate")}`;
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
    $("audit-loading").textContent = more ? t("Caricamento delle modifiche precedenti…") : t("Caricamento dello storico…");
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
    if (!canReadUsers()) return;
    const request = ++state.usersRequest;
    $("users-loading").hidden = false;
    $("users-loading").textContent = t("Caricamento degli utenti…");
    try {
      const data = await api("/api/users");
      if (request !== state.usersRequest || !canReadUsers()) return;
      state.users = data.users;
      state.usersReady = true;
      renderUsers();
    } catch (error) {
      if (request !== state.usersRequest) return;
      $("users-loading").textContent = t("Gli utenti non sono disponibili. Usa Riprova per ricaricare i dati.");
      showLoadError(error);
    }
  }

  async function refreshEntries() {
    const request = ++state.entriesRequest;
    const date = state.selectedDate, userId = timeWorkflow.userId;
    state.entriesReady = false;
    $("entries-loading").hidden = false;
    $("entries-loading").textContent = t("Caricamento delle ore…");
    $("entries-table-wrap").hidden = true;
    $("entries-empty").hidden = true;
    $("day-total").textContent = "—";
    $("day-entries-count").textContent = "";
    $("hours-week-panel").hidden = true;
    try {
      const weekView = timeWorkflow.view === "week";
      const data = await api((weekView ? "/api/time-entries/week?date=" : "/api/time-entries?date=") + encodeURIComponent(date) + (userId ? "&user_id=" + encodeURIComponent(userId) : ""));
      if (request !== state.entriesRequest) return;
      if (weekView) timeWorkflow.week = data; else state.entries = data.entries;
      state.entriesReady = true;
      if (!userId && weekView && data.start <= localDate(new Date()) && data.end >= localDate(new Date())) {
        const todayEntries = data.entries.filter((entry) => entry.date === localDate(new Date()));
        state.todayMinutes = todayEntries.reduce((sum, entry) => sum + Number(entry.minutes), 0); state.todayCount = todayEntries.length;
      } else if (!userId && !weekView && date === localDate(new Date())) {
        state.todayMinutes = Number(data.total_minutes); state.todayCount = data.entries.length;
      }
      renderEntries();
    } catch (error) {
      if (request === state.entriesRequest) {
        $("entries-loading").textContent = t("Le ore non sono disponibili. Usa Riprova per ricaricare i dati.");
        showLoadError(error);
      }
    }
  }

  async function refreshAll() {
    invalidateReports();
    const request = ++state.refreshRequest;
    $("load-error").hidden = true;
    $("retry-load").disabled = true;
    if (!state.jobsReady) $("jobs-loading").textContent = t("Caricamento delle commesse…");
    const jobsTask = api("/api/jobs").then((data) => {
      if (request !== state.refreshRequest) return;
      state.jobs = data.jobs;
      state.jobsReady = true;
      renderJobs();
      renderWorkJobs();
      if (state.activeTab === "work-board" && state.workJobId && !state.workBusy) return refreshWorkBoard();
    }).catch((error) => {
      if (request !== state.refreshRequest) return;
      if (!state.jobsReady) $("jobs-loading").textContent = t("Le commesse non sono disponibili. Usa Riprova per ricaricare i dati.");
      showLoadError(error);
    });
    const tasks = [jobsTask, refreshEntries(), refreshNotifications(), refreshWorkNotifications()];
    if (state.activeTab === "my-work") tasks.push(refreshMyWork());
    if (state.activeTab === "work-schedules") tasks.push(refreshWorkSchedules());
    if (state.activeTab === "hours") { tasks.push(refreshTimeCorrections()); if (can("view_team_hours")) tasks.push(refreshHoursTeam()); }
    if (state.activeTab === "inventory" && !state.inventoryLoading) tasks.push(refreshInventory());
    if (canReadUsers()) tasks.push(refreshUsers());
    if (can("view_suppliers")) tasks.push(refreshRegistry("suppliers"));
    if (can("view_contracts")) tasks.push(refreshRegistry("contracts"));
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
    $(submitId).textContent = busy ? t("Salvataggio…") : label;
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
    $("job-dialog-title").textContent = job ? t("Modifica commessa") : t("Nuova commessa");
    $("job-save").textContent = job ? t("Salva modifiche") : t("Crea commessa");
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
    $("job-delete-description").textContent = t("Stai per eliminare definitivamente “{0}” ({1}).", [state.deleteJob.name, state.deleteJob.code]);
    $("job-delete-dialog").showModal();
  }

  function openEntry(entry = null, jobId = "", taskId = "") {
    if (entry && !requirePermission("edit_time_entries")) return;
    if (busyDialogs.has("entry-dialog")) return;
    if (!state.jobsReady) { showNotice(t("Attendi il caricamento delle commesse, poi riprova."), true); return; }
    const writableJobs = state.jobs.filter(canLogTime);
    if (writableJobs.length === 0) {
      if (!can("manage_jobs")) { showNotice(t("Nessuna commessa abilitata alla registrazione delle ore. Chiedi a un gestore il ruolo adatto alla commessa su cui devi lavorare."), true); return; }
      state.openEntryAfterJob = true;
      openJob();
      $("job-dialog-description").textContent = t("Crea prima una commessa: subito dopo potrai registrare le tue ore.");
      return;
    }
    $("entry-form").reset();
    $("entry-hours").setCustomValidity("");
    $("entry-hours").removeAttribute("aria-invalid");
    $("entry-error").hidden = true;
    $("entry-id").value = entry ? entry.id : "";
    state.editingTimeEntry = entry;
    $("entry-dialog-title").textContent = entry ? t("Modifica registrazione") : t("Registra ore");
    $("entry-save").textContent = entry ? t("Salva modifiche") : t("Salva ore");
    renderEntryJobs(entry ? entry.job_id : jobId);
    loadEntryTasks(entry?.task_id || taskId);
    $("entry-date").value = entry ? entry.date : state.activeTab === "hours" ? state.selectedDate : localDate(new Date());
    if (entry) {
      $("entry-hours").value = `${Math.floor(entry.minutes / 60)}:${String(entry.minutes % 60).padStart(2, "0")}`;
      $("entry-note").value = entry.note || "";
    }
    $("entry-dialog").showModal();
    if (entry || jobId) $("entry-hours").focus(); else $("entry-job").focus();
  }

  async function loadEntryTasks(selected = "") {
    const request = state.entryTasksRequest = (state.entryTasksRequest || 0) + 1, jobId = $("entry-job").value;
    state.entryTaskLoading = true; state.entryTaskDesired = selected;
    const empty = node("option", "", t("Nessuna attività specifica")); empty.value = ""; $("entry-task").replaceChildren(empty); $("entry-task").disabled = true;
    $("entry-task-status").textContent = jobId ? t("Caricamento delle attività…") : t("Scegli prima una commessa."); $("entry-task-retry").hidden = true;
    if (!jobId) { state.entryTaskLoading = false; return; }
    try {
      const data = await api("/api/work-board?job_id=" + encodeURIComponent(jobId));
      if (request !== state.entryTasksRequest || $("entry-job").value !== jobId) return;
      for (const task of data.tasks || []) { const option = node("option", "", task.title + " · " + (workTaskStates[task.status] || task.status)); option.value = String(task.id); $("entry-task").append(option); }
      $("entry-task").value = String(selected || ""); $("entry-task").disabled = false;
      if (selected && !$("entry-task").value) { $("entry-task-status").textContent = t("L’attività precedente non è più disponibile. Scegli un’altra attività o salva senza collegamento."); }
      else $("entry-task-status").textContent = t("Le ore restano nella commessa e possono indicare anche l’attività svolta.");
    } catch (error) { if (request === state.entryTasksRequest) { $("entry-task-status").textContent = error.message; $("entry-task-retry").hidden = false; } }
    finally { if (request === state.entryTasksRequest) state.entryTaskLoading = false; }
  }

  function openDelete(entry) {
    if (!requirePermission("edit_time_entries")) return;
    state.deleteEntry = entry;
    $("delete-error").hidden = true;
    $("delete-description").textContent = t("{0} su “{1}”, il {2}. La registrazione verrà rimossa dal tuo registro ore.", [minutesLabel(entry.minutes), entry.job_name, dateLabel(entry.date)]);
    $("delete-dialog").showModal();
  }

  function openUser(user = null) {
    if (!requirePermission("manage_users") || busyDialogs.has("user-dialog")) return;
    $("user-form").reset();
    $("user-password").setCustomValidity("");
    $("user-error").hidden = true;
    $("user-id").value = user ? user.id : "";
    $("user-dialog-title").textContent = user ? t("Modifica utente") : t("Nuovo utente");
    $("user-dialog-description").textContent = user ? t("Aggiorna il nome, il ruolo e l’accesso dell’utente. Deve rimanere almeno un gestore attivo.") : t("Crea un account e scegli cosa può fare nello spazio di lavoro.");
    $("user-save").textContent = user ? t("Salva modifiche") : t("Crea utente");
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
    $("entries-loading").textContent = t("Caricamento delle ore…");
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

  const workTaskStates = { todo: t("Da fare"), in_progress: t("In corso"), blocked: t("Bloccato"), in_review: t("Da verificare"), done: t("Completato") };
  const workPriorities = { low: t("Bassa"), normal: t("Normale"), high: t("Alta"), urgent: t("Urgente") };
  const workBlockerStates = { open: t("Aperto"), in_progress: t("In gestione"), resolved: t("Risolto") };
  const workImpacts = { low: t("Basso"), medium: t("Medio"), high: t("Alto"), critical: t("Critico") };
  Object.assign(state, { inventoryItems: [], inventoryReady: false, inventoryLoading: false, inventoryConfigured: null, inventorySummary: null, inventoryRequest: 0, inventoryEpoch: 0, inventoryCursor: null, inventoryRetryMore: false, inventoryEditorRequest: 0, inventoryEditingId: null, inventoryEditingAsset: null, inventoryConflict: false, inventoryDetailRequest: 0, inventoryDetailId: null, inventoryDetailAsset: null, inventoryHistoryRequest: 0, inventoryHistoryLoading: false, inventoryHistoryReady: false, inventoryHistoryCursor: null, inventoryHistoryRetryMore: false });
  const inventoryStates = { available: t("Disponibile"), assigned: t("Assegnato"), maintenance: t("In manutenzione"), retired: t("Dismesso"), lost: t("Smarrito") };
  Object.assign(state, { inventoryDashboard: null, inventoryDashboardRequest: 0, inventoryDashboardLoading: false, inventoryDashboardError: "", inventoryAttention: "", inventoryPlaceFilterError: "" });
  const inventoryAttentionLabels = { open_custody: t("Consegne aperte"), overdue_returns: t("Restituzioni in ritardo"), never_verified: t("Mai verificati"), verification_due: t("Verifiche da ripetere"), location_mismatch: t("Ubicazioni da controllare") };
  const inventoryFields = ["asset_tag", "name", "category", "serial_number", "manufacturer", "model", "status", "location", "assignee", "purchase_date", "warranty_until", "notes"];
  const inventoryTypes = { it_device: t("Dispositivo IT"), network_device: t("Apparato di rete"), physical: t("Bene fisico"), software: t("Software"), license: t("Licenza") };
  const inventoryLicenseModels = { per_device: t("Per dispositivo"), per_user: t("Per utente"), concurrent: t("Concorrente"), site: t("Per sede") };
  const inventoryRenewalModes = { manual: t("Manuale"), automatic: t("Automatico"), none: t("Nessun rinnovo") };
  const inventoryTypeFields = {
    it_device: [["hostname", t("Nome host"), 253], ["ip_address", t("Indirizzo IP"), 45], ["mac_address", t("Indirizzo MAC"), 17], ["operating_system", t("Sistema operativo"), 120]],
    network_device: [["hostname", t("Nome host"), 253], ["ip_address", t("Indirizzo IP"), 45], ["mac_address", t("Indirizzo MAC"), 17], ["operating_system", t("Sistema operativo / firmware"), 120], ["network_role", t("Ruolo di rete"), 80], ["vlan", "VLAN", "number", 1, 4094], ["port_count", t("Numero di porte"), "number", 0, 65535]],
    physical: [["asset_kind", t("Tipo di bene"), 80], ["department", t("Reparto · testo libero"), 120], ["maintenance_due", t("Prossima manutenzione"), "date"]],
    software: [["publisher", t("Editore"), 120], ["software_version", t("Versione software"), 80], ["edition", t("Edizione"), 80], ["software_kind", t("Tipo di software"), 80]],
    license: [["license_model", t("Modello di licenza"), inventoryLicenseModels, "per_device"], ["seats_total", t("Posti acquistati"), "number", 1, 1000000, 1], ["expires_on", t("Scadenza"), "date"], ["renewal_on", t("Data di rinnovo"), "date"], ["renewal_mode", t("Rinnovo"), inventoryRenewalModes, "manual"], ["license_reference", t("Riferimento della licenza"), 120]]
  };
  const inventoryRelationNames = { installed_on: t("Software installato"), connected_to: t("Collegamento di rete"), licensed_to: t("Assegnazione di licenza") };
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
    picker.search = node("input"); picker.search.type = "search"; picker.search.maxLength = 160; picker.search.autocomplete = "off"; picker.search.placeholder = t("Cerca per codice o nome…"); picker.search.setAttribute("aria-label", t("Cerca ") + label.toLowerCase());
    picker.type = node("select"); picker.type.setAttribute("aria-label", t("Tipologia del destinatario"));
    picker.find = node("button", "button button-secondary", t("Cerca")); picker.find.type = "button";
    picker.choice = node("select"); picker.choice.id = key + "-choice"; picker.choice.required = true;
    picker.status = node("p", "field-help"); picker.status.setAttribute("role", "status");
    picker.error = node("p", "dialog-error"); picker.error.hidden = true; picker.error.setAttribute("role", "alert");
    picker.more = node("button", "row-button", t("Carica altri risultati")); picker.more.type = "button"; picker.more.hidden = true;
    controls.append(picker.search, picker.type, picker.find); field.append(heading, controls, picker.choice, picker.status, picker.error, picker.more); host.replaceChildren(field);
    picker.render = () => {
      const selected = picker.selected;
      picker.choice.replaceChildren(new Option(t("Seleziona un asset…"), ""));
      const options = [...(selected ? [selected] : []), ...picker.items.filter((asset) => !selected || String(asset.id) !== String(selected.id))];
      options.forEach((asset) => picker.choice.add(new Option(inventoryAssetLabel(asset), String(asset.id))));
      picker.choice.value = selected ? String(selected.id) : "";
      picker.more.hidden = !picker.cursor && !picker.retryMore;
      picker.more.textContent = picker.retryMore ? t("Riprova a caricare altri risultati") : t("Carica altri risultati");
      picker.find.disabled = picker.loading || !picker.enabled;
      picker.more.disabled = picker.loading || !picker.enabled;
      picker.choice.disabled = !picker.enabled;
      picker.search.disabled = !picker.enabled;
      picker.type.disabled = !picker.enabled;
      picker.status.textContent = picker.loading ? t("Ricerca degli asset…") : picker.items.length ? picker.items.length + t(" risultati caricati.") : t("Cerca un asset nell’inventario.");
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
      picker.selected = id ? inventoryAssetCache.get(String(id)) || { id, asset_tag: "#" + id, name: t("Caricamento…"), asset_type: types[0] } : null;
      picker.render();
      if (!id || inventoryAssetCache.has(String(id))) return;
      try {
        const data = await api("/api/inventory/assets/" + encodeURIComponent(id));
        if (request !== picker.request || epoch !== state.inventoryEpoch || !context()) return;
        picker.selected = inventoryRemember(data.asset); picker.render();
      } catch (error) {
        if (request !== picker.request || epoch !== state.inventoryEpoch || !context()) return;
        picker.selected = { id, asset_tag: "#" + id, name: t("Asset selezionato"), asset_type: types[0] }; picker.render();
        picker.error.textContent = t("Non siamo riusciti a leggere il nome dell’asset selezionato. Puoi cercarlo di nuovo."); picker.error.hidden = false;
      }
    };
    picker.load = async (more = false) => {
      if (!picker.enabled || !context() || more && (picker.loading || !picker.cursor)) return;
      const queryText = picker.search.value.trim();
      if (new TextEncoder().encode(queryText).length > 160) { picker.search.setCustomValidity(t("Accorcia la ricerca.")); picker.search.reportValidity(); return; }
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
      group.append(node("legend", "", t("Dettagli · ") + inventoryTypes[type]));
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
        field.append(labelNode, input); if (name === "license_reference") field.append(node("p", "field-help", t("Inserisci un riferimento amministrativo; conserva le chiavi segrete nel sistema dedicato."))); grid.append(field);
      });
      group.append(grid); host.append(group);
    });
    createInventoryPicker("inventory-software-picker", "inventory-software-picker-host", t("Software coperto"), ["software"], () => $("inventory-dialog").open && isInventoryManager());
    createInventoryPicker("inventory-relation-picker", "inventory-relation-target", t("Dispositivo destinatario"), ["it_device", "network_device"], () => $("inventory-detail-dialog").open && !$("inventory-relation-form").hidden && isInventoryManager());
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
      if (input.maxLength > 0 && new TextEncoder().encode(input.value).length > input.maxLength) input.setCustomValidity(t("Accorcia il contenuto di questo campo."));
      if (input.type === "date" && input.value && !validDate(input.value)) input.setCustomValidity(t("Inserisci una data valida."));
      if (input.value !== "") details[input.dataset.inventoryDetail] = input.type === "number" ? Number(input.value) : input.value;
    });
    if (type === "license") {
      const picker = inventoryPickers.get("inventory-software-picker");
      picker.choice.setCustomValidity(picker.selected ? "" : t("Seleziona il software coperto dalla licenza."));
      if (picker.selected) details.software_id = Number(picker.selected.id);
    }
    return details;
  }

  function inventoryTypedDisplay(asset) {
    const type = inventoryAssetType(asset), d = asset.details || {}, fields = [];
    if (type === "license") { const software = inventoryAssetCache.get(String(d.software_id)); fields.push([t("Software coperto"), software ? inventoryAssetLabel(software) : d.software_id ? t("Asset #") + d.software_id : ""]); }
    (inventoryTypeFields[type] || []).forEach(([name, label, kind]) => { const value = d[name]; fields.push([label, typeof kind === "object" ? kind[value] || value : kind === "date" && value && validDate(value) ? dateLabel(value) : value]); });
    if (type === "license" && asset.license_usage) {
      const usage = asset.license_usage;
      fields.push([t("Posti assegnati"), usage.allocated ?? 0], [t("Posti disponibili"), usage.available ?? 0], [t("Validità della licenza"), usage.expired ? t("Scaduta") : t("Valida")]);
      if (["concurrent", "site"].includes(d.license_model)) fields.push([t("Conteggio assegnazioni"), t("Assegnazioni registrate manualmente; non misura l’utilizzo simultaneo.")]);
    }
    return fields;
  }

  let inventorySearchTimer;
  Object.assign(state, { inventoryBackup: null, inventoryExporting: false, inventoryImportRequest: 0, inventoryImportBusy: false, inventoryImportPlan: null });

  function isInventoryManager() { return state.session?.role === "manager"; }

  Object.assign(state, { inventoryPlaces: [], inventoryPlacesReady: false, inventoryPlacesRequest: 0, inventoryUsers: [], inventoryUsersReady: false, inventoryUsersRequest: 0, inventoryDirectoryRequest: 0, inventoryDirectoryBusy: false, inventoryDirectoryEditing: null, inventoryContextRequest: 0, inventoryContextAsset: null, inventoryContextBusy: false, inventoryContextLoading: false, inventoryContextConflict: false, inventoryOperationRequest: 0, inventoryOperationAsset: null, inventoryOperation: null, inventoryOperationBusy: false, inventoryOperationLoading: false, inventoryOperationConflict: false, inventoryCustodyRequest: 0, inventoryCustodyItems: [], inventoryCustodyCursor: null, inventoryCustodyLoading: false, inventoryCustodyRetryMore: false, inventoryVerificationsRequest: 0, inventoryVerificationItems: [], inventoryVerificationsCursor: null, inventoryVerificationsLoading: false, inventoryVerificationsRetryMore: false });
  const inventoryPlaceKinds = { site: t("Sede"), department: t("Reparto"), location: t("Ubicazione") };
  const inventoryConditions = { good: t("Buone"), fair: t("Segni di usura"), damaged: t("Danneggiato") };
  let inventoryPlacesPromise = null, inventoryUsersPromise = null;
  function inventoryTangible(asset) { return Boolean(asset) && ["it_device", "network_device", "physical"].includes(inventoryAssetType(asset)); }
  function inventoryCanCheckout(asset) { return asset && !asset.custody && (asset.status === "available" || asset.status === "assigned" && (asset.context?.assignee_person_id || asset.context?.assignee_user_id)); }
  function inventoryAssigneeName(context) { return context?.assignee_person_name || context?.assignee_user_name || ""; }
  function inventoryCustodyName(custody) { return custody?.person_name || custody?.user_name || (custody?.person_id ? t("Persona #") + custody.person_id : t("Utente #") + custody?.user_id); }
  const inventoryRecipientSelections = { context: null, checkout: null, relation: null };
  function inventoryContextRecipient(asset) {
    const context = asset?.context || {};
    $("inventory-context-recipient-kind").value = context.assignee_user_id ? "user" : "person";
    inventoryRecipientSelections.context = context.assignee_person_id ? { id: context.assignee_person_id, display_name: context.assignee_person_name, person_code: "" } : null;
    inventoryFillUsers($("inventory-context-user"), context.assignee_user_id || "", context.assignee_user_name);
  }
  function inventoryPlacementLabel(asset) { const c = asset?.context; return c ? [c.site_name, c.department_name, c.location_name].filter(Boolean).join(" · ") || asset.location : asset?.location; }
  function inventoryUserLabel(user) { return user.name + (user.email ? " · " + user.email : ""); }
  function inventoryPlace(id) { return state.inventoryPlaces.find((place) => String(place.id) === String(id)); }

  async function loadInventoryPlaces(force = false) {
    if (!state.session?.authenticated) return false;
    if (!force && state.inventoryPlacesReady) return true;
    if (!force && inventoryPlacesPromise) return inventoryPlacesPromise;
    if (force) state.inventoryPlacesReady = false;
    const epoch = state.inventoryEpoch, request = ++state.inventoryPlacesRequest;
    const current = () => epoch === state.inventoryEpoch && request === state.inventoryPlacesRequest && state.session?.authenticated;
    const promise = (async () => {
      const places = [], known = new Set(); let cursor = null, complete = false;
      for (let page = 0; page < 100; page++) {
        const query = new URLSearchParams({ limit: "100" }); if (cursor) query.set("after_id", String(cursor));
        const data = await api("/api/inventory/places?" + query);
        if (!current()) return false;
        if (!Array.isArray(data.places)) throw new Error(t("Il server ha restituito anagrafiche non valide."));
        for (const place of data.places) { if (!known.has(String(place.id))) { known.add(String(place.id)); places.push(place); } }
        if (data.next_after_id === null || data.next_after_id === undefined) { complete = true; break; }
        if (!Number.isSafeInteger(data.next_after_id) || data.next_after_id < 1 || cursor !== null && data.next_after_id >= cursor) throw new Error(t("Il server ha restituito una pagina di anagrafiche non valida."));
        cursor = data.next_after_id;
      }
      if (!complete) throw new Error(t("Le anagrafiche superano il limite di caricamento. Contatta un gestore."));
      if (!current()) return false;
      state.inventoryPlaces = places; state.inventoryPlacesReady = true; renderInventoryPlaceFilters(); return true;
    })();
    inventoryPlacesPromise = promise;
    try { return await promise; } finally { if (inventoryPlacesPromise === promise) inventoryPlacesPromise = null; }
  }

  async function loadInventoryUsers(force = false) {
    if (!isInventoryManager()) return false;
    if (!force && state.inventoryUsersReady) return true;
    if (!force && inventoryUsersPromise) return inventoryUsersPromise;
    if (force) state.inventoryUsersReady = false;
    const epoch = state.inventoryEpoch, request = ++state.inventoryUsersRequest;
    const current = () => epoch === state.inventoryEpoch && request === state.inventoryUsersRequest && isInventoryManager();
    const promise = (async () => {
      const data = await api("/api/inventory/users");
      if (!current()) return false;
      if (!Array.isArray(data.users)) throw new Error(t("Il server ha restituito utenti non validi."));
      state.inventoryUsers = data.users; state.inventoryUsersReady = true; return true;
    })();
    inventoryUsersPromise = promise;
    try { return await promise; } finally { if (inventoryUsersPromise === promise) inventoryUsersPromise = null; }
  }

  function inventoryFillUsers(select, selected = "", name = "", optional = true) {
    select.replaceChildren(new Option(optional ? t("Non assegnato") : t("Seleziona un utente…"), ""));
    state.inventoryUsers.forEach((user) => select.add(new Option(inventoryUserLabel(user), String(user.id))));
    if (selected && !state.inventoryUsers.some((user) => String(user.id) === String(selected))) select.add(new Option((name || t("Utente #") + selected) + t(" · non più attivo"), String(selected)));
    select.value = selected ? String(selected) : "";
  }

  function inventoryFillPlaces(select, kind, selected = "", site = "", snapshotName = "") {
    select.replaceChildren(new Option(kind === "department" ? t("Non indicato") : t("Non indicata"), ""));
    const places = state.inventoryPlaces.filter((place) => place.kind === kind && (place.active || String(place.id) === String(selected)) && (kind === "site" || kind === "department" && !place.parent_id || String(place.parent_id || "") === String(site)));
    places.sort((a, b) => a.name.localeCompare(b.name, "it"));
    places.forEach((place) => select.add(new Option(place.name + (place.active ? "" : t(" · disattivata")), String(place.id))));
    if (selected && !places.some((place) => String(place.id) === String(selected))) select.add(new Option(snapshotName || t("Voce #") + selected, String(selected)));
    select.value = selected ? String(selected) : "";
  }

  function renderInventoryContextPlaces(preserve = true) {
    const c = state.inventoryContextAsset?.context || {};
    const site = preserve ? $("inventory-site-id").value : c.site_id || "";
    const department = preserve ? $("inventory-department-id").value : c.department_id || "";
    const location = preserve ? $("inventory-location-id").value : c.location_id || "";
    inventoryFillPlaces($("inventory-site-id"), "site", site, "", c.site_name);
    inventoryFillPlaces($("inventory-department-id"), "department", department, site, c.department_name);
    inventoryFillPlaces($("inventory-location-id"), "location", location, site, c.location_name);
  }

  function inventoryContextControls() {
    const asset = state.inventoryContextAsset, busy = state.inventoryContextBusy;
    $("inventory-context-fields").disabled = busy || state.inventoryContextLoading;
    $("inventory-context-save").disabled = busy || state.inventoryContextLoading || state.inventoryContextConflict || !inventoryTangible(asset) || !state.inventoryPlacesReady || $("inventory-context-recipient-kind").value === "user" && !state.inventoryUsersReady || !isInventoryManager();
    $("inventory-context-save").textContent = busy ? t("Salvataggio…") : state.inventoryContextLoading ? t("Caricamento anagrafiche…") : t("Salva assegnazione e collocazione");
    $("inventory-context-reload").disabled = busy;
    const person = $("inventory-context-recipient-kind").value === "person", fixed = Boolean(asset?.custody);
    $("inventory-context-recipient-kind").disabled = fixed;
    $("inventory-context-person-field").hidden = !person; $("inventory-context-user-field").hidden = person;
    $("inventory-context-user").disabled = fixed || person;
    $("inventory-context-person-pick").disabled = busy || fixed;
    $("inventory-context-person-clear").disabled = busy || fixed || !inventoryRecipientSelections.context;
    $("inventory-context-person-label").textContent = inventoryRecipientSelections.context ? inventorySubjectLabel("person", inventoryRecipientSelections.context) : t("Non assegnato");
    $("inventory-context-user-help").textContent = asset?.custody ? t("La consegna è ancora aperta. Registra la restituzione prima di cambiare assegnatario; puoi aggiornare i luoghi.") : t("L’assegnazione identifica il responsabile. Usa Consegna per registrare anche il passaggio fisico e la restituzione prevista.");
    if (busy) { busyDialogs.add("inventory-context-dialog"); busyDialogs.add("inventory-detail-dialog"); }
    else { busyDialogs.delete("inventory-context-dialog"); busyDialogs.delete("inventory-detail-dialog"); }
  }

  async function openInventoryContext() {
    const asset = state.inventoryDetailAsset;
    if (!asset || !inventoryTangible(asset) || !isInventoryManager() || state.inventoryContextBusy || state.inventoryRelationBusy || busyDialogs.has("inventory-detail-dialog")) return;
    const request = ++state.inventoryContextRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryContextRequest && epoch === state.inventoryEpoch && $("inventory-context-dialog").open && isInventoryManager();
    state.inventoryContextAsset = asset; state.inventoryContextConflict = false; state.inventoryContextLoading = true;
    $("inventory-context-form").reset(); $("inventory-context-error").hidden = true; $("inventory-placement-error").hidden = true; $("inventory-placement-retry").hidden = true; $("inventory-context-reload").hidden = true;
    const legacy = [!asset.context?.assignee_user_id && !asset.context?.assignee_person_id ? asset.assignee : "", !asset.context?.site_id && !asset.context?.location_id ? asset.location : ""].filter(Boolean);
    $("inventory-context-description").textContent = inventoryAssetLabel(asset) + (legacy.length ? t(". Riferimenti testuali attuali: ") + legacy.join(" · ") + t(". Le selezioni salvate sostituiscono questi riferimenti.") : "");
    inventoryContextRecipient(asset);
    renderInventoryContextPlaces(false); inventoryContextControls(); $("inventory-context-dialog").showModal();
    try {
      await Promise.all([loadInventoryPlaces(true), loadInventoryUsers(true)]);
      if (!current()) return;
      inventoryFillUsers($("inventory-context-user"), asset.context?.assignee_user_id || "", asset.context?.assignee_user_name); renderInventoryContextPlaces(false);
    } catch (error) { if (current()) { dialogError("inventory-placement-error", new Error(inventoryMessage(error))); $("inventory-placement-retry").hidden = false; } }
    finally { if (current()) { state.inventoryContextLoading = false; inventoryContextControls(); } }
  }

  function inventoryUpdateDetail(asset) {
    inventoryRemember(asset);
    if (String(asset.id) !== String(state.inventoryDetailId)) return;
    state.inventoryDetailAsset = asset; $("inventory-detail-title").textContent = asset.name; $("inventory-detail-data").replaceChildren(inventoryDetails(asset));
    renderInventoryOperationalActions(); renderInventoryRelationOptions();
    loadInventoryCustody(); loadInventoryVerifications(); loadInventoryHistory();
  }

  async function reloadInventoryContext() {
    const source = state.inventoryContextAsset;
    if (!source || state.inventoryContextBusy || !isInventoryManager()) return;
    const request = ++state.inventoryContextRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryContextRequest && epoch === state.inventoryEpoch && $("inventory-context-dialog").open && isInventoryManager();
    state.inventoryContextBusy = true; inventoryContextControls();
    try {
      const data = await api("/api/inventory/assets/" + encodeURIComponent(source.id)); if (!current()) return;
      state.inventoryContextAsset = inventoryRemember(data.asset); state.inventoryContextConflict = false;
      if (data.asset.custody) inventoryContextRecipient(data.asset);
      $("inventory-context-error").hidden = true; $("inventory-context-reload").hidden = true;
      inventoryUpdateDetail(data.asset); showNotice(t("Scheda aggiornata. Controlla le selezioni prima di salvarle; la collocazione non è stata modificata."));
    } catch (error) { if (current()) dialogError("inventory-context-error", new Error(inventoryMessage(error))); }
    finally { if (current()) { state.inventoryContextBusy = false; inventoryContextControls(); } }
  }

  async function saveInventoryContext(event) {
    event.preventDefault(); const source = state.inventoryContextAsset;
    if (!source || state.inventoryContextBusy || state.inventoryContextLoading || state.inventoryContextConflict || !state.inventoryPlacesReady || $("inventory-context-recipient-kind").value === "user" && !state.inventoryUsersReady || !isInventoryManager() || !$("inventory-context-form").reportValidity()) return;
    const request = ++state.inventoryContextRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryContextRequest && epoch === state.inventoryEpoch && $("inventory-context-dialog").open && isInventoryManager();
    const fields = { version: String(source.version), [$("inventory-context-recipient-kind").value === "person" ? "assignee_person_id" : "assignee_user_id"]: $("inventory-context-recipient-kind").value === "person" ? inventoryRecipientSelections.context?.id || "" : $("inventory-context-user").value, site_id: $("inventory-site-id").value, department_id: $("inventory-department-id").value, location_id: $("inventory-location-id").value };
    state.inventoryContextBusy = true; $("inventory-context-error").hidden = true; inventoryContextControls();
    try {
      const data = await post("/api/inventory/assets/" + encodeURIComponent(source.id) + "/context", fields); if (!current()) return;
      inventoryUpdateDetail(data.asset); state.inventoryContextBusy = false; inventoryContextControls(); $("inventory-context-dialog").close(); refreshInventory(); showNotice(t("Assegnazione e collocazione aggiornate."));
    } catch (error) {
      if (!current()) return;
      if (error.status === 409) { state.inventoryContextConflict = true; $("inventory-context-reload").hidden = false; }
      dialogError("inventory-context-error", new Error(inventoryMessage(error) + (error.status === 409 ? t(" Le selezioni restano disponibili. Ricarica la scheda prima di riprovare.") : "")));
    } finally { if (current()) { state.inventoryContextBusy = false; inventoryContextControls(); } }
  }

  function renderInventoryDirectory() {
    const kind = $("inventory-directory-kind").value, editing = state.inventoryDirectoryEditing, busy = state.inventoryDirectoryBusy;
    $("inventory-directory-list").replaceChildren();
    state.inventoryPlaces.filter((place) => place.kind === kind).sort((a, b) => a.name.localeCompare(b.name, "it")).forEach((place) => {
      const item = node("li", "inventory-directory-item"), text = node("div");
      text.append(node("strong", "", place.name), node("span", "inventory-cell-secondary", [place.parent_id ? inventoryPlace(place.parent_id)?.name || t("Sede #") + place.parent_id : "", place.active ? t("Attiva") : t("Disattivata")].filter(Boolean).join(" · ")));
      const edit = action(t("Modifica"), t("Modifica ") + place.name, () => editInventoryPlace(place)); edit.disabled = busy;
      item.append(text, edit); $("inventory-directory-list").append(item);
    });
    if (state.inventoryPlacesReady && !$("inventory-directory-list").children.length) $("inventory-directory-list").append(node("li", "field-help", t("Nessuna voce censita in questa anagrafica.")));
    $("inventory-directory-parent-field").hidden = kind === "site"; $("inventory-directory-parent").required = kind === "location";
    $("inventory-directory-parent").previousElementSibling.textContent = kind === "location" ? t("Sede *") : t("Sede · facoltativa");
    $("inventory-directory-active-field").hidden = !editing;
    $("inventory-directory-fields").disabled = busy; $("inventory-directory-kind").disabled = busy;
    $("inventory-directory-save").disabled = busy || !state.inventoryPlacesReady || !isInventoryManager();
    $("inventory-directory-save").textContent = busy ? t("Salvataggio…") : editing ? t("Salva modifiche") : t("Aggiungi voce");
    $("inventory-directory-form-title").textContent = editing ? t("Modifica ") + inventoryPlaceKinds[kind].toLowerCase() : t("Nuova voce");
    $("inventory-directory-cancel-edit").hidden = !editing; $("inventory-directory-cancel-edit").disabled = busy;
    if (busy) busyDialogs.add("inventory-directory-dialog"); else busyDialogs.delete("inventory-directory-dialog");
  }

  function editInventoryPlace(place = null) {
    if (state.inventoryDirectoryBusy) return;
    state.inventoryDirectoryEditing = place; $("inventory-directory-form").reset();
    $("inventory-directory-id").value = place?.id || ""; $("inventory-directory-name").value = place?.name || ""; $("inventory-directory-active").value = place?.active === false ? "0" : "1";
    inventoryFillPlaces($("inventory-directory-parent"), "site", place?.parent_id || "");
    $("inventory-directory-name").setCustomValidity(""); $("inventory-directory-error").hidden = true; renderInventoryDirectory();
    if (place) $("inventory-directory-name").focus();
  }

  async function refreshInventoryDirectory() {
    if (!isInventoryManager() || state.inventoryDirectoryBusy) return;
    const request = ++state.inventoryDirectoryRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryDirectoryRequest && epoch === state.inventoryEpoch && $("inventory-directory-dialog").open && isInventoryManager();
    $("inventory-directory-loading").hidden = false; $("inventory-directory-error").hidden = true; $("inventory-directory-retry").hidden = true;
    try {
      await loadInventoryPlaces(true); if (!current()) return;
      if (state.inventoryDirectoryEditing) {
        const previous = state.inventoryDirectoryEditing;
        state.inventoryDirectoryEditing = state.inventoryPlaces.find((place) => String(place.id) === String(previous.id)) || previous;
        inventoryFillPlaces($("inventory-directory-parent"), "site", $("inventory-directory-parent").value);
        showNotice(t("Elenco aggiornato. I dati inseriti restano disponibili: controllali prima di salvare."));
      } else editInventoryPlace();
    }
    catch (error) { if (current()) { dialogError("inventory-directory-error", new Error(inventoryMessage(error))); $("inventory-directory-retry").hidden = false; } }
    finally { if (current()) { $("inventory-directory-loading").hidden = true; renderInventoryDirectory(); } }
  }

  function openInventoryDirectory() {
    if (!isInventoryManager() || state.inventoryConfigured !== true) return;
    state.inventoryDirectoryEditing = null; $("inventory-directory-form").reset(); $("inventory-directory-dialog").showModal(); renderInventoryDirectory(); refreshInventoryDirectory();
  }

  async function saveInventoryPlace(event) {
    event.preventDefault(); if (!isInventoryManager() || state.inventoryDirectoryBusy || !state.inventoryPlacesReady) return;
    const name = $("inventory-directory-name"); name.value = name.value.trim(); name.setCustomValidity(new TextEncoder().encode(name.value).length > 160 ? t("Accorcia il nome.") : "");
    if (!$("inventory-directory-form").reportValidity()) return;
    const editing = state.inventoryDirectoryEditing, kind = $("inventory-directory-kind").value;
    const request = ++state.inventoryDirectoryRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryDirectoryRequest && epoch === state.inventoryEpoch && $("inventory-directory-dialog").open && isInventoryManager();
    const fields = { kind, name: name.value, parent_id: kind === "site" ? "" : $("inventory-directory-parent").value, active: $("inventory-directory-active").value };
    if (editing) fields.version = String(editing.version);
    state.inventoryDirectoryBusy = true; $("inventory-directory-error").hidden = true; renderInventoryDirectory();
    try {
      await post("/api/inventory/places" + (editing ? "/" + encodeURIComponent(editing.id) : ""), fields); if (!current()) return;
      state.inventoryPlacesReady = false; await loadInventoryPlaces(true); if (!current()) return;
      state.inventoryDirectoryBusy = false; editInventoryPlace(); showNotice(editing ? t("Anagrafica aggiornata; i riferimenti già registrati restano nello storico.") : t("Voce aggiunta all’anagrafica."));
    } catch (error) {
      if (!current()) return;
      dialogError("inventory-directory-error", new Error(inventoryMessage(error) + (error.status === 409 ? t(" I dati inseriti restano disponibili. Ricarica l’elenco prima di riprovare.") : "")));
      if (error.status === 409) $("inventory-directory-retry").hidden = false;
    } finally { if (current()) { state.inventoryDirectoryBusy = false; renderInventoryDirectory(); } }
  }

  function renderInventoryOperationalActions() {
    const asset = state.inventoryDetailAsset, tangible = asset && inventoryTangible(asset), manager = isInventoryManager();
    $("inventory-custody-current").replaceChildren();
    $("inventory-context-open").hidden = !manager || !tangible;
    $("inventory-checkout-open").hidden = !manager || !tangible || Boolean(asset?.custody);
    $("inventory-checkout-open").disabled = !inventoryCanCheckout(asset);
    $("inventory-checkout-open").title = !inventoryCanCheckout(asset) ? t("La consegna richiede un bene disponibile oppure già assegnato a un utente censito. Controlla prima assegnazione e stato.") : "";
    $("inventory-checkin-open").hidden = !manager || !tangible || !asset?.custody;
    $("inventory-verify-open").hidden = !manager || !tangible;
    $("inventory-custody-title").closest("section").hidden = !tangible;
    $("inventory-verifications-title").closest("section").hidden = !tangible;
    if (!tangible) return;
    const custody = asset.custody, c = asset.context;
    if (custody) {
      const overdue = custody.due_on && custody.due_on < localDate(new Date());
      const card = node("div", "inventory-custody-current" + (overdue ? " inventory-custody-overdue" : ""));
      card.append(node("strong", "", t("In consegna a ") + inventoryCustodyName(custody)), node("p", "", t("Dal ") + (validDate(custody.checkout_on) ? dateLabel(custody.checkout_on) : custody.checkout_on) + (custody.due_on ? t(" · restituzione prevista ") + dateLabel(custody.due_on) : t(" · senza data di restituzione prevista"))));
      if (overdue) card.append(node("span", "inventory-status inventory-status-maintenance", t("Restituzione in ritardo")));
      $("inventory-custody-current").append(card);
    } else $("inventory-custody-current").append(node("p", "field-help", (c?.assignee_person_id || c?.assignee_user_id) ? t("Responsabile: ") + inventoryAssigneeName(c) + t(". Nessuna consegna fisica aperta.") : t("Nessuna consegna aperta.")));
  }

  function renderInventoryCustody() {
    $("inventory-custody-history").replaceChildren();
    state.inventoryCustodyItems.forEach((entry) => {
      const item = node("li");
      item.append(node("h4", "", inventoryCustodyName(entry)), node("p", "inventory-history-meta", t("Consegna: ") + dateLabel(entry.checkout_on) + (entry.due_on ? t(" · prevista: ") + dateLabel(entry.due_on) : "")), node("p", "inventory-history-summary", t("Condizioni alla consegna: ") + (inventoryConditions[entry.condition_out] || entry.condition_out)));
      item.append(node("p", "inventory-history-summary", entry.returned_on ? t("Restituito il ") + dateLabel(entry.returned_on) + t(" · condizioni: ") + (inventoryConditions[entry.condition_in] || entry.condition_in || "—") : t("Consegna ancora aperta")));
      if (entry.notes) item.append(node("p", "inventory-history-summary", entry.notes));
      if (entry.return_notes) item.append(node("p", "inventory-history-summary", t("Restituzione: ") + entry.return_notes));
      $("inventory-custody-history").append(item);
    });
    $("inventory-custody-more").hidden = !state.inventoryCustodyCursor; $("inventory-custody-more").disabled = state.inventoryCustodyLoading;
  }

  function renderInventoryVerifications() {
    $("inventory-verifications").replaceChildren();
    state.inventoryVerificationItems.forEach((entry) => {
      const item = node("li");
      item.append(node("h4", "", dateLabel(entry.verified_on) + " · " + (inventoryConditions[entry.condition] || entry.condition)), node("p", "inventory-history-meta", t("Registrata da utente #") + entry.actor_id + (entry.created_at ? " · " + inventoryTime(entry.created_at) : "")));
      if (entry.expected_location_name || entry.observed_location_name) item.append(node("p", "inventory-history-summary", t("Ubicazione prevista: ") + (entry.expected_location_name || "—") + t(" · osservata: ") + (entry.observed_location_name || "—")));
      if (entry.location_matches === false) item.append(node("p", "inventory-verification-mismatch", t("Ubicazione diversa da quella prevista: verifica se aggiornare la collocazione.")));
      if (entry.notes) item.append(node("p", "inventory-history-summary", entry.notes)); $("inventory-verifications").append(item);
    });
    $("inventory-verifications-empty").hidden = state.inventoryVerificationsLoading || Boolean(state.inventoryVerificationItems.length);
    $("inventory-verifications-more").hidden = !state.inventoryVerificationsCursor; $("inventory-verifications-more").disabled = state.inventoryVerificationsLoading;
  }

  async function loadInventoryOperationalHistory(kind, more = false) {
    const id = state.inventoryDetailId; if (!id || !inventoryTangible(state.inventoryDetailAsset)) return;
    const custody = kind === "custody", prefix = custody ? "inventoryCustody" : "inventoryVerifications", requestKey = prefix + "Request", loadingKey = prefix + "Loading", cursorKey = prefix + "Cursor";
    if (more && (state[loadingKey] || !state[cursorKey])) return;
    const request = ++state[requestKey], epoch = state.inventoryEpoch;
    const current = () => request === state[requestKey] && epoch === state.inventoryEpoch && String(id) === String(state.inventoryDetailId) && $("inventory-detail-dialog").open;
    const dom = custody ? "inventory-custody" : "inventory-verifications", itemsKey = custody ? "inventoryCustodyItems" : "inventoryVerificationItems";
    const query = new URLSearchParams({ limit: "20" }); if (more) query.set("after_id", state[cursorKey]); else { state[itemsKey] = []; state[cursorKey] = null; }
    state[loadingKey] = true; state[prefix + "RetryMore"] = more; $(dom + "-loading").hidden = false; $(dom + "-error").hidden = true; $(dom + "-retry").hidden = true;
    try {
      const data = await api("/api/inventory/assets/" + encodeURIComponent(id) + "/" + kind + "?" + query); if (!current()) return;
      const items = data[custody ? "custody" : "verifications"];
      if (!Array.isArray(items)) throw new Error(t("Il server ha restituito uno storico non valido."));
      const known = new Set(state[itemsKey].map((entry) => String(entry.id))); state[itemsKey].push(...items.filter((entry) => !known.has(String(entry.id)))); state[cursorKey] = data.next_after_id || null; state[prefix + "RetryMore"] = false;
    } catch (error) { if (current()) { dialogError(dom + "-error", new Error(inventoryMessage(error))); $(dom + "-retry").hidden = false; } }
    finally { if (current()) { state[loadingKey] = false; $(dom + "-loading").hidden = true; custody ? renderInventoryCustody() : renderInventoryVerifications(); } }
  }
  function loadInventoryCustody(more = false) { return loadInventoryOperationalHistory("custody", more); }
  function loadInventoryVerifications(more = false) { return loadInventoryOperationalHistory("verifications", more); }

  function inventoryOperationControls() {
    const busy = state.inventoryOperationBusy;
    $("inventory-operation-fields").disabled = busy || state.inventoryOperationLoading;
    const source = state.inventoryOperationAsset, operation = state.inventoryOperation;
    const unavailable = operation === "checkout" ? !inventoryCanCheckout(source) : operation === "checkin" ? !source?.custody : !inventoryTangible(source);
    $("inventory-operation-save").disabled = busy || state.inventoryOperationLoading || state.inventoryOperationConflict || unavailable || !isInventoryManager() || operation === "checkout" && $("inventory-operation-recipient-kind").value === "user" && !state.inventoryUsersReady || operation === "verify" && !state.inventoryPlacesReady;
    $("inventory-operation-save").textContent = busy ? t("Salvataggio…") : state.inventoryOperationLoading ? t("Caricamento…") : ({ checkout: t("Registra consegna"), checkin: t("Registra restituzione"), verify: t("Registra verifica") }[state.inventoryOperation] || t("Salva"));
    $("inventory-operation-reload").disabled = busy || state.inventoryOperationLoading;
    const checkout = operation === "checkout", person = $("inventory-operation-recipient-kind").value === "person", fixed = checkout && source?.status === "assigned";
    $("inventory-operation-recipient-field").hidden = !checkout; $("inventory-operation-recipient-kind").disabled = !checkout || fixed;
    $("inventory-operation-person-field").hidden = !checkout || !person; $("inventory-operation-person-pick").disabled = busy || fixed;
    $("inventory-operation-person-label").textContent = inventoryRecipientSelections.checkout ? inventorySubjectLabel("person", inventoryRecipientSelections.checkout) : t("Seleziona una persona.");
    $("inventory-operation-user-field").hidden = !checkout || person; $("inventory-operation-user").disabled = !checkout || person || fixed;
    $("inventory-operation-user").required = checkout && !person;
    if (busy) { busyDialogs.add("inventory-operation-dialog"); busyDialogs.add("inventory-detail-dialog"); }
    else { busyDialogs.delete("inventory-operation-dialog"); busyDialogs.delete("inventory-detail-dialog"); }
  }

  async function openInventoryOperation(operation) {
    const asset = state.inventoryDetailAsset;
    if (!asset || !inventoryTangible(asset) || !isInventoryManager() || state.inventoryOperationBusy || state.inventoryRelationBusy || busyDialogs.has("inventory-detail-dialog") || operation === "checkout" && !inventoryCanCheckout(asset) || operation === "checkin" && !asset.custody) return;
    state.inventoryOperation = operation; state.inventoryOperationAsset = asset; state.inventoryOperationConflict = false; state.inventoryOperationLoading = true;
    const request = ++state.inventoryOperationRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryOperationRequest && epoch === state.inventoryEpoch && $("inventory-operation-dialog").open && isInventoryManager();
    $("inventory-operation-form").reset(); $("inventory-operation-error").hidden = true; $("inventory-operation-reload").hidden = true; $("inventory-operation-users-retry").hidden = true;
    $("inventory-operation-title").textContent = { checkout: t("Consegna del bene"), checkin: t("Restituzione del bene"), verify: t("Verifica fisica") }[operation];
    $("inventory-operation-description").textContent = inventoryAssetLabel(asset) + (operation === "checkin" ? t(" · in consegna a ") + inventoryCustodyName(asset.custody) : operation === "checkout" && asset.status === "assigned" ? t(" · destinatario già assegnato: ") + inventoryAssigneeName(asset.context) + t(". La consegna mantiene questo destinatario.") : "");
    $("inventory-operation-date-label").textContent = { checkout: t("Data di consegna *"), checkin: t("Data di restituzione *"), verify: t("Data della verifica *") }[operation];
    $("inventory-operation-date").value = localDate(new Date()); $("inventory-operation-date").max = localDate(new Date()); $("inventory-operation-date").min = operation === "checkin" ? asset.custody.checkout_on : "0001-01-01";
    $("inventory-operation-due").min = $("inventory-operation-date").value;
    $("inventory-operation-recipient-kind").value = operation === "checkout" && asset.status === "assigned" && asset.context?.assignee_user_id ? "user" : "person";
    inventoryRecipientSelections.checkout = operation === "checkout" && asset.status === "assigned" && asset.context?.assignee_person_id ? { id: asset.context.assignee_person_id, display_name: asset.context.assignee_person_name, person_code: "" } : null;
    $("inventory-operation-user-field").hidden = operation !== "checkout"; $("inventory-operation-user").disabled = operation !== "checkout"; $("inventory-operation-user").required = operation === "checkout";
    $("inventory-operation-due-field").hidden = operation !== "checkout"; $("inventory-operation-due").disabled = operation !== "checkout";
    $("inventory-operation-observed-field").hidden = operation !== "verify"; $("inventory-operation-observed").disabled = operation !== "verify";
    $("inventory-operation-status-field").hidden = operation !== "checkin"; $("inventory-operation-status").disabled = operation !== "checkin";
    inventoryFillUsers($("inventory-operation-user"), operation === "checkout" && asset.status === "assigned" ? asset.context?.assignee_user_id : "", asset.context?.assignee_user_name || "", false);
    $("inventory-operation-dialog").showModal(); inventoryOperationControls();
    try {
      if (operation === "checkout") await loadInventoryUsers(true); else if (operation === "verify") await loadInventoryPlaces(true);
      if (!current()) return;
      if (operation === "checkout") inventoryFillUsers($("inventory-operation-user"), asset.status === "assigned" ? asset.context?.assignee_user_id : "", asset.context?.assignee_user_name || "", false);
      if (operation === "verify") {
        const select = $("inventory-operation-observed"); select.replaceChildren(new Option(t("Non indicata"), ""));
        state.inventoryPlaces.filter((place) => place.kind === "location" && place.active).sort((a, b) => a.name.localeCompare(b.name, "it")).forEach((place) => select.add(new Option((inventoryPlace(place.parent_id)?.name || t("Sede")) + " · " + place.name, String(place.id))));
        select.value = asset.context?.location_id ? String(asset.context.location_id) : "";
      }
    } catch (error) { if (current()) { dialogError("inventory-operation-error", new Error(inventoryMessage(error))); $("inventory-operation-users-retry").hidden = operation === "checkin"; } }
    finally { if (current()) { state.inventoryOperationLoading = false; inventoryOperationControls(); } }
  }

  async function retryInventoryOperationData() {
    if (state.inventoryOperationBusy || state.inventoryOperationLoading || !isInventoryManager()) return;
    const operation = state.inventoryOperation, request = ++state.inventoryOperationRequest, epoch = state.inventoryEpoch;
    const selectedUser = $("inventory-operation-user").value, observed = $("inventory-operation-observed").value;
    const current = () => request === state.inventoryOperationRequest && epoch === state.inventoryEpoch && $("inventory-operation-dialog").open && isInventoryManager();
    state.inventoryOperationLoading = true; inventoryOperationControls();
    try {
      if (operation === "checkout") await loadInventoryUsers(true); else if (operation === "verify") await loadInventoryPlaces(true);
      if (!current()) return;
      if (operation === "checkout") inventoryFillUsers($("inventory-operation-user"), selectedUser, state.inventoryOperationAsset?.context?.assignee_user_name || "", false);
      if (operation === "verify") {
        const select = $("inventory-operation-observed"); select.replaceChildren(new Option(t("Non indicata"), ""));
        state.inventoryPlaces.filter((place) => place.kind === "location" && place.active).sort((a, b) => a.name.localeCompare(b.name, "it")).forEach((place) => select.add(new Option((inventoryPlace(place.parent_id)?.name || t("Sede")) + " · " + place.name, String(place.id))));
        select.value = observed;
      }
      $("inventory-operation-error").hidden = true; $("inventory-operation-users-retry").hidden = true;
    } catch (error) { if (current()) dialogError("inventory-operation-error", new Error(inventoryMessage(error))); }
    finally { if (current()) { state.inventoryOperationLoading = false; inventoryOperationControls(); } }
  }

  async function reloadInventoryOperation() {
    const source = state.inventoryOperationAsset;
    if (!source || state.inventoryOperationBusy || state.inventoryOperationLoading || !isInventoryManager()) return;
    const request = ++state.inventoryOperationRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryOperationRequest && epoch === state.inventoryEpoch && $("inventory-operation-dialog").open && isInventoryManager();
    state.inventoryOperationBusy = true; inventoryOperationControls();
    try {
      const data = await api("/api/inventory/assets/" + encodeURIComponent(source.id)); if (!current()) return;
      state.inventoryOperationAsset = inventoryRemember(data.asset); state.inventoryOperationConflict = false; inventoryUpdateDetail(data.asset);
      $("inventory-operation-error").hidden = true; $("inventory-operation-reload").hidden = true;
      if (state.inventoryOperation === "checkin") $("inventory-operation-date").min = data.asset.custody?.checkout_on || "0001-01-01";
      const operation = state.inventoryOperation;
      if (operation === "checkout" && data.asset.status === "assigned") {
        const context = data.asset.context || {};
        $("inventory-operation-recipient-kind").value = context.assignee_person_id ? "person" : "user";
        inventoryRecipientSelections.checkout = context.assignee_person_id ? { id: context.assignee_person_id, display_name: context.assignee_person_name, person_code: "" } : null;
        inventoryFillUsers($("inventory-operation-user"), context.assignee_user_id || "", context.assignee_user_name || "", false);
      }
      if (operation === "checkin" && !data.asset.custody || operation === "checkout" && !inventoryCanCheckout(data.asset) || !inventoryTangible(data.asset)) dialogError("inventory-operation-error", new Error(t("La scheda aggiornata non consente più questa operazione. I dati inseriti restano visibili; chiudi la finestra per consultare lo stato corrente.")));
      else showNotice(t("Scheda aggiornata. L’operazione non è stata registrata: controlla i dati e riprova."));
    } catch (error) { if (current()) dialogError("inventory-operation-error", new Error(inventoryMessage(error))); }
    finally { if (current()) { state.inventoryOperationBusy = false; inventoryOperationControls(); } }
  }

  async function saveInventoryOperation(event) {
    event.preventDefault(); const source = state.inventoryOperationAsset, operation = state.inventoryOperation;
    if (!source || state.inventoryOperationBusy || state.inventoryOperationLoading || state.inventoryOperationConflict || !isInventoryManager() || operation === "checkout" && ($("inventory-operation-recipient-kind").value === "user" && !state.inventoryUsersReady || !inventoryCanCheckout(source)) || operation === "verify" && !state.inventoryPlacesReady || operation === "checkin" && !source.custody) return;
    const date = $("inventory-operation-date"), due = $("inventory-operation-due"), notes = $("inventory-operation-notes");
    $("inventory-operation-user").setCustomValidity("");
    if (operation === "checkout") {
      const person = $("inventory-operation-recipient-kind").value === "person", selected = person ? inventoryRecipientSelections.checkout?.id : $("inventory-operation-user").value;
      const assigned = person ? source.context?.assignee_person_id : source.context?.assignee_user_id;
      if (!selected || source.status === "assigned" && String(assigned || "") !== String(selected)) { dialogError("inventory-operation-error", new Error(!selected ? t("Seleziona un destinatario.") : t("Seleziona il destinatario già assegnato nella scheda aggiornata."))); return; }
    }
    date.setCustomValidity(!validDate(date.value) || date.value > localDate(new Date()) ? t("Inserisci una data valida, non futura.") : operation === "checkin" && source.custody && date.value < source.custody.checkout_on ? t("La restituzione non può precedere la consegna.") : "");
    due.setCustomValidity(operation === "checkout" && due.value && (!validDate(due.value) || due.value < date.value) ? t("La restituzione prevista non può precedere la consegna.") : "");
    notes.setCustomValidity(new TextEncoder().encode(notes.value).length > 2000 ? t("Accorcia le note.") : "");
    if (!$("inventory-operation-form").reportValidity()) return;
    const fields = { version: String(source.version), notes: notes.value };
    if (operation === "checkout") Object.assign(fields, { [$("inventory-operation-recipient-kind").value === "person" ? "person_id" : "user_id"]: $("inventory-operation-recipient-kind").value === "person" ? inventoryRecipientSelections.checkout.id : $("inventory-operation-user").value, checkout_on: date.value, due_on: due.value, condition_out: $("inventory-operation-condition").value });
    if (operation === "checkin") Object.assign(fields, { returned_on: date.value, condition_in: $("inventory-operation-condition").value, status: $("inventory-operation-status").value });
    if (operation === "verify") Object.assign(fields, { verified_on: date.value, condition: $("inventory-operation-condition").value, observed_location_id: $("inventory-operation-observed").value });
    const request = ++state.inventoryOperationRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryOperationRequest && epoch === state.inventoryEpoch && $("inventory-operation-dialog").open && isInventoryManager();
    state.inventoryOperationBusy = true; $("inventory-operation-error").hidden = true; inventoryOperationControls();
    try {
      const data = await post("/api/inventory/assets/" + encodeURIComponent(source.id) + "/" + operation, fields); if (!current()) return;
      inventoryUpdateDetail(data.asset); state.inventoryOperationBusy = false; inventoryOperationControls(); $("inventory-operation-dialog").close(); refreshInventory();
      showNotice({ checkout: t("Consegna registrata nello storico del bene."), checkin: t("Restituzione registrata e consegna chiusa."), verify: t("Verifica fisica registrata nello storico del bene.") }[operation]);
    } catch (error) {
      if (!current()) return;
      if (error.status === 409) { state.inventoryOperationConflict = true; $("inventory-operation-reload").hidden = false; }
      dialogError("inventory-operation-error", new Error(inventoryMessage(error) + (error.status === 409 ? t(" I dati inseriti restano disponibili. Ricarica la scheda prima di riprovare.") : "")));
    } finally { if (current()) { state.inventoryOperationBusy = false; inventoryOperationControls(); } }
  }

  $("inventory-directory-open").addEventListener("click", openInventoryDirectory);
  $("inventory-directory-retry").addEventListener("click", refreshInventoryDirectory);
  $("inventory-directory-kind").addEventListener("change", () => editInventoryPlace());
  $("inventory-directory-cancel-edit").addEventListener("click", () => editInventoryPlace());
  $("inventory-directory-form").addEventListener("submit", saveInventoryPlace);
  $("inventory-directory-dialog").addEventListener("close", () => { state.inventoryDirectoryRequest++; state.inventoryDirectoryEditing = null; });
  $("inventory-context-open").addEventListener("click", openInventoryContext);
  $("inventory-context-form").addEventListener("submit", saveInventoryContext);
  $("inventory-context-recipient-kind").addEventListener("change", inventoryContextControls);
  $("inventory-context-person-pick").addEventListener("click", () => openInventoryRecipientPicker("context"));
  $("inventory-context-person-clear").addEventListener("click", () => { if (!state.inventoryContextBusy && !state.inventoryContextAsset?.custody) { inventoryRecipientSelections.context = null; inventoryContextControls(); } });
  $("inventory-operation-recipient-kind").addEventListener("change", inventoryOperationControls);
  $("inventory-operation-person-pick").addEventListener("click", () => openInventoryRecipientPicker("checkout"));
  $("inventory-relation-subject-pick").addEventListener("click", () => openInventoryRecipientPicker("relation"));
  $("inventory-context-reload").addEventListener("click", reloadInventoryContext);
  $("inventory-site-id").addEventListener("change", () => {
    const site = $("inventory-site-id").value, department = inventoryPlace($("inventory-department-id").value), location = inventoryPlace($("inventory-location-id").value);
    if (department?.parent_id && String(department.parent_id) !== site) $("inventory-department-id").value = "";
    if (location && String(location.parent_id) !== site) $("inventory-location-id").value = "";
    renderInventoryContextPlaces();
  });
  $("inventory-placement-retry").addEventListener("click", async () => {
    const epoch = state.inventoryEpoch, request = state.inventoryContextRequest;
    try { await Promise.all([loadInventoryPlaces(true), loadInventoryUsers(true)]); if (epoch !== state.inventoryEpoch || request !== state.inventoryContextRequest || !$("inventory-context-dialog").open) return; renderInventoryContextPlaces(); inventoryFillUsers($("inventory-context-user"), $("inventory-context-user").value, state.inventoryContextAsset?.context?.assignee_user_name); $("inventory-placement-error").hidden = true; $("inventory-placement-retry").hidden = true; inventoryContextControls(); }
    catch (error) { if (epoch === state.inventoryEpoch && request === state.inventoryContextRequest) dialogError("inventory-placement-error", new Error(inventoryMessage(error))); }
  });
  $("inventory-context-dialog").addEventListener("close", () => { state.inventoryContextRequest++; state.inventoryContextAsset = null; state.inventoryContextLoading = false; state.inventoryContextConflict = false; });
  $("inventory-checkout-open").addEventListener("click", () => openInventoryOperation("checkout"));
  $("inventory-checkin-open").addEventListener("click", () => openInventoryOperation("checkin"));
  $("inventory-verify-open").addEventListener("click", () => openInventoryOperation("verify"));
  $("inventory-operation-form").addEventListener("submit", saveInventoryOperation);
  $("inventory-operation-reload").addEventListener("click", reloadInventoryOperation);
  $("inventory-operation-users-retry").addEventListener("click", retryInventoryOperationData);
  $("inventory-operation-user").addEventListener("change", () => { $("inventory-operation-user").setCustomValidity(""); inventoryOperationControls(); });
  $("inventory-operation-date").addEventListener("input", () => { $("inventory-operation-date").setCustomValidity(""); $("inventory-operation-due").setCustomValidity(""); $("inventory-operation-due").min = $("inventory-operation-date").value || "0001-01-01"; });
  $("inventory-operation-due").addEventListener("input", () => $("inventory-operation-due").setCustomValidity(""));
  $("inventory-operation-notes").addEventListener("input", () => $("inventory-operation-notes").setCustomValidity(""));
  $("inventory-operation-dialog").addEventListener("close", () => { state.inventoryOperationRequest++; state.inventoryOperationAsset = null; state.inventoryOperation = null; state.inventoryOperationLoading = false; state.inventoryOperationConflict = false; });
  $("inventory-custody-retry").addEventListener("click", () => loadInventoryCustody(state.inventoryCustodyRetryMore)); $("inventory-custody-more").addEventListener("click", () => loadInventoryCustody(true));
  $("inventory-verifications-retry").addEventListener("click", () => loadInventoryVerifications(state.inventoryVerificationsRetryMore)); $("inventory-verifications-more").addEventListener("click", () => loadInventoryVerifications(true));

  function inventoryMessage(error) {
    return error.status === 503 ? t("Il servizio Asset inventory non è disponibile. Chiedi a un gestore di verificarne la configurazione e l’avvio, poi riprova.") : error.message;
  }

  // People are inventory records, independent of application login accounts.
  // Identity membership and direct asset links are separate many-to-many relations.
  const inventoryIdentityKinds = { personal: t("Personale"), shared: t("Condivisa"), service: t("Di servizio") };
  const inventorySubjectRoles = { owner: t("Responsabile"), user: t("Utilizzatore"), administrator: t("Amministratore") };
  const inventorySubjectFields = {
    person: [["person_code", t("Codice persona"), 64, true], ["display_name", t("Nome della persona"), 160, true], ["email", t("Email"), 254], ["organization", t("Organizzazione"), 160], ["job_title", t("Mansione"), 120], ["notes", t("Note"), 2000]],
    identity: [["realm", t("Sistema / dominio"), 160, true], ["login", t("Nome account"), 160, true], ["display_name", t("Nome visualizzato"), 160], ["kind", t("Tipo di identità"), inventoryIdentityKinds], ["notes", t("Note"), 2000]]
  };
  const inventorySubjectDirectory = { kind: "person", request: 0, items: [], cursor: null, loading: false, retryMore: false };
  const inventorySubjectDetail = { kind: "person", id: null, item: null, request: 0, loading: false, busy: false, conflict: false, panels: {} };
  const inventorySubjectPicker = { request: 0, owner: null, items: [], cursor: null, loading: false, selected: null, busy: false, conflict: false, retryMore: false };
  const inventoryAssetSubjects = { request: 0, items: [], cursor: null, loading: false, busy: false, retryMore: false };
  let inventorySubjectFilter = null;

  function inventorySubjectPath(kind) { return kind === "person" ? "people" : "identities"; }
  function inventorySubjectLabel(kind, item) { return kind === "person" ? [item.display_name, item.person_code].filter(Boolean).join(" · ") : (item.display_name ? item.display_name + " · " : "") + item.realm + " / " + item.login; }
  function inventorySubjectRecord(kind, response) { return response[kind]; }
  function inventorySubjectPost(path, fields) { return api(path, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: new URLSearchParams(fields) }); }
  function inventorySubjectInputValue(key) { const input = $("inventory-subject-input-" + key); return input.type === "checkbox" ? input.checked ? "1" : "0" : input.value; }
  function inventorySubjectReset() {
    Object.keys(inventoryRecipientSelections).forEach((key) => inventoryRecipientSelections[key] = null);
    inventorySubjectDirectory.request++; inventorySubjectDetail.request++; inventorySubjectPicker.request++; inventoryAssetSubjects.request++;
    inventorySubjectDetail.panels = {};
    Object.assign(inventorySubjectDirectory, { items: [], cursor: null, loading: false, retryMore: false });
    Object.assign(inventorySubjectDetail, { item: null, id: null, loading: false, busy: false, conflict: false });
    Object.assign(inventorySubjectPicker, { owner: null, items: [], cursor: null, loading: false, selected: null, busy: false, conflict: false });
    Object.assign(inventoryAssetSubjects, { items: [], cursor: null, loading: false, busy: false });
    inventorySubjectFilter = null;
    ["inventory-subject-directory-dialog", "inventory-subject-detail-dialog", "inventory-subject-picker-dialog"].forEach((id) => { busyDialogs.delete(id); if ($(id).open) $(id).close(); });
    ["inventory-subject-directory-list", "inventory-subject-detail-fields", "inventory-subject-picker-list", "inventory-subjects-list", "inventory-subject-links-list", "inventory-subject-assets-list", "inventory-subject-history-list"].forEach((id) => $(id).replaceChildren());
    $("inventory-subject-picker-form").reset(); $("inventory-subject-directory-search").reset();
    $("inventory-subject-filter").hidden = true;
  }
  function inventorySubjectFilterControls() {
    $("inventory-subject-filter").hidden = !inventorySubjectFilter;
    $("inventory-subject-filter").querySelector("span").textContent = inventorySubjectFilter ? (inventorySubjectFilter.kind === "person" ? t("Beni collegati direttamente alla persona: {0}", [inventorySubjectFilter.label]) : t("Beni collegati direttamente all’identità: {0}", [inventorySubjectFilter.label])) : "";
    $("inventory-people-open").disabled = $("inventory-identities-open").disabled = state.inventoryConfigured !== true;
  }
  function inventorySubjectDirectoryControls() {
    const directory = inventorySubjectDirectory;
    $("inventory-subject-directory-title").textContent = directory.kind === "person" ? t("Persone") : t("Identità");
    $("inventory-subject-directory-kind-field").hidden = directory.kind !== "identity";
    $("inventory-subject-directory-new").hidden = !isInventoryManager();
    $("inventory-subject-directory-new").textContent = directory.kind === "person" ? t("Nuova persona") : t("Nuova identità");
    $("inventory-subject-directory-loading").hidden = !directory.loading;
    $("inventory-subject-directory-more").hidden = !directory.cursor;
    $("inventory-subject-directory-more").disabled = directory.loading;
  }
  function openInventorySubjectDirectory(kind) {
    if (state.inventoryConfigured !== true || !state.session?.authenticated) return;
    inventorySubjectDirectory.kind = kind; inventorySubjectDirectory.request++;
    $("inventory-subject-directory-search").reset();
    if (!$("inventory-subject-directory-dialog").open) $("inventory-subject-directory-dialog").showModal();
    loadInventorySubjectDirectory();
  }
  async function loadInventorySubjectDirectory(more = false) {
    const directory = inventorySubjectDirectory;
    if (more && (directory.loading || !directory.cursor)) return;
    const request = ++directory.request, epoch = state.inventoryEpoch, kind = directory.kind;
    const current = () => request === directory.request && epoch === state.inventoryEpoch && $("inventory-subject-directory-dialog").open;
    const query = new URLSearchParams({ limit: "30", q: $("inventory-subject-directory-q").value.trim() });
    if (new TextEncoder().encode(query.get("q")).length > 200) { dialogError("inventory-subject-directory-error", new Error(t("La ricerca è troppo lunga. Accorciala e riprova."))); return; }
    if ($("inventory-subject-directory-active").value) query.set("active", $("inventory-subject-directory-active").value);
    if (kind === "identity" && $("inventory-subject-directory-kind").value) query.set("kind", $("inventory-subject-directory-kind").value);
    if (more) query.set("after_id", directory.cursor); else { directory.items = []; directory.cursor = null; $("inventory-subject-directory-list").replaceChildren(); }
    directory.loading = true; directory.retryMore = more; $("inventory-subject-directory-error").hidden = $("inventory-subject-directory-retry").hidden = true; inventorySubjectDirectoryControls();
    try {
      const data = await api("/api/inventory/" + inventorySubjectPath(kind) + "?" + query);
      if (!current()) return;
      directory.items.push(...data.items); directory.cursor = data.next_after_id || null; directory.retryMore = false;
      $("inventory-subject-directory-list").replaceChildren();
      for (const item of directory.items) {
        const row = node("li", "inventory-directory-item"), text = node("div");
        text.append(node("strong", "", inventorySubjectLabel(kind, item)), node("span", "inventory-cell-secondary", (item.active ? t("Attivo") : t("Disattivato")) + " · " + (kind === "person" ? (item.identity_count === 1 ? t("1 identità collegata") : t("{0} identità collegate", [item.identity_count || 0])) : (inventoryIdentityKinds[item.kind] || item.kind) + " · " + (item.people_count === 1 ? t("1 persona collegata") : t("{0} persone collegate", [item.people_count || 0])))));
        row.append(text, action(t("Scheda"), inventorySubjectLabel(kind, item), () => openInventorySubjectDetail(kind, item.id)));
        $("inventory-subject-directory-list").append(row);
      }
      if (!directory.items.length) $("inventory-subject-directory-list").append(node("li", "field-help", t("Nessuna voce per questi filtri.")));
    } catch (error) { if (current()) { dialogError("inventory-subject-directory-error", new Error(inventoryMessage(error))); $("inventory-subject-directory-retry").hidden = false; } }
    finally { if (current()) { directory.loading = false; inventorySubjectDirectoryControls(); } }
  }
  function inventorySubjectBuildFields(kind, item = null) {
    const container = $("inventory-subject-detail-fields"); container.replaceChildren();
    for (const [key, title, max, required] of inventorySubjectFields[kind]) {
      const field = node("div", "work-field"), label = node("label", "", title + (required ? " *" : ""));
      label.htmlFor = "inventory-subject-input-" + key;
      const input = node(typeof max === "object" ? "select" : key === "notes" ? "textarea" : "input"); input.id = label.htmlFor; input.name = key;
      if (typeof max === "object") Object.entries(max).forEach(([value, title]) => input.append(new Option(title, value)));
      else { input.maxLength = max; if (key === "notes") input.rows = 4; else { input.type = key === "email" ? "email" : "text"; input.autocomplete = "off"; } }
      input.required = Boolean(required); input.value = item?.[key] ?? (key === "kind" ? "personal" : "");
      input.addEventListener("input", () => input.setCustomValidity("")); field.append(label, input); container.append(field);
    }
    const active = node("label", "inventory-mine-control"), input = node("input"); input.id = "inventory-subject-input-active"; input.type = "checkbox"; input.checked = item?.active ?? true;
    active.append(input, node("span", "", t("Attivo"))); container.append(active);
  }
  function inventorySubjectDetailControls() {
    const detail = inventorySubjectDetail, manager = isInventoryManager();
    $("inventory-subject-detail-loading").hidden = !detail.loading;
    $("inventory-subject-detail-fields").disabled = detail.loading || detail.busy || Boolean(detail.id && !detail.item) || !manager;
    $("inventory-subject-detail-save").hidden = !manager;
    $("inventory-subject-detail-save").disabled = detail.loading || detail.busy || detail.conflict || Boolean(detail.id && !detail.item);
    $("inventory-subject-detail-save").textContent = detail.busy ? t("Salvataggio…") : detail.id ? t("Salva modifiche") : detail.kind === "person" ? t("Crea persona") : t("Crea identità");
    $("inventory-subject-view-assets").hidden = !detail.item;
    $("inventory-subject-view-assets").disabled = detail.loading || detail.busy;
    $("inventory-subject-detail-reload").disabled = detail.loading || detail.busy;
    $("inventory-subject-detail-panels").hidden = !detail.item;
    $("inventory-subject-person-add").hidden = detail.kind !== "identity" || !manager || !detail.item?.active;
    $("inventory-subject-person-add").disabled = detail.loading || detail.busy || detail.conflict;
    $("inventory-subject-detail-description").textContent = detail.item ? (detail.kind === "person" ? t("Persona censita nell’inventario, indipendente dagli account di accesso all’app.") : t("Utenza censita in un sistema. Può appartenere a più persone; non memorizzare password o segreti.")) + " " + t("Versione {0} · aggiornato {1}", [detail.item.version, inventoryTime(detail.item.updated_at)]) : detail.kind === "person" ? t("Censisci una persona anche se non possiede un account nell’app.") : t("Censisci l’utenza e il sistema in cui esiste. Il tipo descrive l’uso; i collegamenti alle persone restano espliciti.");
    if (detail.busy) busyDialogs.add("inventory-subject-detail-dialog"); else busyDialogs.delete("inventory-subject-detail-dialog");
  }
  function openInventorySubjectDetail(kind, id = null) {
    if (!state.session?.authenticated || inventorySubjectDetail.busy) return;
    const detail = inventorySubjectDetail;
    detail.request++; Object.assign(detail, { kind, id, item: null, loading: false, busy: false, conflict: false, panels: {} });
    $("inventory-subject-detail-title").textContent = id ? kind === "person" ? t("Scheda persona") : t("Scheda identità") : kind === "person" ? t("Nuova persona") : t("Nuova identità");
    $("inventory-subject-detail-kicker").textContent = kind === "person" ? t("PERSONA") : t("IDENTITÀ");
    $("inventory-subject-detail-error").hidden = $("inventory-subject-detail-reload").hidden = true;
    inventorySubjectBuildFields(kind); inventorySubjectDetailControls();
    if (!$("inventory-subject-detail-dialog").open) $("inventory-subject-detail-dialog").showModal();
    if (id) loadInventorySubjectDetail();
  }
  async function loadInventorySubjectDetail(keepDraft = false) {
    const detail = inventorySubjectDetail;
    if (!detail.id || detail.busy) return;
    const preserveDraft = keepDraft && Boolean(detail.item);
    const request = ++detail.request, epoch = state.inventoryEpoch, kind = detail.kind, id = detail.id;
    const current = () => request === detail.request && epoch === state.inventoryEpoch && $("inventory-subject-detail-dialog").open;
    detail.loading = true; $("inventory-subject-detail-error").hidden = true; inventorySubjectDetailControls();
    try {
      const data = await api("/api/inventory/" + inventorySubjectPath(kind) + "/" + id);
      if (!current()) return;
      detail.item = inventorySubjectRecord(kind, data); detail.conflict = false;
      if (!preserveDraft) inventorySubjectBuildFields(kind, detail.item);
      $("inventory-subject-detail-title").textContent = inventorySubjectLabel(kind, detail.item);
      $("inventory-subject-detail-reload").hidden = true;
      $("inventory-subject-links-title").textContent = kind === "person" ? t("Identità collegate") : t("Persone collegate");
      $("inventory-subject-link-policy").textContent = kind === "person" ? t("Apri un’identità per gestire le persone che la usano. La persona può avere più identità.") : t("Ogni persona collegata usa questa identità con il ruolo indicato. Anche un’identità personale può avere più persone collegate.");
      ["links", "assets", "history"].forEach((panel) => loadInventorySubjectPanel(panel));
    } catch (error) { if (current()) { dialogError("inventory-subject-detail-error", new Error(inventoryMessage(error))); $("inventory-subject-detail-reload").hidden = false; } }
    finally { if (current()) { detail.loading = false; inventorySubjectDetailControls(); } }
  }
  async function saveInventorySubject(event) {
    event.preventDefault(); const detail = inventorySubjectDetail;
    if (!isInventoryManager() || detail.busy || detail.loading || detail.conflict || detail.id && !detail.item) return;
    const fields = {};
    for (const [key, , max] of inventorySubjectFields[detail.kind]) {
      const input = $("inventory-subject-input-" + key), value = input.value;
      if (typeof max === "number" && new TextEncoder().encode(value).length > max) { input.setCustomValidity(t("Il testo è troppo lungo in byte. Accorcialo e riprova.")); input.reportValidity(); return; }
      fields[key] = value;
    }
    if (!$("inventory-subject-detail-form").reportValidity()) return;
    fields.active = inventorySubjectInputValue("active"); if (detail.item) fields.version = detail.item.version;
    const epoch = state.inventoryEpoch, request = detail.request, kind = detail.kind;
    const current = () => epoch === state.inventoryEpoch && request === detail.request && $("inventory-subject-detail-dialog").open;
    detail.busy = true; $("inventory-subject-detail-error").hidden = true; inventorySubjectDetailControls();
    try {
      const data = await inventorySubjectPost("/api/inventory/" + inventorySubjectPath(kind) + (detail.id ? "/" + detail.id : ""), fields);
      if (!current()) return;
      detail.item = inventorySubjectRecord(kind, data); detail.id = detail.item.id; detail.conflict = false;
      $("inventory-subject-detail-title").textContent = inventorySubjectLabel(kind, detail.item);
      $("inventory-subject-links-title").textContent = kind === "person" ? t("Identità collegate") : t("Persone collegate");
      $("inventory-subject-link-policy").textContent = kind === "person" ? t("Apri un’identità per gestire le persone che la usano. La persona può avere più identità.") : t("Ogni persona collegata usa questa identità con il ruolo indicato. Anche un’identità personale può avere più persone collegate.");
      ["links", "assets", "history"].forEach((panel) => loadInventorySubjectPanel(panel));
      if ($("inventory-subject-directory-dialog").open) loadInventorySubjectDirectory();
      showNotice(kind === "person" ? t("Persona salvata.") : t("Identità salvata."));
    } catch (error) { if (current()) { detail.conflict = error.status === 409 && Boolean(detail.id); $("inventory-subject-detail-reload").hidden = !detail.conflict; dialogError("inventory-subject-detail-error", new Error(inventoryMessage(error) + (detail.conflict ? t(" I dati inseriti restano disponibili. Ricarica la versione prima di riprovare.") : ""))); } }
    finally { if (current()) { detail.busy = false; inventorySubjectDetailControls(); } }
  }
  function inventorySubjectHistoryRow(entry) {
    const row = node("li"), change = entry.change?.operation || entry.action;
    const labels = { created: t("Creazione"), updated: t("Aggiornamento"), person_linked: t("Persona collegata"), person_unlinked: t("Collegamento persona rimosso"), identity_linked: t("Identità collegata"), identity_unlinked: t("Collegamento identità rimosso"), identity_person_added: inventorySubjectDetail.kind === "person" ? t("Identità collegata") : t("Persona collegata"), identity_person_removed: inventorySubjectDetail.kind === "person" ? t("Collegamento identità rimosso") : t("Collegamento persona rimosso") };
    row.append(node("h4", "", (labels[change] || t("Aggiornamento")) + t(" · versione ") + entry.version), node("p", "inventory-history-meta", inventoryTime(entry.created_at || entry.changed_at) + t(" · utente #") + entry.actor_id));
    if (entry.change?.link) {
      const link = entry.change.link, kind = inventorySubjectDetail.kind === "person" ? "identity" : "person", subject = link[kind];
      if (subject) row.append(node("p", "inventory-history-summary", inventorySubjectLabel(kind, subject) + " · " + (inventorySubjectRoles[link.role] || link.role) + (link.notes ? " · " + link.notes : "")));
    }
    const details = node("details"), snapshot = entry.snapshot || {}, list = node("dl", "inventory-detail-grid");
    details.append(node("summary", "", t("Visualizza i dati di questa versione")));
    for (const [key, title] of inventorySubjectFields[inventorySubjectDetail.kind]) {
      const item = node("div"); item.append(node("dt", "", title), node("dd", "", key === "kind" ? inventoryIdentityKinds[snapshot[key]] || snapshot[key] || "—" : snapshot[key] || "—")); list.append(item);
    }
    const active = node("div"); active.append(node("dt", "", t("Stato")), node("dd", "", snapshot.active ? t("Attivo") : t("Disattivato"))); list.append(active);
    details.append(list); row.append(details); return row;
  }
  async function loadInventorySubjectPanel(panel, more = false) {
    const detail = inventorySubjectDetail;
    if (!detail.item) return;
    if (!detail.panels[panel]) detail.panels[panel] = { request: 0, cursor: null, loading: false, retryMore: false };
    const page = detail.panels[panel];
    if (more && (page.loading || !page.cursor)) return;
    const request = ++page.request, detailRequest = detail.request, epoch = state.inventoryEpoch, kind = detail.kind, id = detail.id;
    const current = () => detailRequest === detail.request && epoch === state.inventoryEpoch && request === page.request && $("inventory-subject-detail-dialog").open;
    const prefix = "inventory-subject-" + panel, path = panel === "links" ? kind === "person" ? "identities" : "people" : panel;
    const query = new URLSearchParams({ limit: "20" }); if (more) query.set("after_id", page.cursor); else { page.cursor = null; $(prefix + "-list").replaceChildren(); }
    page.loading = true; page.retryMore = more; $(prefix + "-error").hidden = $(prefix + "-retry").hidden = true; $(prefix + "-loading").hidden = false; $(prefix + "-more").disabled = true;
    try {
      const data = await api("/api/inventory/" + inventorySubjectPath(kind) + "/" + id + "/" + path + "?" + query);
      if (!current()) return;
      for (const link of data.items) {
        if (panel === "history") { $(prefix + "-list").append(inventorySubjectHistoryRow(link)); continue; }
        const row = node("li", "inventory-relation-card"), heading = node("div", "inventory-relation-heading"), linkedKind = kind === "person" ? "identity" : "person", subject = link[linkedKind];
        if (panel === "assets") {
          heading.append(node("strong", "", link.asset.name + " · " + link.asset.asset_tag)); row.append(heading, node("p", "inventory-relation-summary", inventorySubjectRoles[link.role] || link.role));
          row.append(action(t("Scheda asset"), link.asset.name, () => {
            if (busyDialogs.has("inventory-subject-detail-dialog") || busyDialogs.has("inventory-detail-dialog")) return;
            $("inventory-subject-detail-dialog").close();
            if ($("inventory-subject-directory-dialog").open) $("inventory-subject-directory-dialog").close();
            // Reuse an already open asset dialog; its detail loader invalidates old requests.
            openInventoryDetail(link.asset.id);
          }));
        } else {
          heading.append(node("strong", "", inventorySubjectLabel(linkedKind, subject))); row.append(heading, node("p", "inventory-relation-summary", (subject.active ? "" : t("Disattivato") + " · ") + (inventorySubjectRoles[link.role] || link.role) + (link.notes ? " · " + link.notes : "")));
          const actions = node("div", "inventory-row-actions"); actions.append(action(t("Scheda"), inventorySubjectLabel(linkedKind, subject), () => openInventorySubjectDetail(linkedKind, subject.id)));
          if (kind === "identity" && isInventoryManager()) { const remove = action(t("Rimuovi collegamento"), t("Rimuovi collegamento"), () => removeInventoryPersonLink(link)); remove.disabled = detail.busy || detail.conflict; actions.append(remove); }
          row.append(actions);
        }
        $(prefix + "-list").append(row);
      }
      if (!$(prefix + "-list").children.length) $(prefix + "-list").append(node("li", "field-help", panel === "history" ? t("Nessuno storico disponibile.") : t("Nessun collegamento registrato.")));
      page.cursor = data.next_after_id || null; page.retryMore = false;
    } catch (error) { if (current()) { dialogError(prefix + "-error", new Error(inventoryMessage(error))); $(prefix + "-retry").hidden = false; } }
    finally { if (current()) { page.loading = false; $(prefix + "-loading").hidden = true; $(prefix + "-more").hidden = !page.cursor; $(prefix + "-more").disabled = false; } }
  }
  async function removeInventoryPersonLink(link) {
    const detail = inventorySubjectDetail;
    if (!isInventoryManager() || detail.kind !== "identity" || !detail.item || detail.busy || detail.conflict) return;
    const request = detail.request, epoch = state.inventoryEpoch;
    const current = () => request === detail.request && epoch === state.inventoryEpoch && $("inventory-subject-detail-dialog").open;
    detail.busy = true; inventorySubjectDetailControls();
    try {
      const data = await inventorySubjectPost("/api/inventory/identities/" + detail.id + "/people/" + link.id + "/remove", { version: detail.item.version });
      if (!current()) return;
      detail.item = data.identity; ["links", "history"].forEach((panel) => loadInventorySubjectPanel(panel));
      if ($("inventory-subject-directory-dialog").open) loadInventorySubjectDirectory(); showNotice(t("Collegamento rimosso."));
    } catch (error) { if (current()) { detail.conflict = error.status === 409; $("inventory-subject-detail-reload").hidden = !detail.conflict; dialogError("inventory-subject-detail-error", new Error(inventoryMessage(error))); } }
    finally { if (current()) { detail.busy = false; inventorySubjectDetailControls(); } }
  }
  function openInventoryRecipientPicker(type) {
    const source = type === "context" ? state.inventoryContextAsset : type === "checkout" ? state.inventoryOperationAsset : state.inventoryDetailAsset;
    const parent = type === "context" ? "inventory-context-dialog" : type === "checkout" ? "inventory-operation-dialog" : "inventory-detail-dialog";
    if (!source || !$(parent).open || busyDialogs.has(parent) || !isInventoryManager() || inventorySubjectPicker.busy) return;
    if (type === "context" && source.custody || type === "checkout" && source.status === "assigned") return;
    const kind = type === "relation" ? $("inventory-relation-recipient-kind").value : "person";
    if (!["person", "identity"].includes(kind)) return;
    const picker = inventorySubjectPicker;
    picker.request++; Object.assign(picker, { owner: { type, id: source.id, parent, selection: true, kind }, items: [], cursor: null, loading: false, selected: inventoryRecipientSelections[type], busy: false, conflict: false });
    $("inventory-subject-picker-form").reset(); $("inventory-subject-picker-kind").value = kind;
    $("inventory-subject-picker-title").textContent = kind === "person" ? t("Scegli persona") : t("Scegli identità");
    $("inventory-subject-picker-kind-field").hidden = true;
    $("inventory-subject-picker-role-field").hidden = $("inventory-subject-picker-notes-field").hidden = true;
    $("inventory-subject-picker-role").disabled = $("inventory-subject-picker-notes").disabled = true;
    $("inventory-subject-picker-error").hidden = $("inventory-subject-picker-reload").hidden = true;
    $("inventory-subject-picker-description").textContent = kind === "identity" ? t("Scegli un’identità censita. Un’identità condivisa consuma solo la quantità di posti indicata.") : t("Scegli una persona censita, anche senza account Punto.");
    inventorySubjectPickerControls(); $("inventory-subject-picker-dialog").showModal(); loadInventorySubjectPicker();
  }
  function inventorySubjectPickerControls() {
    const picker = inventorySubjectPicker;
    $("inventory-subject-picker-fields").disabled = picker.busy;
    $("inventory-subject-picker-loading").hidden = !picker.loading;
    $("inventory-subject-picker-more").hidden = !picker.cursor;
    $("inventory-subject-picker-more").disabled = picker.loading || picker.busy;
    $("inventory-subject-picker-search").disabled = picker.busy;
    $("inventory-subject-picker-save").disabled = picker.busy || picker.conflict || !picker.selected || !isInventoryManager();
    $("inventory-subject-picker-save").textContent = picker.busy ? t("Salvataggio…") : picker.owner?.selection ? t("Conferma selezione") : t("Salva collegamento");
    $("inventory-subject-picker-selected").textContent = picker.selected ? t("Selezionato: {0}", [inventorySubjectLabel($("inventory-subject-picker-kind").value, picker.selected)]) : t("Seleziona una voce dai risultati.");
    $("inventory-subject-picker-reload").disabled = picker.busy;
    if (picker.busy) { busyDialogs.add("inventory-subject-picker-dialog"); busyDialogs.add(picker.owner?.parent || (picker.owner?.type === "asset" ? "inventory-detail-dialog" : "inventory-subject-detail-dialog")); }
    else { busyDialogs.delete("inventory-subject-picker-dialog"); if (picker.owner) busyDialogs.delete(picker.owner.parent || (picker.owner.type === "asset" ? "inventory-detail-dialog" : "inventory-subject-detail-dialog")); }
  }
  function openInventorySubjectPicker(type) {
    const item = type === "asset" ? state.inventoryDetailAsset : inventorySubjectDetail.item;
    if (!isInventoryManager() || !item || inventorySubjectPicker.busy || inventorySubjectDetail.busy || inventoryAssetSubjects.busy || busyDialogs.has(type === "asset" ? "inventory-detail-dialog" : "inventory-subject-detail-dialog")) return;
    const picker = inventorySubjectPicker;
    picker.request++; Object.assign(picker, { owner: { type, id: item.id, version: item.version }, items: [], cursor: null, loading: false, selected: null, busy: false, conflict: false });
    $("inventory-subject-picker-form").reset(); $("inventory-subject-picker-kind").value = "person";
    $("inventory-subject-picker-title").textContent = t("Collega persona o identità");
    $("inventory-subject-picker-role-field").hidden = $("inventory-subject-picker-notes-field").hidden = false;
    $("inventory-subject-picker-role").disabled = $("inventory-subject-picker-notes").disabled = false;
    $("inventory-subject-picker-kind-field").hidden = type !== "asset";
    $("inventory-subject-picker-role").replaceChildren(...(type === "asset" ? ["user", "owner", "administrator"] : ["user", "owner"]).map((role) => new Option(inventorySubjectRoles[role], role)));
    $("inventory-subject-picker-error").hidden = $("inventory-subject-picker-reload").hidden = true;
    $("inventory-subject-picker-description").textContent = type === "asset" ? t("Collega una persona o un’identità direttamente a questo bene. Le persone di un’identità condivisa non ereditano il collegamento.") : t("Seleziona una persona che usa questa identità. Le altre persone collegate restano associate.");
    inventorySubjectPickerControls(); $("inventory-subject-picker-dialog").showModal(); loadInventorySubjectPicker();
  }
  async function loadInventorySubjectPicker(more = false) {
    const picker = inventorySubjectPicker;
    if (!picker.owner || more && (picker.loading || !picker.cursor)) return;
    const request = ++picker.request, epoch = state.inventoryEpoch, kind = $("inventory-subject-picker-kind").value;
    const current = () => request === picker.request && epoch === state.inventoryEpoch && $("inventory-subject-picker-dialog").open;
    const query = new URLSearchParams({ limit: "30", active: "1", q: $("inventory-subject-picker-q").value.trim() });
    if (new TextEncoder().encode(query.get("q")).length > 200) { dialogError("inventory-subject-picker-load-error", new Error(t("La ricerca è troppo lunga. Accorciala e riprova."))); return; }
    if (more) query.set("after_id", picker.cursor); else { picker.items = []; picker.cursor = null; $("inventory-subject-picker-list").replaceChildren(); }
    picker.loading = true; picker.retryMore = more; $("inventory-subject-picker-load-error").hidden = $("inventory-subject-picker-retry").hidden = true; inventorySubjectPickerControls();
    try {
      const data = await api("/api/inventory/" + inventorySubjectPath(kind) + "?" + query);
      if (!current()) return;
      picker.items.push(...data.items); picker.cursor = data.next_after_id || null; picker.retryMore = false; $("inventory-subject-picker-list").replaceChildren();
      for (const item of picker.items) {
        const row = node("li"), button = node("button", "inventory-subject-choice", inventorySubjectLabel(kind, item) + (kind === "identity" ? " · " + (inventoryIdentityKinds[item.kind] || item.kind) + (item.kind === "shared" ? " · " + (Number(item.people_count) === 1 ? t("1 persona collegata") : t("{0} persone collegate", [item.people_count || 0])) : "") : "")); button.type = "button";
        button.setAttribute("aria-pressed", String(picker.selected?.id === item.id));
        button.addEventListener("click", () => { if (picker.busy) return; picker.selected = item; $("inventory-subject-picker-list").querySelectorAll("button").forEach((option) => option.setAttribute("aria-pressed", String(option === button))); inventorySubjectPickerControls(); });
        row.append(button); $("inventory-subject-picker-list").append(row);
      }
      if (!picker.items.length) $("inventory-subject-picker-list").append(node("li", "field-help", t("Nessuna voce attiva per questi filtri.")));
    } catch (error) { if (current()) { dialogError("inventory-subject-picker-load-error", new Error(inventoryMessage(error))); $("inventory-subject-picker-retry").hidden = false; } }
    finally { if (current()) { picker.loading = false; inventorySubjectPickerControls(); } }
  }
  async function reloadInventorySubjectPicker() {
    const picker = inventorySubjectPicker;
    if (!picker.owner || picker.busy) return;
    const epoch = state.inventoryEpoch, request = picker.request, owner = picker.owner;
    const current = () => epoch === state.inventoryEpoch && request === picker.request && $("inventory-subject-picker-dialog").open;
    picker.busy = true; inventorySubjectPickerControls();
    try {
      const data = await api("/api/inventory/" + (owner.type === "asset" ? "assets" : "identities") + "/" + owner.id);
      if (!current()) return;
      const item = owner.type === "asset" ? data.asset : data.identity; owner.version = item.version;
      if (owner.type === "asset" && String(state.inventoryDetailId) === String(owner.id)) inventoryUpdateDetail(item);
      if (owner.type === "identity" && inventorySubjectDetail.kind === "identity" && String(inventorySubjectDetail.id) === String(owner.id)) { inventorySubjectDetail.item = item; inventorySubjectDetailControls(); }
      picker.conflict = false; $("inventory-subject-picker-error").hidden = $("inventory-subject-picker-reload").hidden = true;
    } catch (error) { if (current()) dialogError("inventory-subject-picker-error", new Error(inventoryMessage(error))); }
    finally { if (current()) { picker.busy = false; inventorySubjectPickerControls(); } }
  }
  async function saveInventorySubjectLink(event) {
    event.preventDefault(); const picker = inventorySubjectPicker, owner = picker.owner;
    if (!owner || !picker.selected || picker.busy || picker.conflict || !isInventoryManager()) return;
    if (owner.selection) {
      const source = owner.type === "context" ? state.inventoryContextAsset : owner.type === "checkout" ? state.inventoryOperationAsset : state.inventoryDetailAsset;
      if (!source || String(source.id) !== String(owner.id) || !$(owner.parent).open || busyDialogs.has(owner.parent)) return;
      inventoryRecipientSelections[owner.type] = picker.selected;
      if (owner.type === "context") inventoryContextControls(); else if (owner.type === "checkout") inventoryOperationControls(); else inventoryRelationControls();
      $("inventory-subject-picker-dialog").close(); return;
    }
    if (new TextEncoder().encode($("inventory-subject-picker-notes").value).length > 2000) { dialogError("inventory-subject-picker-error", new Error(t("Il testo è troppo lungo in byte. Accorcialo e riprova."))); return; }
    const kind = $("inventory-subject-picker-kind").value, fields = { version: owner.version, role: $("inventory-subject-picker-role").value, notes: $("inventory-subject-picker-notes").value };
    if (owner.type === "asset") { fields.subject_kind = kind; fields.subject_id = picker.selected.id; } else fields.person_id = picker.selected.id;
    const epoch = state.inventoryEpoch, request = picker.request;
    const current = () => epoch === state.inventoryEpoch && request === picker.request && $("inventory-subject-picker-dialog").open;
    picker.busy = true; $("inventory-subject-picker-error").hidden = true; inventorySubjectPickerControls();
    try {
      const data = await inventorySubjectPost("/api/inventory/" + (owner.type === "asset" ? "assets/" + owner.id + "/subjects" : "identities/" + owner.id + "/people"), fields);
      if (!current()) return;
      if (owner.type === "asset" && String(state.inventoryDetailId) === String(owner.id)) { inventoryUpdateDetail(data.asset); loadInventoryAssetSubjects(); loadInventoryHistory(); refreshInventory(); }
      if (owner.type === "identity" && inventorySubjectDetail.kind === "identity" && String(inventorySubjectDetail.id) === String(owner.id)) { inventorySubjectDetail.item = data.identity; ["links", "history"].forEach((panel) => loadInventorySubjectPanel(panel)); inventorySubjectDetailControls(); }
      if ($("inventory-subject-directory-dialog").open) loadInventorySubjectDirectory();
      picker.busy = false; inventorySubjectPickerControls(); $("inventory-subject-picker-dialog").close(); showNotice(t("Collegamento salvato."));
    } catch (error) { if (current()) { picker.conflict = error.status === 409; $("inventory-subject-picker-reload").hidden = !picker.conflict; dialogError("inventory-subject-picker-error", new Error(inventoryMessage(error) + (picker.conflict ? t(" La selezione e le note restano disponibili. Ricarica la versione prima di riprovare.") : ""))); } }
    finally { if (current()) { picker.busy = false; inventorySubjectPickerControls(); } }
  }
  function resetInventoryAssetSubjects() {
    inventoryAssetSubjects.request++;
    Object.assign(inventoryAssetSubjects, { items: [], cursor: null, loading: false, busy: false, retryMore: false });
    $("inventory-subjects-list").replaceChildren(); $("inventory-subjects-error").hidden = $("inventory-subjects-retry").hidden = $("inventory-subjects-more").hidden = true;
    $("inventory-subject-add").hidden = !isInventoryManager();
  }
  async function loadInventoryAssetSubjects(more = false) {
    const page = inventoryAssetSubjects, id = state.inventoryDetailId;
    if (!id || more && (page.loading || !page.cursor)) return;
    const request = ++page.request, epoch = state.inventoryEpoch;
    const current = () => request === page.request && epoch === state.inventoryEpoch && String(id) === String(state.inventoryDetailId) && $("inventory-detail-dialog").open;
    const query = new URLSearchParams({ limit: "20" }); if (more) query.set("after_id", page.cursor); else { page.items = []; page.cursor = null; }
    page.loading = true; page.retryMore = more; $("inventory-subjects-loading").hidden = false; $("inventory-subjects-error").hidden = $("inventory-subjects-retry").hidden = true; $("inventory-subjects-more").disabled = true; $("inventory-subject-add").hidden = !isInventoryManager();
    try {
      const data = await api("/api/inventory/assets/" + id + "/subjects?" + query);
      if (!current()) return;
      page.items.push(...data.items); page.cursor = data.next_after_id || null; page.retryMore = false;
      renderInventoryAssetSubjects();
    } catch (error) { if (current()) { dialogError("inventory-subjects-error", new Error(inventoryMessage(error))); $("inventory-subjects-retry").hidden = false; } }
    finally { if (current()) { page.loading = false; $("inventory-subjects-loading").hidden = true; $("inventory-subjects-more").hidden = !page.cursor; $("inventory-subjects-more").disabled = false; } }
  }
  function renderInventoryAssetSubjects() {
    const page = inventoryAssetSubjects; $("inventory-subjects-list").replaceChildren();
    for (const link of page.items) {
      const row = node("li", "inventory-relation-card"), heading = node("div", "inventory-relation-heading"), label = inventorySubjectLabel(link.subject_kind, link.subject);
      heading.append(node("strong", "", label), node("span", "inventory-type-badge", link.subject_kind === "person" ? t("Persona") : t("Identità")));
      row.append(heading, node("p", "inventory-relation-summary", (inventorySubjectRoles[link.role] || link.role) + (link.notes ? " · " + link.notes : "") + (link.subject.active ? "" : " · " + t("Disattivato"))));
      const actions = node("div", "inventory-row-actions"); actions.append(action(t("Scheda"), label, () => openInventorySubjectDetail(link.subject_kind, link.subject_id)));
      if (isInventoryManager()) { const remove = action(t("Rimuovi collegamento"), t("Rimuovi collegamento"), () => removeInventoryAssetSubject(link)); remove.disabled = page.busy; actions.append(remove); }
      row.append(actions); $("inventory-subjects-list").append(row);
    }
    if (!page.items.length) $("inventory-subjects-list").append(node("li", "field-help", t("Nessun collegamento registrato.")));
    $("inventory-subject-add").disabled = page.busy || ["retired", "lost"].includes(state.inventoryDetailAsset?.status);
  }
  async function removeInventoryAssetSubject(link) {
    const page = inventoryAssetSubjects, asset = state.inventoryDetailAsset;
    if (!isInventoryManager() || !asset || page.busy || busyDialogs.has("inventory-detail-dialog")) return;
    const request = page.request, epoch = state.inventoryEpoch;
    const current = () => request === page.request && epoch === state.inventoryEpoch && String(asset.id) === String(state.inventoryDetailId) && $("inventory-detail-dialog").open;
    page.busy = true; busyDialogs.add("inventory-detail-dialog"); renderInventoryAssetSubjects();
    try {
      const data = await inventorySubjectPost("/api/inventory/assets/" + asset.id + "/subjects/" + link.id + "/remove", { version: asset.version });
      if (!current()) return;
      inventoryUpdateDetail(data.asset); page.busy = false; busyDialogs.delete("inventory-detail-dialog"); loadInventoryAssetSubjects(); loadInventoryHistory(); refreshInventory(); showNotice(t("Collegamento rimosso."));
    } catch (error) {
      if (!current()) return;
      dialogError("inventory-subjects-error", new Error(inventoryMessage(error) + (error.status === 409 ? t(" Ricarica la scheda prima di riprovare.") : ""))); $("inventory-subjects-retry").hidden = false;
    } finally { if (epoch === state.inventoryEpoch) { page.busy = false; busyDialogs.delete("inventory-detail-dialog"); if (String(asset.id) === String(state.inventoryDetailId)) renderInventoryAssetSubjects(); } }
  }
  $("inventory-people-open").addEventListener("click", () => openInventorySubjectDirectory("person"));
  $("inventory-identities-open").addEventListener("click", () => openInventorySubjectDirectory("identity"));
  $("inventory-subject-directory-search").addEventListener("submit", (event) => { event.preventDefault(); loadInventorySubjectDirectory(); });
  $("inventory-subject-directory-more").addEventListener("click", () => loadInventorySubjectDirectory(true));
  $("inventory-subject-directory-retry").addEventListener("click", () => loadInventorySubjectDirectory(inventorySubjectDirectory.retryMore));
  $("inventory-subject-directory-new").addEventListener("click", () => { if (isInventoryManager()) openInventorySubjectDetail(inventorySubjectDirectory.kind); });
  $("inventory-subject-directory-dialog").addEventListener("close", () => { inventorySubjectDirectory.request++; inventorySubjectDirectory.loading = false; });
  $("inventory-subject-detail-form").addEventListener("submit", saveInventorySubject);
  $("inventory-subject-detail-reload").addEventListener("click", () => loadInventorySubjectDetail(true));
  $("inventory-subject-person-add").addEventListener("click", () => openInventorySubjectPicker("identity"));
  $("inventory-subject-detail-dialog").addEventListener("close", () => { inventorySubjectDetail.request++; inventorySubjectDetail.id = null; inventorySubjectDetail.item = null; inventorySubjectDetail.loading = false; inventorySubjectDetail.panels = {}; });
  ["links", "assets", "history"].forEach((panel) => { $("inventory-subject-" + panel + "-retry").addEventListener("click", () => loadInventorySubjectPanel(panel, inventorySubjectDetail.panels[panel]?.retryMore)); $("inventory-subject-" + panel + "-more").addEventListener("click", () => loadInventorySubjectPanel(panel, true)); });
  $("inventory-subject-view-assets").addEventListener("click", () => {
    const detail = inventorySubjectDetail; if (!detail.item || detail.busy || busyDialogs.has("inventory-detail-dialog")) return;
    inventorySubjectFilter = { kind: detail.kind, id: detail.item.id, label: inventorySubjectLabel(detail.kind, detail.item) };
    $("inventory-search-form").reset(); $("inventory-mine").checked = false; $("inventory-site-filter").value = $("inventory-department-filter").value = ""; state.inventoryAttention = "";
    $("inventory-subject-detail-dialog").close();
    if ($("inventory-subject-directory-dialog").open) $("inventory-subject-directory-dialog").close();
    if ($("inventory-detail-dialog").open) $("inventory-detail-dialog").close();
    refreshInventory();
  });
  $("inventory-subject-filter-clear").addEventListener("click", () => { inventorySubjectFilter = null; refreshInventory(); });
  $("inventory-subject-add").addEventListener("click", () => openInventorySubjectPicker("asset"));
  $("inventory-subject-picker-search").addEventListener("click", () => loadInventorySubjectPicker());
  $("inventory-subject-picker-q").addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); loadInventorySubjectPicker(); } });
  $("inventory-subject-picker-kind").addEventListener("change", () => { inventorySubjectPicker.selected = null; loadInventorySubjectPicker(); });
  $("inventory-subject-picker-more").addEventListener("click", () => loadInventorySubjectPicker(true));
  $("inventory-subject-picker-retry").addEventListener("click", () => loadInventorySubjectPicker(inventorySubjectPicker.retryMore));
  $("inventory-subject-picker-reload").addEventListener("click", reloadInventorySubjectPicker);
  $("inventory-subject-picker-form").addEventListener("submit", saveInventorySubjectLink);
  $("inventory-subject-picker-dialog").addEventListener("close", () => { inventorySubjectPicker.request++; inventorySubjectPicker.owner = null; inventorySubjectPicker.loading = false; inventorySubjectPicker.selected = null; inventorySubjectPicker.items = []; $("inventory-subject-picker-list").replaceChildren(); });
  $("inventory-subjects-more").addEventListener("click", () => loadInventoryAssetSubjects(true));
  $("inventory-subjects-retry").addEventListener("click", async () => {
    const id = state.inventoryDetailId, epoch = state.inventoryEpoch;
    try { const data = await api("/api/inventory/assets/" + id); if (epoch !== state.inventoryEpoch || String(id) !== String(state.inventoryDetailId) || !$("inventory-detail-dialog").open) return; inventoryUpdateDetail(data.asset); loadInventoryAssetSubjects(inventoryAssetSubjects.retryMore); }
    catch (error) { if (epoch === state.inventoryEpoch) dialogError("inventory-subjects-error", new Error(inventoryMessage(error))); }
  });

  function inventoryFilterQuery() {
    const query = new URLSearchParams({ q: $("inventory-search").value.trim(), verification_days: $("inventory-verification-days").value });
    [["status", "inventory-filter"], ["asset_type", "inventory-type-filter"], ["site_id", "inventory-site-filter"], ["department_id", "inventory-department-filter"]].forEach(([key, id]) => { if ($(id).value) query.set(key, $(id).value); });
    if ($("inventory-mine").checked) query.set("mine", "1");
    if (state.inventoryAttention) query.set("attention", state.inventoryAttention);
    if (inventorySubjectFilter) query.set(inventorySubjectFilter.kind === "person" ? "person_id" : "identity_id", inventorySubjectFilter.id);
    return query;
  }

  function renderInventoryPlaceFilters() {
    const site = $("inventory-site-filter").value, department = $("inventory-department-filter").value;
    const label = (place) => place.name + (place.active ? "" : t(" (disattivato)"));
    const places = [...state.inventoryPlaces].sort((a, b) => a.name.localeCompare(b.name, locale));
    $("inventory-site-filter").replaceChildren(new Option(t("Tutte le sedi"), ""), ...places.filter((place) => place.kind === "site").map((place) => new Option(label(place), String(place.id))));
    $("inventory-site-filter").value = places.some((place) => place.kind === "site" && String(place.id) === site) ? site : "";
    $("inventory-department-filter").replaceChildren(new Option(t("Tutti i reparti"), ""), ...places.filter((place) => place.kind === "department" && (!site || !place.parent_id || String(place.parent_id) === site)).map((place) => new Option(label(place), String(place.id))));
    $("inventory-department-filter").value = [...$("inventory-department-filter").options].some((option) => option.value === department) ? department : "";
    $("inventory-site-filter").disabled = $("inventory-department-filter").disabled = state.inventoryConfigured !== true || !state.inventoryPlacesReady;
    $("inventory-place-filter-error").hidden = !state.inventoryPlaceFilterError;
    $("inventory-place-filter-error").querySelector("span").textContent = state.inventoryPlaceFilterError;
  }

  async function refreshInventoryPlaceFilters() {
    const epoch = state.inventoryEpoch;
    try {
      await loadInventoryPlaces();
      if (epoch !== state.inventoryEpoch) return;
      state.inventoryPlaceFilterError = "";
    } catch (error) {
      if (epoch !== state.inventoryEpoch) return;
      state.inventoryPlaceFilterError = t("Filtri per sede e reparto non disponibili. ") + inventoryMessage(error);
    }
    renderInventoryPlaceFilters();
  }

  function renderInventoryDashboard() {
    const data = state.inventoryDashboard;
    $("inventory-dashboard-loading").hidden = !state.inventoryDashboardLoading;
    $("inventory-dashboard-error").hidden = !state.inventoryDashboardError;
    $("inventory-dashboard-error").querySelector("span").textContent = state.inventoryDashboardError;
    $("inventory-attention-clear").hidden = !state.inventoryAttention;
    $("inventory-dashboard-cards").hidden = !data;
    $("inventory-distribution").hidden = !data;
    const scope = data ? data.counts.total === 1 ? t("1 bene per i filtri selezionati · {0}. Verifiche dopo {1} giorni.", [dateLabel(data.today), data.verification_days]) : t("{0} beni per i filtri selezionati · {1}. Verifiche dopo {2} giorni.", [data.counts.total, dateLabel(data.today), data.verification_days]) : "";
    $("inventory-dashboard-scope").textContent = data ? scope + (state.inventoryAttention ? " " + t("Filtro operativo: {0}.", [inventoryAttentionLabels[state.inventoryAttention]]) : "") : t("I conteggi operativi seguono i filtri e comprendono tutti i risultati, oltre la pagina visibile.");
    $("inventory-dashboard-cards").replaceChildren();
    $("inventory-distribution-groups").replaceChildren();
    if (!data) return;
    for (const [key, label] of Object.entries(inventoryAttentionLabels)) {
      const button = node("button", "inventory-dashboard-card" + (state.inventoryAttention === key ? " is-selected" : ""));
      button.type = "button"; button.dataset.inventoryAttention = key;
      button.setAttribute("aria-pressed", String(state.inventoryAttention === key));
      button.append(node("span", "", label), node("strong", "", String(data.counts[key] || 0)));
      button.addEventListener("click", () => { state.inventoryAttention = state.inventoryAttention === key ? "" : key; refreshInventory(); });
      $("inventory-dashboard-cards").append(button);
    }
    const groups = [[t("Per sede"), data.by_site, t("Sede non indicata")], [t("Per reparto"), data.by_department, t("Reparto non indicato")], [t("Per utente Punto"), data.by_user, t("Senza utente Punto")]];
    for (const [title, entries, absent] of groups) {
      const section = node("section"); section.append(node("h4", "", title));
      const list = node("ul");
      for (const group of entries || []) {
        const item = node("li"); item.append(node("span", "", group.name || absent), node("strong", "", String(group.count))); list.append(item);
      }
      if (!entries?.length) section.append(node("p", "field-help", t("Nessun bene per questi filtri."))); else section.append(list);
      $("inventory-distribution-groups").append(section);
    }
    $("inventory-distribution-limit").hidden = !data.groups_truncated;
  }

  async function refreshInventoryDashboard(query = inventoryFilterQuery()) {
    if (!state.session?.authenticated || state.inventoryConfigured !== true) return;
    const request = ++state.inventoryDashboardRequest, epoch = state.inventoryEpoch;
    state.inventoryDashboardLoading = true; state.inventoryDashboardError = ""; state.inventoryDashboard = null; renderInventoryDashboard();
    try {
      const data = await api("/api/inventory/dashboard?" + query);
      if (request !== state.inventoryDashboardRequest || epoch !== state.inventoryEpoch) return;
      if (!data.counts || !validDate(data.today)) throw new Error(t("Il server ha restituito un cruscotto non valido."));
      state.inventoryDashboard = data;
    } catch (error) {
      if (request !== state.inventoryDashboardRequest || epoch !== state.inventoryEpoch) return;
      state.inventoryDashboardError = inventoryMessage(error);
    } finally {
      if (request === state.inventoryDashboardRequest && epoch === state.inventoryEpoch) { state.inventoryDashboardLoading = false; renderInventoryDashboard(); }
    }
  }

  let inventoryQrAsset = null;
  function openInventoryQr() {
    const asset = state.inventoryDetailAsset;
    if (!asset || !state.session?.authenticated || !window.InventoryQr) return;
    inventoryQrAsset = { id: asset.id, asset_tag: asset.asset_tag, name: asset.name };
    $("inventory-qr-error").hidden = true;
    try { $("inventory-qr-preview").replaceChildren(window.InventoryQr.createLabel(inventoryQrAsset)); }
    catch (error) { dialogError("inventory-qr-error", error); return; }
    $("inventory-qr-dialog").showModal();
  }
  $("inventory-qr-open").addEventListener("click", openInventoryQr);
  $("inventory-qr-download").addEventListener("click", () => {
    if (!inventoryQrAsset || !state.session?.authenticated) return;
    try { window.InventoryQr.downloadLabel(inventoryQrAsset); } catch (error) { dialogError("inventory-qr-error", error); }
  });
  $("inventory-qr-print").addEventListener("click", () => {
    if (!inventoryQrAsset || !state.session?.authenticated) return;
    try { window.InventoryQr.printLabel(inventoryQrAsset); } catch (error) { dialogError("inventory-qr-error", error); }
  });
  $("inventory-qr-dialog").addEventListener("close", () => { inventoryQrAsset = null; $("inventory-qr-preview").replaceChildren(); });

  function resetInventory() {
    inventorySubjectReset();
    clearTimeout(inventorySearchTimer);
    state.inventoryEpoch++;
    state.inventoryDashboardRequest++;
    Object.assign(state, { inventoryDashboard: null, inventoryDashboardLoading: false, inventoryDashboardError: "", inventoryAttention: "", inventoryPlaceFilterError: "" });
    $("inventory-verification-days").value = "90";
    $("inventory-site-filter").replaceChildren(new Option(t("Tutte le sedi"), ""));
    $("inventory-department-filter").replaceChildren(new Option(t("Tutti i reparti"), ""));
    if ($("inventory-qr-dialog").open) $("inventory-qr-dialog").close();
    inventoryQrAsset = null;
    state.inventoryPlacesRequest++; state.inventoryUsersRequest++; state.inventoryDirectoryRequest++; state.inventoryContextRequest++; state.inventoryOperationRequest++; state.inventoryCustodyRequest++; state.inventoryVerificationsRequest++;
    inventoryPlacesPromise = null; inventoryUsersPromise = null;
    Object.assign(state, { inventoryPlaces: [], inventoryPlacesReady: false, inventoryUsers: [], inventoryUsersReady: false, inventoryDirectoryBusy: false, inventoryDirectoryEditing: null, inventoryContextBusy: false, inventoryContextLoading: false, inventoryContextAsset: null, inventoryContextConflict: false, inventoryOperationBusy: false, inventoryOperationLoading: false, inventoryOperationAsset: null, inventoryOperation: null, inventoryOperationConflict: false, inventoryCustodyItems: [], inventoryCustodyCursor: null, inventoryCustodyLoading: false, inventoryCustodyRetryMore: false, inventoryVerificationItems: [], inventoryVerificationsCursor: null, inventoryVerificationsLoading: false, inventoryVerificationsRetryMore: false });
    ["inventory-directory-dialog", "inventory-context-dialog", "inventory-operation-dialog"].forEach((id) => { busyDialogs.delete(id); if ($(id).open) $(id).close(); });
    ["inventory-directory-form", "inventory-context-form", "inventory-operation-form"].forEach((id) => $(id).reset());
    $("inventory-mine").checked = false; $("inventory-custody-history").replaceChildren(); $("inventory-verifications").replaceChildren(); $("inventory-directory-list").replaceChildren();
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
    dialogBusy("inventory-dialog", false, "inventory-save", t("Crea asset"));
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
    $("inventory-directory-open").hidden = !manager;
    $("inventory-directory-open").disabled = state.inventoryConfigured !== true;
    $("inventory-mine").disabled = state.inventoryConfigured !== true;
    $("inventory-new").disabled = state.inventoryConfigured !== true;
    $("inventory-import").hidden = !manager;
    $("inventory-import").disabled = state.inventoryConfigured !== true || state.inventoryImportBusy;
    $("inventory-export").disabled = state.inventoryConfigured !== true || state.inventoryExporting;
    $("inventory-export").textContent = state.inventoryExporting ? t("Esportazione…") : t("Esporta CSV");
    const backup = state.inventoryBackup;
    $("inventory-backup-status").hidden = !manager || state.inventoryConfigured !== true;
    $("inventory-backup-status").textContent = !backup?.enabled ? t("Backup automatico non configurato.") : backup.last_error ? t("Backup automatico: ") + backup.last_error + (backup.last_success ? t(" Ultima copia riuscita: ") + inventoryTime(backup.last_success) + "." : "") : t("Backup automatico ogni ") + backup.interval_hours + t(" ore · ") + backup.keep + t(" copie conservate. ") + (backup.last_success ? t("Ultima copia: ") + inventoryTime(backup.last_success) + "." : t("Prima copia in preparazione.")) + (backup.next_due ? t(" Prossima: ") + inventoryTime(backup.next_due) + "." : "");
    $("inventory-refresh").disabled = state.inventoryLoading;
    $("inventory-permission").textContent = manager ? t("Gestore · Censisci i beni e aggiorna assegnazioni, stato e garanzie. Per dismettere un bene, scegli Dismesso.") : t("Consulta i beni aziendali e il loro storico. Un gestore può creare e aggiornare le schede.");
    $("inventory-count").textContent = state.inventorySummary ? state.inventorySummary.total : "—";
    $("inventory-summary").hidden = !state.inventorySummary;
    ["total", ...Object.keys(inventoryStates)].forEach((key) => { $("inventory-" + key).textContent = state.inventorySummary ? state.inventorySummary[key] : "—"; });
    renderInventoryPlaceFilters(); renderInventoryDashboard();
    inventorySubjectFilterControls();
    const filtered = Boolean(inventorySubjectFilter || $("inventory-search").value.trim() || $("inventory-filter").value || $("inventory-type-filter").value || $("inventory-mine").checked || $("inventory-site-filter").value || $("inventory-department-filter").value || state.inventoryAttention);
    $("inventory-clear").hidden = !filtered;
    $("inventory-loading").hidden = !state.inventoryLoading;
    $("inventory-loading").textContent = state.inventoryItems.length ? t("Aggiornamento dell’inventario…") : t("Caricamento dell’inventario…");
    $("inventory-table-wrap").hidden = !state.inventoryItems.length;
    $("inventory-empty").hidden = !state.inventoryReady || Boolean(state.inventoryItems.length) || state.inventoryLoading;
    $("inventory-empty-title").textContent = filtered ? t("Nessun bene per questi filtri.") : t("Il primo bene parte da qui.");
    $("inventory-empty-description").textContent = filtered ? t("Prova un’altra ricerca oppure azzera i filtri.") : manager ? t("Usa Nuovo asset per censire dotazioni, dispositivi e altri beni aziendali.") : t("Un gestore può censire dotazioni, dispositivi e altri beni aziendali.");
    $("inventory-footer").hidden = !state.inventoryReady;
    $("inventory-visible").textContent = state.inventoryItems.length + (state.inventoryItems.length === 1 ? t(" bene caricato") : t(" beni caricati")) + (filtered ? t(" per i filtri selezionati.") : t(" nell’inventario."));
    $("inventory-more").hidden = !state.inventoryCursor;
    $("inventory-more").disabled = state.inventoryLoading;
    $("inventory-more").textContent = state.inventoryLoading ? t("Caricamento…") : t("Carica altri asset");
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
      if (asset.context?.assignee_person_id || asset.context?.assignee_user_id) assigned.append(node("span", "inventory-cell-secondary", asset.context.assignee_person_id ? t("Persona") : t("Utente Punto")));
      assigned.append(node("span", "inventory-cell-line", inventoryAssigneeName(asset.context) || asset.assignee || t("Non assegnato")), node("span", "inventory-cell-secondary", inventoryPlacementLabel(asset) || t("Sede non indicata")));
      if (asset.custody?.due_on && asset.custody.due_on < localDate(new Date())) assigned.append(node("span", "inventory-cell-secondary inventory-return-overdue", t("Restituzione in ritardo")));
      const actions = node("td");
      const group = node("div", "inventory-row-actions");
      group.append(action(t("Scheda"), t("Apri scheda e storico di ") + asset.name, () => openInventoryDetail(asset.id)));
      if (manager) group.append(action(t("Modifica"), t("Modifica ") + asset.name, () => openInventoryEditor(asset.id)));
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
    const query = inventoryFilterQuery(); query.set("limit", "30");
    if (!more) { state.inventoryDashboardRequest++; state.inventoryDashboard = null; state.inventoryDashboardLoading = false; state.inventoryDashboardError = ""; }
    if (new TextEncoder().encode(query.get("q")).length > 160) {
      state.inventoryLoading = false;
      $("inventory-search").setCustomValidity(t("La ricerca è troppo lunga. Accorciala e riprova."));
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
          $("inventory-error-text").textContent = t("Asset inventory non è ancora configurato. Chiedi a un gestore di collegare il servizio di inventario, poi riprova.");
          $("inventory-error").hidden = false;
          return;
        }
        state.inventoryConfigured = true;
        state.inventoryBackup = service.backup || null;
        state.inventorySummary = service.summary || null;
        const dashboardQuery = new URLSearchParams(query); dashboardQuery.delete("limit");
        refreshInventoryDashboard(dashboardQuery);
        refreshInventoryPlaceFilters();
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
    const query = inventoryFilterQuery(); query.set("limit", "100");
    state.inventoryExporting = true; renderInventory();
    const parts = []; let previous = null, size = 0, count = 0, complete = false;
    try {
      for (let page = 0; page < 1000; page++) {
        const data = await api("/api/inventory/assets/export?" + query);
        if (!current()) return;
        if (typeof data.csv !== "string" || !Number.isInteger(data.exported) || data.exported < 0) throw new Error(t("Il server ha restituito un’esportazione non valida."));
        const newline = data.csv.indexOf("\n");
        if (newline < 0) throw new Error(t("L’esportazione non contiene le intestazioni CSV."));
        const chunk = page ? data.csv.slice(newline + 1) : data.csv;
        size += new TextEncoder().encode(chunk).length;
        if (size > 20 * 1024 * 1024) throw new Error(t("L’esportazione supera 20 MiB. Restringi i filtri e riprova."));
        parts.push(chunk); count += data.exported;
        if (data.next_after_id === null) { complete = true; break; }
        if (!Number.isSafeInteger(data.next_after_id) || data.next_after_id < 1 || previous !== null && data.next_after_id >= previous) throw new Error(t("Il server ha restituito una pagina CSV non valida."));
        previous = data.next_after_id; query.set("after_id", String(previous));
      }
      if (!complete) throw new Error(t("Troppi asset da esportare insieme. Restringi i filtri e riprova."));
      if (!current()) return;
      inventoryDownload("\ufeff" + parts.join(""), "inventario-" + localDate(new Date()) + ".csv");
      showNotice(t("Esportazione completata: {0} asset per i filtri selezionati.", [count]));
    } catch (error) { if (current()) showNotice(inventoryMessage(error), true); }
    finally { if (current()) { state.inventoryExporting = false; renderInventory(); } }
  }

  function renderInventoryImport() {
    const busy = state.inventoryImportBusy;
    ["inventory-import-file", "inventory-import-mode", "inventory-import-template", "inventory-import-preview-button"].forEach((id) => { $(id).disabled = busy; });
    $("inventory-import-preview-button").textContent = busy ? t("Operazione in corso…") : t("Controlla file");
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
    if (!file || file.size > 256 * 1024 || !file.size) { dialogError("inventory-import-error", new Error(t("Scegli un CSV non vuoto di al massimo 256 KiB."))); return; }
    invalidateInventoryImport();
    const request = ++state.inventoryImportRequest, epoch = state.inventoryEpoch;
    const current = () => request === state.inventoryImportRequest && epoch === state.inventoryEpoch && isInventoryManager() && $("inventory-import-dialog").open;
    const mode = $("inventory-import-mode").value;
    state.inventoryImportBusy = true; renderInventoryImport();
    try {
      let csv;
      try { csv = new TextDecoder("utf-8", { fatal: true }).decode(await file.arrayBuffer()); }
      catch { throw new Error(t("Il file deve essere un CSV con codifica UTF-8.")); }
      if (!current()) return;
      const data = await post("/api/inventory/import/preview", { csv, mode });
      if (!current()) return;
      if (!Array.isArray(data.items) || !data.summary || typeof data.can_commit !== "boolean" || data.can_commit && !/^[a-f0-9]{32}$/i.test(data.preview_id || "")) throw new Error(t("Il server ha restituito un’anteprima non valida."));
      state.inventoryImportPlan = data;
      $("inventory-import-summary").textContent = t("{0} {1} · {2} da creare · {3} da aggiornare · {4} con errori.", [data.summary.total, data.summary.total === 1 ? t("riga") : t("righe"), data.summary.create, data.summary.update, data.summary.errors]);
      for (const row of data.items) {
        const tr = node("tr");
        tr.append(node("td", "", row.row), node("td", "", [row.asset_tag, row.name].filter(Boolean).join(" · ")), node("td", "", { create: t("Crea"), update: t("Aggiorna"), error: t("Errore") }[row.action] || row.action), node("td", "", row.errors?.length ? row.errors.map((message) => t(message)).join(" · ") : t("Pronto")));
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
      showNotice(t("Importazione completata: {0} asset creati e {1} aggiornati.", [plan.summary.create, plan.summary.update]));
      refreshInventory();
    } catch (error) {
      if (!current()) return;
      state.inventoryImportPlan = null;
      dialogError("inventory-import-error", new Error(inventoryMessage(error) + t(" Controlla nuovamente il file prima di confermare.")));
    } finally { if (current()) { state.inventoryImportBusy = false; renderInventoryImport(); } }
  }

  $("inventory-export").addEventListener("click", exportInventory);
  $("inventory-import").addEventListener("click", openInventoryImport);
  $("inventory-import-form").addEventListener("submit", previewInventoryImport);
  $("inventory-import-commit").addEventListener("click", commitInventoryImport);
  ["inventory-import-file", "inventory-import-mode"].forEach((id) => $(id).addEventListener("change", invalidateInventoryImport));
  $("inventory-import-dialog").addEventListener("close", invalidateInventoryImport);
  $("inventory-import-template").addEventListener("click", () => inventoryDownload("﻿\"asset_tag\",\"name\",\"asset_type\",\"details\"\r\n\"IT-0001\",\"Computer ufficio\",\"it_device\",\"{}\"\r\n", "modello-inventario.csv"));

  function inventoryDatesAndAssignment() {
    const assigned = $("inventory-status").value === "assigned";
    $("inventory-assignee").required = assigned;
    $("inventory-assignee-required").hidden = !assigned;
    $("inventory-assignee").setCustomValidity(assigned && !$("inventory-assignee").value.trim() ? t("Indica la persona o il reparto a cui è assegnato il bene.") : "");
    const purchase = $("inventory-purchase_date").value;
    const warranty = $("inventory-warranty_until").value;
    $("inventory-warranty_until").min = validDate(purchase) ? purchase : "0001-01-01";
    $("inventory-warranty_until").setCustomValidity(purchase && warranty && warranty < purchase ? t("La garanzia deve terminare alla data di acquisto o dopo.") : "");
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
    const context = asset?.context, structuredUser = Boolean(context?.assignee_person_id || context?.assignee_user_id || asset?.custody), structuredPlace = Boolean(context?.site_id || context?.department_id || context?.location_id);
    $("inventory-assignee").readOnly = structuredUser;
    $("inventory-location").readOnly = structuredPlace || Boolean(asset?.custody);
    $("inventory-status").disabled = structuredUser;
    $("inventory-asset_type").disabled = structuredUser || structuredPlace;
    $("inventory-editor-context-help").hidden = !structuredUser && !structuredPlace;
    state.inventoryEditingAsset = asset;
    state.inventoryConflict = false;
    $("inventory-editor-error").hidden = true;
    $("inventory-editor-reload").hidden = true;
    $("inventory-fields").disabled = false;
    $("inventory-save").disabled = false;
    $("inventory-dialog-title").textContent = asset ? t("Modifica asset") : t("Nuovo asset");
    $("inventory-save").textContent = asset ? t("Salva modifiche") : t("Crea asset");
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
    $("inventory-dialog-title").textContent = t("Modifica asset");
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
      $("inventory-editor-reload").textContent = t("Riprova a caricare la scheda aggiornata");
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
    return Number.isNaN(date.getTime()) ? value : date.toLocaleString(locale);
  }

  function inventoryDetails(asset, timestamps = true) {
    const details = node("dl", "inventory-detail-grid");
    const fields = [[t("Codice inventario"), asset.asset_tag], [t("Nome"), asset.name], [t("Tipologia"), inventoryTypes[inventoryAssetType(asset)] || asset.asset_type], [t("Stato"), inventoryStates[asset.status] || asset.status], [t("Categoria"), asset.category], [t("Numero di serie"), asset.serial_number], [t("Produttore"), asset.manufacturer], [t("Modello"), asset.model], [t("Assegnato a"), asset.assignee], [t("Ubicazione"), asset.location], [t("Acquistato il"), asset.purchase_date && validDate(asset.purchase_date) ? dateLabel(asset.purchase_date) : asset.purchase_date], [t("Garanzia fino al"), asset.warranty_until && validDate(asset.warranty_until) ? dateLabel(asset.warranty_until) : asset.warranty_until], [t("Versione"), asset.version]];
    if (asset.context) {
      fields.push([asset.context.assignee_person_id ? t("Persona assegnataria") : t("Utente assegnatario"), inventoryAssigneeName(asset.context)], [t("Sede censita"), asset.context.site_name], [t("Reparto censito"), asset.context.department_name], [t("Ubicazione censita"), asset.context.location_name]);
    }
    if (asset.custody) fields.push([t("Consegna aperta dal"), dateLabel(asset.custody.checkout_on)], [t("Restituzione prevista"), asset.custody.due_on ? dateLabel(asset.custody.due_on) : ""]);
    fields.push(...inventoryTypedDisplay(asset));
    if (timestamps) fields.push([t("Creato il"), inventoryTime(asset.created_at)], [t("Aggiornato il"), inventoryTime(asset.updated_at)]);
    fields.push([t("Note"), asset.notes]);
    fields.forEach(([label, value]) => {
      const item = node("div", label === t("Note") ? "inventory-detail-wide" : "");
      item.append(node("dt", "", label), node("dd", "", value === null || value === undefined || value === "" ? "—" : String(value)));
      details.append(item);
    });
    return details;
  }

  function openInventoryDetail(id) {
    if (state.inventoryRelationBusy || state.inventoryOperationBusy || state.inventoryContextBusy || busyDialogs.has("inventory-detail-dialog")) return;
    state.inventoryDetailId = id;
    state.inventoryDetailAsset = null;
    resetInventoryRelations();
    resetInventoryAssetSubjects();
    state.inventoryDetailRequest++;
    state.inventoryHistoryRequest++;
    state.inventoryHistoryReady = false;
    state.inventoryHistoryLoading = false;
    state.inventoryHistoryCursor = null;
    $("inventory-detail-title").textContent = t("Scheda asset");
    $("inventory-detail-data").replaceChildren();
    state.inventoryCustodyRequest++; state.inventoryVerificationsRequest++;
    state.inventoryCustodyItems = []; state.inventoryVerificationItems = []; state.inventoryCustodyCursor = null; state.inventoryVerificationsCursor = null;
    $("inventory-custody-history").replaceChildren(); $("inventory-verifications").replaceChildren(); $("inventory-custody-current").replaceChildren();
    $("inventory-history").replaceChildren();
    $("inventory-detail-content").hidden = true;
    $("inventory-detail-edit").hidden = true;
    $("inventory-tickets").hidden = true;
    $("inventory-new-ticket").hidden = true;
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
      $("inventory-tickets").hidden = !state.session?.tickets?.configured;
      $("inventory-new-ticket").hidden = !state.session?.tickets?.configured;
      if (state.session?.tickets?.configured) {
        $("inventory-tickets").href = ticketAppURL("asset", id);
        $("inventory-new-ticket").href = ticketAppURL("asset", id, true);
      }
      renderInventoryRelationOptions();
      renderInventoryOperationalActions();
      loadInventoryRelations();
      loadInventoryHistory();
      loadInventoryCustody(); loadInventoryVerifications(); loadInventoryAssetSubjects();
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
        const operation = entry.change?.operation;
        const title = { context_updated: t("Assegnazione e collocazione"), checked_out: t("Consegna"), checked_in: t("Restituzione"), verified: t("Verifica fisica"), relation_added: t("Relazione aggiunta"), relation_removed: t("Relazione rimossa"), subject_added: t("Relazione aggiunta"), subject_removed: t("Relazione rimossa") }[operation] || (entry.action === "created" ? t("Creazione") : t("Aggiornamento"));
        item.append(node("h4", "", title + t(" · versione ") + entry.version), node("p", "inventory-history-meta", inventoryTime(entry.created_at) + t(" · utente #") + entry.actor_id));
        if (entry.change?.link?.subject_kind) {
          const link = entry.change.link, subject = link.subject_snapshot || link.subject;
          if (subject) item.append(node("p", "inventory-history-summary", inventorySubjectLabel(link.subject_kind, subject) + " · " + (inventorySubjectRoles[link.role] || link.role) + (link.notes ? " · " + link.notes : "")));
        }
        if (entry.change?.relation) {
          const operation = entry.change.operation === "relation_removed" ? t("Relazione rimossa") : t("Relazione aggiunta");
          item.append(node("p", "inventory-history-summary", operation + " · " + inventoryRelationSummary(entry.change.relation)));
        }
        if (entry.change?.context) {
          const context = entry.change.context;
          item.append(node("p", "inventory-history-summary", [inventoryAssigneeName(context) || t("Non assegnato"), context.site_name, context.department_name, context.location_name].filter(Boolean).join(" · ")));
        }
        if (entry.change?.custody) {
          const custody = entry.change.custody;
          item.append(node("p", "inventory-history-summary", operation === "checked_in" ? t("Restituito da ") + inventoryCustodyName(custody) + " · " + dateLabel(custody.returned_on) + " · " + (inventoryConditions[custody.condition_in] || custody.condition_in) : inventoryCustodyName(custody) + " · " + dateLabel(custody.checkout_on) + (custody.due_on ? t(" · restituzione prevista ") + dateLabel(custody.due_on) : "")));
        }
        if (entry.change?.verification) {
          const verification = entry.change.verification;
          item.append(node("p", "inventory-history-summary", dateLabel(verification.verified_on) + " · " + (inventoryConditions[verification.condition] || verification.condition) + (verification.observed_location_name ? " · " + verification.observed_location_name : "") + (verification.location_matches === false ? t(" · ubicazione diversa da quella prevista") : "")));
        }
        if (entry.snapshot) {
          item.append(node("p", "inventory-history-summary", (inventoryStates[entry.snapshot.status] || entry.snapshot.status) + (entry.snapshot.assignee ? " · " + entry.snapshot.assignee : "") + (entry.snapshot.location ? " · " + entry.snapshot.location : "")));
          const snapshot = node("details");
          snapshot.append(node("summary", "", t("Visualizza i dati di questa versione")), inventoryDetails(entry.snapshot, false));
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
    inventoryRecipientSelections.relation = null;
    state.inventoryRelationsRequest++; state.inventoryRelations = []; state.inventoryRelationsReady = false; state.inventoryRelationsCursor = null; state.inventoryRelationsLoading = false; state.inventoryRelationsRetryMore = false; state.inventoryRelationConflict = false;
    $("inventory-relations").replaceChildren(); $("inventory-relation-form").reset(); $("inventory-relation-form").hidden = true;
    $("inventory-relation-error").hidden = true; $("inventory-relation-reload").hidden = true; $("inventory-relations-error").hidden = true; $("inventory-relations-retry").hidden = true; $("inventory-relations-more").hidden = true;
    const picker = inventoryPickers.get("inventory-relation-picker"); if (picker) { picker.request++; picker.selected = null; picker.items = []; picker.loading = false; picker.cursor = null; picker.retryMore = false; picker.render(); }
  }

  function inventoryRelationSummary(relation) {
    const source = relation.source || relation.source_asset || inventoryAssetCache.get(String(relation.source_id));
    const target = relation.target || relation.target_asset || inventoryAssetCache.get(String(relation.target_id));
    const origin = source ? inventoryAssetLabel(source) : t("Asset #") + relation.source_id;
    const subject = relation.subject_snapshot || relation.subject;
    const destination = relation.subject_kind && subject ? inventorySubjectLabel(relation.subject_kind, subject) : target ? inventoryAssetLabel(target) : relation.target_id ? t("Asset #") + relation.target_id : relation.assignee || t("Destinatario non indicato");
    let summary = (inventoryRelationNames[relation.relation_type] || relation.relation_type) + " · " + origin + " → " + destination;
    if (relation.relation_type === "licensed_to") summary += " · " + relation.quantity + (Number(relation.quantity) === 1 ? " posto" : " posti");
    if (relation.installed_version) summary += t(" · versione ") + relation.installed_version;
    if (relation.source_port || relation.target_port) summary += t(" · porte ") + (relation.source_port || "—") + " → " + (relation.target_port || "—");
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
    $("inventory-relation-help").textContent = inventoryAssetType(asset) === "license" ? t("Assegna posti a dispositivi, persone o identità secondo il modello della licenza. Ogni quantità viene conteggiata una sola volta.") : t("Consulta software installato, collegamenti di rete e licenze assegnate a questo bene.");
    if (!allowed.length) $("inventory-relation-form").hidden = true;
    inventoryRelationControls();
  }

  function inventoryRelationControls() {
    const asset = state.inventoryDetailAsset; if (!asset) return;
    const type = $("inventory-relation-type").value, model = asset.details?.license_model || "per_device";
    const license = type === "licensed_to";
    const kinds = !license || model === "per_device" ? ["device"] : model === "per_user" ? ["person", "identity", "legacy"] : ["device", "person", "identity", "legacy"];
    const recipient = $("inventory-relation-recipient-kind"), previous = recipient.value;
    recipient.replaceChildren(...kinds.map((kind) => new Option(({ device: t("Dispositivo"), person: t("Persona"), identity: t("Identità"), legacy: t("Testo libero precedente") })[kind], kind)));
    recipient.value = kinds.includes(previous) ? previous : kinds[0];
    const structured = license && ["person", "identity"].includes(recipient.value), legacy = license && recipient.value === "legacy", device = !structured && !legacy;
    $("inventory-relation-recipient").hidden = !license || kinds.length < 2; recipient.disabled = !license || kinds.length < 2;
    $("inventory-relation-target").hidden = !device; $("inventory-relation-subject-field").hidden = !structured;
    $("inventory-relation-subject-pick").disabled = state.inventoryRelationBusy || state.inventoryRelationConflict;
    $("inventory-relation-subject-label").textContent = inventoryRecipientSelections.relation ? inventorySubjectLabel(recipient.value, inventoryRecipientSelections.relation) : t("Seleziona un destinatario.");
    $("inventory-relation-assignee-field").hidden = !legacy;
    $("inventory-relation-assignee").disabled = !legacy; $("inventory-relation-assignee").required = legacy;
    $("inventory-relation-quantity-field").hidden = !license; $("inventory-relation-quantity").disabled = !license;
    $("inventory-relation-version-field").hidden = type !== "installed_on"; $("inventory-relation-installed-version").disabled = type !== "installed_on";
    $("inventory-relation-ports").hidden = type !== "connected_to";
    $("inventory-relation-source-port").disabled = type !== "connected_to"; $("inventory-relation-target-port").disabled = type !== "connected_to";
    const picker = inventoryPickers.get("inventory-relation-picker"), types = type === "connected_to" ? ["network_device"] : ["it_device", "network_device"];
    if (picker.types.join() !== types.join()) picker.setTypes(types);
    picker.enabled = device; picker.choice.required = device; picker.render();
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
      heading.append(node("strong", "", inventoryRelationNames[relation.relation_type] || relation.relation_type), node("span", "inventory-cell-secondary", outgoing ? t("In uscita") : t("In entrata")));
      item.append(heading, node("p", "inventory-relation-summary", inventoryRelationSummary(relation)));
      if (relation.notes) item.append(node("p", "inventory-history-summary", relation.notes));
      const actions = node("div", "inventory-row-actions");
      if (relation.subject_kind && relation.subject_id) { const open = action(t("Apri anagrafica"), t("Apri anagrafica"), () => { if (!state.inventoryRelationBusy) openInventorySubjectDetail(relation.subject_kind, relation.subject_id); }); open.disabled = state.inventoryRelationBusy; actions.append(open); }
      if (counterpart) { const open = action(t("Apri scheda collegata"), t("Apri ") + counterpart.name, () => { if (!state.inventoryRelationBusy) openInventoryDetail(counterpart.id); }); open.disabled = state.inventoryRelationBusy; actions.append(open); }
      if (outgoing && isInventoryManager()) {
        const remove = action(t("Rimuovi relazione"), t("Rimuovi ") + inventoryRelationNames[relation.relation_type], () => {
          if (state.inventoryRelationBusy || state.inventoryRelationConflict) return;
          const confirmation = node("div", "inventory-relation-confirm");
          confirmation.append(node("p", "", t("Rimuovere questa relazione? La rimozione resta nello storico. Potrai registrare una nuova relazione in seguito.")));
          const cancel = node("button", "button button-secondary", t("Mantieni relazione")); cancel.type = "button"; cancel.addEventListener("click", () => { confirmation.remove(); remove.hidden = false; });
          const confirm = node("button", "button button-danger", t("Conferma rimozione")); confirm.type = "button"; confirm.addEventListener("click", () => mutateInventoryRelation(null, relation));
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
      showNotice(t("Scheda aggiornata. La relazione non è stata salvata: controlla i dati e riprova."));
    } catch (error) { if (current()) dialogError("inventory-relations-error", new Error(inventoryMessage(error))); }
    finally { if (current()) $("inventory-relation-reload").disabled = false; }
  }

  async function mutateInventoryRelation(fields, removing = null) {
    const source = state.inventoryDetailAsset;
    if (!source || state.inventoryRelationBusy || state.inventoryRelationConflict || !isInventoryManager() || busyDialogs.has("inventory-detail-dialog")) return;
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
      $("inventory-relation-form").hidden = true; $("inventory-relation-form").reset(); inventoryRecipientSelections.relation = null;
      const picker = inventoryPickers.get("inventory-relation-picker"); picker.setSelected(null);
      showNotice(removing ? t("Relazione rimossa e conservata nello storico.") : t("Relazione aggiunta all’inventario."));
      loadInventoryRelations(); loadInventoryHistory(); refreshInventory();
    } catch (error) {
      if (!current()) return;
      const errorId = removing ? "inventory-relations-error" : "inventory-relation-error";
      if (error.status === 409) {
        state.inventoryRelationConflict = true;
        dialogError(errorId, new Error(inventoryMessage(error) + t(" I dati inseriti restano disponibili. Ricarica la scheda prima di riprovare.")));
        $("inventory-relation-reload").hidden = false;
      } else dialogError(errorId, new Error(inventoryMessage(error)));
    } finally {
      if (epoch === state.inventoryEpoch) { state.inventoryRelationBusy = false; busyDialogs.delete("inventory-detail-dialog"); if (current()) { inventoryRelationControls(); renderInventoryRelations(); } }
    }
  }

  buildInventoryTypedFields();
  inventoryShowTypeFields();
  $("inventory-relation-new").addEventListener("click", () => {
    if (!isInventoryManager() || state.inventoryRelationBusy || !state.inventoryDetailAsset || busyDialogs.has("inventory-detail-dialog")) return;
    inventoryRecipientSelections.relation = null; $("inventory-relation-recipient-kind").value = state.inventoryDetailAsset.details?.license_model === "per_user" ? "person" : "device";
    $("inventory-relation-form").hidden = false; inventoryRelationControls();
    inventoryPickers.get("inventory-relation-picker").load(); $("inventory-relation-type").focus();
  });
  $("inventory-relation-cancel").addEventListener("click", () => { if (!state.inventoryRelationBusy) $("inventory-relation-form").hidden = true; });
  $("inventory-relation-type").addEventListener("change", inventoryRelationControls);
  $("inventory-relation-recipient-kind").addEventListener("change", () => { inventoryRecipientSelections.relation = null; inventoryRelationControls(); });
  $("inventory-relations-retry").addEventListener("click", () => loadInventoryRelations(state.inventoryRelationsRetryMore));
  $("inventory-relations-more").addEventListener("click", () => loadInventoryRelations(true));
  $("inventory-relation-reload").addEventListener("click", reloadInventoryRelationSource);
  $("inventory-relation-form").addEventListener("submit", (event) => {
    event.preventDefault(); if (!isInventoryManager() || state.inventoryRelationBusy || state.inventoryRelationConflict) return;
    const type = $("inventory-relation-type").value, picker = inventoryPickers.get("inventory-relation-picker"), kind = type === "licensed_to" ? $("inventory-relation-recipient-kind").value : "device";
    picker.choice.setCustomValidity(kind === "device" && !picker.selected ? t("Seleziona un dispositivo destinatario.") : "");
    if (["person", "identity"].includes(kind) && !inventoryRecipientSelections.relation) { dialogError("inventory-relation-error", new Error(t("Seleziona un destinatario."))); return; }
    const inputs = ["assignee", "installed-version", "source-port", "target-port", "notes"];
    inputs.forEach((name) => { const input = $("inventory-relation-" + name); if (!input.disabled) { input.value = input.value.trim(); input.setCustomValidity(input.maxLength > 0 && new TextEncoder().encode(input.value).length > input.maxLength ? t("Accorcia il contenuto di questo campo.") : ""); } });
    if (!$("inventory-relation-form").reportValidity()) return;
    const fields = { relation_type: type, notes: $("inventory-relation-notes").value };
    if (kind === "legacy") fields.assignee = $("inventory-relation-assignee").value; else if (["person", "identity"].includes(kind)) fields[kind + "_id"] = String(inventoryRecipientSelections.relation.id); else fields.target_id = String(picker.selected.id);
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
      if (input.maxLength > 0 && new TextEncoder().encode(input.value).length > input.maxLength) input.setCustomValidity(t("Il contenuto è troppo lungo. Accorcialo: i caratteri accentati e i simboli possono occupare più spazio."));
    });
    const typedDetails = inventoryTypedValues();
    if (!$("inventory-form").reportValidity()) return;
    const fields = Object.fromEntries(inventoryFields.map((field) => [field, $("inventory-" + field).value]));
    fields.category = fields.category || "hardware";
    fields.asset_type = $("inventory-asset_type").value;
    fields.details = JSON.stringify(typedDetails);
    for (const field of ["purchase_date", "warranty_until"]) {
      if (fields[field] && !validDate(fields[field])) { dialogError("inventory-editor-error", new Error(t("Inserisci date valide."))); return; }
    }
    const editing = state.inventoryEditingAsset;
    if (state.inventoryEditingId && !editing) return;
    if (editing) fields.version = String(editing.version);
    const epoch = state.inventoryEpoch;
    const request = state.inventoryEditorRequest;
    const current = () => epoch === state.inventoryEpoch && request === state.inventoryEditorRequest && isInventoryManager();
    $("inventory-editor-error").hidden = true;
    dialogBusy("inventory-dialog", true, "inventory-save", editing ? t("Salva modifiche") : t("Crea asset"));
    try {
      await post("/api/inventory/assets" + (editing ? "/" + encodeURIComponent(editing.id) : ""), fields);
      if (!current()) return;
      $("inventory-dialog").close();
      showNotice(editing ? t("Scheda asset aggiornata. La versione precedente resta nello storico.") : t("Asset aggiunto all’inventario."));
      refreshInventory();
    } catch (error) {
      if (!current() || !$("inventory-dialog").open) return;
      if (error.status === 409 && editing && !/già presente/i.test(error.sourceMessage || error.message)) {
        state.inventoryConflict = true;
        dialogError("inventory-editor-error", new Error(inventoryMessage(error) + t(" Le tue modifiche sono ancora visibili. Copiale se vuoi conservarle, poi ricarica la versione aggiornata prima di salvare.")));
        $("inventory-editor-reload").textContent = t("Ricarica versione aggiornata e scarta le modifiche non salvate");
        $("inventory-editor-reload").hidden = false;
      } else dialogError("inventory-editor-error", new Error(inventoryMessage(error)));
    } finally {
      // Closing after a successful save invalidates editor requests; release its lock.
      if (epoch === state.inventoryEpoch && (request === state.inventoryEditorRequest || !$("inventory-dialog").open)) {
        dialogBusy("inventory-dialog", false, "inventory-save", editing ? t("Salva modifiche") : t("Crea asset"));
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
    state.inventoryDashboardRequest++; state.inventoryDashboard = null; state.inventoryDashboardLoading = false; state.inventoryDashboardError = "";
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
  $("inventory-mine").addEventListener("change", () => refreshInventory());
  $("inventory-site-filter").addEventListener("change", () => { renderInventoryPlaceFilters(); refreshInventory(); });
  $("inventory-department-filter").addEventListener("change", () => refreshInventory());
  $("inventory-verification-days").addEventListener("change", () => refreshInventory());
  $("inventory-attention-clear").addEventListener("click", () => { state.inventoryAttention = ""; refreshInventory(); });
  $("inventory-dashboard-retry").addEventListener("click", () => refreshInventoryDashboard());
  $("inventory-place-filter-retry").addEventListener("click", () => refreshInventoryPlaceFilters());
  $("inventory-asset_type").addEventListener("change", () => { inventoryShowTypeFields(); const picker = inventoryPickers.get("inventory-software-picker"); if ($("inventory-asset_type").value === "license" && !picker.items.length && !picker.loading) picker.load(); });
  $("inventory-clear").addEventListener("click", () => { inventorySubjectFilter = null; $("inventory-search-form").reset(); $("inventory-mine").checked = false; $("inventory-site-filter").value = ""; $("inventory-department-filter").value = ""; state.inventoryAttention = ""; $("inventory-search").setCustomValidity(""); refreshInventory(); $("inventory-search").focus(); });
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
    if (!isInventoryManager() || !id || busyDialogs.has("inventory-detail-dialog")) return;
    $("inventory-detail-dialog").close();
    openInventoryEditor(id);
  });
  $("inventory-detail-dialog").addEventListener("close", () => {
    state.inventoryDetailRequest++;
    state.inventoryHistoryRequest++;
    resetInventoryAssetSubjects();
    state.inventoryCustodyRequest++; state.inventoryVerificationsRequest++;
    state.inventoryCustodyLoading = false; state.inventoryVerificationsLoading = false;
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
    const placeholder = node("option", "", state.jobsReady ? state.jobs.length ? t("Seleziona una commessa") : t("Nessuna commessa abilitata") : t("Caricamento delle commesse…"));
    placeholder.value = "";
    $("work-job").replaceChildren(placeholder);
    for (const job of state.jobs) {
      const option = node("option", "", `${job.code} · ${job.name}`);
      option.value = String(job.id);
      $("work-job").append(option);
    }
    if (selected && state.jobsReady && !state.jobs.some((job) => String(job.id) === selected)) {
      resetWorkBoard(true);
      if (state.activeTab === "work-board") showNotice(t("La commessa selezionata non è più disponibile. Scegli una commessa abilitata."), true);
    }
    $("work-job").value = state.workJobId;
    $("work-job").disabled = !state.jobsReady || state.workBusy;
    $("work-refresh").disabled = !state.jobsReady || state.workBusy || state.workLoading;
    $("work-no-job-description").textContent = state.jobsReady && !state.jobs.length ? t("Un gestore deve abilitarti a una commessa per consultarne il lavoro.") : t("Seleziona una commessa per consultarne le attività e i blocchi.");
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
      if (String(data.job_id) !== jobId || !Array.isArray(data.tasks) || !Array.isArray(data.blockers)) throw new Error(t("La risposta non corrisponde alla commessa selezionata. Riprova."));
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
    return value && validDate(value) ? dateLabel(value) : t("Nessuna scadenza");
  }

  function workDetails(text) {
    const details = node("details", "work-card-details");
    details.append(node("summary", "", t("Leggi la descrizione")), node("p", "work-card-description", text));
    return details;
  }

  function workTaskCard(task) {
    const card = node("article", "work-task-card");
    card.dataset.taskId = String(task.id);
    card.tabIndex = -1;
    card.setAttribute("aria-label", task.title);
    const tags = node("div", "work-card-tags");
    tags.append(node("span", "work-card-id", `#${task.id}`), node("span", "work-card-tag", workTaskStates[task.status] || task.status), node("span", "work-card-tag priority-" + (Object.hasOwn(workPriorities, task.priority) ? task.priority : "normal"), t("Priorità ") + (workPriorities[task.priority] || task.priority)));
    card.append(tags, node("h4", "", task.title));
    if (task.parent_id) {
      const parent = state.workTasks.find((item) => String(item.id) === String(task.parent_id));
      card.append(node("p", "work-task-context", t("Sottoattività di ") + (parent ? parent.title : "#" + task.parent_id)));
    }
    card.append(node("p", "work-card-meta", t("Responsabile: ") + (task.assigned_user_name || t("Da assegnare"))), node("p", "work-card-meta", workDateText(task.due_date)));
    if (task.description) card.append(workDetails(task.description));
    const openBlockers = state.workBlockers.filter((item) => String(item.task_id) === String(task.id) && item.status !== "resolved").length;
    if (openBlockers) card.append(node("p", "work-card-meta", `${openBlockers} ${openBlockers === 1 ? t("blocco aperto") : t("blocchi aperti")}`));
    const actions = node("div", "work-card-actions");
    const timeJob = state.jobs.find((job) => String(job.id) === String(state.workJobId));
    if (timeJob && canLogTime(timeJob)) actions.append(action(t("Registra ore"), t("Registra ore per l’attività ") + task.title, () => openEntry(null, timeJob.id, task.id)));
    if (task.can_edit) {
      actions.append(action(t("Modifica"), t("Modifica l’attività ") + task.title, () => openWorkTask(task)));
      actions.append(action(t("+ Sottoattività"), t("Aggiungi una sottoattività a ") + task.title, () => openWorkTask(null, task.id)));
      actions.append(action(t("Elimina"), t("Elimina l’attività ") + task.title, () => openWorkDelete("task", task), true));
    }
    if (state.workPermissions.report_blockers) actions.append(action(t("Segnala blocco"), t("Segnala un blocco per ") + task.title, () => openWorkBlocker(null, task.id)));
    if (task.can_update_status) {
      const control = node("form", "work-task-status-control");
      const id = "work-status-task-" + task.id + "-" + $("work-view").value;
      const label = node("label", "", t("Stato")); label.htmlFor = id;
      const select = node("select"); select.id = id; select.setAttribute("aria-label", t("Stato dell’attività ") + task.title);
      for (const [value, text] of Object.entries(workTaskStates)) { const option = node("option", "", text); option.value = value; select.append(option); }
      select.value = task.status;
      const save = node("button", "row-button", t("Applica")); save.type = "submit"; save.disabled = true; save.setAttribute("aria-label", t("Applica lo stato all’attività ") + task.title);
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
    $("work-scope").textContent = state.workPermissions.manage_tasks ? t("Gestisci attività e responsabili. I collaboratori abilitati aggiornano lo stato delle attività assegnate e segnalano i blocchi.") : state.workPermissions.report_blockers ? t("Consulta il lavoro della commessa, aggiorna lo stato delle attività assegnate a te e segnala o gestisci i tuoi blocchi.") : t("Sola consultazione: puoi leggere attività e blocchi della commessa.");
    const query = $("work-search").value.trim().toLocaleLowerCase(locale);
    const status = $("work-status").value;
    const priority = $("work-priority").value;
    const tasks = state.workTasks.filter((task) => (!status || task.status === status) && (!priority || task.priority === priority) && [task.title, task.description, task.assigned_user_name].some((value) => String(value || "").toLocaleLowerCase(locale).includes(query)));
    const done = state.workTasks.filter((task) => task.status === "done").length;
    $("work-summary").textContent = t("{0} attività visibili su {1} · {2} completate nella commessa", [tasks.length, state.workTasks.length, done]);
    const listView = $("work-view").value === "list";
    $("work-task-board").replaceChildren();
    $("work-task-list").replaceChildren();
    $("work-task-board").hidden = listView || !tasks.length;
    $("work-task-list").hidden = !listView || !tasks.length;
    $("work-tasks-empty").hidden = Boolean(tasks.length);
    $("work-tasks-empty-title").textContent = state.workTasks.length ? t("Nessuna attività corrisponde ai filtri.") : t("Il prossimo passo prende forma qui.");
    $("work-tasks-empty-description").textContent = state.workTasks.length ? t("Cambia la ricerca, lo stato o la priorità per ritrovare le attività.") : state.workPermissions.manage_tasks ? t("Crea la prima attività, assegna un responsabile e scegli una scadenza.") : t("Il gestore può creare la prima attività per questa commessa.");
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
        if (!items.length) column.append(node("p", "work-list-empty", t("Nessuna attività")));
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
      tags.append(node("span", "work-card-id", "#" + blocker.id), node("span", "work-card-tag", workBlockerStates[blocker.status] || blocker.status), node("span", "work-card-tag impact-" + (Object.hasOwn(workImpacts, blocker.impact) ? blocker.impact : "medium"), t("Impatto ") + (workImpacts[blocker.impact] || blocker.impact)));
      item.append(tags, node("h4", "", blocker.title));
      const task = state.workTasks.find((task) => String(task.id) === String(blocker.task_id));
      item.append(node("p", "work-card-meta", task ? t("Attività: ") + task.title : t("Blocco della commessa")), node("p", "work-card-meta", t("Responsabile: ") + (blocker.owner_name || t("Da assegnare")) + " · " + workDateText(blocker.due_date)));
      if (blocker.description) item.append(workDetails(blocker.description));
      if (blocker.next_action) { const next = node("p", "work-blocker-action"); next.append(node("strong", "", t("PROSSIMA AZIONE")), document.createTextNode(blocker.next_action)); item.append(next); }
      const actions = node("div", "work-card-actions");
      if (blocker.can_edit) actions.append(action(t("Gestisci"), t("Gestisci il blocco ") + blocker.title, () => openWorkBlocker(blocker)));
      if (state.workPermissions.manage_tasks) actions.append(action(t("Elimina"), t("Elimina il blocco ") + blocker.title, () => openWorkDelete("blocker", blocker), true));
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
      const unavailable = node("option", "", (currentLabel || "#" + value) + t(" · accesso da verificare")); unavailable.value = String(value); select.append(unavailable);
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
    $("work-task-dialog-title").textContent = task ? t("Modifica attività") : parentId ? t("Nuova sottoattività") : t("Nuova attività");
    $("work-task-save").textContent = task ? t("Salva modifiche") : t("Crea attività");
    const job = state.jobs.find((job) => String(job.id) === state.workJobId);
    $("work-task-dialog-description").textContent = t("{0} · {1}. Ogni attività ha un avanzamento indipendente dalle sue sottoattività.", [job?.code || "", job?.name || ""]);
    ["title", "description", "status", "priority"].forEach((field) => { $("work-task-" + field).value = task?.[field] || (field === "status" ? "todo" : field === "priority" ? "normal" : ""); });
    $("work-task-due").value = task?.due_date || "";
    workSelect("work-task-assignee", state.workAssignees, task?.assigned_user_id, t("Da assegnare"), task?.assigned_user_name);
    const excluded = new Set(task ? [String(task.id)] : []);
    let changed = true;
    while (changed) { changed = false; for (const item of state.workTasks) if (excluded.has(String(item.parent_id)) && !excluded.has(String(item.id))) { excluded.add(String(item.id)); changed = true; } }
    workSelect("work-task-parent", state.workTasks.filter((item) => !excluded.has(String(item.id))), task?.parent_id || parentId, t("Nessuna · attività principale"));
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
    $("work-blocker-dialog-title").textContent = blocker ? t("Gestisci blocco") : t("Segnala blocco");
    $("work-blocker-dialog-description").textContent = t("Descrivi l’impedimento e il prossimo passo per risolverlo. La risoluzione del blocco non cambia automaticamente lo stato dell’attività.");
    $("work-blocker-save").textContent = blocker ? t("Salva modifiche") : t("Segnala blocco");
    ["title", "description", "impact", "status"].forEach((field) => { $("work-blocker-" + field).value = blocker?.[field] || (field === "impact" ? "medium" : field === "status" ? "open" : ""); });
    $("work-blocker-action").value = blocker?.next_action || "";
    $("work-blocker-due").value = blocker?.due_date || "";
    workSelect("work-blocker-task", state.workTasks, blocker?.task_id || taskId, t("Intera commessa"));
    workSelect("work-blocker-owner", state.workAssignees, blocker?.owner_id, t("Da assegnare"), blocker?.owner_name);
    $("work-blocker-owner-field").hidden = !state.workPermissions.manage_tasks;
    $("work-blocker-owner-help").hidden = state.workPermissions.manage_tasks;
    $("work-blocker-owner-help").textContent = blocker ? t("Responsabile: ") + (blocker.owner_name || t("Da assegnare")) + ". Un gestore può modificare il responsabile." : t("Il blocco sarà assegnato a te. Un gestore può modificare il responsabile.");
    $("work-blocker-dialog").showModal();
    $("work-blocker-title").focus();
  }

  function openWorkDelete(kind, item) {
    if (!state.workReady || !state.workPermissions.manage_tasks || state.workBusy || busyDialogs.has("work-delete-dialog")) return;
    if (kind === "task" && state.workTasks.some((task) => String(task.parent_id) === String(item.id))) { showNotice(t("Questa attività contiene sottoattività. Spostale o eliminale prima di eliminare l’attività principale."), true); return; }
    state.workDeleting = { kind, item };
    $("work-delete-title").textContent = kind === "task" ? t("Eliminare questa attività?") : t("Eliminare questo blocco?");
    $("work-delete-description").textContent = t("“{0}” sarà eliminato definitivamente.", [item.title]) + (kind === "task" ? t(" Gli eventuali blocchi collegati resteranno nel registro della commessa. Le ore registrate non cambiano.") : t(" L’operazione non può essere annullata dall’app."));
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
      showNotice(t("Stato dell’attività aggiornato."));
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
    due.setCustomValidity(due.value && !validDate(due.value) ? t("Inserisci una data valida.") : "");
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
    const label = edited ? t("Salva modifiche") : taskKind ? t("Crea attività") : t("Segnala blocco");
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
      showNotice(taskKind ? edited ? t("Attività aggiornata.") : t("Attività creata.") : edited ? t("Blocco aggiornato.") : t("Blocco segnalato."));
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
    dialogBusy("work-delete-dialog", true, "work-delete-save", t("Elimina"));
    try {
      await post("/api/" + (kind === "task" ? "tasks" : "blockers") + "/" + item.id + "/delete", { revision: item.revision });
      if (!workContextCurrent(epoch, jobId)) return;
      state.auditReady = false;
      $("work-delete-dialog").close();
      reloadFocus = true;
      showNotice(kind === "task" ? t("Attività eliminata.") : t("Blocco eliminato."));
      await refreshWorkBoard();
    } catch (error) {
      if (!workContextCurrent(epoch, jobId)) return;
      if (error.status === 409) { $("work-delete-dialog").close(); reloadFocus = true; showNotice(error.message, true); await refreshWorkBoard(); }
      else dialogError("work-delete-error", error);
    } finally {
      dialogBusy("work-delete-dialog", false, "work-delete-save", t("Elimina"));
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
    if (can("manage_contracts") && !state.contractsReady) { dialogError("job-error", new Error(t("Ricarica l’elenco dei contratti prima di salvare."))); return; }
    if (!$("job-form").reportValidity()) return;
    $("job-error").hidden = true;
    const id = $("job-id").value;
    const fields = Object.fromEntries(["code", "name", "client", "description", "status"].map((field) => [field, $("job-" + field).value]));
    fields.is_common = $("job-common").checked ? "1" : "0";
    if (can("manage_contracts")) fields.contract_id = $("job-contract").value;
    const followWithEntry = state.openEntryAfterJob && !id;
    dialogBusy("job-dialog", true, "job-save", id ? t("Salva modifiche") : t("Crea commessa"));
    try {
      const result = await post("/api/jobs" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("job-dialog").close();
      showNotice(id ? t("Commessa aggiornata. Tutto al punto.") : t("Commessa creata. Un nuovo punto di partenza."));
      await refreshAll();
      if (followWithEntry && state.jobs.some((job) => String(job.id) === String(result.id))) openEntry(null, result.id);
    } catch (error) {
      dialogError("job-error", error);
    } finally {
      dialogBusy("job-dialog", false, "job-save", id ? t("Salva modifiche") : t("Crea commessa"));
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
    if (!/^([01]\d|2[0-3]):[0-5]\d$/.test($("alert-rule-time").value)) { dialogError("alert-rule-error", new Error(t("Scegli un orario valido, con ore e minuti."))); return; }
    let emails;
    try { emails = alertRecipientEmails(); } catch (error) {
      $("alert-rule-emails").setCustomValidity(error.message);
      $("alert-rule-emails").reportValidity();
      return;
    }
    const selected = [...$("alert-rule-users").querySelectorAll("input:checked")];
    if (selected.some((checkbox) => checkbox.dataset.unavailable === "1")) { dialogError("alert-rule-error", new Error(t("Deseleziona gli utenti non più attivi prima di salvare la regola."))); return; }
    if (selected.length > 50) { dialogError("alert-rule-error", new Error(t("Seleziona al massimo 50 utenti destinatari."))); return; }
    if (!selected.length && !emails.length) { dialogError("alert-rule-error", new Error(t("Scegli almeno un utente destinatario oppure inserisci un indirizzo email."))); return; }
    const id = state.editingAlertRule?.id;
    const email = state.session.email;
    const fields = { name: $("alert-rule-name").value, type: $("alert-rule-type").value, run_time: $("alert-rule-time").value, enabled: $("alert-rule-enabled").checked ? "1" : "0", recipient_user_ids: selected.map((checkbox) => checkbox.value).join(","), recipient_emails: emails.join(",") };
    if (fields.type === "inventory_due") fields.days_before = $("alert-rule-days").value;
    if (fields.type === "inventory_verification") fields.verification_days = $("alert-rule-verification-days").value;
    $("alert-rule-error").hidden = true;
    dialogBusy("alert-rule-dialog", true, "alert-rule-save", id ? t("Salva modifiche") : t("Crea regola"));
    try {
      await post("/api/alert-rules" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      if (!isAlertManager() || state.session.email !== email) return;
      state.auditReady = false;
      $("alert-rule-dialog").close();
      state.alertRulesRequest++;
      state.alertRulesLoading = false;
      showNotice(id ? t("Regola aggiornata.") : fields.enabled === "1" ? t("Regola creata. La prossima esecuzione è indicata nell’elenco.") : t("Regola creata e sospesa. Puoi attivarla o eseguire un controllo manuale."));
      await refreshAlertRules();
    } catch (error) {
      if (isAlertManager() && state.session.email === email) dialogError("alert-rule-error", error);
    } finally {
      dialogBusy("alert-rule-dialog", false, "alert-rule-save", id ? t("Salva modifiche") : t("Crea regola"));
    }
  });

  $("job-delete").addEventListener("click", openDeleteJob);
  $("job-delete-dialog").addEventListener("close", () => { state.deleteJob = null; });
  $("job-delete-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_jobs") || !state.deleteJob || busyDialogs.has("job-delete-dialog")) return;
    const job = state.deleteJob;
    $("job-delete-error").hidden = true;
    dialogBusy("job-delete-dialog", true, "job-delete-save", t("Elimina commessa e ore"));
    $("job-delete-save").textContent = t("Eliminazione…");
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
      showNotice(t("Commessa “{0}” eliminata insieme a tutte le ore e alle abilitazioni collegate.", [job.name]));
      await refreshAll();
    } catch (error) {
      dialogError("job-delete-error", error);
    } finally {
      dialogBusy("job-delete-dialog", false, "job-delete-save", t("Elimina commessa e ore"));
    }
  });

  $("entry-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if ($("entry-id").value && !requirePermission("edit_time_entries")) return;
    if (busyDialogs.has("entry-dialog")) return;
    if (state.entryTaskLoading) { dialogError("entry-error", new Error(t("Attendi il caricamento delle attività, poi riprova."))); return; }
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
    if (!validDate($("entry-date").value)) { dialogError("entry-error", new Error(t("Scegli una giornata lavorativa valida."))); return; }
    const id = $("entry-id").value;
    const fields = { job_id: $("entry-job").value, date: $("entry-date").value, minutes, note: $("entry-note").value.trim() };
    fields.task_id = $("entry-task").value;
    if (id && state.editingTimeEntry?.revision !== undefined) fields.revision = state.editingTimeEntry.revision;
    $("entry-error").hidden = true;
    dialogBusy("entry-dialog", true, "entry-save", id ? t("Salva modifiche") : t("Salva ore"));
    try {
      await post("/api/time-entries" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("entry-dialog").close();
      timeWorkflow.userId = ""; renderHoursTeam();
      state.selectedDate = fields.date;
      $("entry-date-filter").value = fields.date;
      selectTab("hours");
      showNotice(id ? t("Registrazione aggiornata.") : t("Ore salvate. Il tuo lavoro ha la sua traccia."));
      await refreshAll();
    } catch (error) {
      dialogError("entry-error", error);
    } finally {
      dialogBusy("entry-dialog", false, "entry-save", id ? t("Salva modifiche") : t("Salva ore"));
    }
  });

  $("delete-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("edit_time_entries")) return;
    if (busyDialogs.has("delete-dialog") || !state.deleteEntry) return;
    $("delete-error").hidden = true;
    dialogBusy("delete-dialog", true, "delete-save", t("Elimina registrazione"));
    $("delete-save").textContent = t("Eliminazione…");
    try {
      await api("/api/time-entries/" + encodeURIComponent(state.deleteEntry.id), { method: "DELETE" });
      state.auditReady = false;
      $("delete-dialog").close();
      state.deleteEntry = null;
      showNotice(t("Registrazione eliminata. Totali aggiornati."));
      await refreshAll();
    } catch (error) {
      dialogError("delete-error", error);
    } finally {
      dialogBusy("delete-dialog", false, "delete-save", t("Elimina registrazione"));
    }
  });

  $("hours-team-user").addEventListener("change", () => { timeWorkflow.userId = $("hours-team-user").value; renderHoursTeam(); refreshEntries(); });
  $("hours-team-retry").addEventListener("click", refreshHoursTeam);
  $("access-groups-form").addEventListener("submit", saveAccessProfiles);
  $("access-groups-retry").addEventListener("click", loadAccessProfiles);
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
        $("user-password").setCustomValidity(t("La password deve contenere da 8 a 1024 byte. Usa almeno 8 caratteri."));
        $("user-password").reportValidity();
        return;
      }
    }
    const fields = { name: $("user-name").value, role: $("user-role").value };
    if (id) fields.active = $("user-active").value;
    else { fields.email = $("user-email").value; fields.password = $("user-password").value; }
    const editingSelf = id && $("user-email").value === state.session.email;
    $("user-error").hidden = true;
    dialogBusy("user-dialog", true, "user-save", id ? t("Salva modifiche") : t("Crea utente"));
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
      showNotice(id ? t("Utente aggiornato.") : t("Utente creato. L’account è pronto per accedere."));
      if (can("manage_users")) await refreshUsers();
    } catch (error) {
      dialogError("user-error", error);
    } finally {
      fields.password = "";
      dialogBusy("user-dialog", false, "user-save", id ? t("Salva modifiche") : t("Crea utente"));
    }
  });

  $("assignment-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog") || !state.assignmentJob || !state.assignmentsReady) return;
    $("assignment-end").setCustomValidity("");
    const startsOn = $("assignment-start").value;
    const endsOn = $("assignment-end").value;
    if (startsOn && !validDate(startsOn) || endsOn && !validDate(endsOn)) { dialogError("assignment-error", new Error(t("Scegli date valide per il periodo di abilitazione."))); return; }
    if (startsOn && endsOn && startsOn > endsOn) $("assignment-end").setCustomValidity(t("La data finale deve essere uguale o successiva alla data iniziale."));
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
      showNotice(t("Abilitazione salvata. Ruolo e periodo di accesso alla commessa sono aggiornati."));
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
      showNotice(t("Abilitazione revocata. Le ore già registrate sono conservate."));
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
    if (!requireGroupEditing() || !state.selectedGroup) return;
    const group = state.selectedGroup;
    confirmOrganization(t("Eliminare il gruppo {0}? È possibile eliminare solo gruppi senza membri, sottogruppi o abilitazioni alle commesse.", [group.name]), async () => {
      await api(`/api/groups/${encodeURIComponent(group.id)}`, { method: "DELETE" });
      showNotice(t("Gruppo eliminato."));
      await refreshOrganization(null);
    });
  });
  $("organization-confirm-cancel").addEventListener("click", () => {
    if (state.organizationBusy) return;
    state.organizationConfirm = null;
    $("organization-confirm").hidden = true;
  });
  $("organization-confirm-save").addEventListener("click", async () => {
    if (!requireGroupEditing() || state.organizationBusy || !state.organizationConfirm) return;
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
    if (!requireGroupEditing() || state.organizationBusy || !state.selectedGroup || !state.groupMembersReady || !$("group-member-form").reportValidity()) return;
    const groupId = state.selectedGroup.id;
    const userId = $("group-member-user").value;
    $("group-members-error").hidden = true;
    setOrganizationBusy(true);
    try {
      await post(`/api/groups/${encodeURIComponent(groupId)}/members`, { user_id: userId });
      state.auditReady = false;
      showNotice(t("Utente aggiunto al gruppo. Riceve anche le abilitazioni dei gruppi padre."));
      await refreshOrganization(groupId);
      await refreshAll();
    } catch (error) { dialogError("group-members-error", error); }
    finally { setOrganizationBusy(false); }
  });
  $("group-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requireGroupEditing() || busyDialogs.has("group-dialog")) return;
    $("group-name").value = $("group-name").value.trim();
    if (!$("group-form").reportValidity()) return;
    const id = $("group-id").value;
    const fields = { name: $("group-name").value, parent_id: $("group-parent").value };
    $("group-error").hidden = true;
    dialogBusy("group-dialog", true, "group-save", id ? t("Salva modifiche") : t("Crea gruppo"));
    try {
      const data = await post("/api/groups" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("group-dialog").close();
      showNotice(id ? t("Gruppo aggiornato. Le abilitazioni ereditate seguono la nuova struttura.") : t("Gruppo creato. Puoi aggiungere i membri."));
      await refreshOrganization(id || data.id);
      await refreshAll();
    } catch (error) { dialogError("group-error", error); }
    finally { dialogBusy("group-dialog", false, "group-save", id ? t("Salva modifiche") : t("Crea gruppo")); }
  });
  $("job-role-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_job_roles") || busyDialogs.has("job-role-dialog")) return;
    ["job-role-code", "job-role-name"].forEach((id) => { $(id).value = $(id).value.trim(); });
    if (!$("job-role-form").reportValidity()) return;
    const id = $("job-role-id").value;
    const fields = { code: $("job-role-code").value, name: $("job-role-name").value, can_log_time: $("job-role-log-time").value };
    $("job-role-error").hidden = true;
    dialogBusy("job-role-dialog", true, "job-role-save", id ? t("Salva modifiche") : t("Crea ruolo"));
    try {
      await post("/api/job-roles" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("job-role-dialog").close();
      showNotice(id ? t("Ruolo aggiornato. Le abilitazioni che lo utilizzano sono aggiornate.") : t("Ruolo creato. Puoi assegnarlo agli utenti o ai gruppi nelle commesse."));
      await refreshOrganization();
      await refreshAll();
    } catch (error) { dialogError("job-role-error", error); }
    finally { dialogBusy("job-role-dialog", false, "job-role-save", id ? t("Salva modifiche") : t("Crea ruolo")); }
  });
  $("group-grant-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_assignments") || busyDialogs.has("assignment-dialog") || !state.assignmentJob || !state.assignmentsReady) return;
    $("group-grant-end").setCustomValidity("");
    const startsOn = $("group-grant-start").value;
    const endsOn = $("group-grant-end").value;
    if (startsOn && !validDate(startsOn) || endsOn && !validDate(endsOn)) { dialogError("assignment-error", new Error(t("Scegli date valide per il periodo di abilitazione del gruppo."))); return; }
    if (startsOn && endsOn && startsOn > endsOn) $("group-grant-end").setCustomValidity(t("La data finale deve essere uguale o successiva alla data iniziale."));
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
      showNotice(t("Gruppo abilitato. L’accesso vale anche per i membri dei suoi sottogruppi."));
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
      showNotice(t("Abilitazione del gruppo revocata. Le ore già registrate sono conservate."));
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
    dialogBusy("supplier-dialog", true, "supplier-save", id ? t("Salva modifiche") : t("Crea fornitore"));
    try {
      await post("/api/suppliers" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("supplier-dialog").close();
      showNotice(id ? t("Fornitore aggiornato.") : t("Fornitore censito. Puoi associarlo a un contratto."));
      await refreshRegistry("suppliers");
      if (can("manage_contracts") && state.contractsReady) await refreshRegistry("contracts");
    } catch (error) {
      dialogError("supplier-error", error);
    } finally {
      dialogBusy("supplier-dialog", false, "supplier-save", id ? t("Salva modifiche") : t("Crea fornitore"));
    }
  });

  $("contract-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_contracts") || busyDialogs.has("contract-dialog")) return;
    if (!state.suppliersReady) { dialogError("contract-error", new Error(t("Ricarica l’elenco dei fornitori prima di salvare."))); return; }
    ["code", "title", "notes"].forEach((key) => { $("contract-" + key).value = $("contract-" + key).value.trim(); });
    $("contract-value").setCustomValidity("");
    $("contract-ends_on").setCustomValidity("");
    let cents;
    try { cents = valueToCents($("contract-value").value); } catch (error) { $("contract-value").setCustomValidity(error.message); }
    const startsOn = $("contract-starts_on").value;
    const endsOn = $("contract-ends_on").value;
    if (startsOn && !validDate(startsOn) || endsOn && !validDate(endsOn)) { dialogError("contract-error", new Error(t("Scegli date valide per il contratto."))); return; }
    if (startsOn && endsOn && startsOn > endsOn) $("contract-ends_on").setCustomValidity(t("La data finale deve essere uguale o successiva alla data iniziale."));
    if (!$("contract-form").reportValidity()) return;
    const id = $("contract-id").value;
    const fields = { code: $("contract-code").value, title: $("contract-title").value, supplier_id: $("contract-supplier").value, status: $("contract-status").value, starts_on: startsOn, ends_on: endsOn, value_cents: cents, currency: "EUR", notes: $("contract-notes").value };
    $("contract-error").hidden = true;
    dialogBusy("contract-dialog", true, "contract-save", id ? t("Salva modifiche") : t("Crea contratto"));
    try {
      await post("/api/contracts" + (id ? "/" + encodeURIComponent(id) : ""), fields);
      state.auditReady = false;
      $("contract-dialog").close();
      showNotice(id ? t("Contratto aggiornato.") : t("Contratto censito. Usa il pulsante PDF per archiviare i documenti originali."));
      await refreshRegistry("contracts");
      await refreshAll();
    } catch (error) {
      dialogError("contract-error", error);
    } finally {
      dialogBusy("contract-dialog", false, "contract-save", id ? t("Salva modifiche") : t("Crea contratto"));
    }
  });

  $("document-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requirePermission("manage_contracts") || busyDialogs.has("documents-dialog") || !state.documentsContract) return;
    $("document-file").setCustomValidity("");
    const file = $("document-file").files[0];
    if (file && !/\.pdf$/i.test(file.name)) $("document-file").setCustomValidity("Seleziona un documento con estensione .pdf.");
    if (file && (file.size === 0 || file.size > 10 * 1024 * 1024)) $("document-file").setCustomValidity(t("Il documento deve avere una dimensione da 1 byte a 10 MiB."));
    if (!$("document-form").reportValidity() || !file) return;
    const id = state.documentsContract.id;
    $("documents-error").hidden = true;
    dialogBusy("documents-dialog", true, "document-save", t("Carica PDF"));
    $("document-save").textContent = t("Caricamento…");
    try {
      await api(`/api/contracts/${encodeURIComponent(id)}/documents?filename=${encodeURIComponent(file.name)}`, { method: "POST", headers: { "Content-Type": "application/pdf" }, body: file, timeoutMs: 60000 });
      if (!can("manage_contracts") || !state.documentsContract || String(state.documentsContract.id) !== String(id)) return;
      state.auditReady = false;
      $("document-form").reset();
      showNotice(t("PDF archiviato. Il documento originale è disponibile per il download."));
      await loadDocuments();
      await refreshRegistry("contracts");
    } catch (error) {
      if (state.documentsContract && String(state.documentsContract.id) === String(id)) dialogError("documents-error", error);
    } finally {
      dialogBusy("documents-dialog", false, "document-save", t("Carica PDF"));
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
    if (!can("manage_labor_rates") || busyDialogs.has("labor-rate-dialog")) return;
    const editing = state.editingLaborRate;
    $("labor-rate-hourly").setCustomValidity("");
    $("labor-rate-to").setCustomValidity("");
    let cents;
    try {
      cents = valueToCents($("labor-rate-hourly").value);
      if (cents === "" || BigInt(cents) > 99999999n) throw new Error(t("Inserisci una tariffa da 0 a 999.999,99 EUR, con al massimo due decimali."));
    } catch (error) { $("labor-rate-hourly").setCustomValidity(error.message); }
    const from = $("labor-rate-from").value;
    const to = $("labor-rate-to").value;
    if (!validDate(from) || to && !validDate(to)) { dialogError("labor-rate-error", new Error(t("Scegli date valide per la tariffa."))); return; }
    if (to && from > to) $("labor-rate-to").setCustomValidity(t("La data finale deve essere uguale o successiva alla data iniziale."));
    if (!$("labor-rate-form").reportValidity()) return;
    const fields = { user_id: editing ? String(editing.user_id) : $("labor-rate-user").value, hourly_cents: cents, starts_on: from, ends_on: to, currency: "EUR" };
    $("labor-rate-error").hidden = true;
    dialogBusy("labor-rate-dialog", true, "labor-rate-save", editing ? t("Salva modifiche") : t("Crea tariffa"));
    try {
      await post("/api/labor-rates" + (editing ? "/" + encodeURIComponent(editing.id) : ""), fields);
      if (!can("manage_labor_rates")) return;
      state.auditReady = false;
      $("labor-rate-dialog").close();
      showNotice(editing ? t("Tariffa aggiornata. Le stime sono state ricalcolate.") : t("Tariffa creata. Le ore nel periodo di validità sono ora valorizzate."));
      await Promise.all([refreshReports(), refreshLaborRates()]);
    } catch (error) {
      if (can("manage_labor_rates") && $("labor-rate-dialog").open) dialogError("labor-rate-error", error);
    } finally {
      dialogBusy("labor-rate-dialog", false, "labor-rate-save", editing ? t("Salva modifiche") : t("Crea tariffa"));
      $("labor-rate-user").disabled = Boolean(editing) || !can("manage_labor_rates");
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
    $("documents-dialog-title").textContent = t("Documenti del contratto");
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
    $("job-dialog-description").textContent = t("Definisci i dettagli della commessa. Potrai aggiornarli in ogni momento.");
  });
  $("user-dialog").addEventListener("close", () => { $("user-form").reset(); $("user-password").value = ""; $("user-password").setCustomValidity(""); });
  $("user-password").addEventListener("input", () => $("user-password").setCustomValidity(""));
  $("entry-hours").addEventListener("input", () => { $("entry-hours").setCustomValidity(""); $("entry-hours").removeAttribute("aria-invalid"); });
  $("entry-job").addEventListener("change", () => loadEntryTasks());
  $("entry-task-retry").addEventListener("click", () => loadEntryTasks(state.entryTaskDesired || ""));
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
  $("jobs-show-archived").addEventListener("change", renderJobs);
  $("my-work-refresh").addEventListener("click", refreshMyWork);
  $("my-work-log").addEventListener("click", () => { state.selectedDate = localDate(new Date()); $("entry-date-filter").value = state.selectedDate; selectTab("hours"); openEntry(); });
  $("my-work-assets").addEventListener("click", () => { $("inventory-search-form").reset(); $("inventory-mine").checked = true; state.inventoryAttention = ""; inventorySubjectFilter = null; selectTab("inventory"); refreshInventory(); });
  $("my-work-corrections").addEventListener("click", () => { selectTab("hours"); $("time-corrections-title").tabIndex = -1; $("time-corrections-title").focus(); $("time-corrections-title").scrollIntoView({ block: "start" }); });
  $("my-work-notifications").addEventListener("click", () => selectTab("notifications"));
  $("job-overview-retry").addEventListener("click", loadJobOverview);
  $("job-overview-dialog").addEventListener("close", () => { workspaceFlows.overviewRequest++; workspaceFlows.overviewJob = null; });
  $("job-access-user").addEventListener("change", loadJobAccess);
  $("job-access-retry").addEventListener("click", loadJobAccess);
  $("job-access-dialog").addEventListener("close", () => { workspaceFlows.accessRequest++; workspaceFlows.accessJob = null; });
  $("job-wizard-open").addEventListener("click", openJobWizard);
  $("job-wizard-retry").addEventListener("click", loadJobWizardCatalogues);
  $("job-wizard-template-apply").addEventListener("click", applyJobWizardTemplate);
  $("job-wizard-user-add").addEventListener("click", () => addWizardTeam("user"));
  $("job-wizard-group-add").addEventListener("click", () => addWizardTeam("group"));
  $("job-wizard-task-add").addEventListener("click", addWizardTask);
  $("job-wizard-next").addEventListener("click", () => { const wizard = workspaceFlows.wizard; if (wizard.ready && wizard.step < 3 && wizardValidateStep(wizard.step)) { wizard.step++; renderJobWizardStep(); $("job-wizard-step-" + wizard.step).querySelector("input,select,button")?.focus(); } });
  $("job-wizard-back").addEventListener("click", () => { if (workspaceFlows.wizard.step > 0 && !workspaceFlows.wizard.createdId) { workspaceFlows.wizard.step--; renderJobWizardStep(); } });
  $("job-wizard-form").addEventListener("submit", saveJobWizard);
  $("job-wizard-dialog").addEventListener("close", () => { workspaceFlows.wizard.request++; });
  $("work-schedules-refresh").addEventListener("click", refreshWorkSchedules);
  $("work-schedules-user").addEventListener("change", refreshWorkSchedules);
  $("work-schedule-add").addEventListener("click", () => openWorkSchedule("schedule"));
  $("work-exception-add").addEventListener("click", () => openWorkSchedule("exception"));
  $("work-holiday-add").addEventListener("click", () => openWorkSchedule("holiday"));
  $("work-schedule-form").addEventListener("submit", saveWorkSchedule);
  $("work-schedule-delete-form").addEventListener("submit", deleteWorkSchedule);
  $("work-notifications-refresh").addEventListener("click", refreshWorkNotifications);
  $("hours-view-day").addEventListener("click", () => { if (timeWorkflow.view !== "day") { timeWorkflow.view = "day"; updateTimeWorkflowControls(); refreshEntries(); } });
  $("hours-view-week").addEventListener("click", () => { if (timeWorkflow.view !== "week") { timeWorkflow.view = "week"; updateTimeWorkflowControls(); refreshEntries(); } });
  $("hours-batch-open").addEventListener("click", () => openHoursBatch());
  $("hours-copy-week").addEventListener("click", copyPreviousHoursWeek);
  $("hours-batch-add").addEventListener("click", () => { if (busyDialogs.has("hours-batch-dialog") || timeWorkflow.batchRows.length >= 100) return; timeWorkflow.batchRows.push({ job_id: "", date: state.selectedDate, minutes: 60, note: "", key: ++timeWorkflow.batchSequence }); renderHoursBatch(); $("hours-batch-rows").lastElementChild?.querySelector("select")?.focus(); });
  $("hours-batch-form").addEventListener("submit", saveHoursBatch);
  $("time-corrections-refresh").addEventListener("click", refreshTimeCorrections);
  $("time-correction-form").addEventListener("submit", saveTimeCorrection);
  $("time-correction-delete").addEventListener("change", () => { $("time-correction-fields").disabled = $("time-correction-delete").checked; });
  $("time-correction-hours").addEventListener("input", () => $("time-correction-hours").setCustomValidity(""));
  $("time-decision-form").addEventListener("submit", saveTimeDecision);
  $("job-lifecycle-form").addEventListener("submit", saveJobLifecycle);
  $("job-lifecycle-retry").addEventListener("click", loadJobLifecycle);
  $("job-lifecycle-action").addEventListener("change", updateJobLifecycleHelp);
  $("job-lifecycle-dialog").addEventListener("close", () => { timeWorkflow.lifecycleRequest++; timeWorkflow.lifecycle = null; timeWorkflow.lifecycleJob = null; });
  $("clear-filters").addEventListener("click", () => { $("job-search").value = ""; $("job-filter").value = ""; renderJobs(); $("job-search").focus(); });
  $("entry-date-filter").value = state.selectedDate;
  $("entry-date-filter").addEventListener("change", () => {
    if (validDate($("entry-date-filter").value)) setDate($("entry-date-filter").value);
    else $("entry-date-filter").value = state.selectedDate;
  });
  $("previous-day").addEventListener("click", () => moveDay(timeWorkflow.view === "week" ? -7 : -1));
  $("next-day").addEventListener("click", () => moveDay(timeWorkflow.view === "week" ? 7 : 1));
  $("go-today").addEventListener("click", () => setDate(localDate(new Date())));
  $("calendar-ticket-job").addEventListener("change", renderCalendarTickets);
  $("calendar-refresh").addEventListener("click", () => refreshCalendar(true));
  $("calendar-connect").addEventListener("click", () => calendarAction("oauth/start"));
  $("calendar-sync").addEventListener("click", () => calendarAction("sync"));
  $("calendar-disconnect").addEventListener("click", () => $("calendar-disconnect-dialog").showModal());
  $("calendar-disconnect-confirm").addEventListener("click", () => calendarAction("disconnect"));
  $("calendar-selection").addEventListener("submit", (event) => { event.preventDefault(); if ($("calendar-choice").value) calendarAction("selection", { calendar_id: $("calendar-choice").value }); });
  $("calendar-choice").addEventListener("change", renderCalendarStatus);
  $("calendar-calendars-more").addEventListener("click", async () => { if (calendar.busy || !calendar.calendarCursor) return; calendar.busy = true; renderCalendarStatus(); try { await loadCalendarCatalog(calendar.request, true); } catch (error) { $("calendar-error").textContent = calendarError(error); $("calendar-error").hidden = false; } finally { calendar.busy = false; renderCalendarStatus(); } });
  $("calendar-events-more").addEventListener("click", async () => { if (calendar.loading || !calendar.cursor) return; calendar.loading = true; $("calendar-events-more").disabled = true; try { await loadCalendarEvents(calendar.request, true); } catch (error) { $("calendar-error").textContent = calendarError(error); $("calendar-error").hidden = false; } finally { calendar.loading = false; $("calendar-events-more").disabled = false; } });
  function changeCalendarWeek(days, selected = null) {
    calendar.request++; calendar.loading = false; calendar.events = []; calendar.cursor = null;
    const next = new Date(calendar.date + "T12:00:00"); next.setDate(next.getDate() + days);
    calendar.date = selected || localDate(next); renderCalendarEvents(); refreshCalendar();
  }
  $("calendar-previous").addEventListener("click", () => changeCalendarWeek(-7));
  $("calendar-next").addEventListener("click", () => changeCalendarWeek(7));
  $("calendar-today").addEventListener("click", () => changeCalendarWeek(0, localDate(new Date())));
  $("calendar-date").addEventListener("change", () => { if (validDate($("calendar-date").value)) changeCalendarWeek(0, $("calendar-date").value); });
  $("ticket-calendar-consent-allow").addEventListener("click", async () => { if (!calendar.consentState || state.ticketConnecting) return; $("ticket-calendar-consent-allow").disabled = true; busyDialogs.add("ticket-calendar-consent"); try { await connectTicketSession(calendar.consentState, true); } finally { busyDialogs.delete("ticket-calendar-consent"); $("ticket-calendar-consent-allow").disabled = false; } });
  $("ticket-calendar-consent").addEventListener("close", () => { calendar.consentState = null; if (location.hash.startsWith("#ticket-connect")) { history.replaceState(null, "", "#calendar"); state.routeHash = location.hash; } });
  window.setInterval(() => { if (state.session?.authenticated && state.activeTab === "calendar" && calendar.status?.connected && !document.hidden && !calendar.loading && !calendar.busy) refreshCalendar(); }, 10000);
  $("notice-close").addEventListener("click", () => { $("notice").hidden = true; clearTimeout(noticeTimer); });
  $("retry-load").addEventListener("click", refreshAll);
  $("logout").addEventListener("click", async () => {
    $("logout").disabled = true;
    try { await post("/api/logout", {}); window.location.replace("/"); } catch (error) { showNotice(error.message, true); $("logout").disabled = false; }
  });
  $("current-year").textContent = new Date().getFullYear();
  const mobileNavigation = window.matchMedia(t("(max-width: 760px)"));
  const updateTabOrientation = () => document.querySelector(".app-navigation").setAttribute("aria-orientation", mobileNavigation.matches ? "horizontal" : "vertical");
  updateTabOrientation();
  mobileNavigation.addEventListener("change", updateTabOrientation);
  window.setInterval(() => { if (state.session?.authenticated && !document.hidden) { refreshNotifications(); refreshWorkNotifications(); } }, 60000);

  async function initialize() {
    try {
      if (!await refreshSession()) return;
      await loadIdentityManagement();
      $("session-status").hidden = true;
      $("workspace").hidden = false;
      try {
        const pending = JSON.parse(sessionStorage.getItem("punto.ticket.return") || "null");
        sessionStorage.removeItem("punto.ticket.return");
        if (!location.hash && pending && Date.now() - pending.at < 600000 && (/^#ticket-connect\?state=[a-f0-9]{64}(?:&calendar=1)?$/.test(pending.hash) || /^#(?:jobs\?job_id|inventory\?asset_id)=[1-9]\d*$/.test(pending.hash) || pending.hash === "#calendar")) history.replaceState(null, "", location.pathname + pending.hash);
      } catch { /* Il ripristino del collegamento è facoltativo. */ }
      const calendarCallback = new URL(location.href);
      if (calendarCallback.searchParams.has("calendar")) {
        const success = calendarCallback.searchParams.get("calendar") === "connected";
        calendarCallback.searchParams.delete("calendar"); calendarCallback.hash = "calendar";
        history.replaceState(null, "", calendarCallback);
        showNotice(success ? t("Google Calendar collegato. Scegli il calendario da sincronizzare.") : t("Collegamento Google non completato. Riprova dalla sezione Calendario."), !success);
      }
      applyReportRoute();
      await refreshAll();
    } catch (error) {
      $("session-status").textContent = error.message + t(" Ricarica la pagina per riprovare.");
    }
  }
  initialize();
})();
