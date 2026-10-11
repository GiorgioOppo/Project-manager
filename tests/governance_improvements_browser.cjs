const assert = require("node:assert/strict"), fs = require("node:fs");
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || "playwright");
const c = JSON.parse(fs.readFileSync(process.argv[2], "utf8")), errors = [];
const base = "/api/improvements/governance/";
async function tab(page, name) { await page.locator("#tab-" + name).click(); await page.locator("#panel-" + name).waitFor({state:"visible"}); }
async function get(page, route) { const r = await page.request.get(c.origin + route); assert.equal(r.status(), 200, await r.text()); return r.json(); }
async function context(browser, cookie, width) {
  const ctx = await browser.newContext({locale:"it-IT",viewport:{width,height:950}}), eq = cookie.indexOf("=");
  await ctx.addCookies([{name:cookie.slice(0,eq),value:cookie.slice(eq+1),url:c.origin}]);
  const page = await ctx.newPage(); page.on("pageerror", e => errors.push(e.message)); page.setDefaultTimeout(12000);
  await page.goto(c.origin + "/app"); await page.locator("#workspace").waitFor({state:"visible"});
  await page.waitForFunction(() => window.PuntoGovernance); await page.waitForLoadState("networkidle"); return {ctx,page};
}
async function shot(page, name) {
  const dir = process.env.PUNTO_GOVERNANCE_SCREENSHOTS;
  if (dir) {fs.mkdirSync(dir,{recursive:true});await page.screenshot({path:dir+"/"+name+".png",fullPage:true});}
}
async function select(page, id, value) { await page.locator("#"+id+` option[value='${value}']`).waitFor({state:"attached"}); await page.locator("#"+id).selectOption(String(value)); }
(async () => {
 const browser = await chromium.launch({headless:true,executablePath:process.env.PUNTO_CHROME_BINARY || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"});
 try {
  const admin = await context(browser,c.manager_cookie,1280), p = admin.page;
  await tab(p,"users"); await select(p,"ig-admin-user",c.second); await p.locator("#ig-admin-reason").fill("Nomina verificata dal browser");
  await p.locator("#ig-admin-form").getByRole("button",{name:"Salva amministrazione"}).click();
  await p.waitForFunction(() => window.PuntoExtensions.api("/api/improvements/governance/admins").then(d=>d.admins.filter(a=>a.active).length===2));
  await p.locator("#ig-admin-list").getByText(/second-admin-browser@example.test/).waitFor(); await shot(p,"amministrazione-desktop");
  const row = p.locator("#users-body tr").filter({hasText:"worker-browser@example.test"});
  await row.getByRole("button",{name:/Gestisci i profili funzionali/}).click();
  await p.locator('#access-groups-options input[value="sales"]').check(); await p.locator('#access-groups-options input[value="finance"]').check();
  await p.locator("#access-groups-save").click(); await p.locator("#access-groups-dialog").waitFor({state:"hidden"});
  await p.locator("#ig-profile-requests").getByRole("button",{name:"Aggiorna approvazioni"}).click();
  const ownRequest = p.locator("#ig-profile-request-list .ig-item").filter({hasText:"In attesa"}); await ownRequest.getByRole("button",{name:"Annulla richiesta"}).waitFor();
  assert.equal(await ownRequest.getByRole("button",{name:"Approva",exact:true}).count(),0);
  assert.deepEqual((await get(p,"/api/users/"+c.worker+"/access-groups")).selected,[]);
  const reviewer = await context(browser,c.second_cookie,1280), rp = reviewer.page; await tab(rp,"users");
  const pending = rp.locator("#ig-profile-request-list .ig-item").filter({hasText:"In attesa"});
  await pending.locator("textarea").fill("Verifica distinta dal proponente");
  await rp.locator("#ig-profile-requests").getByRole("button",{name:"Aggiorna approvazioni"}).click();
  await rp.waitForLoadState("networkidle");
  assert.equal(await pending.locator("textarea").inputValue(),"Verifica distinta dal proponente");
  await pending.getByRole("button",{name:"Approva",exact:true}).click();
  try { await rp.locator("#ig-profile-request-list .ig-item").getByText(/Approvata ·/).waitFor(); }
  catch (e) { await shot(rp,"approvazione-errore"); throw new Error(e.message+"\n"+await rp.locator("#ig-profile-requests").innerText()+"\n"+JSON.stringify(await get(rp,base+"profile-requests"))); }
  assert.deepEqual((await get(rp,"/api/users/"+c.worker+"/access-groups")).selected,["sales","finance"]); await shot(rp,"approvazione-distinta-desktop");
  const worker = await context(browser,c.worker_cookie,1280);
  const session = await get(worker.page,"/api/session"); assert.equal(session.permissions.view_costs,true); assert.equal(session.permissions.manage_contracts,true); assert.equal(session.permissions.manage_space,false);
  await worker.ctx.close();
  await tab(p,"organization"); await select(p,"ig-delegation-user",c.area); await select(p,"ig-delegation-group",c.group);
  for (const key of ["can_manage_jobs","can_manage_groups","can_manage_users","can_approve_corrections"]) await p.locator("#ig-delegation-"+key).check();
  await p.locator("#ig-delegation-start").fill(c.today); await p.locator("#ig-delegation-reason").fill("Responsabilità di area verificata dal browser");
  await p.locator("#ig-delegation-form").getByRole("button",{name:"Attribuisci delega"}).click(); await p.locator("#ig-delegation-list").getByText("Responsabilità di area verificata dal browser",{exact:true}).waitFor(); await shot(p,"delega-area-desktop");
  const area = await context(browser,c.area_cookie,1280), ap = area.page; await tab(ap,"organization");
  assert.equal(await ap.locator("#ig-admins").isHidden(),true); assert.equal(await ap.locator("#ig-delegation-form").isHidden(),true);
  await select(ap,"ig-area-user",c.worker); await ap.locator("#ig-area-name").fill("Lavoratore aggiornato da area");
  await ap.locator("#ig-area-user-form").getByRole("button",{name:"Salva lavoratore"}).click();
  await ap.waitForFunction(id=>window.PuntoExtensions.api("/api/users").then(d=>d.users.some(u=>u.id===id&&u.name==="Lavoratore aggiornato da area")),c.worker);
  assert.equal((await get(ap,"/api/groups")).groups.some(g=>g.id===c.destination),false);
  await select(p,"ig-impact-group",c.child); await select(p,"ig-impact-parent",c.destination);
  await p.locator("#ig-impact-form").getByRole("button",{name:"Calcola anteprima"}).click(); await p.locator("#ig-impact-result").getByText(/Persone coinvolte: [1-9]/).waitFor();
  assert.equal((await get(p,"/api/groups")).groups.find(g=>g.id===c.child).parent_id,c.group);
  await shot(p,"impatto-gruppo-desktop");
  for (const current of [admin,reviewer,area]) await current.ctx.close();
  for (const [cookie,label] of [[c.manager_cookie,"amministratore"],[c.area_cookie,"area"]]) {
   const mobile=await context(browser,cookie,375);
   for (const name of label==="area"?["organization"]:["users","organization"]) {await tab(mobile.page,name);assert.equal(await mobile.page.evaluate(()=>document.documentElement.scrollWidth>innerWidth+1),false,"mobile overflow "+label+" "+name);await shot(mobile.page,label+"-"+name+"-mobile");}
   await mobile.ctx.close();
  }
  assert.deepEqual(errors,[]); console.log("PASS governance desktop1280/mobile375: administrator appointment, distinct Sales/Finance approval, live capabilities, area delegation and scoped worker edit, non-mutating group-impact preview; no JS errors or mobile overflow.");
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
