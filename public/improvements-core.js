"use strict";
(() => {
  function initialize() {
    const P = window.PuntoExtensions, $ = (id) => document.getElementById(id), t = P.t;
    const prefix = "punto.draft.v1.", kinds = ["entry", "batch", "wizard"];
    let owner = null, fingerprint = "";
    const key = (kind) => prefix + owner + "." + kind;
    const fieldValues = (form) => Object.fromEntries([...form.querySelectorAll("input[id],select[id],textarea[id]")].filter((input) => !["password", "file"].includes(input.type)).map((input) => [input.id, input.type === "checkbox" ? input.checked : input.value]));
    function read(kind) {
      try { const value = JSON.parse(localStorage.getItem(key(kind)) || "null"); return value?.owner === owner && Date.now() - value.saved < 7 * 86400000 ? value : null; } catch { return null; }
    }
    function discard(kind) { try { localStorage.removeItem(key(kind)); } catch { /* Storage can be disabled. */ } render(); }
    function clearAll() { try { Object.keys(localStorage).filter((item) => item.startsWith(prefix)).forEach((item) => localStorage.removeItem(item)); } catch { /* Storage can be disabled. */ } }
    function save(kind, silent = false) {
      if (!owner || (kind === "wizard" && !P.can("manage_jobs"))) return;
      const form = $(kind === "entry" ? "entry-form" : kind === "batch" ? "hours-batch-form" : "job-wizard-form");
      let content;
      if (kind === "batch") content = { rows: P.timeWorkflow.batchRows, requestKey: P.timeWorkflow.batchKey, payload: P.timeWorkflow.batchPayload };
      if (kind === "wizard") { const w = P.workspaceFlows.wizard; content = { step: w.step, team: w.team, tasks: w.tasks, key: w.key, payload: w.payload, createdId: w.createdId, createdDefinition: w.createdDefinition, sequence: w.sequence }; }
      if (kind === "entry") content = { entry: P.getState().editingTimeEntry || null };
      try {
        const value = JSON.stringify({ owner, saved: Date.now(), fields: fieldValues(form), content });
        if (value.length > 512000) throw new Error(t("La bozza è troppo grande."));
        localStorage.setItem(key(kind), value); render(); if (!silent) P.showNotice(t("Bozza salvata su questo dispositivo per 7 giorni. Sarà eliminata quando esci.")); return true;
      } catch (error) { if (!silent) P.showNotice(error.message || t("Impossibile salvare la bozza su questo dispositivo."), true); return false; }
    }
    async function restore(kind) {
      const draft = read(kind); if (!draft) return;
      if (kind === "wizard" && !P.can("manage_jobs")) return;
      if (kind === "entry") {
        if (draft.content.entry && !P.can("edit_time_entries")) { P.showNotice(t("Il tuo ruolo non consente questa operazione."), true); return; }
        P.openEntry(draft.content.entry, draft.fields?.["entry-job"], draft.fields?.["entry-task"]);
      }
      if (kind === "batch") {
        if (!Array.isArray(draft.content.rows) || draft.content.rows.length > 100) return;
        P.openHoursBatch(draft.content.rows); P.timeWorkflow.batchKey = draft.content.requestKey; P.timeWorkflow.batchPayload = draft.content.payload;
      }
      if (kind === "wizard") { await P.openJobWizard(); Object.assign(P.workspaceFlows.wizard, draft.content); }
      for (const [id, value] of Object.entries(draft.fields || {})) {
        const input = $(id), formId = kind === "entry" ? "entry-form" : kind === "batch" ? "hours-batch-form" : "job-wizard-form";
        if (!input || input.closest("form")?.id !== formId) continue;
        if (input.type === "checkbox") input.checked = value === true; else input.value = String(value);
      }
      if (kind === "wizard") { P.renderJobWizardTeam(); P.renderJobWizardTasks(); P.renderJobWizardStep(); }
      if (kind === "batch") P.renderHoursBatch();
      P.showNotice(t("Bozza recuperata. Controlla i dati e conferma per salvarli nel registro."));
    }
    function button(label, click) { const b = document.createElement("button"); b.type = "button"; b.className = "button button-secondary"; b.textContent = t(label); b.addEventListener("click", click); return b; }
    function render() {
      if (!owner) return;
      for (const kind of kinds) {
        const toolbar = $("draft-" + kind + "-controls"); if (toolbar) toolbar.hidden = kind === "wizard" && !P.can("manage_jobs");
        const restoreButton = $("draft-" + kind + "-restore"); if (restoreButton) restoreButton.hidden = !read(kind);
        const discardButton = $("draft-" + kind + "-discard"); if (discardButton) discardButton.hidden = !read(kind);
      }
    }
    for (const kind of kinds) {
      const form = $(kind === "entry" ? "entry-form" : kind === "batch" ? "hours-batch-form" : "job-wizard-form");
      const bar = document.createElement("div"); bar.className = "time-preview-actions"; bar.id = "draft-" + kind + "-controls";
      const resume = button("Riprendi bozza", () => restore(kind)); resume.id = "draft-" + kind + "-restore";
      const remove = button("Elimina bozza", () => discard(kind)); remove.id = "draft-" + kind + "-discard";
      bar.append(button("Salva bozza sul dispositivo", () => save(kind)), resume, remove); form.querySelector(".dialog-footer")?.before(bar);
    }
    function selectCopy(rows) {
      let dialog = $("select-copy-dialog"); if (dialog) dialog.remove();
      dialog = document.createElement("dialog"); dialog.id = "select-copy-dialog"; dialog.className = "work-dialog";
      const heading = document.createElement("h2"); heading.textContent = t("Scegli le righe da copiare");
      const count = document.createElement("p"); count.setAttribute("aria-live", "polite");
      const list = document.createElement("div"), selected = new Set(rows.length <= 100 ? rows.map((_, i) => i) : []);
      const confirm = button("Prepara anteprima", () => { const choice = [...selected].map((i) => rows[i]); dialog.close(); P.openHoursBatch(choice); });
      const update = () => { count.textContent = t("Selezionate {0} righe su {1}. Massimo 100 per conferma.", [selected.size, rows.length]); confirm.disabled = !selected.size || selected.size > 100; };
      rows.forEach((row, index) => { const label = document.createElement("label"), input = document.createElement("input"); label.className = "access-profile-option"; input.type = "checkbox"; input.checked = selected.has(index); input.addEventListener("change", () => { if (input.checked) selected.add(index); else selected.delete(index); update(); }); const text = document.createElement("span"); text.textContent = `${row.date} · ${row.minutes} min · ${P.getState().jobs.find((job) => Number(job.id) === Number(row.job_id))?.name || "#" + row.job_id} · ${row.note || ""}`; label.append(input, text); list.append(label); });
      dialog.append(heading, count, list, confirm, button("Annulla", () => dialog.close())); document.body.append(dialog); update(); dialog.showModal();
    }
    const copy = button("Copia giorni selezionati", () => {
      if (!P.viewingOwnHours()) return;
      const dialog = document.createElement("dialog"), form = document.createElement("form"); dialog.className = "work-dialog";
      const heading = document.createElement("h2"); heading.textContent = t("Copia giorni selezionati"); form.append(heading);
      const inputs = {};
      for (const [name, label] of [["from", "Dal giorno"], ["to", "Al giorno"], ["target", "Primo giorno di destinazione"]]) {
        const wrapper = document.createElement("label"), input = document.createElement("input"); wrapper.className = "work-field"; wrapper.textContent = t(label); input.type = "date"; input.required = true; input.value = P.getState().selectedDate; wrapper.append(input); form.append(wrapper); inputs[name] = input;
      }
      const error = document.createElement("p"); error.setAttribute("role", "alert");
      const preview = button("Carica le righe", async () => {
        if (!form.reportValidity()) return;
        const { from, to, target } = Object.fromEntries(Object.entries(inputs).map(([name, input]) => [name, input.value]));
        if (![from, to, target].every(P.validDate) || to < from || (Date.parse(to) - Date.parse(from)) / 86400000 > 6) { error.textContent = t("Seleziona un intervallo valido di massimo 7 giorni."); return; }
        preview.disabled = true; const user = P.session()?.user_id, rows = [];
        try {
          for (let day = new Date(from + "T12:00:00"), destination = new Date(target + "T12:00:00"); P.localDate(day) <= to; day.setDate(day.getDate() + 1), destination.setDate(destination.getDate() + 1)) {
            const data = await P.api("/api/time-entries?date=" + P.localDate(day));
            if (P.session()?.user_id !== user) throw new Error(t("La sessione è cambiata."));
            for (const entry of data.entries || []) rows.push({ job_id: entry.job_id, task_id: entry.task_id, task_title: entry.task_title, date: P.localDate(destination), minutes: entry.minutes, note: entry.note || "" });
          }
          if (!rows.length) { error.textContent = t("Nessuna registrazione da copiare."); return; }
          dialog.close(); selectCopy(rows);
        } catch (e) { error.textContent = e.message; } finally { preview.disabled = false; }
      });
      form.append(error, preview, button("Annulla", () => dialog.close())); form.addEventListener("submit", (e) => e.preventDefault()); dialog.append(form); document.body.append(dialog); dialog.addEventListener("close", () => dialog.remove()); dialog.showModal();
    }); copy.id = "hours-copy-selected"; $("hours-copy-week").after(copy);
    $("logout").addEventListener("click", clearAll, { capture: true });
    const persistAttempt = (kind) => !read(kind) || save(kind, true);
    window.PuntoDrafts = Object.freeze({ discard, selectCopy, persistAttempt });
    P.register(({ session }) => {
      const next = session?.authenticated ? session.user_id : null, signature = JSON.stringify(session?.permissions || {});
      if (owner !== null && owner !== next) clearAll();
      owner = next;
      if (!owner) { clearAll(); return; }
      try { Object.keys(localStorage).filter((item) => item.startsWith(prefix) && !item.startsWith(prefix + owner + ".")).forEach((item) => localStorage.removeItem(item)); } catch { /* Optional local storage. */ }
      if (fingerprint && fingerprint !== signature) discard("wizard"); fingerprint = signature;
      copy.hidden = !P.viewingOwnHours(); render();
    });
  }
  if (window.PuntoExtensions) initialize(); else window.addEventListener("punto:extensions-ready", initialize, { once: true });
})();
