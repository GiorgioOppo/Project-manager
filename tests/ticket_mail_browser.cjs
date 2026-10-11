const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || 'playwright');
const fixture = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.PUNTO_CHROME_BINARY || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' });
  try {
    for (const [index, width] of (fixture.widths || [1280, 375]).entries()) {
      const context = await browser.newContext({ viewport: { width, height: 950 }, locale: 'it-IT' });
      const split = fixture.cookie.indexOf('=');
      await context.addCookies([{ name: fixture.cookie.slice(0, split), value: fixture.cookie.slice(split + 1), url: fixture.origin }]);
      const page = await context.newPage(), errors = [];
      page.on('pageerror', error => errors.push(error.message)); page.setDefaultTimeout(10000);
      await page.goto(fixture.origin + '/?section=raw&raw=' + fixture.raw_ids[index]);
      await page.locator('#app-view').waitFor({ state: 'visible' });
      await page.locator('#raw-detail-content').waitFor({ state: 'visible' });
      assert.match(await page.locator('#raw-body').textContent(), /<b>non HTML<\/b>/);
      assert.equal(await page.locator('#raw-body b').count(), 0);
      await page.locator('#raw-claim').click(); await page.locator('#raw-detail-state').getByText('In revisione', { exact: true }).waitFor();
      await page.locator('#raw-convert').click(); await page.locator('#ticket-editor').waitFor({ state: 'visible' });
      await page.locator('#ticket-title').fill('Conversione browser ' + width);
      const converted = page.waitForResponse(response => response.url().endsWith('/convert') && response.request().method() === 'POST');
      await page.locator('#save-ticket').click(); const response = await converted; assert.equal(response.status(), 201, await response.text());
      const result = await response.json(), id = result.ticket.id;
      await page.locator('#ticket-detail').waitFor({ state: 'visible' });
      await page.locator('#customer-body').waitFor(); await page.locator('#customer-to').fill('client@example.test');
      await page.locator('#customer-subject').fill('Risposta browser ' + width); await page.locator('#customer-body').fill('Conferma cliente browser ' + width);
      await page.locator('#customer-save').click();
      await page.locator('.customer-message').getByText('Risposta browser ' + width, { exact: true }).waitFor();
      await page.locator('.customer-message').filter({ hasText: 'Risposta browser ' + width }).getByRole('button', { name: 'Anteprima e invio' }).click();
      await page.locator('#operations-dialog-confirm').click();
      await page.locator('.customer-message').filter({ hasText: 'Risposta browser ' + width }).getByText('Inviato', { exact: true }).waitFor();
      await page.locator('#time-date').fill('2026-10-11'); await page.locator('#time-minutes').fill('25');
      await page.locator('#time-description').fill('Intervento reale browser ' + width); await page.locator('#time-confirmed').check();
      const logged = page.waitForResponse(response => response.url().endsWith('/api/tickets/' + id + '/time') && response.request().method() === 'POST');
      await page.locator('#time-save').click(); const loggedResponse = await logged;
      if (fixture.lost_ack) {
        assert.equal(loggedResponse.status(), 503, await loggedResponse.text());
        const firstAttempt = loggedResponse.request().postDataJSON();
        await page.locator('#ticket-time-panel').getByText(/25 minuti · Esito da verificare/).waitFor();
        await page.goto(fixture.origin + '/?ticket=' + id);
        await page.locator('[data-time-resume]').waitFor();
        assert.equal(await page.locator('#time-save').isDisabled(), true, 'unknown operation blocks a fresh registration');
        await page.locator('[data-time-resume]').click();
        assert.equal(await page.locator('#time-date').inputValue(), firstAttempt.date);
        assert.equal(await page.locator('#time-minutes').inputValue(), String(firstAttempt.minutes));
        assert.equal(await page.locator('#time-description').inputValue(), firstAttempt.description);
        assert.equal(await page.locator('#time-description').evaluate(field => field.readOnly), true);
        assert.equal(await page.locator('#time-confirmed').isChecked(), false, 'resuming requires explicit confirmation');
        await page.locator('#time-confirmed').check();
        const resumed = page.waitForResponse(response => response.url().endsWith('/api/tickets/' + id + '/time') && response.request().method() === 'POST');
        await page.locator('#time-save').click(); const resumedResponse = await resumed;
        assert.equal(resumedResponse.status(), 201, await resumedResponse.text());
        assert.deepEqual(resumedResponse.request().postDataJSON(), firstAttempt, 'reload resumes the exact original idempotency key and payload');
      } else {
        assert.equal(loggedResponse.status(), 201, await loggedResponse.text());
      }
      await page.locator('#ticket-time-panel').getByText(/25 minuti · Registrato/).waitFor();
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth + 1), false, width + 'px horizontal overflow');
      const visibleOverflow = await page.locator('#ticket-detail').evaluate(dialog => dialog.scrollWidth > dialog.clientWidth + 1);
      assert.equal(visibleOverflow, false, width + 'px dialog horizontal overflow');
      if (process.env.PUNTO_TICKET_MAIL_SCREENSHOTS) { fs.mkdirSync(process.env.PUNTO_TICKET_MAIL_SCREENSHOTS, { recursive: true }); await page.screenshot({ path: process.env.PUNTO_TICKET_MAIL_SCREENSHOTS + '/ticket-mail-' + width + '.png', fullPage: true }); }
      assert.deepEqual(errors, []); await context.close();
      console.log('PASS ' + width + 'px: RAW safe text→claim→conversion→customer draft/confirmed send→explicit real time' + (fixture.lost_ack ? '→lost acknowledgement→reload→resume exact key' : '') + '; zero JS errors/overflow.');
    }
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
