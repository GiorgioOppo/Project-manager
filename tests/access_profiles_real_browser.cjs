/* Real HTTP authorization journeys; the Python fixture owns all accounts/data. */
const assert = require('node:assert/strict'), fs = require('node:fs');
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || 'playwright');
const config = JSON.parse(fs.readFileSync(process.argv[2], 'utf8')), errors = [], snaps = process.env.PUNTO_ACCESS_SCREENSHOTS;
async function screenshot(page, name) { if (snaps) { fs.mkdirSync(snaps, { recursive: true }); await page.screenshot({ path: `${snaps}/${name}.png`, fullPage: true }); } }
async function tab(page, name) { await page.locator('#tab-' + name).click(); await page.locator('#panel-' + name).waitFor({ state: 'visible' }); }
async function close(page, id) { await page.locator('#' + id + ' [data-close-dialog="' + id + '"]').first().click(); }
async function session(browser, cookie, width) {
  const context = await browser.newContext({ locale: 'it-IT', viewport: { width, height: 960 } }), index = cookie.indexOf('=');
  await context.addCookies([{ name: cookie.slice(0, index), value: cookie.slice(index + 1), url: config.origin }]);
  const page = await context.newPage(); page.setDefaultTimeout(8000); page.on('pageerror', e => errors.push(e.message));
  await page.goto(config.origin + '/app'); await page.locator('#workspace').waitFor({ state: 'visible' }); await page.waitForFunction(() => !document.getElementById('retry-load').disabled);
  return { context, page };
}
async function profiles(page, userId, selected) {
  await tab(page, 'users');
  // Names are known fixture identities; the action label remains accessible to users.
  const row = page.locator('#users-body tr').filter({ hasText: userId === config.manager_id ? 'manager@punto.local' : 'finance@punto.local' });
  await row.getByRole('button', { name: /Gestisci i profili funzionali/ }).click();
  await page.waitForFunction(() => !document.getElementById('access-groups-save').disabled);
  for (const code of ['sales', 'finance']) await page.locator(`#access-groups-options input[value="${code}"]`).setChecked(selected.includes(code));
  await page.locator('#access-groups-save').click(); await page.locator('#access-groups-dialog').waitFor({ state: 'hidden' });
  await page.waitForFunction(() => !document.getElementById('retry-load').disabled);
}
async function denied(page, endpoint) {
  const response = await page.request.get(config.origin + endpoint); assert.equal(response.status(), 403, await response.text()); assert.doesNotMatch(await response.text(), /DETTAGLIO-ALTRO-GRUPPO/);
}
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.PUNTO_CHROME_BINARY || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' });
  try {
    for (const width of [1280, 375]) {
      const manager = await session(browser, config.manager_cookie, width), mp = manager.page;
      assert.equal(await mp.locator('#tab-contracts').isVisible(), false);
      await tab(mp, 'hours'); await mp.waitForFunction(() => document.getElementById('hours-team-user').options.length >= 2);
      assert.equal(await mp.locator(`#hours-team-user option[value="${config.outsider_id}"]`).count(), 0);
      await mp.locator('#hours-team-user').selectOption(String(config.team_id)); await mp.locator('#entries-body').getByText('Ore della squadra').waitFor();
      assert.equal(await mp.locator('#entries-body button').count(), 1); assert.equal(await mp.locator('#hours-copy-week').isVisible(), false);
      await mp.locator('#entries-body button').click(); assert.equal(await mp.locator('#hours-entry-detail-actions button').count(), 1); await close(mp, 'hours-entry-detail');
      await mp.locator('#hours-view-week').click(); await mp.locator('#hours-week-grid table').waitFor(); assert.equal(await mp.locator('#hours-week-grid button').count(), 1);
      await screenshot(mp, 'real-team-' + width);
      for (const endpoint of [`/api/time-entries?date=${config.today}&user_id=${config.outsider_id}`, `/api/time-entries/week?date=${config.today}&user_id=${config.outsider_id}`, `/api/reports?view=date&from=${config.today}&to=${config.today}&user_id=${config.outsider_id}`, '/api/contracts', `/api/reports?view=labor_cost&from=${config.today}&to=${config.today}`]) await denied(mp, endpoint);
      await mp.goto(`${config.origin}/app#analisi/periodo?from=${config.today}&to=${config.today}&user_id=${config.outsider_id}`);
      await mp.locator('#workspace').waitFor({ state: 'visible' }); await mp.waitForFunction(() => !document.getElementById('retry-load').disabled);
      assert.doesNotMatch(await mp.locator('#report-body').textContent(), /DETTAGLIO-ALTRO-GRUPPO|Persona altro gruppo/);
      await profiles(mp, config.manager_id, ['finance']);
      assert.match(await mp.locator('#session-role').textContent(), /Finance/);
      await tab(mp, 'users'); assert.match(await mp.locator('#users-body tr').filter({ hasText: 'manager@punto.local' }).locator('.role-badge').allTextContents().then(values => values.join(' ')), /Finance/);
      await tab(mp, 'reports'); await mp.locator('#report-tab-costo-ore').click();
      await mp.locator('#report-body').getByText('Commessa segregata', { exact: true }).waitFor(); await mp.locator('#report-rates-body tr').first().waitFor();
      assert.equal(await mp.locator('#report-user-field').isVisible(), false); assert.doesNotMatch(await mp.locator('#report-head').textContent(), /Utente/);
      assert.doesNotMatch(await mp.locator('#report-body').textContent(), /Persona altro gruppo|outside@punto.local|DETTAGLIO-ALTRO-GRUPPO/);
      assert.equal(await mp.locator('#report-new-rate').isVisible(), true);
      await profiles(mp, config.manager_id, []); await tab(mp, 'reports'); assert.equal(await mp.locator('#report-tab-costo-ore').isVisible(), false); assert.equal(await mp.locator('#report-rates-body tr').count(), 0);
      await denied(mp, '/api/labor-rates');
      await profiles(mp, config.manager_id, ['sales']);
      assert.match(await mp.locator('#session-role').textContent(), /Commerciali/);
      await tab(mp, 'users'); assert.match(await mp.locator('#users-body tr').filter({ hasText: 'manager@punto.local' }).locator('.role-badge').allTextContents().then(values => values.join(' ')), /Commerciali/);
      await tab(mp, 'contracts'); await mp.locator('#contracts-body tr').waitFor(); assert.equal(await mp.locator('#new-contract').isVisible(), true);
      await profiles(mp, config.finance_id, ['finance']);
      const finance = await session(browser, config.finance_cookie, width), fp = finance.page;
      assert.equal(await fp.locator('#tab-users').isVisible(), false); await tab(fp, 'hours'); assert.equal(await fp.locator('#hours-team-toolbar').isVisible(), false);
      await fp.locator('#entries-body').getByText('Ore Finance personali').waitFor();
      await denied(fp, `/api/time-entries?date=${config.today}&user_id=${config.outsider_id}`);
      await tab(fp, 'contracts'); await fp.locator('#contracts-body tr').waitFor(); assert.equal(await fp.locator('#new-contract').isVisible(), false);
      assert.equal(await fp.locator('#contracts-body button').count(), 1);
      await fp.locator('#contracts-body').getByRole('button', { name: /Apri i documenti/ }).click(); await fp.locator('#documents-list a').waitFor(); assert.equal(await fp.locator('#document-form').isVisible(), false);
      const download = await fp.request.get(config.origin + await fp.locator('#documents-list a').getAttribute('href')); assert.equal(download.status(), 200); await close(fp, 'documents-dialog');
      await tab(fp, 'reports'); await fp.locator('#report-tab-costo-ore').click(); await fp.locator('#report-body').getByText('Commessa segregata', { exact: true }).waitFor();
      assert.equal(await fp.locator('#report-new-rate').isVisible(), false); await fp.locator('#report-rates-body tr').first().waitFor(); assert.equal(await fp.locator('#report-rates-body button').count(), 0);
      await denied(fp, `/api/reports?view=labor_cost&from=${config.today}&to=${config.today}&user_id=${config.outsider_id}`);
      await screenshot(fp, 'real-finance-' + width);
      await profiles(mp, config.finance_id, ['sales']); await fp.reload(); await fp.locator('#workspace').waitFor({ state: 'visible' }); await fp.waitForFunction(() => !document.getElementById('retry-load').disabled);
      await tab(fp, 'contracts'); assert.equal(await fp.locator('#new-contract').isVisible(), true); await tab(fp, 'reports'); assert.equal(await fp.locator('#report-tab-costo-ore').isVisible(), false);
      await profiles(mp, config.finance_id, []); await denied(fp, '/api/contracts');
      await fp.reload(); await fp.locator('#workspace').waitFor({ state: 'visible' }); assert.equal(await fp.locator('#tab-contracts').isVisible(), false);
      await profiles(mp, config.manager_id, []); assert.equal(await mp.locator('#tab-contracts').isVisible(), false); await denied(mp, '/api/contracts');
      for (const page of [mp, fp]) assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
      await manager.context.close(); await finance.context.close();
    }
    const localized = await session(browser, config.manager_cookie, 1280), lp = localized.page;
    const grant = await lp.request.post(config.origin + `/api/users/${config.manager_id}/access-groups`, { form: { groups: '["finance"]' }, headers: { Origin: config.origin } });
    assert.equal(grant.status(), 200);
    for (const language of ['en', 'fr']) {
      const catalog = JSON.parse(fs.readFileSync(require('node:path').resolve(__dirname, `../public/locales/${language}.json`), 'utf8'));
      await lp.goto(config.origin + '/app?lang=' + language + '#users'); await lp.locator('#workspace').waitFor({ state: 'visible' }); await lp.waitForFunction(() => !document.getElementById('retry-load').disabled);
      const row = lp.locator('#users-body tr').filter({ hasText: 'manager@punto.local' }); await row.locator('button').last().click(); await lp.waitForFunction(() => !document.getElementById('access-groups-save').disabled);
      assert.equal(await lp.locator('#access-groups-title').textContent(), catalog['Profili funzionali']);
      assert.equal(await lp.locator('#access-groups-description').textContent(), catalog['I profili definiscono le funzioni disponibili. Le squadre organizzative definiscono quali persone un gestore può consultare. Operatori è automatico; Gestori deriva dal ruolo dell’identità.']);
      await screenshot(lp, 'real-profiles-' + language); await close(lp, 'access-groups-dialog');
      await tab(lp, 'reports'); await lp.locator('#report-tab-costo-ore').click(); await lp.locator('#report-body tr').waitFor();
      assert.equal(await lp.locator('#report-scope').textContent(), catalog['Vista Finance: costi aggregati per commessa, senza il dettaglio delle ore delle altre persone.']);
      await lp.setViewportSize({ width: 375, height: 960 }); assert.equal(await lp.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
      assert.equal(await lp.evaluate(() => document.getElementById('session-role').getBoundingClientRect().right <= document.getElementById('logout').getBoundingClientRect().left), true, 'Profile badges must fit beside logout on mobile');
      await screenshot(lp, 'real-financial-' + language); await lp.setViewportSize({ width: 1280, height: 960 });
    }
    await localized.context.close();
    assert.deepEqual(errors, []); console.log(JSON.stringify({ result: 'PASS', realHTTP: true, widths: [1280,375], checks: ['Live Finance/Sales grant and revoke on existing sessions', 'Team and descendant read-only day/week, outside group URL denial', 'Finance personal hours and global job cost aggregates', 'Finance PDFs read-only and rates read-only; manager-Finance rate write', 'Revocation clears frontend privileged caches', 'No page errors or document overflow'] }, null, 2));
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
