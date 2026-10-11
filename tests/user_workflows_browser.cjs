/* Real HTTP journeys; the Python fixture owns all accounts and data. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || "playwright");
const config = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const errors = [];
const snapDir = process.env.PUNTO_WORKFLOWS_SCREENSHOTS;
async function screenshot(page, name) {
  if (snapDir) { fs.mkdirSync(snapDir, { recursive: true }); await page.screenshot({ path: `${snapDir}/${name}.png`, fullPage: true }); }
}
async function tab(page, id) {
  await page.locator(`#tab-${id}`).click();
  await page.locator(`#panel-${id}`).waitFor({ state: "visible" });
}
async function close(page, id) { await page.locator(`#${id} [data-close-dialog="${id}"]`).first().click(); }
async function save(page, button, dialog) {
  await page.locator(`#${button}`).click();
  try { await page.locator(`#${dialog}`).waitFor({ state: "hidden", timeout: 6000 }); }
  catch (e) { throw new Error(`${dialog} remained open: ${await page.locator(`#${dialog}`).innerText()}`); }
}
async function api(page, path) {
  const response = await page.request.get(config.origin + path);
  assert.equal(response.status(), 200, await response.text());
  return response.json();
}
async function session(browser, cookie, width) {
  const context = await browser.newContext({ locale: "it-IT", viewport: { width, height: 960 } });
  const equals = cookie.indexOf("=");
  await context.addCookies([{ name: cookie.slice(0, equals), value: cookie.slice(equals + 1), url: config.origin }]);
  const page = await context.newPage();
  page.on("pageerror", e => errors.push(e.message));
  page.setDefaultTimeout(8000);
  await page.goto(config.origin + "/app");
  const stylesheet = await page.request.get(config.origin + "/time-workflows.css");
  assert.equal(stylesheet.status(), 200);
  await page.locator("#workspace").waitFor({ state: "visible" });
  await page.waitForFunction(() => !document.getElementById("retry-load").disabled);
  return { context, page };
}
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.PUNTO_CHROME_BINARY || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" });
  try {
    const worker = await session(browser, config.worker_cookie, 1280), wp = worker.page;
    await tab(wp, "my-work");
    await wp.locator("#my-work-tasks").getByRole("heading", { name: "Consegna di prova" }).waitFor();
    await wp.locator("#my-work-tasks").getByRole("button", { name: "Registra ore per Consegna di prova", exact: true }).click();
    await wp.waitForFunction(id => document.getElementById("entry-task").value === String(id), config.task_id);
    await wp.locator("#entry-hours").fill("0:15");
    await wp.locator("#entry-note").fill("Dal task personale");
    await save(wp, "entry-save", "entry-dialog");
    assert.equal((await api(wp, "/api/time-entries?date=" + config.today)).entries.find(e => e.note === "Dal task personale").task_id, config.task_id);
    await screenshot(wp, "giornata-desktop");
    await tab(wp, "hours");
    await wp.locator("#entries-body").getByRole("button", { name: "Richiedi rettifica delle ore per Verifica dei flussi" }).first().click();
    await wp.locator("#time-correction-hours").fill("0:45");
    await wp.locator("#time-correction-reason").fill("Correzione verificata nel browser");
    await save(wp, "time-correction-save", "time-correction-dialog");
    await wp.locator("#time-corrections-list").getByText("In attesa", { exact: true }).waitFor();
    await wp.locator("#hours-view-week").click();
    await wp.locator("#hours-week-grid table").waitFor();
    await wp.locator("#hours-copy-week").click();
    await wp.locator("#hours-batch-dialog").waitFor({ state: "visible" });
    assert.equal(await wp.locator("#hours-batch-rows .hours-batch-row").count(), 1);
    await wp.locator('#hours-batch-rows textarea').fill("Copia confermata");
    await screenshot(wp, "settimana-anteprima");
    await save(wp, "hours-batch-save", "hours-batch-dialog");
    assert.equal((await api(wp, "/api/time-entries/week?date=" + config.today)).entries.find(e => e.note === "Copia confermata").task_id, config.task_id);
    await tab(wp, "jobs");
    await wp.locator(`#jobs-body tr[data-job-id="${config.job_id}"]`).getByRole("button", { name: "Apri riepilogo della commessa Verifica dei flussi", exact: true }).click();
    await wp.locator("#job-overview-content").getByRole("button", { name: "Registra ore nella commessa", exact: true }).waitFor();
    await close(wp, "job-overview-dialog");
    await tab(wp, "notifications");
    await wp.locator("#work-notifications-list").getByRole("button", { name: /Segna.*lett/ }).first().click();
    await wp.locator("#work-notifications-refresh").waitFor({ state: "visible" });
    await wp.locator("#work-notifications-list").getByRole("button", { name: /Apri/ }).first().click();
    await wp.locator("#panel-work-board").waitFor({ state: "visible" });

    const manager = await session(browser, config.manager_cookie, 1280), mp = manager.page;
    await tab(mp, "hours");
    await mp.locator("#time-corrections-list").getByRole("button", { name: /Approva richiesta/ }).first().click();
    await mp.locator("#time-decision-reason").fill("Verificato con la persona");
    await save(mp, "time-decision-save", "time-decision-dialog");
    await mp.locator("#time-corrections-list").getByText("Approvata", { exact: true }).waitFor();
    await tab(mp, "jobs");
    const jobRow = mp.locator(`#jobs-body tr[data-job-id="${config.job_id}"]`);
    await jobRow.getByRole("button", { name: "Apri riepilogo della commessa Verifica dei flussi", exact: true }).click();
    await mp.locator("#job-overview-content").getByText("Ore per attività", { exact: true }).waitFor();
    await screenshot(mp, "riepilogo-desktop");
    await close(mp, "job-overview-dialog");
    await jobRow.getByRole("button", { name: "Spiega accesso alla commessa Verifica dei flussi", exact: true }).click();
    await mp.locator("#job-access-user").selectOption(String(config.worker_id));
    await mp.locator("#job-access-content").getByText("Abilitazione diretta", { exact: true }).waitFor();
    await close(mp, "job-access-dialog");
    await tab(mp, "work-schedules");
    await mp.locator("#work-schedules-user").selectOption(String(config.worker_id));
    await mp.locator("#work-schedule-add").click();
    await mp.locator("#work-schedule-mon").fill("240");
    await save(mp, "work-schedule-save", "work-schedule-dialog");
    await mp.locator("#work-schedules-list").getByText(/4 h/).waitFor();
    await mp.locator("#work-exception-add").click();
    await mp.locator("#work-schedule-reason").fill("Assenza concordata");
    await save(mp, "work-schedule-save", "work-schedule-dialog");
    await mp.locator("#work-exceptions-list").getByText("Assenza concordata", { exact: true }).waitFor();
    await mp.locator("#work-holiday-add").click();
    await mp.locator("#work-schedule-reason").fill("Festività di prova");
    await save(mp, "work-schedule-save", "work-schedule-dialog");
    await mp.locator("#work-holidays-list").getByText("Festività di prova", { exact: true }).waitFor();
    await tab(mp, "jobs");
    await mp.locator("#job-wizard-open").click();
    await mp.locator("#job-wizard-next").waitFor({ state: "visible" });
    await mp.locator("#job-wizard-code").fill("BROWSER-WIZARD");
    await mp.locator("#job-wizard-name").fill("Commessa guidata da browser");
    await mp.locator("#job-wizard-next").click();
    await mp.locator("#job-wizard-user-add").click();
    await mp.locator('[id^="wizard-team-target-"]').selectOption(String(config.worker_id));
    await mp.locator('[id^="wizard-team-role-"]').selectOption("2");
    await mp.locator("#job-wizard-next").click();
    await mp.locator("#job-wizard-task-add").click();
    await mp.locator('[id^="wizard-task-title-"]').fill("Attività iniziale guidata");
    await mp.locator('[id^="wizard-task-person-"]').selectOption(String(config.worker_id));
    await mp.locator("#job-wizard-next").click();
    await mp.locator("#job-wizard-save-template").fill("Modello browser");
    await screenshot(mp, "wizard-riepilogo");
    await save(mp, "job-wizard-save", "job-wizard-dialog");
    assert((await api(mp, "/api/job-templates")).templates.some(t => t.name === "Modello browser"));
    await mp.locator("#job-overview-dialog").waitFor({ state: "visible" });
    await close(mp, "job-overview-dialog");
    for (const operation of ["close", "archive", "reopen"]) {
      if (operation === "reopen") await mp.locator("#jobs-show-archived").check();
      await jobRow.getByRole("button", { name: "Gestisci chiusura e archivio di Verifica dei flussi", exact: true }).click();
      await mp.locator("#job-lifecycle-action").selectOption(operation);
      await mp.locator("#job-lifecycle-reason").fill("Verifica " + operation);
      await save(mp, "job-lifecycle-save", "job-lifecycle-dialog");
    }
    assert.equal((await api(mp, `/api/jobs/${config.job_id}/closure`)).archived, false);
    for (const identity of [config.manager_cookie, config.worker_cookie]) {
      const mobile = await session(browser, identity, 375), p = mobile.page;
      for (const name of ["my-work", "hours", "work-schedules", "notifications"]) await tab(p, name);
      await tab(p, "my-work");
      await screenshot(p, identity === config.worker_cookie ? "giornata-mobile" : "gestore-mobile");
      assert.equal(await p.evaluate(() => document.documentElement.scrollWidth > innerWidth + 1), false, "Mobile page overflows horizontally");
      if (identity === config.worker_cookie) assert.equal(await p.locator("#work-schedules-actions").isVisible(), false);
      await mobile.context.close();
    }
    assert.deepEqual(errors, []);
    await manager.context.close(); await worker.context.close();
    process.stdout.write("Workflow journeys passed: real HTTP, manager/worker, 1280px and 375px, no browser errors.\n");
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
