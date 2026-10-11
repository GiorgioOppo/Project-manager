/* Runs only against the two temporary servers started by test_i18n.py. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(__dirname, "..");
const [punto, ticket] = process.argv.slice(2);
const locales = { it: "it-IT", en: "en-GB", zh: "zh-CN", es: "es-ES", fr: "fr-FR", de: "de-DE" };
const userText = "Commesse <script>window.injected = true</script> $& 中文";
const credentials = { email: "i18n@example.test", password: "I18n-test-only!2026" };
const catalog = (language) => language === "it" ? {} : JSON.parse(fs.readFileSync(path.join(root, "public/locales", language + ".json"), "utf8"));

(async () => {
  const browser = await chromium.launch({ executablePath: process.env.PUNTO_CHROME_BINARY || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", headless: true });
  try {
    for (const [language, locale] of Object.entries(locales)) {
      const translations = catalog(language);
      const expected = (source) => translations[source] || source;
      const context = await browser.newContext({ locale: "it-IT", viewport: { width: 1280, height: 900 } });
      const page = await context.newPage();
      const errors = [];
      page.on("pageerror", (error) => errors.push(error.message));
      await page.goto(punto + "/?lang=" + language);
      await page.waitForFunction(() => window.PuntoI18n && !document.getElementById("login-form").hidden);
      assert.equal(await page.locator("html").getAttribute("lang"), locale);
      assert.equal(await page.locator('label[for="email"]').textContent(), expected("Indirizzo email"));
      await page.locator("#login-submit").click();
      assert.equal(await page.locator("#email-error").textContent(), expected("Inserisci il tuo indirizzo email."));
      await page.locator("#email").fill(credentials.email);
      await page.locator("#password").fill(credentials.password);
      await page.locator("#login-submit").click();
      await page.locator("#workspace").waitFor({ state: "visible" });
      if (language === "it") {
        // The navigation fixture exercises Sales/Finance pages as well as
        // administration; space administration alone does not grant them.
        const session = await (await page.request.get(punto + "/api/session")).json();
        assert.ok(Number.isInteger(session.user_id));
        const profiles = await page.request.post(punto + `/api/users/${session.user_id}/access-groups`, {
          form: { groups: '["sales","finance"]', reason: "Profili per la fixture isolata delle traduzioni" },
          headers: { Origin: punto },
        });
        assert.equal(profiles.status(), 200, await profiles.text());
        await page.reload();
        await page.locator("#workspace").waitFor({ state: "visible" });
      }
      assert.equal(await page.locator("#tab-jobs span").first().textContent(), expected("Commesse"));
      assert.equal(await page.locator("#language-select").inputValue(), language);
      await page.setViewportSize({ width: 320, height: 780 });
      if (!(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1))) {
        await page.screenshot({ path: "/tmp/punto-i18n-mobile.png", fullPage: true });
        const wide = await page.evaluate(() => [...document.querySelectorAll("body *")].filter((element) => element.getBoundingClientRect().right > window.innerWidth + 1).slice(0, 18).map((element) => [element.tagName, element.id, element.className, element.textContent.trim().slice(0, 100), Math.round(element.getBoundingClientRect().right)]));
        assert.fail(language + " Punto mobile overflow: " + JSON.stringify(wide));
      }
      await page.setViewportSize({ width: 1280, height: 900 });
      await page.locator("#new-job").click();
      await page.locator("#job-code").fill("I18N-" + language);
      await page.locator("#job-name").fill(userText);
      await page.locator("#job-status").selectOption("active");
      const saved = page.waitForResponse((response) => response.url().endsWith("/api/jobs") && response.request().method() === "POST");
      await page.locator("#job-save").click();
      assert.equal((await saved).status(), 201);
      await page.locator("#job-dialog").waitFor({ state: "hidden" });
      await page.locator("#jobs-body").getByText(userText, { exact: true }).first().waitFor();
      assert.equal(await page.evaluate(() => window.injected), undefined);
      for (const tab of ["work-board", "hours", "reports", "users", "organization", "alerts", "suppliers", "contracts", "audit", "jobs"]) {
        await page.locator("#tab-" + tab).click();
        await page.locator("#panel-" + tab).waitFor({ state: "visible" });
      }
      // This checks the actual drill-down callback, whose route values are machine codes.
      await page.locator("#tab-reports").click();
      await page.locator("#report-tab-commesse").click();
      assert.match(page.url(), /#analisi\/commesse/);
      await page.locator("#tab-jobs").click();
      await page.locator("#new-job").click();
      await page.locator("#job-code").fill("UNSAVED");
      // A native modal makes the header unavailable to pointer users. A change
      // event still verifies the safeguard if a keyboard/script changes it.
      let confirmMessage = "";
      page.once("dialog", async (dialog) => { confirmMessage = dialog.message(); await dialog.dismiss(); });
      const next = language === "en" ? "de" : "en";
      await page.locator("#language-select").selectOption(next, { force: true });
      assert.ok(confirmMessage);
      assert.equal(await page.locator("#language-select").inputValue(), language);
      await page.locator('#job-dialog [data-close-dialog="job-dialog"]').first().click();
      await page.locator("#language-select").selectOption(next);
      await page.waitForFunction((key) => window.PuntoI18n?.language === key && document.getElementById("language-select")?.value === key, next);
      assert.equal(await page.evaluate(() => localStorage.getItem("punto.language")), next);
      assert.equal(new URL(page.url()).hash, "#jobs");
      assert.deepEqual(errors, []);
      await context.close();

      const ticketContext = await browser.newContext({ locale: "it-IT" });
      const ticketPage = await ticketContext.newPage();
      const ticketErrors = [];
      ticketPage.on("pageerror", (error) => ticketErrors.push(error.message));
      await ticketPage.goto(ticket + "/?lang=" + language);
      await ticketPage.locator("#login-view").waitFor({ state: "visible" });
      assert.equal(await ticketPage.locator("html").getAttribute("lang"), locale);
      await ticketPage.locator("#login-email").fill(credentials.email);
      await ticketPage.locator("#login-password").fill(credentials.password);
      await ticketPage.locator('#login-form [type="submit"]').click();
      await ticketPage.locator("#app-view").waitFor({ state: "visible" });
      await ticketPage.setViewportSize({ width: 320, height: 780 });
      assert.ok(await ticketPage.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1), language + " Ticket mobile overflow");
      await ticketPage.setViewportSize({ width: 1280, height: 900 });
      await ticketPage.locator("#new-ticket").click();
      await ticketPage.locator("#ticket-title").fill(userText);
      await ticketPage.locator("#editor-impact").selectOption("high");
      await ticketPage.locator("#ticket-priority").selectOption("high");
      const created = ticketPage.waitForResponse((response) => response.url().endsWith("/api/tickets") && response.request().method() === "POST");
      await ticketPage.locator("#save-ticket").click();
      const createdResponse = await created;
      assert.equal(createdResponse.status(), 201, await createdResponse.text());
      await ticketPage.locator("#ticket-editor").waitFor({ state: "hidden" });
      await ticketPage.waitForFunction((value) => document.getElementById("detail-title").textContent === value, userText);
      assert.equal(await ticketPage.locator("#detail-title").textContent(), userText);
      assert.equal(await ticketPage.locator("#detail-status").inputValue(), "open");
      assert.equal(await ticketPage.evaluate(() => window.injected), undefined);
      assert.deepEqual(ticketErrors, []);
      await ticketContext.close();
      process.stdout.write(language + ": login, Punto forms/navigation, Ticket forms and data safety passed\n");
    }
    // Failed catalogs and unavailable storage must leave both local and SSO login usable.
    const fallback = await browser.newContext();
    const page = await fallback.newPage();
    await page.route("**/locales/en.json", (route) => route.fulfill({ status: 503, body: "unavailable" }));
    await page.goto(punto + "/?lang=en");
    await page.locator("#login-form").waitFor({ state: "visible" });
    assert.equal(await page.locator("html").getAttribute("lang"), "it-IT");
    assert.ok(await page.locator(".language-error").isVisible());
    await fallback.close();
    const restricted = await browser.newContext({ locale: "ja-JP" });
    await restricted.addInitScript(() => {
      Object.defineProperty(window, "localStorage", { get() { throw new DOMException("blocked", "SecurityError"); } });
      Object.defineProperty(window, "sessionStorage", { get() { throw new DOMException("blocked", "SecurityError"); } });
    });
    const restrictedPage = await restricted.newPage();
    await restrictedPage.goto(punto + "/?lang=../../not-a-locale");
    await restrictedPage.locator("#login-form").waitFor({ state: "visible" });
    assert.equal(await restrictedPage.locator("#language-select").inputValue(), "it");
    await restrictedPage.locator("#language-select").selectOption("fr");
    await restrictedPage.waitForFunction(() => window.PuntoI18n?.language === "fr");
    assert.equal(await restrictedPage.locator("html").getAttribute("lang"), "fr-FR");
    await restricted.close();
    const ssoContext = await browser.newContext({ locale: "fr-FR" });
    const ssoPage = await ssoContext.newPage();
    await ssoPage.route("**/api/auth/config", (route) => route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ local: false, sso: true }) }));
    await ssoPage.goto(punto + "/?lang=fr");
    await ssoPage.locator("#sso-signin").waitFor({ state: "visible" });
    assert.ok(await ssoPage.locator("#login-form").isHidden());
    assert.equal(await ssoPage.locator("#sso-signin a").textContent(), catalog("fr")["Accedi con l’account aziendale"]);
    await ssoContext.close();
  } finally { await browser.close(); }
})().catch((error) => { console.error(error); process.exitCode = 1; });
