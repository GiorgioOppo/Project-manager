/* Isolated Punto/Ticket routing with actual static files and intercepted HTTP APIs.
 * Run with node tests/inventory_ticket_workflows_browser.cjs.
 * Optional: PUNTO_PLAYWRIGHT_MODULE, PUNTO_CHROME_BINARY,
 * PUNTO_INVENTORY_TICKET_SCREENSHOTS. No running backend or connector is required.
 */
const { chromium } = require(process.env.PUNTO_PLAYWRIGHT_MODULE || "playwright");
const fs = require("node:fs"), assert = require("node:assert/strict"), path = require("node:path");
const root = path.resolve(__dirname, "..");
const snapDir = process.env.PUNTO_INVENTORY_TICKET_SCREENSHOTS;
async function screenshot(page, name) {
  if (snapDir) {
    fs.mkdirSync(snapDir, { recursive: true });
    await page.screenshot({ path: path.join(snapDir, name + ".png"), fullPage: true });
  }
}
async function navigateTicket(page, url) {
  const loaded = page.waitForResponse(response => new URL(response.url()).pathname === "/api/tickets");
  await page.goto(url);
  await loaded;
  await page.waitForFunction(() => document.querySelector("#list-loading").hidden);
}
(async()=>{
  const browser=await chromium.launch({headless:true,executablePath:process.env.PUNTO_CHROME_BINARY || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'});
  try {
    const context=await browser.newContext({viewport:{width:1440,height:1100},locale:'en-GB'});
    const errors=[], tickets=[], validations=[], connects=[], unknown=[];
    let configured=true, connected=false;
    const notices=['inventory_due','inventory_return','inventory_verification'].map((kind,i)=>({id:i+1,event_kind:kind,asset_id:42+i,title:'Asset notice '+i,message:'ASSET-'+(42+i)+' — Notebook',created_at:'2026-10-11T08:00:00Z',read_at:null}));
    notices.push({id:9,event_kind:'inventory_return',asset_id:null,title:'Unavailable asset',message:'Asset no longer available',created_at:'2026-10-11T08:00:00Z',read_at:null});
    context.on('page',page=>{page.setDefaultTimeout(10000);page.on('pageerror',e=>errors.push(e.message));});
    await context.addInitScript(()=>{
      if(location.hostname!=='ticket.test')return;
      localStorage.setItem('ticket.preferences.v1.2',JSON.stringify({view:'list',queue:'all',status:'closed',priority:'high',assignee:'3',sort:'due'}));
      if(location.pathname==='/callback')sessionStorage.setItem('ticket.punto.intent',JSON.stringify({userId:2,query:'?queue=mine&view=list&asset_id=43&new=1'}));
    });
    await context.route(/^http:\/\/(punto|ticket)\.test\//,async route=>{
      const req=route.request(), u=new URL(req.url()),path=u.pathname;
      const json=(body,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(body)});
      if(u.hostname==='punto.test'&&path.startsWith('/api/')){
        if(path==='/api/session')return json({authenticated:true,id:2,email:'lucia@example.test',name:'Lucia',role:'worker',permissions:Object.fromEntries(['manage_jobs','manage_users','edit_time_entries','manage_assignments','view_audit','manage_suppliers','manage_contracts','manage_groups','manage_job_roles'].map(k=>[k,false])),tickets:{configured,origin:'http://ticket.test'}});
        if(path==='/api/jobs')return json({ok:true,jobs:[{id:1,code:'JOB-1',name:'Progetto',client:'Esempio',status:'active',total_minutes:0,can_log_time:true}]});
        if(path==='/api/time-entries')return json({ok:true,entries:[],total_minutes:0});
        if(path==='/api/notifications')return json({ok:true,notifications:notices,unread_count:notices.length});
        if(path==='/api/work-notifications')return json({ok:true,notifications:[],unread_count:0});
        if(path==='/api/my-work')return json({ok:true,today:'2026-10-11',today_minutes:0,tasks_count:0,blockers_count:0,tasks:[],blockers:[]});
        if(path==='/api/inventory/dashboard')return json({ok:true,counts:{total:3,overdue_returns:1,never_verified:1,verification_due:1}});
        if(path==='/api/auth/config')return json({managed_users:false,managed_groups:false});
        unknown.push(req.url());return json({ok:true});
      }
      if(u.hostname==='ticket.test'&&path.startsWith('/api/')){
        if(path==='/api/auth/config')return json({sso:false,local:true,managed_users:false});
        if(path==='/api/session')return json({user:{id:2,name:'Lucia',email:'lucia@example.test',role:'agent'},csrf_token:'fixture'});
        if(path==='/api/users')return json({items:[{id:2,name:'Lucia',active:true},{id:3,name:'Marta',active:true}]});
        if(path==='/api/integrations/punto/status')return json({configured:true,connected,user_name:'Lucia',origin:'http://punto.test'});
        if(path==='/api/integrations/punto/connect'){connects.push(JSON.parse(req.postData()));return json({configured:true,connected:true,user_name:'Lucia',origin:'http://punto.test'});}
        if(path==='/api/integrations/punto/validate'){
          validations.push(Object.fromEntries(u.searchParams));const id=Number(u.searchParams.get('asset_id'));
          return json({job:null,asset:{id,asset_tag:'ASSET-'+id,name:'Notebook '+id,status:'active'}});
        }
        if(path==='/api/tickets'){tickets.push(Object.fromEntries(u.searchParams));return json({items:[],total:0,stats:{},queues:{}});}
        unknown.push(req.url());return json({ok:true});
      }
      const base=u.hostname==='punto.test'?root+'/public':root+'/ticket-app/public';
      const file=path==='/'||path==='/callback'?'/index.html':path==='/app'?'/app.html':path;
      const mime=file.endsWith('.js')?'application/javascript':file.endsWith('.json')?'application/json':file.endsWith('.css')?'text/css':file.endsWith('.svg')?'image/svg+xml':'text/html';
      try{return route.fulfill({contentType:mime,body:fs.readFileSync(base+file)});}catch{return route.fulfill({status:404,body:''});}
    });
    const page=await context.newPage();
    await page.goto('http://punto.test/app?lang=en#my-work');
    await page.locator('#my-work-tickets').waitFor({state:'visible'});
    await page.waitForFunction(()=>document.querySelectorAll('#my-work-inventory-notices a').length===3);
    const mineURL=new URL(await page.locator('#my-work-tickets').getAttribute('href'));
    assert.equal(mineURL.origin,'http://ticket.test');assert.equal(mineURL.searchParams.get('queue'),'mine');assert.equal(mineURL.searchParams.get('lang'),'en');
    const dashboardLinks=await page.locator('#my-work-inventory-notices a').evaluateAll(nodes=>nodes.map(n=>n.href));
    assert.deepEqual(dashboardLinks.map(h=>new URL(h).searchParams.get('asset_id')),['42','43','44']);
    for(const href of dashboardLinks){const u=new URL(href);assert.equal(u.searchParams.get('new'),'1');assert.equal(u.searchParams.get('lang'),'en');}
    await page.locator('#tab-notifications').click();await page.locator('#notifications-list .notification-card').first().waitFor();
    const notificationLinks=await page.locator('#notifications-list a').evaluateAll(nodes=>nodes.map(n=>n.href));
    assert.equal(notificationLinks.length,6);
    for(let i=0;i<3;i++){
      const pair=notificationLinks.slice(i*2,i*2+2).map(h=>new URL(h));
      assert(pair.every(u=>u.searchParams.get('asset_id')===String(42+i)&&u.searchParams.get('lang')==='en'));
      assert.equal(pair[0].searchParams.get('new'),'1');assert.equal(pair[1].searchParams.get('new'),null);
    }
    assert.equal(await page.locator('#notifications-list .notification-card').last().locator('a').count(),0);
    await screenshot(page,'inventory-ticket-notifications');
    const ticketPage=await context.newPage();let start=tickets.length, vstart=validations.length;
    await navigateTicket(ticketPage,mineURL.href);await ticketPage.waitForFunction(()=>document.querySelector('.queue-button[data-queue="mine"]').getAttribute('aria-pressed')==='true'&&document.querySelector('#list-loading').hidden);
    assert.equal(await ticketPage.locator('#list-heading').textContent(),'My tickets');
    assert(await ticketPage.locator('#assignee-filter').isDisabled());
    for(const id of ['status-filter','priority-filter','assignee-filter','search-input'])assert.equal(await ticketPage.locator('#'+id).inputValue(),'');
    assert(tickets.length>start);assert(tickets.slice(start).every(q=>q.queue==='mine'&&!q.priority&&!q.assignee_id&&!q.status));assert.equal(validations.length,vstart);
    await screenshot(ticketPage,'my-tickets-queue');connected=true;
    start=tickets.length;await navigateTicket(ticketPage,notificationLinks[0]);await ticketPage.locator('#ticket-editor').waitFor({state:'visible'});await ticketPage.waitForFunction(()=>document.querySelector('#list-loading').hidden);
    assert.equal(await ticketPage.locator('#editor-asset-label').textContent(),'ASSET-42 · Notebook 42');
    assert.equal(await ticketPage.locator('#filter-asset').textContent(),'ASSET-42 · Notebook 42');
    assert.equal(new URL(ticketPage.url()).searchParams.get('new'),null);
    assert(tickets.length>start&&tickets.slice(start).every(q=>q.queue==='all'&&q.asset_id==='42'&&!q.status&&!q.priority&&!q.assignee_id&&!q.search));
    start=tickets.length;await navigateTicket(ticketPage,notificationLinks[5]);await ticketPage.waitForFunction(()=>document.querySelector('#list-loading').hidden&&document.querySelector('#filter-asset').textContent.includes('ASSET-44'));
    assert(await ticketPage.locator('#ticket-editor').isHidden());assert(tickets.length>start&&tickets.slice(start).every(q=>q.asset_id==='44'&&q.queue==='all'&&!q.status&&!q.priority&&!q.assignee_id&&!q.search));
    start=tickets.length;await navigateTicket(ticketPage,'http://ticket.test/callback?lang=en#punto_grant='+('a'.repeat(64))+'&state='+('b'.repeat(64)));
    await ticketPage.locator('#ticket-editor').waitFor({state:'visible'});await ticketPage.waitForFunction(()=>document.querySelector('#list-loading').hidden);
    const callbackURL=new URL(ticketPage.url());assert.equal(callbackURL.searchParams.get('queue'),'mine');assert.equal(callbackURL.searchParams.get('asset_id'),'43');assert.equal(callbackURL.searchParams.get('new'),null);assert.equal(callbackURL.hash,'');
    assert.equal(await ticketPage.locator('#editor-asset-label').textContent(),'ASSET-43 · Notebook 43');assert.equal(connects.length,1);
    assert(tickets.length>start&&tickets.slice(start).every(q=>q.queue==='mine'&&q.asset_id==='43'&&!q.priority&&!q.assignee_id&&!q.status));
    assert.equal(await ticketPage.evaluate(()=>sessionStorage.getItem('ticket.punto.intent')),null);
    configured=false;await page.goto('http://punto.test/app?lang=en#my-work');await page.reload();await page.waitForFunction(()=>document.querySelector('#my-work-refresh').disabled===false&&document.querySelectorAll('#my-work-inventory-notices .workflow-work-item').length===4);
    assert(await page.locator('#my-work-tickets').isHidden());assert(await page.locator('#my-work-tickets-help').isHidden());assert.equal(await page.locator('#my-work-inventory-notices a').count(),0);
    await page.locator('#tab-notifications').click();await page.locator('#notifications-list .notification-card').first().waitFor();assert.equal(await page.locator('#notifications-list a').count(),0);
    assert.deepEqual(errors,[]);assert.deepEqual(unknown,[]);
    console.log(JSON.stringify({result:'PASS',checks:['I miei ticket: queue=mine and language propagated, disconnected connector supported, saved filters cleared','All three inventory events: new/linked Ticket links carry asset and language','New Ticket opens editor with validated asset; linked Ticket filters asset without opening editor, saved filters cleared','Collega Punto callback restores queue, asset, new draft and clears one-shot callback/intent','Unconfigured Ticket hides links; unavailable asset has no action'],ticketRequests:tickets,validatedAssets:validations,callbackConnections:connects.length,pageErrors:errors,unknownFixtureEndpoints:[...new Set(unknown)],screenshotDirectory:snapDir || null},null,2));
  } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
