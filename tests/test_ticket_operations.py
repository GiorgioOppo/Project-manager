"""Operational workflows run against isolated native Ticket/Punto fixtures."""
from contextlib import contextmanager
from datetime import datetime, timezone
import sqlite3
import subprocess
import unittest
from zoneinfo import ZoneInfo
import test_tickets as tickets
import test_ticket_integration as integration

@contextmanager
def database(path):
    db=sqlite3.connect(path)
    try:
        with db: yield db
    finally: db.close()

def epoch(value):
    return int(datetime.fromisoformat(value).timestamp())

class TicketOperationsTests(unittest.TestCase):
    request=tickets.TicketApplicationTests.request
    api=tickets.TicketApplicationTests.api
    login=tickets.TicketApplicationTests.login
    create_ticket=tickets.TicketApplicationTests.create_ticket
    update_ticket=tickets.TicketApplicationTests.update_ticket
    create_user=tickets.TicketApplicationTests.create_user
    stop=tickets.TicketApplicationTests.stop
    connect=integration.TicketPuntoIntegrationTests.connect
    @classmethod
    def setUpClass(cls):
        result=subprocess.run(['make','-C',str(tickets.APP),'build/punto-tickets'],capture_output=True,text=True,timeout=90)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        cls.binary=tickets.APP/'build/punto-tickets'
    def setUp(self):
        self.punto=integration.FakePunto();self.addCleanup(self.punto.close)
        tickets.TicketApplicationTests.setUp(self);self.admin=self.login()
    def start(self):
        self.environment['TICKETS_PUNTO_ORIGIN']=self.punto.url
        tickets.TicketApplicationTests.start(self)
    def detail(self,ticket):
        return self.api('GET',f"/api/tickets/{ticket['id']}",session=self.admin)[0]['ticket']
    def policy(self,**extra):
        return self.api('POST','/api/operations/sla-policies',{'name':'Accordo','priority':'normal','response_minutes':120,'resolution_minutes':120,'timezone':'Europe/Rome','weekdays':[1,2,3,4,5],'day_start':'09:00','day_end':'17:00',**extra},self.admin,expected=201)[0]['policy']
    def test_priority_matrix_and_motivated_override_are_revisioned(self):
        matrix=[['low','normal','high'],['normal','normal','high'],['high','high','urgent']]
        for i,impact in enumerate(('low','medium','high')):
            for j,urgency in enumerate(('low','medium','high')):
                ticket=self.create_ticket(self.admin,impact=impact,urgency=urgency)
                self.assertEqual(ticket['priority'],matrix[i][j]);self.assertEqual(ticket['proposed_priority'],matrix[i][j])
        self.api('POST','/api/tickets',{'title':'Override senza motivo','impact':'high','urgency':'high','priority':'low'},self.admin,expected=400)
        ticket=self.create_ticket(self.admin,impact='high',urgency='high',priority='high',priority_override_reason='Il servizio alternativo è già attivo')
        self.assertEqual(ticket['proposed_priority'],'urgent')
        self.assertIn('servizio alternativo',ticket['priority_override_reason'])
        changed=self.update_ticket(self.admin,ticket,impact='low',urgency='low')
        self.assertEqual(changed['priority'],'low');self.assertEqual(changed['priority_override_reason'],'')
        detail=self.api('GET',f"/api/tickets/{ticket['id']}",session=self.admin)[0]
        self.assertTrue(any(row['action']=='operations_updated' for row in detail['history']))
        unicode_reason='🔧'*500
        unicode_ticket=self.create_ticket(self.admin,impact='high',urgency='high',priority='low',priority_override_reason=unicode_reason)
        history=self.api('GET',f"/api/tickets/{unicode_ticket['id']}",session=self.admin)[0]['history']
        self.assertTrue(any(unicode_reason in row['detail'] for row in history if row['action']=='operations_updated'))
    def test_waiting_requires_reason_and_followup_and_reopen_is_counted(self):
        ticket=self.create_ticket(self.admin)
        self.api('PUT',f"/api/tickets/{ticket['id']}",{'revision':ticket['revision'],'status':'waiting'},self.admin,expected=400)
        waiting=self.update_ticket(self.admin,ticket,status='waiting',waiting_reason='supplier',follow_up_at=2000000000)
        self.assertTrue(waiting['sla']['paused']);self.assertEqual(waiting['waiting_reason'],'supplier')
        self.api('PUT',f"/api/tickets/{ticket['id']}",{'revision':waiting['revision'],'waiting_reason':None,'follow_up_at':None},self.admin,expected=400)
        with database(self.environment['TICKETS_DB_PATH']) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM ticket_wait_intervals WHERE ended_at IS NULL').fetchone()[0],1)
        resolved=self.update_ticket(self.admin,waiting,status='resolved')
        self.assertIsNone(resolved['waiting_reason']);self.assertIsNone(resolved['follow_up_at']);self.assertFalse(resolved['sla']['paused'])
        reopened=self.update_ticket(self.admin,resolved,status='open')
        self.assertEqual(reopened['sla']['reopened_count'],1);self.assertIsNone(reopened['sla']['resolved_at'])
    def test_business_hours_skip_weekend_and_both_rome_dst_transitions(self):
        self.policy()
        for start,expected in [('2026-03-27T16:30:00+01:00','2026-03-30T10:30:00+02:00'),('2026-10-23T16:30:00+02:00','2026-10-26T10:30:00+01:00')]:
            ticket=self.create_ticket(self.admin)
            with database(self.environment['TICKETS_DB_PATH']) as db:
                db.execute('UPDATE ticket_operations SET started_at=? WHERE ticket_id=?',(epoch(start),ticket['id']))
            sla=self.detail(ticket)['sla']
            self.assertEqual(sla['response_due_at'],epoch(expected));self.assertEqual(sla['resolution_due_at'],epoch(expected))
    def test_waiting_pauses_only_business_resolution_minutes(self):
        self.policy();ticket=self.create_ticket(self.admin)
        start=epoch('2026-03-27T15:00:00+01:00')
        with database(self.environment['TICKETS_DB_PATH']) as db:
            db.execute('UPDATE ticket_operations SET started_at=? WHERE ticket_id=?',(start,ticket['id']))
            db.execute('INSERT INTO ticket_wait_intervals(ticket_id,started_at,ended_at) VALUES(?,?,?)',(ticket['id'],epoch('2026-03-27T16:00:00+01:00'),epoch('2026-03-30T10:00:00+02:00')))
        sla=self.detail(ticket)['sla'];self.assertEqual(sla['paused_minutes'],120)
        self.assertEqual(sla['response_due_at'],epoch('2026-03-27T17:00:00+01:00'))
        self.assertEqual(sla['resolution_due_at'],epoch('2026-03-30T11:00:00+02:00'))
        self.api('POST',f"/api/tickets/{ticket['id']}/comments",{'body':'Nota interna'},self.admin,expected=201)
        self.assertIsNone(self.detail(ticket)['sla']['first_response_at'])
    def test_sla_snapshot_survives_unrelated_ticket_updates(self):
        policy=self.policy(name='Accordo originale')
        ticket=self.create_ticket(self.admin)
        deadline=ticket['sla']['response_due_at']
        self.api('PUT',f"/api/operations/sla-policies/{policy['id']}",{'revision':policy['revision'],'name':'Accordo modificato','response_minutes':60},self.admin)
        changed=self.update_ticket(self.admin,ticket,title='Aggiornamento del titolo')
        self.assertEqual(changed['sla']['policy_name'],'Accordo originale')
        self.assertEqual(changed['sla']['response_due_at'],deadline)
        changed=self.update_ticket(self.admin,changed,priority='high')
        self.assertEqual(changed['sla']['policy_name'],'Standard alta')
    def test_job_sla_agreement_and_rules_respect_live_acl(self):
        grant=self.connect(self.admin,all_jobs=True)
        policy=self.policy(job_id=21,priority='all',name='SLA commessa')
        ticket=self.create_ticket(self.admin,job_id=21)
        self.assertEqual(ticket['sla']['policy_name'],'SLA commessa')
        for job in (21,22):
            self.api('POST','/api/operations/rules',{'name':f'Regola privata {job}','contains':'stampante','category':'Hardware','group_name':'Assistenza','job_id':job},self.admin,expected=201)
        self.punto.grants[grant]['all_jobs']=False;self.punto.grants[grant]['job_ids']=[21]
        suggested=self.api('POST','/api/operations/suggest',{'title':'La stampante si ferma','impact':'high','urgency':'medium'},self.admin)[0]
        self.assertEqual(suggested['proposed_priority'],'high');self.assertEqual([row['job_id'] for row in suggested['matches']],[21])
        self.assertNotIn('Regola privata 22',str(suggested))
        self.punto.grants[grant]['job_ids']=[22]
        listed=self.api('GET','/api/operations/sla-policies',session=self.admin)[0]
        self.assertNotIn(policy['id'],[row['id'] for row in listed['items']])
    def test_sla_rules_configuration_revision_and_validation(self):
        rule=self.api('POST','/api/operations/rules',{'name':'Regola','contains':'VPN','category':'Rete'},self.admin,expected=201)[0]['rule']
        changed=self.api('PUT',f"/api/operations/rules/{rule['id']}",{'revision':rule['revision'],'group_name':'Infrastruttura'},self.admin)[0]['rule']
        self.assertEqual(changed['group_name'],'Infrastruttura')
        self.api('PUT',f"/api/operations/rules/{rule['id']}",{'revision':rule['revision'],'active':False},self.admin,expected=409)
        for change in ({'weekdays':[]},{'weekdays':[1,1]},{'timezone':'Fake/Zone'},{'day_end':'08:00'},{'resolution_minutes':0}):
            self.api('POST','/api/operations/sla-policies',{'name':'Non valida',**change},self.admin,expected=400)
        other=self.login(self.create_user(self.admin)['email'])
        self.api('POST','/api/operations/rules',{'name':'No','category':'No'},other,expected=403)
    def test_rule_mailbox_acl_and_existing_config_cannot_be_removed_to_bypass_access(self):
        grant=self.connect(self.admin,all_jobs=True)
        with database(self.environment['TICKETS_DB_PATH']) as db:
            mailbox=db.execute("INSERT INTO raw_mailboxes(name,address,job_id) VALUES('Riservata','private@example.test',21)").lastrowid
        rule=self.api('POST','/api/operations/rules',{'name':'Nome riservato','mailbox_id':mailbox,'category':'Hardware'},self.admin,expected=201)[0]['rule']
        policy=self.policy(job_id=21)
        self.punto.grants[grant]['all_jobs']=False;self.punto.grants[grant]['job_ids']=[22]
        self.assertNotIn(rule['id'],[row['id'] for row in self.api('GET','/api/operations/rules',session=self.admin)[0]['items']])
        suggested=self.api('POST','/api/operations/suggest',{'title':'Guasto','mailbox_id':mailbox},self.admin)[0]
        self.assertEqual(suggested['matches'],[]);self.assertNotIn('Nome riservato',str(suggested))
        self.api('PUT',f"/api/operations/rules/{rule['id']}",{'revision':rule['revision'],'mailbox_id':None},self.admin,expected=404)
        self.api('PUT',f"/api/operations/sla-policies/{policy['id']}",{'revision':policy['revision'],'job_id':None},self.admin,expected=404)
        self.api('POST','/api/operations/rules',{'name':'Non consentita','mailbox_id':mailbox,'category':'Hardware'},self.admin,expected=404)
    def test_shared_views_validate_filters_and_only_owner_or_admin_can_edit(self):
        first=self.login(self.create_user(self.admin)['email']);second=self.login(self.create_user(self.admin,email='other@example.test')['email'])
        view=self.api('POST','/api/operations/views',{'name':'Mie urgenze','filters':{'queue':'mine','priority':'urgent','sort':'priority'},'view':'board'},first,expected=201)[0]['view']
        self.assertEqual(self.api('GET','/api/operations/views',session=second)[0]['items'][0]['id'],view['id'])
        self.api('PUT',f"/api/operations/views/{view['id']}",{'revision':1,'name':'Rubata'},second,expected=403)
        self.api('PUT',f"/api/operations/views/{view['id']}",{'revision':1,'active':False},self.admin)
        self.assertEqual(self.api('GET','/api/operations/views',session=first)[0]['items'],[])
        self.api('POST','/api/operations/views',{'name':'No','filters':{'unexpected':'x'}},first,expected=400)
    def test_server_drafts_are_private_revisioned_persistent_and_presence_tracks_session(self):
        ticket=self.create_ticket(self.admin);other=self.login(self.create_user(self.admin)['email'])
        self.api('POST',f"/api/tickets/{ticket['id']}/presence",{'editing':True},other)
        presence=self.api('GET',f"/api/tickets/{ticket['id']}/presence",session=self.admin)[0]
        self.assertEqual(len(presence['items']),1)
        draft=self.api('PUT',f"/api/tickets/{ticket['id']}/draft",{'revision':0,'base_revision':1,'body':{'description':'Bozza privata'}},self.admin)[0]['draft']
        self.assertEqual(draft['revision'],1)
        self.api('PUT',f"/api/tickets/{ticket['id']}/draft",{'revision':0,'base_revision':1,'body':{'description':'Sovrascrittura'}},self.admin,expected=409)
        self.assertIsNone(self.api('GET',f"/api/tickets/{ticket['id']}/presence",session=other)[0]['draft'])
        self.api('POST','/api/logout',{},other)
        self.assertEqual(self.api('GET',f"/api/tickets/{ticket['id']}/presence",session=self.admin)[0]['items'],[])
        self.stop();self.start();self.admin=self.login()
        recovered=self.api('GET',f"/api/tickets/{ticket['id']}/presence",session=self.admin)[0]['draft']
        self.assertEqual(recovered['body']['description'],'Bozza privata')
    def test_bulk_preview_exclusions_recheck_acl_and_replay_once(self):
        grant=self.connect(self.admin,all_jobs=True)
        local=self.create_ticket(self.admin);private=self.create_ticket(self.admin,job_id=21,title='Titolo riservato')
        preview=self.api('POST','/api/tickets/bulk/preview',{'tickets':[{'id':local['id'],'revision':1},{'id':private['id'],'revision':1},{'id':9999,'revision':1}],'changes':{'status':'in_progress'}},self.admin)[0]
        self.assertEqual(len(preview['eligible']),2);self.assertEqual(preview['excluded'],[{'id':9999,'reason':'not_found'}])
        self.punto.grants[grant]['all_jobs']=False;self.punto.grants[grant]['job_ids']=[22]
        applied=self.api('POST','/api/tickets/bulk/apply',{'token':preview['token']},self.admin)[0]
        self.assertEqual(applied['updated'],[local['id']]);self.assertFalse(applied['replayed'])
        replay=self.api('POST','/api/tickets/bulk/apply',{'token':preview['token']},self.admin)[0]
        self.assertTrue(replay['replayed']);self.assertEqual(self.detail(local)['revision'],2)
        hidden=self.api('POST','/api/tickets/bulk/preview',{'tickets':[{'id':private['id'],'revision':1}],'changes':{'category':'Test'}},self.admin)[0]
        self.assertEqual(hidden['eligible'],[]);self.assertNotIn('Titolo riservato',str(hidden))
        history=self.api('GET',f"/api/tickets/{local['id']}",session=self.admin)[0]['history']
        self.assertEqual(sum(row['action']=='bulk_updated' for row in history),1)
    def test_bulk_revision_change_and_session_binding_prevent_stale_updates(self):
        ticket=self.create_ticket(self.admin);targets=[{'id':ticket['id'],'revision':1}]
        preview=self.api('POST','/api/tickets/bulk/preview',{'tickets':targets,'changes':{'category':'Nuova'}},self.admin)[0]
        self.update_ticket(self.admin,ticket,title='Modifica concorrente')
        other=self.login();self.api('POST','/api/tickets/bulk/apply',{'token':preview['token']},other,expected=409)
        result=self.api('POST','/api/tickets/bulk/apply',{'token':preview['token']},self.admin)[0]
        self.assertEqual(result['updated'],[]);self.assertEqual(result['excluded'][0]['reason'],'revision_changed')
        self.assertEqual(self.detail(ticket)['category'],'')
    def test_dashboard_ticket_raw_group_counts_are_filtered_by_live_acl(self):
        grant=self.connect(self.admin,all_jobs=True)
        self.create_ticket(self.admin,group_name='Helpdesk');self.create_ticket(self.admin,group_name='Riservato',job_id=21)
        with database(self.environment['TICKETS_DB_PATH']) as db:
            box=db.execute("INSERT INTO raw_mailboxes(name,address) VALUES('Coda','raw@example.test')").lastrowid
            for i,job in enumerate((None,21),1):
                db.execute("INSERT INTO raw_messages(mailbox_id,source,source_key,delivery_identity,raw_sha256,raw_eml,received_at,auth_status,state,job_id) VALUES(?,'imap','source',?,?,?,?,'pass','new',?)",(box,str(i),str(i),b'From: x@example.test\r\n\r\nBody',int(datetime.now(timezone.utc).timestamp())-120,job))
        self.punto.grants[grant]['all_jobs']=False;self.punto.grants[grant]['job_ids']=[22]
        data=self.api('GET','/api/reports/operations',session=self.admin)[0]
        self.assertEqual(data['counts']['active'],1);self.assertEqual(data['raw']['backlog'],1)
        self.assertEqual([row['group_name'] for row in data['group_workload']],['Helpdesk']);self.assertNotIn('Riservato',str(data))
    def test_schema_four_upgrade_backfills_legacy_sla_without_changing_ticket_revision(self):
        ticket=self.create_ticket(self.admin,priority='urgent');self.stop()
        with database(self.environment['TICKETS_DB_PATH']) as db:
            for table in ('ticket_wait_intervals','ticket_drafts','operations_views','operations_rules','sla_policies','ticket_operations'):
                db.execute('DROP TABLE '+table)
            db.execute('PRAGMA user_version=4')
        self.start();self.admin=self.login();restored=self.detail(ticket)
        self.assertEqual(restored['revision'],ticket['revision']);self.assertEqual(restored['priority'],'urgent');self.assertEqual(restored['sla']['response_due_at'],ticket['sla']['response_due_at'])
        with database(self.environment['TICKETS_DB_PATH']) as db:
            self.assertEqual(db.execute('PRAGMA user_version').fetchone()[0],5)
            self.assertEqual(db.execute('SELECT count(*) FROM ticket_operations WHERE ticket_id=?',(ticket['id'],)).fetchone()[0],1)

if __name__=='__main__':unittest.main()
