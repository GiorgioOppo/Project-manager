/* Isolated browser fixture: exercises group guards without deployed data. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const http = require("node:http");
const path = require("node:path");
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(__dirname, "../public");
const groupsUrl = "https://auth.example.test/admin/punto/console/#/punto/groups";
let scenario;
const writes = [];
const users = [{ id: 1, name: "Gestore centrale", email: "admin@example.test", role: "manager", active: true }, { id: 2, name: "Operatore centrale", email: "worker@example.test", role: "worker", active: true }];
const groups = [{ id: 3, name: "Area tecnica", parent_id: null, members_count: 1, children_count: 1 }, { id: 4, name: "Progettazione", parent_id: 3, members_count: 0, children_count: 0 }];
const roles = [{ id: 1, code: "reader", name: "Lettore", can_log_time: false, builtin: true }, { id: 2, code: "contributor", name: "Collaboratore", can_log_time: true, builtin: true }, { id: 3, code: "reviewer", name: "Verificatore", can_log_time: false, builtin: false }];
const server = http.createServer((req, res) => {
  const url = new URL(req.url, "http://localhost");
  const json = (status, body) => { res.writeHead(status, { "Content-Type": "application/json" }); res.end(JSON.stringify(body)); };
  if (req.method !== "GET") { writes.push([req.method, url.pathname]); json(405, { ok: false, error: "Read-only fixture" }); return; }
  if (url.pathname === "/api/auth/config") {
    json(200, { local: true, sso: true, managed_users: true, users_admin_url: "https://auth.example.test/admin/punto/console/", ...(scenario.managed ? { managed_groups: true, groups_admin_url: scenario.badUrl ? "javascript:alert(1)" : scenario.noUrl ? "" : groupsUrl } : {}) }); return;
  }
  if (url.pathname === "/api/session") {
    const manager = scenario.role !== "worker";
    json(200, { authenticated: true, ...users[manager ? 0 : 1], permissions: { manage_groups: manager, manage_job_roles: manager, manage_assignments: manager, manage_jobs: manager, edit_time_entries: manager, manage_users: false, ...(scenario.managed ? { edit_groups: false } : {}) } }); return;
  }
  if (url.pathname === "/api/groups") { json(200, { groups: scenario.empty ? [] : groups }); return; }
  if (/^\/api\/groups\/\d+\/members$/.test(url.pathname)) { json(200, { members: [{ user_id: 1, ...users[0] }] }); return; }
  if (url.pathname === "/api/job-roles") { json(200, { roles }); return; }
  if (url.pathname === "/api/users") { json(200, { users }); return; }
  if (url.pathname === "/api/jobs") { json(200, { jobs: [{ id: 7, code: "TEST-7", name: "Commessa condivisa", status: "active", client: "", total_minutes: 0, assigned_users: 1, assigned_groups: 1, can_log_time: true }] }); return; }
  if (url.pathname === "/api/jobs/7/assignments") { json(200, { assignments: [] }); return; }
  if (url.pathname === "/api/jobs/7/group-grants") { json(200, { grants: [] }); return; }
  if (url.pathname === "/api/time-entries") { json(200, { entries: [], total_minutes: 0 }); return; }
  if (url.pathname === "/api/notifications") { json(200, { notifications: [], unread_count: 0 }); return; }
  if (url.pathname === "/api/identity-sync") { json(200, { enabled: false, items: [] }); return; }
  const file = path.resolve(root, "." + (url.pathname === "/app" ? "/app.html" : url.pathname));
  if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { "Content-Type": file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : file.endsWith(".json") ? "application/json" : "text/html" });
  res.end(fs.readFileSync(file));
});

(async () => {
  await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
  const browser = await chromium.launch({ headless: true, executablePath: process.env.PUNTO_CHROME_BINARY || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" });
  try {
    for (const width of [375, 1280]) {
      for (scenario of [{ managed: true }, { managed: true, noUrl: true }, { managed: true, empty: true }, { managed: false }, { managed: true, role: "worker" }, { managed: true, badUrl: true }]) {
        const context = await browser.newContext({ locale: "it-IT", viewport: { width, height: 900 } });
        const page = await context.newPage();
        const errors = [];
        page.on("pageerror", error => errors.push(error.message));
        await page.goto(`http://127.0.0.1:${server.address().port}/app`);
        if (scenario.badUrl) {
          await page.waitForFunction(() => document.getElementById("session-status").textContent.includes("Configurazione accesso non disponibile"));
          assert.equal(await page.locator("#workspace").isVisible(), false);
          assert.equal(await page.locator("#keycloak-groups-link").getAttribute("href"), null);
        } else {
          await page.locator("#workspace").waitFor({ state: "visible" });
          await page.waitForFunction(() => !document.getElementById("retry-load").disabled);
          assert.equal(await page.locator("#tab-organization").isVisible(), scenario.role !== "worker");
          if (scenario.role !== "worker") {
            await page.locator("#tab-organization").click();
            await page.locator("#organization-content").waitFor({ state: "visible" });
            assert.equal(await page.locator("#keycloak-groups-note").isVisible(), scenario.managed);
            assert.equal(await page.locator("#keycloak-groups-link").isVisible(), scenario.managed && !scenario.noUrl);
            if (scenario.managed && !scenario.noUrl) {
              assert.equal(await page.locator("#keycloak-groups-link").getAttribute("href"), groupsUrl);
              assert.equal(await page.locator("#keycloak-groups-link").getAttribute("rel"), "noopener noreferrer");
            }
            assert.equal(await page.locator("#new-group").isVisible(), !scenario.managed);
            assert.equal(await page.locator("#new-job-role").isVisible(), true);
            await page.locator("#new-job-role").click();
            await page.locator("#job-role-dialog").waitFor({ state: "visible" });
            await page.locator('#job-role-dialog [data-close-dialog="job-role-dialog"]').first().click();
            if (!scenario.empty) {
              await page.locator(".group-tree-button").first().click();
              await page.locator("#group-members .organization-member").waitFor();
              assert.match(await page.locator("#group-detail-path").textContent(), /Area tecnica/);
              assert.equal(await page.locator("#group-detail-actions").isVisible(), !scenario.managed);
              assert.equal(await page.locator("#group-member-form").isVisible(), !scenario.managed);
              assert.equal(await page.locator("#group-members button").count(), scenario.managed ? 0 : 1);
            } else assert.match(await page.locator("#groups-empty").textContent(), /Keycloak/);
            if (scenario.managed) {
              for (const id of ["new-group", "edit-group", "delete-group", "organization-confirm-save"]) await page.locator("#" + id).dispatchEvent("click");
              for (const id of ["group-form", "group-member-form"]) await page.locator("#" + id).dispatchEvent("submit");
              assert.equal(await page.locator("#group-dialog").isVisible(), false);
              assert.equal(await page.locator("#organization-confirm").isVisible(), false);
            } else {
              await page.locator("#new-group").click();
              await page.locator("#group-dialog").waitFor({ state: "visible" });
              await page.locator('#group-dialog [data-close-dialog="group-dialog"]').first().click();
            }
            await page.locator("#tab-jobs").click();
            await page.locator('#jobs-body button[aria-label^="Gestisci le abilitazioni"]').click();
            await page.locator("#assignment-dialog").waitFor({ state: "visible" });
            await page.waitForFunction(() => !document.getElementById("group-grant-group").disabled);
            assert.equal(await page.locator("#group-grant-group option").count(), scenario.empty ? 1 : 3);
            assert.equal(await page.locator("#group-grant-role option").count(), 3);
            assert.equal(await page.locator("#assignment-user option").count(), 3);
          }
        }
        assert.deepEqual(errors, []);
        await context.close();
      }
    }
    assert.deepEqual(writes, [], "Hidden group handlers must not write to any API");
    process.stdout.write("Punto groups UI: managed/local, member guards, hierarchy, job roles and grant selectors passed at 375/1280px.\n");
  } finally {
    await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; server.close(); });
