/* Isolated browser checks against stub APIs; never uses deployed data/accounts. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const http = require("node:http");
const path = require("node:path");
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(__dirname, "../ticket-app/public");
const consoleUrl = "https://auth.example.test/admin/punto/console/";
let scenario;
let writes = 0;
const users = [{ id: 1, name: "Amministratore centrale", email: "admin@example.test", role: "admin", active: true },
               { id: 2, name: "Operatore centrale", email: "agent@example.test", role: "agent", active: true }];
const server = http.createServer((req, res) => {
  const url = new URL(req.url, "http://localhost");
  const json = (status, body) => { res.writeHead(status, { "Content-Type": "application/json" }); res.end(JSON.stringify(body)); };
  if (req.method !== "GET") { writes++; json(405, { ok: false, error: "Read-only UI fixture" }); return; }
  if (url.pathname === "/api/auth/config") {
    json(200, { sso: true, local: true, ...(scenario.managed ? { managed_users: true, users_admin_url: scenario.badUrl ? "javascript:alert(1)" : scenario.noUrl ? "" : consoleUrl } : {}) });
    return;
  }
  if (url.pathname === "/api/session") {
    if (scenario.login) json(401, { ok: false, error: "Accedi per continuare." });
    else json(200, { ok: true, user: users[scenario.role === "agent" ? 1 : 0], csrf_token: "fixture-csrf" });
    return;
  }
  if (url.pathname === "/api/users") { json(200, { items: users }); return; }
  if (url.pathname === "/api/integrations/punto/status") { json(200, { configured: false, connected: false }); return; }
  if (url.pathname === "/api/tickets") { json(200, { items: [], total: 0, stats: {}, queues: {} }); return; }
  const file = path.resolve(root, "." + (url.pathname === "/" ? "/index.html" : url.pathname));
  if (!file.startsWith(root + path.sep) || !fs.existsSync(file)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { "Content-Type": file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : file.endsWith(".json") ? "application/json" : "text/html" });
  res.end(fs.readFileSync(file));
});

(async () => {
  await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
  const browser = await chromium.launch({ headless: true, executablePath: process.env.PUNTO_CHROME_BINARY || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" });
  try {
    for (const width of [320, 375, 1280]) {
      for (scenario of [{ managed: true, role: "admin" }, { managed: true, role: "agent" }, { managed: true, role: "admin", noUrl: true }, { managed: false, role: "admin" }, { managed: true, login: true }, { managed: true, badUrl: true }]) {
        const context = await browser.newContext({ locale: "it-IT", viewport: { width, height: 900 } });
        const page = await context.newPage();
        const errors = [];
        page.on("pageerror", error => errors.push(error.message));
        await page.goto(`http://127.0.0.1:${server.address().port}/`);
        if (scenario.badUrl || scenario.login) {
          await page.locator("#login-view").waitFor({ state: "visible" });
          assert.equal(await page.locator("#login-form").isVisible(), !scenario.badUrl);
          assert.equal(await page.locator("#sso-login").isVisible(), !scenario.badUrl);
          if (scenario.badUrl) assert.equal(await page.locator("#login-error").isVisible(), true);
        } else {
          await page.locator("#app-view").waitFor({ state: "visible" });
          await page.waitForFunction(() => !document.getElementById("refresh-tickets").disabled);
          assert.equal(await page.locator("#manage-users").isVisible(), !scenario.managed);
          assert.equal(await page.locator("#manage-central-users").isVisible(), scenario.managed && scenario.role === "admin" && !scenario.noUrl);
          if (scenario.managed) {
            if (scenario.role === "admin" && !scenario.noUrl) {
              assert.equal(await page.locator("#manage-central-users").getAttribute("href"), consoleUrl);
              assert.equal(await page.locator("#manage-central-users").getAttribute("rel"), "noopener noreferrer");
            }
            await page.locator("#manage-users").dispatchEvent("click");
            await page.locator("#new-user").dispatchEvent("click");
            assert.equal(await page.locator("#users-dialog").isVisible(), false);
            assert.equal(await page.locator("#user-editor").isVisible(), false);
            assert.equal(await page.locator("#users-rows tr").count(), 0);
          } else {
            await page.locator("#manage-users").click();
            await page.locator("#users-dialog").waitFor({ state: "visible" });
            assert.equal(await page.locator("#new-user").isVisible(), true);
            await page.locator('#users-dialog [data-close-dialog="users-dialog"]').first().click();
          }
          await page.locator("#new-ticket").click();
          await page.locator("#ticket-editor").waitFor({ state: "visible" });
          assert.equal(await page.locator("#ticket-assignee option").count(), 3, "Assignees remain available in managed mode");
          await page.locator('#ticket-editor [data-close-dialog="ticket-editor"]').first().click();
        }
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), true, JSON.stringify({ width, scenario }));
        assert.deepEqual(errors, []);
        await context.close();
      }
    }
    assert.equal(writes, 0);
    process.stdout.write("Ticket managed UI: administrator/operator, legacy admin, fallback, invalid URL and 320/375/1280px layouts passed.\n");
  } finally {
    await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; server.close(); });
