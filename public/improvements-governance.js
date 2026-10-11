(() => {
  "use strict";
  function initialize() {
    const P = window.PuntoExtensions;
    if (!P || window.PuntoGovernance) return;
    const $ = id => document.getElementById(id), t = P.t;
    const route = name => "/api/improvements/governance/" + name;
    let epoch = 0, identity = "", people = [], groups = [], requests = [], delegations = [], status = null;
    let loading = false, reload = false;
    const decisionDrafts = new Map();
    const el = (tag, text, cls) => { const n = document.createElement(tag); if (text !== undefined) n.textContent = text; if (cls) n.className = cls; return n; };
    const button = (text, action) => { const n = el("button", text, "row-button"); n.type = "button"; n.addEventListener("click", action); return n; };
    const field = (parent, label, id, type = "text") => { const wrap = el("div", undefined, "ig-field"), lab = el("label", label), input = el(type === "select" ? "select" : type === "textarea" ? "textarea" : "input"); input.id = id; lab.htmlFor = id; if (input.tagName === "INPUT") input.type = type; wrap.append(lab, input); parent.append(wrap); return input; };
    const option = (select, value, text) => { const n = el("option", text); n.value = value; select.append(n); };
    const selectValues = (select, rows, label, value = "id") => { const current = select.value; select.replaceChildren(); option(select, "", label); for (const row of rows) option(select, row[value], row.name || row.user_name || row.email); select.value = current; };
    const section = (parent, id, title, help) => { const box = el("section", undefined, "ig-section"); box.id = id; box.append(el("h3", title)); if (help) box.append(el("p", help, "field-help")); const error = el("p", "", "ig-error"); error.hidden = true; error.setAttribute("role", "alert"); box.append(error); parent?.append(box); return box; };
    const error = (box, message) => { const n = box.querySelector(".ig-error"); n.hidden = !message; n.textContent = message || ""; };
    const run = async (box, work) => { error(box, ""); const controls = [...box.querySelectorAll("button")]; controls.forEach(n => n.disabled = true); const scope = epoch; try { await work(); } catch (failure) { if (scope === epoch) error(box, failure.message || String(failure)); } finally { if (scope === epoch) controls.forEach(n => n.disabled = false); } };
    const admin = () => Boolean(P.session()?.permissions?.manage_space);
    const manager = () => P.session()?.role === "manager";
    const personId = () => Number(P.session()?.user_id || P.session()?.user?.id || 0);
    const authenticated = () => Boolean(P.session()?.authenticated);
    const today = () => P.localDate ? P.localDate(new Date()) : new Date().toLocaleDateString("en-CA");
    const name = id => people.find(person => Number(person.id) === Number(id))?.name || "#" + id;
    const jobName = id => P.getState().jobs?.find(job => Number(job.id) === Number(id))?.name || "#" + id;
    const reason = (parent, id) => { const input = field(parent, t("Motivo"), id, "textarea"); input.maxLength = 2000; input.required = true; input.rows = 2; return input; };

    const adminBox = section($("panel-users"), "ig-admins", t("Amministrazione dello spazio"), t("L’amministratore gestisce lo spazio e attribuisce le deleghe. Un nuovo Gestore area riceve solo i poteri delegati. Le ore restano limitate alla propria squadra e ai sottogruppi."));
    const adminForm = el("form", undefined, "ig-grid"); adminForm.id = "ig-admin-form";
    const adminUser = field(adminForm, t("Gestore attivo"), "ig-admin-user", "select"); adminUser.required = true;
    const adminAction = field(adminForm, t("Amministrazione"), "ig-admin-action", "select"); option(adminAction, "1", t("Attribuisci amministrazione")); option(adminAction, "0", t("Revoca amministrazione"));
    const adminReason = reason(adminForm, "ig-admin-reason"), adminSave = el("button", t("Salva amministrazione"), "row-button"); adminSave.type = "submit"; adminForm.append(adminSave); adminBox.append(adminForm);
    const adminList = el("ul", undefined, "ig-list"); adminList.id = "ig-admin-list"; adminBox.append(adminList);
    adminForm.addEventListener("submit", event => { event.preventDefault(); if (!adminForm.reportValidity()) return; run(adminBox, async () => { await P.post(route("admins"), { user_id: adminUser.value, admin: adminAction.value, reason: adminReason.value.trim() }); adminReason.value = ""; await P.refreshAll(); await load(); }); });

    const requestBox = section($("panel-users"), "ig-profile-requests", t("Approvazioni Commerciali e Finance"), t("Ogni modifica dei profili sensibili, anche la propria, richiede un secondo amministratore. L’eccezione per un unico amministratore attivo è esplicita e registrata nello storico."));
    const bootstrapNote = el("p", "", "ig-notice"); bootstrapNote.id = "ig-bootstrap-note"; bootstrapNote.setAttribute("role", "status");
    const requestList = el("ul", undefined, "ig-list"); requestList.id = "ig-profile-request-list"; requestBox.append(bootstrapNote, button(t("Aggiorna approvazioni"), () => load()), requestList);
    const statusLabel = value => ({ pending: t("In attesa"), approved: t("Approvata"), rejected: t("Rifiutata"), cancelled: t("Annullata"), conflict: t("Conflitto") }[value] || value);
    function renderRequests() {
      // Drafts belong to this account/authorization epoch, never local storage.
      const pendingIds = new Set(requests.filter(request => request.status === "pending").map(request => String(request.id)));
      for (const id of decisionDrafts.keys()) if (!pendingIds.has(id)) decisionDrafts.delete(id);
      const focused = document.activeElement;
      const focus = focused?.tagName === "TEXTAREA" && requestList.contains(focused)
        ? { id: focused.id, start: focused.selectionStart, end: focused.selectionEnd, direction: focused.selectionDirection } : null;
      bootstrapNote.textContent = status?.bootstrap_single_admin ? t("Eccezione bootstrap attiva: sei l’unico amministratore attivo. Le modifiche dei profili vengono applicate e tracciate immediatamente.") : t("Doppia approvazione attiva: chi propone non può approvare la propria richiesta.");
      requestList.replaceChildren();
      for (const request of requests) {
        const item = el("li", undefined, "ig-item");
        const codes = (request.groups || []).map(code => code === "sales" ? t("Commerciali") : t("Finance"));
        item.append(el("strong", request.user_name + " · " + (codes.join(", ") || t("Nessun profilo sensibile"))), el("p", statusLabel(request.status) + " · " + t("Proponente") + ": " + request.requester_name + " · " + request.created_at), el("p", request.reason));
        if (request.bootstrap) item.append(el("p", t("Eccezione bootstrap registrata"), "field-help"));
        if (request.decision_reason) item.append(el("p", t("Decisione") + ": " + request.decision_reason, "field-help"));
        if (request.status === "pending") {
          const draftId = String(request.id);
          const decisionReason = reason(item, "ig-profile-reason-" + request.id);
          decisionReason.value = decisionDrafts.get(draftId) || "";
          decisionReason.addEventListener("input", () => decisionDrafts.set(draftId, decisionReason.value));
          const actions = el("div", undefined, "ig-actions");
          const decide = action => run(requestBox, async () => { if (!decisionReason.value.trim()) { decisionReason.reportValidity(); throw new Error(t("Inserisci un motivo per la decisione.")); } const scope = epoch; await P.post(route("profile-requests/" + request.id + "/decision"), { action, reason: decisionReason.value.trim() }); if (scope !== epoch) return; decisionDrafts.delete(draftId); await P.refreshAll(); await load(); });
          if (Number(request.requester_id) === personId()) actions.append(button(t("Annulla richiesta"), () => decide("cancel")));
          else actions.append(button(t("Approva"), () => decide("approve")), button(t("Rifiuta"), () => decide("reject")));
          item.append(actions);
        }
        requestList.append(item);
      }
      if (!requests.length) requestList.append(el("li", t("Nessuna richiesta di profili."), "field-help"));
      if (focus) {
        const replacement = $(focus.id);
        if (replacement && requestList.contains(replacement)) {
          replacement.focus({ preventScroll: true });
          replacement.setSelectionRange(focus.start, focus.end, focus.direction);
        }
      }
    }

    const delegationBox = section($("panel-organization"), "ig-delegations", t("Deleghe di area"), t("Le capacità valgono nel gruppo scelto e nei suoi sottogruppi, entro date inclusive. La delega alle rettifiche richiede anche l’appartenenza corrente alla squadra del richiedente e impedisce l’autoapprovazione."));
    const delegationForm = el("form", undefined, "ig-grid"); delegationForm.id = "ig-delegation-form";
    const delegationUser = field(delegationForm, t("Gestore attivo"), "ig-delegation-user", "select"), delegationGroup = field(delegationForm, t("Area organizzativa"), "ig-delegation-group", "select"); delegationUser.required = true; delegationGroup.required = true;
    const caps = [["can_manage_jobs", "Gestire commesse"], ["can_manage_groups", "Gestire gruppi"], ["can_manage_users", "Gestire dati e accesso dei lavoratori"], ["can_approve_corrections", "Approvare rettifiche della squadra"]];
    const checkboxes = {};
    for (const [key, label] of caps) { const wrap = el("label", undefined, "ig-check"), input = el("input"); input.type = "checkbox"; input.id = "ig-delegation-" + key; wrap.append(input, el("span", t(label))); delegationForm.append(wrap); checkboxes[key] = input; }
    const delegationStart = field(delegationForm, t("Dal"), "ig-delegation-start", "date"), delegationEnd = field(delegationForm, t("Al, facoltativo"), "ig-delegation-end", "date"); delegationStart.value = today(); delegationStart.required = true;
    const delegationReason = reason(delegationForm, "ig-delegation-reason"), delegationSave = el("button", t("Attribuisci delega"), "row-button"); delegationSave.type = "submit"; delegationForm.append(delegationSave); delegationBox.append(delegationForm, button(t("Aggiorna deleghe"), () => load()));
    const delegationList = el("ul", undefined, "ig-list"); delegationList.id = "ig-delegation-list"; delegationBox.append(delegationList);
    delegationForm.addEventListener("submit", event => { event.preventDefault(); if (!delegationForm.reportValidity()) return; run(delegationBox, async () => { const values = { user_id: delegationUser.value, group_id: delegationGroup.value, starts_on: delegationStart.value, ends_on: delegationEnd.value, reason: delegationReason.value.trim() }; for (const [key] of caps) values[key] = checkboxes[key].checked ? 1 : 0; await P.post(route("delegations"), values); delegationReason.value = ""; await P.refreshAll(); await load(); }); });
    function renderDelegations() {
      delegationList.replaceChildren();
      for (const delegation of delegations) {
        const item = el("li", undefined, "ig-item"), active = Boolean(Number(delegation.active));
        const validity = !active ? t("Revocata") : delegation.starts_on > today() ? t("Futura") : delegation.ends_on && delegation.ends_on < today() ? t("Scaduta") : t("Attiva");
        item.append(el("strong", delegation.user_name + " · " + delegation.group_name), el("p", validity + " · " + delegation.starts_on + " → " + (delegation.ends_on || t("Senza scadenza"))), el("p", caps.filter(([key]) => Number(delegation[key])).map(([, label]) => t(label)).join(" · ")), el("p", delegation.reason, "field-help"));
        if (admin() && active) {
          const revokeReason = reason(item, "ig-revoke-reason-" + delegation.id);
          item.append(button(t("Revoca delega"), () => run(delegationBox, async () => { if (!revokeReason.value.trim()) throw new Error(t("Inserisci il motivo della revoca.")); await P.post(route("delegations/" + delegation.id + "/revoke"), { revision: delegation.revision, reason: revokeReason.value.trim() }); await P.refreshAll(); await load(); })));
        }
        delegationList.append(item);
      }
      if (!delegations.length) delegationList.append(el("li", t("Nessuna delega assegnata."), "field-help"));
    }

    const areaBox = section($("panel-organization"), "ig-area-workers", t("Lavoratori della tua area"), t("Puoi aggiornare nome e accesso dei lavoratori compresi in una delega attiva. Ruoli, profili sensibili e nuovi account restano all’amministratore dello spazio."));
    const areaForm = el("form", undefined, "ig-grid"); areaForm.id = "ig-area-user-form";
    const areaUser = field(areaForm, t("Lavoratore"), "ig-area-user", "select"), areaName = field(areaForm, t("Nome"), "ig-area-name"), areaActive = field(areaForm, t("Accesso"), "ig-area-active", "select"); areaUser.required = true; areaName.required = true; areaName.maxLength = 120; option(areaActive, "1", t("Attivo")); option(areaActive, "0", t("Disattivato"));
    const areaSave = el("button", t("Salva lavoratore"), "row-button"); areaSave.type = "submit"; areaForm.append(areaSave); areaBox.append(areaForm);
    areaUser.addEventListener("change", () => { const person = people.find(person => String(person.id) === areaUser.value); areaName.value = person?.name || ""; areaActive.value = person?.active ? "1" : "0"; });
    areaForm.addEventListener("submit", event => { event.preventDefault(); if (!areaForm.reportValidity()) return; run(areaBox, async () => { await P.post("/api/users/" + areaUser.value, { name: areaName.value.trim(), active: areaActive.value, role: "worker" }); await P.refreshAll(); await load(); }); });

    const impactBox = section($("panel-organization"), "ig-group-impact", t("Anteprima impatto gruppi"), t("Simula uno spostamento o una variazione dei membri. Il risultato mostra accessi alle commesse e persone le cui ore diventano visibili o cessano di esserlo. L’anteprima non salva modifiche."));
    const impactForm = el("form", undefined, "ig-grid"); impactForm.id = "ig-impact-form";
    const impactGroup = field(impactForm, t("Gruppo"), "ig-impact-group", "select"), impactOperation = field(impactForm, t("Modifica da simulare"), "ig-impact-operation", "select"); impactGroup.required = true;
    option(impactOperation, "move", t("Sposta gruppo")); option(impactOperation, "add_member", t("Aggiungi membro")); option(impactOperation, "remove_member", t("Rimuovi membro"));
    const impactParent = field(impactForm, t("Nuovo gruppo padre"), "ig-impact-parent", "select"), impactUser = field(impactForm, t("Persona"), "ig-impact-user", "select");
    const impactSave = el("button", t("Calcola anteprima"), "row-button"); impactSave.type = "submit"; impactForm.append(impactSave); impactBox.append(impactForm);
    const impactResult = el("div", undefined, "ig-impact-result"); impactResult.id = "ig-impact-result"; impactResult.setAttribute("aria-live", "polite"); impactBox.append(impactResult);
    const impactFields = () => { impactParent.closest(".ig-field").hidden = impactOperation.value !== "move"; impactUser.closest(".ig-field").hidden = impactOperation.value === "move"; impactUser.required = impactOperation.value !== "move"; impactResult.replaceChildren(); }; impactOperation.addEventListener("change", impactFields); impactFields();
    function renderImpact(data, destination = impactResult) {
      destination.replaceChildren(el("p", t("Persone coinvolte") + ": " + data.affected_count, "ig-notice"));
      const list = el("ul", undefined, "ig-list");
      for (const change of data.changes || []) {
        const item = el("li", undefined, "ig-item"); item.append(el("strong", change.user_name));
        for (const [key, label, lookup] of [["job_access_added", "Commessa accessibile", jobName], ["job_access_removed", "Commessa non più accessibile", jobName], ["hour_users_added", "Ore ora visibili", name], ["hour_users_removed", "Ore non più visibili", name]]) if (change[key]?.length) item.append(el("p", t(label) + ": " + change[key].map(lookup).join(", ")));
        for (const permission of change.job_permission_changes || []) item.append(el("p", jobName(permission.job_id) + " · " + (permission.after_can_log_time ? t("Registrazione ore abilitata") : t("Registrazione ore disabilitata"))));
        list.append(item);
      }
      destination.append(list, el("p", data.notice, "field-help"));
    }
    impactForm.addEventListener("submit", event => { event.preventDefault(); if (!impactForm.reportValidity()) return; run(impactBox, async () => { const scope = epoch, fields = { group_id: impactGroup.value, operation: impactOperation.value }; if (impactOperation.value === "move") fields.parent_id = impactParent.value || 0; else fields.user_id = impactUser.value; const data = await P.post(route("group-impact"), fields); if (scope === epoch) renderImpact(data); }); });
    // The same preview is available beside the actual group editor.
    const dialogPreview = section($("group-form"), "ig-group-dialog-preview", t("Impatto dello spostamento"), t("L’anteprima usa il gruppo padre selezionato e non salva la modifica."));
    $("group-form")?.querySelector(".dialog-footer")?.before(dialogPreview);
    const dialogResult = el("div", undefined, "ig-impact-result"); dialogResult.setAttribute("aria-live", "polite");
    dialogPreview.append(button(t("Anteprima dello spostamento"), () => run(dialogPreview, async () => { const id = $("group-id").value; if (!id) throw new Error(t("L’anteprima dello spostamento richiede un gruppo esistente.")); const data = await P.post(route("group-impact"), { group_id: id, operation: "move", parent_id: $("group-parent").value || 0 }); renderImpact(data, dialogResult); })), dialogResult);
    $("group-parent")?.addEventListener("change", () => dialogResult.replaceChildren());
    if ($("group-dialog")) new MutationObserver(() => { dialogResult.replaceChildren(); dialogPreview.hidden = !$("group-id").value; }).observe($("group-dialog"), { attributes: true, attributeFilter: ["open"] });

    function choices() {
      const managers = people.filter(person => person.active && person.role === "manager");
      selectValues(adminUser, managers, t("Seleziona un gestore")); selectValues(delegationUser, managers, t("Seleziona un gestore"));
      for (const select of [delegationGroup, impactGroup]) selectValues(select, groups, t("Seleziona un gruppo"));
      selectValues(impactParent, groups, admin() ? t("Primo livello") : t("Seleziona un gruppo della tua area"));
      selectValues(impactUser, people, t("Seleziona una persona"));
      selectValues(areaUser, people.filter(person => person.can_manage && person.role === "worker"), t("Seleziona un lavoratore"));
    }
    async function load() {
      if (!authenticated() || !manager()) return;
      if (loading) { reload = true; return; }
      loading = true; const scope = epoch;
      for (const box of [adminBox, requestBox, delegationBox, areaBox, impactBox]) error(box, "");
      try {
        const results = await Promise.allSettled([P.api(route("status")), P.api("/api/users"), P.api("/api/groups"), P.api(route("delegations")), ...(admin() ? [P.api(route("admins")), P.api(route("profile-requests"))] : [])]);
        if (scope !== epoch) return;
        if (results[0].status === "fulfilled") {
          status = results[0].value;
          if (!status.admin) { decisionDrafts.clear(); requests = []; requestList.replaceChildren(); requestBox.hidden = true; }
        } else {
          if ([401, 403].includes(results[0].reason.status)) { decisionDrafts.clear(); requests = []; requestList.replaceChildren(); }
          throw results[0].reason;
        }
        if (results[1].status === "fulfilled") people = results[1].value.users || []; else { people = []; error(areaBox, results[1].reason.message); }
        if (results[2].status === "fulfilled") groups = results[2].value.groups || []; else { groups = []; error(impactBox, results[2].reason.message); }
        if (results[3].status === "fulfilled") delegations = results[3].value.delegations || []; else error(delegationBox, results[3].reason.message);
        if (admin()) {
          if (results[4].status === "fulfilled") { adminList.replaceChildren(); for (const person of results[4].value.admins || []) adminList.append(el("li", person.name + " · " + person.email + (person.active ? "" : " · " + t("Disattivato")))); } else error(adminBox, results[4].reason.message);
          if (status.admin && results[5].status === "fulfilled") { requests = results[5].value.requests || []; renderRequests(); }
          else if (results[5].status === "rejected") {
            if ([401, 403].includes(results[5].reason.status)) { decisionDrafts.clear(); requests = []; requestList.replaceChildren(); }
            error(requestBox, results[5].reason.message);
          }
        }
        choices(); renderDelegations();
      } catch (failure) { if (scope === epoch) error(delegationBox, failure.message); }
      finally { loading = false; if (reload) { reload = false; load(); } }
    }
    P.register(({ session }) => {
      const next = session?.authenticated ? String(session.user_id || session.email) + ":" + Boolean(session.permissions?.manage_space) : "";
      if (next !== identity) { identity = next; epoch++; decisionDrafts.clear(); people = []; groups = []; requests = []; delegations = []; status = null; adminList.replaceChildren(); requestList.replaceChildren(); delegationList.replaceChildren(); impactResult.replaceChildren(); dialogResult.replaceChildren(); }
      adminBox.hidden = requestBox.hidden = !admin(); delegationBox.hidden = impactBox.hidden = !authenticated() || !manager(); delegationForm.hidden = !admin(); areaBox.hidden = !authenticated() || !manager() || admin() || session?.permissions?.edit_users === false;
      if (authenticated() && manager()) load();
    });
    window.PuntoGovernance = Object.freeze({ refresh: load });
  }
  if (window.PuntoExtensions) initialize(); else window.addEventListener("punto:extensions-ready", initialize, { once: true });
})();
