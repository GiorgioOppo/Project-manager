(() => {
  "use strict";
  function initialize() {
    const P = window.PuntoExtensions;
    if (!P || window.PuntoOperations) return;
    const t = P.t, $ = id => document.getElementById(id);
    const path = route => "/api/improvements/operations/" + route;
    let epoch = 0, account = "", correctionsBusy = false, correctionsCursor = null, correctionsToken = 0;
    let absenceCursor = null, lifecycleJob = "", templates = [], currentTemplate = null;
    let absenceToken = 0, capacityToken = 0, checklistToken = 0, templateToken = 0;
    const node = (tag, text, cls = "") => { const el = document.createElement(tag); if (text !== undefined) el.textContent = text; if (cls) el.className = cls; return el; };
    const button = (text, handler) => { const el = node("button", text, "row-button"); el.type = "button"; el.addEventListener("click", handler); return el; };
    const field = (parent, label, id, type = "text", value = "") => { const wrap = node("div", undefined, "io-field"); const lab = node("label", label); lab.htmlFor = id; const input = node(type === "select" ? "select" : type === "textarea" ? "textarea" : "input"); input.id = id; if (input.tagName === "INPUT") input.type = type; input.value = value; wrap.append(lab, input); parent.append(wrap); return input; };
    const option = (select, value, text) => { const item = node("option", text); item.value = value; select.append(item); return item; };
    const section = (parent, id, title, help = "") => { const box = node("section", undefined, "io-section"); box.id = id; const head = node("div", undefined, "io-heading"); head.append(node("h3", title)); box.append(head); if (help) box.append(node("p", help, "field-help")); const error = node("p", "", "io-error"); error.setAttribute("role", "alert"); error.hidden = true; box.append(error); parent?.append(box); return box; };
    const fail = (box, error) => { const el = box.querySelector(".io-error"); if (el) { el.hidden = false; el.textContent = error.message || String(error); } };
    const clearError = box => { const el = box.querySelector(".io-error"); if (el) { el.hidden = true; el.textContent = ""; } };
    const hours = value => (Number(value) / 60).toLocaleString(P.locale || "it", { maximumFractionDigits: 2 }) + " h";
    const run = async (box, action) => { clearError(box); const controls = [...box.querySelectorAll("button")]; controls.forEach(el => el.disabled = true); try { await action(); } catch (error) { fail(box, error); } finally { controls.forEach(el => el.disabled = false); } };
    const query = values => new URLSearchParams(Object.entries(values).filter(([, value]) => value !== "" && value !== null && value !== undefined)).toString();
    const authenticated = () => Boolean(P.session()?.authenticated);
    const person = () => P.session()?.user?.id || P.session()?.user_id || P.session()?.id || "";

    const correctionBox = document.querySelector(".time-corrections");
    const filters = node("div", undefined, "io-filters"); filters.id = "io-correction-filters";
    const status = field(filters, t("Stato"), "io-correction-status", "select");
    for (const [key, label] of [["pending", "In attesa"], ["", "Tutti gli stati"], ["approved", "Approvata"], ["rejected", "Rifiutata"], ["cancelled", "Annullata"], ["conflict", "Conflitto: registrazione cambiata"]]) option(status, key, t(label));
    const requester = field(filters, t("Persona"), "io-correction-person", "select"); option(requester, "", t("Tutte le persone visibili"));
    const from = field(filters, t("Dal"), "io-correction-from", "date"), to = field(filters, t("Al"), "io-correction-to", "date");
    const more = button(t("Carica altre richieste"), () => refreshCorrections(true)); more.id = "io-correction-more"; more.hidden = true;
    correctionBox?.querySelector(".workflow-section-heading")?.after(filters); correctionBox?.append(more);
    for (const input of [status, requester, from, to]) input.addEventListener("change", () => refreshCorrections());
    async function refreshCorrections(append = false) {
      if (!authenticated() || !correctionBox) return;
      const token = ++correctionsToken, scope = epoch;
      correctionsBusy = true; $("time-corrections-loading").hidden = false; $("time-corrections-error").hidden = true;
      more.disabled = true; $("time-corrections-refresh").disabled = true;
      if (!append) correctionsCursor = null;
      try {
        const data = await P.api("/api/time-corrections?" + query({ status: status.value, user_id: requester.value, from: from.value, to: to.value, limit: 25, after_id: append ? correctionsCursor : null }));
        if (scope !== epoch || token !== correctionsToken) return;
        P.timeWorkflow.requests = append ? P.timeWorkflow.requests.concat(data.requests || []) : data.requests || [];
        P.timeWorkflow.requestsReady = true; correctionsCursor = data.next_after_id; more.hidden = !data.has_more;
        P.renderTimeCorrections();
      } catch (error) { if (scope === epoch && token === correctionsToken) { $("time-corrections-error").hidden = false; $("time-corrections-error").textContent = error.message; } }
      finally { if (token === correctionsToken) { correctionsBusy = false; $("time-corrections-loading").hidden = true; $("time-corrections-refresh").disabled = false; more.disabled = false; } }
    }
    async function refreshPeople() {
      const scope = epoch;
      try { const data = await P.api("/api/time-team"); if (scope !== epoch) return; const selected = requester.value; requester.replaceChildren(); option(requester, "", t("Tutte le persone visibili")); for (const user of data.users || []) option(requester, user.id, user.name || user.email); requester.value = selected;
        const selectedCapacity = capacityPerson.value || String(person()); capacityPerson.replaceChildren(); for(const user of data.users || []) option(capacityPerson,user.id,user.name || user.email); capacityPerson.value = selectedCapacity; if(!capacityPerson.value)capacityPerson.value=String(person()); }
      catch (_) { requester.replaceChildren(); option(requester, "", t("Le mie richieste")); }
    }

    const absenceBox = section($("panel-work-schedules"), "io-absences", t("Richieste di assenza"), t("Richiedi un intervallo. Le giornate vengono aggiornate soltanto dopo l’approvazione di un gestore delegato diverso da te."));
    const absenceForm = node("form", undefined, "io-grid"); absenceForm.id = "io-absence-form";
    const absenceStart = field(absenceForm, t("Dal"), "io-absence-start", "date", P.localDate(new Date())); absenceStart.required = true;
    const absenceEnd = field(absenceForm, t("Al"), "io-absence-end", "date", P.localDate(new Date())); absenceEnd.required = true;
    const absenceMinutes = field(absenceForm, t("Minuti di lavoro attesi"), "io-absence-minutes", "number", "0"); Object.assign(absenceMinutes, { min: "0", max: "1440", step: "1", required: true });
    const absenceReason = field(absenceForm, t("Motivo"), "io-absence-reason", "text"); absenceReason.required = true; absenceReason.maxLength = 1000;
    const absenceSubmit = node("button", t("Invia richiesta"), "button button-primary"); absenceSubmit.type = "submit"; absenceForm.append(absenceSubmit); absenceBox.append(absenceForm);
    const absenceFilter = field(absenceBox, t("Stato"), "io-absence-status", "select"); for (const [key, label] of [["pending", "In attesa"], ["", "Tutti gli stati"], ["approved", "Approvata"], ["rejected", "Rifiutata"], ["cancelled", "Annullata"]]) option(absenceFilter, key, t(label));
    const absenceList = node("ul", undefined, "io-list"); absenceList.id = "io-absence-list"; absenceBox.append(absenceList);
    const absenceMore = button(t("Carica altre richieste"), () => loadAbsences(true)); absenceMore.hidden = true; absenceBox.append(button(t("Aggiorna richieste"), () => loadAbsences()), absenceMore);
    absenceForm.addEventListener("submit", event => { event.preventDefault(); if (!absenceForm.reportValidity()) return; run(absenceBox, async () => { await P.post(path("absences"), { starts_on: absenceStart.value, ends_on: absenceEnd.value, expected_minutes: absenceMinutes.value, reason: absenceReason.value.trim() }); absenceReason.value = ""; await loadAbsences(); }); });
    absenceFilter.addEventListener("change", () => loadAbsences());
    async function loadAbsences(append = false) {
      if (!authenticated()) return; const scope = epoch, token = ++absenceToken; clearError(absenceBox);
      try {
        const data = await P.api(path("absences") + "?" + query({ status: absenceFilter.value, limit: 25, after_id: append ? absenceCursor : null })); if (scope !== epoch || token !== absenceToken) return;
        if (!append) absenceList.replaceChildren(); absenceCursor = data.next_after_id; absenceMore.hidden = !data.has_more;
        for (const request of data.requests || []) {
          const item = node("li", undefined, "io-card"); item.append(node("strong", request.user_name + " · " + request.starts_on + " → " + request.ends_on));
          item.append(node("p", request.reason + " · " + hours(request.expected_minutes) + " · " + t(({ pending: "In attesa", approved: "Approvata", rejected: "Rifiutata", cancelled: "Annullata" })[request.status] || request.status)));
          if (request.decision_reason) item.append(node("p", request.decision_reason));
          const actions = node("div", undefined, "io-actions");
          if (request.can_decide || request.can_cancel) {
            const reason = field(item, t("Motivo della decisione"), "io-absence-decision-" + request.id); reason.maxLength = 1000;
            const decide = decision => run(absenceBox, async () => { if (decision === "rejected" && !reason.value.trim()) throw new Error(t("Il rifiuto richiede un motivo.")); await P.post(path("absence-decision"), { id: request.id, revision: request.revision, decision, reason: reason.value.trim() }); await loadAbsences(); await P.refreshAll(); });
            if (request.can_decide) actions.append(button(t("Approva"), () => decide("approved")), button(t("Rifiuta"), () => decide("rejected")));
            if (request.can_cancel) actions.append(button(t("Annulla richiesta"), () => decide("cancelled")));
          }
          item.append(actions); absenceList.append(item);
        }
        if (!absenceList.children.length) absenceList.append(node("li", t("Nessuna richiesta di assenza."), "field-help"));
      } catch (error) { if (scope === epoch) fail(absenceBox, error); }
    }

    const capacityBox = section($("panel-my-work"), "io-capacity", t("Capacità e lavoro pianificato"), t("Confronta il calendario personale con le stime delle attività aperte assegnate. Le attività senza scadenza rientrano nel carico pianificato."));
    const capacityForm = node("form", undefined, "io-grid"); const capacityPerson = field(capacityForm,t("Persona"),"io-capacity-person","select");
    const capacityFrom = field(capacityForm, t("Dal"), "io-capacity-from", "date", P.localDate(new Date())); const end = new Date(P.localDate(new Date()) + "T12:00:00"); end.setDate(end.getDate() + 6);
    const capacityTo = field(capacityForm, t("Al"), "io-capacity-to", "date", P.localDate(end)); const capacityButton = node("button", t("Calcola capacità"), "row-button"); capacityButton.type = "submit"; capacityForm.append(capacityButton); capacityBox.append(capacityForm);
    const capacityResult = node("div", "", "io-metrics"); capacityResult.id = "io-capacity-result"; capacityBox.append(capacityResult);
    capacityForm.addEventListener("submit", event => { event.preventDefault(); loadCapacity(); });
    capacityPerson.addEventListener("change",loadCapacity);
    async function loadCapacity() { if (!authenticated()) return; const scope = epoch, token = ++capacityToken; capacityResult.replaceChildren(); await run(capacityBox, async () => { const data = await P.api(path("capacity") + "?" + query({ from: capacityFrom.value, to: capacityTo.value, user_id: capacityPerson.value || person() })); if (scope !== epoch || token !== capacityToken) return; capacityResult.replaceChildren(); for (const [label, value] of [["Lavoro atteso", data.expected_minutes], ["Ore registrate", data.recorded_minutes], ["Stime attività aperte", data.planned_minutes], ["Capacità non pianificata", data.available_minutes]]) { const card = node("div", undefined, "io-metric"); card.append(node("span", t(label)), node("strong", hours(value))); capacityResult.append(card); } }); }
    const estimateBox = section($("panel-work-board"), "io-estimates", t("Stima delle attività"), t("Le stime delle attività assegnate alimentano il confronto con la capacità personale."));
    const estimateForm = node("form", undefined, "io-grid"); const estimateTask = field(estimateForm, t("Attività"), "io-estimate-task", "select"); const estimateMinutes = field(estimateForm, t("Stima in minuti"), "io-estimate-minutes", "number", "0"); Object.assign(estimateMinutes, { min: "0", max: "1000000", step: "1", required: true }); const estimateSave = node("button", t("Salva stima"), "row-button"); estimateSave.type = "submit"; estimateForm.append(estimateSave); estimateBox.append(estimateForm);
    function updateEstimates() { const state = P.getState(), selected = estimateTask.value; estimateTask.replaceChildren(); option(estimateTask, "", t("Seleziona attività")); for (const task of state.workTasks || []) option(estimateTask, task.id, task.title + " · " + hours(task.estimated_minutes || 0)); estimateTask.value = selected; estimateBox.hidden = !state.workPermissions?.manage_tasks; }
    estimateTask.addEventListener("change", () => { const task = P.getState().workTasks.find(item => String(item.id) === estimateTask.value); estimateMinutes.value = task?.estimated_minutes || 0; });
    estimateForm.addEventListener("submit", event => { event.preventDefault(); const task = P.getState().workTasks.find(item => String(item.id) === estimateTask.value); if (!task || !estimateForm.reportValidity()) return; run(estimateBox, async () => { await P.post(path("task-plan"), { task_id: task.id, estimated_minutes: estimateMinutes.value, revision: task.revision }); await P.refreshAll(); updateEstimates(); }); });
    if ($("work-summary")) new MutationObserver(updateEstimates).observe($("work-summary"), { childList: true, characterData: true, subtree: true });

    const templateBox = section($("panel-jobs"), "io-templates", t("Modelli e versioni"), t("Modifica un modello, consulta le versioni precedenti o archivialo. Le commesse già create conservano i propri dati."));
    const templateSelect = field(templateBox, t("Modello"), "io-template-select", "select");
    const templateForm = node("form", undefined, "io-grid"); templateForm.id = "io-template-form";
    const templateName = field(templateForm, t("Nome del modello"), "io-template-name"); templateName.required = true; templateName.maxLength = 120;
    const templateJobName = field(templateForm, t("Nome commessa proposto"), "io-template-job-name"); templateJobName.maxLength = 120;
    const templateClient = field(templateForm, t("Cliente"), "io-template-client"); templateClient.maxLength = 120;
    const templateDescription = field(templateForm, t("Descrizione"), "io-template-description", "textarea"); templateDescription.maxLength = 1000;
    const archivedWrap = node("label", undefined, "io-check"); const templateArchived = node("input"); templateArchived.type = "checkbox"; templateArchived.id = "io-template-archived"; archivedWrap.append(templateArchived, node("span", t("Archiviato"))); templateForm.append(archivedWrap);
    const templateSave = node("button", t("Salva nuova versione"), "row-button"); templateSave.type = "submit"; templateForm.append(templateSave); templateBox.append(templateForm);
    const versionSelect = field(templateBox, t("Versione da ripristinare"), "io-template-version", "select"); const restore = button(t("Carica versione nell’editor"), () => { const version = versionSelect.selectedOptions[0]?._version; if (version) fillTemplate(version); }); templateBox.append(restore, button(t("Aggiorna modelli"), () => loadTemplates()));
    function fillTemplate(item) { templateName.value = item.name; let definition; try { definition = JSON.parse(item.definition_json); } catch (_) { definition = {}; } templateForm._definition = definition; templateJobName.value = definition.name || ""; templateClient.value = definition.client || ""; templateDescription.value = definition.description || ""; templateArchived.checked = Boolean(Number(item.archived)); }
    async function loadTemplates() { if (!authenticated() || !P.can("manage_jobs")) return; const scope = epoch, token=++templateToken; await run(templateBox, async () => { const data = await P.api(path("templates")); if (scope !== epoch || token !== templateToken) return; templates = data.templates || []; const selected = templateSelect.value; templateSelect.replaceChildren(); option(templateSelect, "", t("Seleziona modello")); for (const item of templates) option(templateSelect, item.id, item.name + " · v" + item.revision + (Number(item.archived) ? " · " + t("Archiviato") : "")); templateSelect.value = selected; templateForm.hidden = !currentTemplate; }); }
    templateSelect.addEventListener("change", async () => { currentTemplate = templates.find(item => String(item.id) === templateSelect.value) || null; templateForm.hidden = !currentTemplate; versionSelect.replaceChildren(); option(versionSelect, "", t("Seleziona versione")); if (!currentTemplate) return; fillTemplate(currentTemplate); const scope = epoch, id=currentTemplate.id; await run(templateBox, async () => { const data = await P.api(path("templates") + "?id=" + id + "&history=1"); if (scope !== epoch || id!==currentTemplate?.id) return; for (const version of data.templates || []) option(versionSelect, version.revision, "v" + version.revision + " · " + version.created_at)._version = version; }); });
    templateForm.addEventListener("submit", event => { event.preventDefault(); if (!currentTemplate || !templateForm.reportValidity()) return; run(templateBox, async () => { const definition = { ...templateForm._definition, name: templateJobName.value, client: templateClient.value, description: templateDescription.value }; await P.post(path("templates"), { id: currentTemplate.id, revision: currentTemplate.revision, name: templateName.value.trim(), archived: templateArchived.checked ? 1 : 0, definition: JSON.stringify(definition) }); currentTemplate = null; templateForm.hidden = true; versionSelect.replaceChildren(); await P.refreshAll(); await loadTemplates(); }); });

    const checklistBox = section($("job-lifecycle-form"), "io-checklist", t("Checklist di chiusura"), t("Controlla lavoro aperto, rettifiche e risorse collegate. Lo stato di ticket e dotazioni viene verificato e segnato manualmente."));
    $("job-lifecycle-form")?.querySelector(".dialog-footer")?.before(checklistBox);
    const checklistSummary = node("p", "", "field-help"), checklistList = node("ul", undefined, "io-list"); checklistBox.append(checklistSummary, checklistList);
    const checklistFields = node("div", undefined, "io-grid"); const checklistKind = field(checklistFields, t("Tipo risorsa"), "io-checklist-kind", "select"); option(checklistKind, "ticket", t("Ticket")); option(checklistKind, "asset", t("Dotazione"));
    const checklistId = field(checklistFields, t("Identificativo risorsa"), "io-checklist-resource", "number"); checklistId.min = "1"; checklistId.step = "1"; const checklistLabel = field(checklistFields, t("Descrizione"), "io-checklist-label"); checklistLabel.maxLength = 160;
    checklistFields.append(button(t("Collega alla checklist"), () => run(checklistBox, async () => { await P.post(path("checklist"), { job_id: lifecycleJob, kind: checklistKind.value, resource_id: checklistId.value, label: checklistLabel.value.trim(), status: "open", revision: 0 }); checklistId.value = ""; checklistLabel.value = ""; await loadChecklist(); }))); checklistBox.append(checklistFields);
    const checklistConfirm = node("label", undefined, "io-check"); const checkbox = node("input"); checkbox.type = "checkbox"; checkbox.id = "io-checklist-confirmed"; checklistConfirm.append(checkbox, node("span", t("Ho verificato gli elementi aperti e confermo la chiusura della commessa."))); checklistBox.append(checklistConfirm, button(t("Aggiorna checklist"), () => loadChecklist()));
    async function loadChecklist() { lifecycleJob = String(P.timeWorkflow.lifecycleJob?.id || ""); if (!lifecycleJob || !$("job-lifecycle-dialog").open) return; const scope = epoch, job = lifecycleJob, token=++checklistToken; checkbox.checked = false; await run(checklistBox, async () => { const data = await P.api(path("checklist") + "?job_id=" + job); if (scope !== epoch || job !== lifecycleJob || token !== checklistToken) return; checklistSummary.textContent = t("{0} attività · {1} blocchi · {2} rettifiche in attesa", [data.open_tasks, data.open_blockers, data.pending_corrections]); checklistConfirm.hidden = data.ready; checklistList.replaceChildren(); for (const item of data.linked_resources || []) { const row = node("li", undefined, "io-card"); row.append(node("span", t(item.kind === "ticket" ? "Ticket" : "Dotazione") + " #" + item.resource_id + " · " + item.label + " · " + t(item.status === "done" ? "Verificato" : "Da verificare")), button(t(item.status === "done" ? "Da verificare" : "Segna verificato"), () => run(checklistBox, async () => { await P.post(path("checklist"), { job_id: job, kind: item.kind, resource_id: item.resource_id, label: item.label, status: item.status === "done" ? "open" : "done", revision: item.revision }); await loadChecklist(); }))); checklistList.append(row); } }); }
    const lifecycleDialog = $("job-lifecycle-dialog"); if (lifecycleDialog) new MutationObserver(() => { if (lifecycleDialog.open) loadChecklist(); else { lifecycleJob = ""; checklistList.replaceChildren(); checkbox.checked = false; } }).observe(lifecycleDialog, { attributes: true, attributeFilter: ["open"] });
    let lastTab = "";
    window.PuntoOperations = Object.freeze({ refreshCorrections });
    P.register(({ session, state }) => {
      const next = session?.authenticated ? String(person()) : "";
      if (next !== account) { epoch++; correctionsToken++; correctionsCursor = null; correctionsBusy = false; account = next; lastTab = ""; templates = []; currentTemplate = null; absenceList.replaceChildren(); capacityResult.replaceChildren(); templateSelect.replaceChildren(); templateForm.hidden = true; checklistList.replaceChildren(); P.timeWorkflow.requests = []; absenceReason.value = ""; templateForm.reset(); estimateTask.replaceChildren(); checklistLabel.value = ""; checklistId.value = ""; requester.value = ""; if (next) refreshPeople(); }
      for (const el of [absenceBox, capacityBox]) el.hidden = !next;
      capacityPerson.closest(".io-field").hidden=!P.can("view_team_hours");
      templateBox.hidden = !next || !P.can("manage_jobs"); if(templateBox.hidden){templates=[];currentTemplate=null;templateSelect.replaceChildren();versionSelect.replaceChildren();templateForm.reset();templateForm.hidden=true;} updateEstimates();
      if (!next) return;
      const changedTab = state.activeTab !== lastTab; lastTab = state.activeTab;
      if (state.activeTab === "hours" && (!correctionsBusy || changedTab)) refreshCorrections();
      if (state.activeTab === "work-schedules") loadAbsences();
      if (state.activeTab === "my-work") { if(changedTab)refreshPeople(); loadCapacity(); }
      if (state.activeTab === "jobs" && P.can("manage_jobs")) loadTemplates();
    });
  }
  if (window.PuntoExtensions) initialize(); else window.addEventListener("punto:extensions-ready", initialize, { once: true });
})();
