/* Isolated browser authorization fixture using the actual frontend files.
 * Optional PUNTO_PLAYWRIGHT_MODULE, PUNTO_CHROME_BINARY, PUNTO_ACCESS_SCREENSHOTS.
 */
const assert = require('node:assert/strict'), fs = require('node:fs'), path = require('node:path');
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '../public'), snaps = process.env.PUNTO_ACCESS_SCREENSHOTS;
const users = [{ id: 1, name: 'Gestore', email: 'manager@example.test', role: 'manager', active: true }, { id: 2, name: 'Collega squadra', email: 'team@example.test', role: 'worker', active: true }, { id: 3, name: 'Persona esterna', email: 'outside@example.test', role: 'worker', active: true }];
const today = new Date().toLocaleDateString('en-CA', { timeZone: 'Europe/Rome' });
const job = { id: 7, code: 'JOB-7', name: 'Lavoro autorizzato', client: '', status: 'active', can_log_time: true, total_minutes: 60 };
const contract = { id: 1, code: 'CTR-1', title: 'Accordo commerciale', supplier_id: 1, supplier_name: 'Fornitore', status: 'active', value_cents: 100000, documents_count: 1 };
const groups = [{ code: 'operator', name: 'Operatori', implicit: true, editable: false }, { code: 'manager', name: 'Gestori', implicit: true, editable: false }, { code: 'sales', name: 'Commerciali', implicit: false, editable: true }, { code: 'finance', name: 'Finance', implicit: false, editable: true }];
function permissions(manager, selected, managed) {
  const sales = selected.includes('sales'), finance = selected.includes('finance');
  return { manage_jobs: manager, manage_assignments: manager, manage_groups: manager, manage_job_roles: manager, edit_time_entries: manager, manage_users: manager && !managed, manage_access_groups: manager, view_team_hours: manager, view_contracts: sales || finance, manage_contracts: sales, view_suppliers: sales || finance, manage_suppliers: sales, view_costs: finance, view_labor_rates: finance, manage_labor_rates: manager && finance, view_audit: manager };
}
async function snapshot(page, name) { if (snaps) { fs.mkdirSync(snaps, { recursive: true }); await page.screenshot({ path: path.join(snaps, name + '.png'), fullPage: true }); } }
async function close(page, id) { await page.locator('#' + id + ' [data-close-dialog="' + id + '"]').first().click(); }
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.PUNTO_CHROME_BINARY || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' });
  const errors = [], denied = [], writes = [], reportRequests = [];
  try {
    for (const width of [1280, 375]) for (const scenario of [{ name: 'manager', manager: true, selected: [], managed: false }, { name: 'managed', manager: true, selected: [], managed: true }, { name: 'finance', manager: false, selected: ['finance'], managed: false }, { name: 'sales', manager: false, selected: ['sales'], managed: false }, { name: 'manager-finance', manager: true, selected: ['finance'], managed: false }]) {
      let selected = [...scenario.selected], holdRates = false, releaseRates, rateCaptured;
      const context = await browser.newContext({ locale: 'it-IT', viewport: { width, height: 960 } }), page = await context.newPage();
      page.setDefaultTimeout(8000); page.on('pageerror', e => errors.push(e.message));
      await context.route('http://profiles.test/**', async route => {
        const req = route.request(), url = new URL(req.url()), endpoint = url.pathname;
        const json = (body, status = 200) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
        const perms = permissions(scenario.manager, selected, scenario.managed), effective = ['operator', ...(scenario.manager ? ['manager'] : []), ...selected];
        const deny = () => { denied.push(endpoint); return json({ ok: false, error: 'Forbidden fixture access' }, 403); };
        const session = { ...users[0], user_id: 1, role: scenario.manager ? 'manager' : 'worker', authenticated: true, permissions: perms, access_groups: effective };
        if (endpoint === '/api/session') return json(session);
        if (endpoint === '/api/auth/config') return json({ managed_users: scenario.managed, users_admin_url: scenario.managed ? 'https://auth.example.test/admin/' : '' });
        if (endpoint === '/api/jobs') return json({ jobs: [job] });
        if (endpoint === '/api/time-team') return perms.view_team_hours ? json({ scope: 'team', users: users.slice(0, 2) }) : deny();
        if (['/api/time-entries', '/api/time-entries/week'].includes(endpoint)) {
          if (req.method() === 'POST') { writes.push([endpoint, req.postData()]); return json({ ok: true, id: 88 }); }
          const userId = Number(url.searchParams.get('user_id') || 1);
          if (userId !== 1 && (!perms.view_team_hours || userId !== 2)) return deny();
          const date = url.searchParams.get('date') || today, entry = { id: userId * 10, user_id: userId, job_id: 7, job_code: job.code, job_name: job.name, date, minutes: 60, note: userId === 1 ? 'Ore personali' : 'Ore del collega', revision: 1 };
          return json({ entries: [entry], total_minutes: 60, start: date, end: date, days: [{ date, total_minutes: 60 }] });
        }
        if (endpoint === '/api/users') return scenario.manager ? json({ users: users.map(u => ({ ...u, access_groups: u.id === 1 ? effective : ['operator'] })) }) : deny();
        if (endpoint === '/api/access-groups') return perms.manage_access_groups ? json({ groups }) : deny();
        if (/^\/api\/users\/\d+\/access-groups$/.test(endpoint)) {
          if (!perms.manage_access_groups) return deny();
          if (req.method() === 'POST') { selected = JSON.parse(new URLSearchParams(req.postData()).get('groups')); writes.push([endpoint, [...selected]]); }
          return json({ user_id: 1, active: true, role: 'manager', selected, effective: ['operator', 'manager', ...selected] });
        }
        if (endpoint === '/api/contracts') return perms.view_contracts ? json({ contracts: [contract] }) : deny();
        if (endpoint === '/api/suppliers') return perms.view_suppliers ? json({ suppliers: [{ id: 1, code: 'SUP-1', name: 'Fornitore', active: true }] }) : deny();
        if (endpoint === '/api/contracts/1/documents') return perms.view_contracts ? json({ documents: [{ id: 1, file_name: 'accordo.pdf', byte_size: 1024, created_at: new Date().toISOString() }] }) : deny();
        if (endpoint === '/api/labor-rates') {
          if (!perms.view_labor_rates) return deny();
          if (holdRates) { rateCaptured(); await new Promise(resolve => { releaseRates = resolve; }); }
          return json({ rates: [{ id: 1, user_id: 2, user_name: 'Collega squadra', user_email: 'team@example.test', hourly_cents: 1000, starts_on: today, ends_on: '' }] });
        }
        if (endpoint === '/api/reports') {
          reportRequests.push(Object.fromEntries(url.searchParams));
          const view = url.searchParams.get('view'), financial = ['cost', 'labor_cost'].includes(view);
          if (financial && (!perms.view_costs || url.searchParams.has('user_id'))) return deny();
          const summary = { minutes: 60, entry_count: 1, job_count: 1, user_count: 1, estimated_cost_cents: 1000, priced_minutes: 60, unpriced_minutes: 0, known_value_cents: 100000, contract_count: 1, unpriced_contract_count: 0, uncontracted_minutes: 0 };
          const row = view === 'labor_cost' ? { job_id: 7, code: job.code, job_name: job.name, minutes: 60, entry_count: 1, priced_minutes: 60, unpriced_minutes: 0, cost_cents: 1000 } : view === 'cost' ? { ...contract, contract_id: 1, minutes: 60, job_count: 1 } : { date: today, minutes: 60, entry_count: 1, job_count: 1, user_count: 1 };
          return json({ view, from: url.searchParams.get('from'), to: url.searchParams.get('to'), today, scope: financial ? 'financial' : scenario.manager ? 'team' : 'personal', summary, rows: [row], options: { jobs: [job], users: financial ? [] : scenario.manager ? users.slice(0, 2) : [] } });
        }
        if (endpoint === '/api/time-corrections') return json({ requests: [], scope: scenario.manager ? 'team' : 'personal' });
        if (endpoint === '/api/work-board') return json({ tasks: [], blockers: [], can_manage: scenario.manager });
        if (endpoint === '/api/notifications' || endpoint === '/api/work-notifications') return json({ notifications: [], unread_count: 0 });
        if (endpoint === '/api/identity-sync') return json({ enabled: false, items: [] });
        if (endpoint.startsWith('/api/')) return json({ ok: true, roles: [], groups: [], templates: [] });
        const file = path.join(root, endpoint === '/app' ? 'app.html' : endpoint);
        try { return route.fulfill({ contentType: file.endsWith('.js') ? 'text/javascript' : file.endsWith('.css') ? 'text/css' : file.endsWith('.json') ? 'application/json' : 'text/html', body: fs.readFileSync(file) }); } catch { return route.fulfill({ status: 404, body: '' }); }
      });
      await page.goto('http://profiles.test/app'); await page.locator('#workspace').waitFor({ state: 'visible' }); await page.waitForFunction(() => !document.getElementById('retry-load').disabled);
      assert.equal(await page.locator('#tab-contracts').isVisible(), selected.length > 0);
      assert.equal(await page.locator('#tab-users').isVisible(), scenario.manager);
      await page.locator('#tab-hours').click();
      assert.equal(await page.locator('#hours-team-toolbar').isVisible(), scenario.manager);
      if (scenario.manager) {
        await page.waitForFunction(() => document.querySelector('#hours-team-user').options.length === 2);
        assert.equal(await page.locator('#hours-team-user option[value="3"]').count(), 0);
        await page.locator('#hours-team-user').selectOption('2'); await page.locator('#entries-body').getByText('Ore del collega').waitFor();
        assert.equal(await page.locator('#entries-body button').count(), 1); assert.equal(await page.locator('#hours-copy-week').isVisible(), false);
        await page.locator('#entries-body button').click(); assert.equal(await page.locator('#hours-entry-detail-actions button').count(), 1); await close(page, 'hours-entry-detail');
        await page.locator('#hours-view-week').click(); await page.locator('#hours-week-grid table').waitFor();
        assert.equal(await page.locator('#hours-week-grid .hours-week-add').count(), 0);
        await snapshot(page, scenario.name + '-team-' + width);
        await page.locator('#hours-team-user').selectOption(''); await page.waitForFunction(() => !document.getElementById('hours-copy-week').hidden);
        await page.locator('#hours-view-day').click();
      }
      if (selected.length) {
        await page.locator('#tab-contracts').click(); await page.locator('#contracts-body tr').waitFor();
        const sales = selected.includes('sales'); assert.equal(await page.locator('#new-contract').isVisible(), sales);
        assert.equal(await page.locator('#contracts-body button').count(), sales ? 2 : 1);
        await page.locator('#contracts-body').getByRole('button', { name: /Apri i documenti/ }).click(); await page.locator('#documents-list a').waitFor();
        assert.equal(await page.locator('#document-form').isVisible(), sales); assert.match(await page.locator('#documents-list a').getAttribute('href'), /download$/); await close(page, 'documents-dialog');
        await page.locator('#tab-suppliers').click(); await page.locator('#suppliers-body tr').waitFor(); assert.equal(await page.locator('#suppliers-body button').count(), sales ? 1 : 0);
      }
      await page.locator('#tab-reports').click(); await page.locator('#report-results').waitFor({ state: 'visible' });
      const finance = selected.includes('finance'); assert.equal(await page.locator('#report-tab-costo-ore').isVisible(), finance);
      if (finance) {
        await page.locator('#report-tab-costo-ore').click(); await page.locator('#report-body').getByText('Lavoro autorizzato', { exact: true }).waitFor();
        assert.equal(await page.locator('#report-user-field').isVisible(), false); assert.doesNotMatch(await page.locator('#report-head').textContent(), /Utente/); assert.doesNotMatch(await page.locator('#report-body').textContent(), /team@example|Collega squadra/);
        await page.locator('#report-rates-body tr').waitFor(); assert.equal(await page.locator('#report-new-rate').isVisible(), scenario.manager); assert.equal(await page.locator('#report-rates-body button').count(), scenario.manager ? 1 : 0);
        await snapshot(page, scenario.name + '-financial-' + width);
      }
      if (scenario.name === 'managed') {
        await page.locator('#tab-users').click(); await page.locator('#users-body tr').first().getByRole('button', { name: /Gestisci i profili/ }).click();
        await page.waitForFunction(() => !document.getElementById('access-groups-save').disabled);
        assert.equal(await page.locator('#new-user').isVisible(), false); assert.equal(await page.locator('#users-body tr').first().locator('a').textContent(), 'Identità in Keycloak ↗');
        assert.equal(await page.locator('#access-groups-options input[value="manager"]').isDisabled(), true);
        await page.locator('#access-groups-options input[value="finance"]').check(); await snapshot(page, 'managed-profiles-' + width);
        await page.locator('#access-groups-save').click(); await page.locator('#access-groups-dialog').waitFor({ state: 'hidden' }); await page.waitForFunction(() => !document.getElementById('tab-contracts').hidden);
        await page.locator('#tab-reports').click(); await page.locator('#report-tab-costo-ore').click(); await page.locator('#report-rates-body tr').waitFor();
        await page.locator('#tab-users').click(); await page.locator('#users-body tr').first().getByRole('button', { name: /Gestisci i profili/ }).click(); await page.waitForFunction(() => !document.getElementById('access-groups-save').disabled);
        await page.locator('#access-groups-options input[value="finance"]').uncheck(); await page.locator('#access-groups-save').click(); await page.locator('#access-groups-dialog').waitFor({ state: 'hidden' }); await page.waitForFunction(() => document.getElementById('tab-contracts').hidden);
        assert.equal(await page.locator('#report-rates-body tr').count(), 0); assert.equal(await page.locator('#report-body tr').count(), 0);
        await page.locator('#tab-reports').click(); assert.equal(await page.locator('#report-tab-costo-ore').isVisible(), false);
      }
      if (scenario.name === 'manager-finance') {
        holdRates = true; const captured = new Promise(resolve => { rateCaptured = resolve; });
        await page.locator('#report-rates-retry').dispatchEvent('click'); await captured;
        await page.locator('#tab-users').click(); await page.locator('#users-body tr').first().getByRole('button', { name: /Gestisci i profili/ }).click(); await page.waitForFunction(() => !document.getElementById('access-groups-save').disabled);
        await page.locator('#access-groups-options input[value="finance"]').uncheck(); await page.locator('#access-groups-save').click(); await page.locator('#access-groups-dialog').waitFor({ state: 'hidden' }); await page.waitForFunction(() => document.getElementById('tab-contracts').hidden);
        const received = page.waitForResponse(response => new URL(response.url()).pathname === '/api/labor-rates'); releaseRates(); await received;
        await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
        assert.equal(await page.locator('#report-rates-body tr').count(), 0);
        await page.locator('#tab-reports').click(); assert.equal(await page.locator('#report-tab-costo-ore').isVisible(), false);
      }
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
      await context.close();
    }
    assert.deepEqual(errors, []); assert.deepEqual(denied, []);
    assert(reportRequests.filter(r => ['cost', 'labor_cost'].includes(r.view)).every(r => !r.user_id));
    console.log(JSON.stringify({ result: 'PASS', scenarios: 10, checks: ['Functional profile navigation and procurement read-only access', 'Team day/week read-only hours excludes outsiders', 'Finance job aggregates and independent rate permissions', 'Managed Keycloak identities retain local functional profiles', 'Self-profile assignment/revocation clears cached privileged views and rejects delayed rate responses', '375/1280 layouts without overflow or JS errors'], profileWrites: writes.filter(([endpoint]) => endpoint.endsWith('/access-groups')).length }, null, 2));
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
