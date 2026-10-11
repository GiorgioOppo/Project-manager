"""Ticket hours use explicit delegation and the canonical Punto timesheet rules."""
import json
import os
import sqlite3
import subprocess
import unittest
from datetime import datetime, timedelta
from urllib.parse import parse_qs, urlencode, urlsplit
from unittest.mock import patch
from zoneinfo import ZoneInfo

import test_roles as roles

PREFIX = '/api/integrations/tickets/'

class TicketTimeBridgeTests(unittest.TestCase):
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    assign_job = roles.RoleIntegrationTests.assign_job

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(['make'], cwd=roles.ROOT, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def setUp(self):
        with patch.dict(os.environ, {'TICKETS_APP_ORIGIN': 'http://127.0.0.1:19837', 'AUTH_MODE': 'local'}):
            roles.RoleIntegrationTests.setUp(self)
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        status, result, _ = self.api('POST', '/api/jobs', {'code': 'TICKET-TIME', 'name': 'Ore da Ticket', 'status': 'active'}, cookie=self.manager_cookie)
        self.assertEqual(status, 201, result)
        self.job_id = result['id']
        self.assign_job(self.job_id, self.worker_id)

    def grant(self, time=False, calendar=False, cookie=None):
        fields = {'state': 'a'*64}
        if time: fields['time'] = '1'
        if calendar: fields['calendar'] = '1'
        status, result, _ = self.api('POST', PREFIX+'grant', fields, cookie=cookie or self.worker_cookie)
        self.assertEqual(status, 200, result)
        return parse_qs(urlsplit(result['url']).fragment)['punto_grant'][0]

    def bridge_read(self, resource, grant, expected=200):
        status, result, _ = self.api('GET', PREFIX+resource, headers={'Authorization': 'Bearer '+grant})
        self.assertEqual(status, expected, result)
        return result

    def admin_post(self, path, fields, expected=201):
        status, result, _ = self.api('POST', path, fields, cookie=self.manager_cookie)
        self.assertEqual(status, expected, result)
        return result

    def delegated(self, method, resource, grant, fields=None, expected=200, **kwargs):
        status, result, _ = self.api(method, PREFIX+'time/'+resource, fields,
                                   headers={'Authorization': 'Bearer '+grant, **kwargs.pop('headers', {})}, **kwargs)
        self.assertEqual(status, expected, result)
        return result

    def entry(self, **changes):
        return {'job_id': self.job_id, 'date': '2026-10-11', 'minutes': 60,
                'note': 'Ticket #42: diagnosi effettiva', **changes}

    def fields(self, key='ticket-hours-test-42', **changes):
        return {'request_key': key, 'entries': json.dumps([self.entry(**changes)])}

    def test_explicit_separate_capability_and_actor_identity(self):
        readonly = self.grant()
        self.delegated('GET', 'status', readonly, expected=403)
        self.delegated('POST', 'create', readonly, self.fields(), expected=403)
        grant = self.grant(time=True, calendar=True)
        status, scope, _ = self.api('GET', PREFIX+'scope', headers={'Authorization': 'Bearer '+grant})
        self.assertEqual(status, 200, scope)
        self.assertIs(scope['time_access'], True)
        self.assertIs(scope['calendar_access'], True)
        current = self.delegated('GET', 'status', grant)
        self.assertEqual(current['user_id'], self.worker_id)
        self.delegated('GET', 'status?user_id=1', grant, expected=400)
        self.delegated('POST', 'create', grant, {**self.fields(), 'user_id': 1}, expected=400)

    def test_real_hours_idempotency_recovery_and_no_calendar_auto_booking(self):
        grant = self.grant(time=True)
        created = self.delegated('POST', 'create', grant, self.fields(), expected=201)
        replay = self.delegated('POST', 'create', grant, self.fields())
        self.assertTrue(replay['replayed'])
        self.assertEqual(created['ids'], replay['ids'])
        self.delegated('POST', 'create', grant, self.fields(minutes=61), expected=409)
        recovered = self.delegated('GET', 'entries?'+urlencode({'job_id': self.job_id, 'request_key': 'ticket-hours-test-42'}), grant)
        self.assertTrue(recovered['found'])
        self.assertEqual(recovered['ids'], created['ids'])
        self.assertEqual(recovered['items'][0]['minutes'], 60)
        with sqlite3.connect(self.db_path) as db:
            self.assertEqual(db.execute('SELECT user_id,minutes,note FROM time_entries').fetchall(),
                             [(self.worker_id, 60, 'Ticket #42: diagnosi effettiva')])
        missing = self.delegated('GET', 'entries?'+urlencode({'job_id': self.job_id, 'request_key': 'different-key'}), grant)
        self.assertFalse(missing['found'])
        self.assertEqual(missing['items'], [])

    def test_live_acl_closed_job_and_day_limits_are_canonical(self):
        grant = self.grant(time=True)
        self.delegated('POST', 'create', grant, self.fields(minutes=1440), expected=201)
        self.delegated('POST', 'create', grant, self.fields(key='next-hours-key'), expected=409)
        with sqlite3.connect(self.db_path) as db:
            db.execute('UPDATE jobs SET time_closed=1 WHERE id=?', (self.job_id,))
        self.delegated('POST', 'create', grant, self.fields(key='closed-hours-key', date='2026-10-12'), expected=403)
        self.api('DELETE', f'/api/jobs/{self.job_id}/assignments/{self.worker_id}', cookie=self.manager_cookie)
        self.delegated('GET', 'entries?job_id='+str(self.job_id), grant, expected=403)
        self.delegated('POST', 'create', grant, self.fields(minutes=1440), expected=403)

    def test_input_methods_and_logout_revoke_all_access(self):
        grant = self.grant(time=True)
        self.delegated('GET', 'create', grant, expected=405)
        self.delegated('POST', 'status', grant, {}, expected=405)
        self.delegated('POST', 'create', grant, {'request_key': 'valid-key-42', 'entries': json.dumps([self.entry(), self.entry()])}, expected=400)
        self.delegated('POST', 'create', grant, self.fields(minutes=0), expected=400)
        self.delegated('POST', 'create', grant, self.fields(date='2026-02-30'), expected=400)
        self.api('POST', '/api/logout', {}, cookie=self.worker_cookie)
        self.delegated('GET', 'status', grant, expected=401)

    def test_area_manager_scope_matches_workspace_and_rechecks_descendant_delegation(self):
        area_id = self.create_user(email='ticket-area@example.test', role='manager')
        area_cookie = self.login('ticket-area@example.test', roles.WORKER_PASSWORD)
        managed = self.admin_post('/api/jobs', {'code': 'AREA-MANAGED', 'name': 'Delegated job', 'status': 'active'})['id']
        reader = self.admin_post('/api/jobs', {'code': 'AREA-READER', 'name': 'Read only', 'status': 'active'})['id']
        common = self.admin_post('/api/jobs', {'code': 'AREA-COMMON', 'name': 'Common', 'status': 'active', 'is_common': 1})['id']
        self.admin_post(f'/api/jobs/{reader}/assignments', {'user_id': area_id, 'role_id': 1, 'starts_on': '', 'ends_on': ''})
        parent = self.admin_post('/api/groups', {'name': 'Delegated parent', 'parent_id': ''})['id']
        child = self.admin_post('/api/groups', {'name': 'Delegated child', 'parent_id': parent})['id']
        self.admin_post(f'/api/jobs/{managed}/group-grants', {'group_id': child, 'role_id': 2, 'starts_on': '', 'ends_on': ''})
        token = self.grant(time=True, cookie=area_cookie)

        def assert_scope(expected):
            scope = self.bridge_read('scope', token)
            self.assertIs(scope['all_jobs'], False)
            self.assertEqual(set(scope['job_ids']), expected)
            catalog = self.bridge_read('jobs?limit=100', token)
            self.assertEqual({job['id'] for job in catalog['items']}, expected)
            self.assertEqual(catalog['total'], len(expected))
            code, canonical, _ = self.api('GET', '/api/jobs', cookie=area_cookie)
            self.assertEqual(code, 200, canonical)
            self.assertEqual({job['id'] for job in canonical['jobs']}, expected)

        assert_scope({reader, common})
        self.bridge_read(f'validate?job_id={managed}', token, expected=403)
        self.bridge_read(f'validate?job_id={self.job_id}', token, expected=403)
        self.delegated('POST', 'create', token, self.fields(job_id=self.job_id), expected=403)
        self.delegated('POST', 'create', token, self.fields(job_id=reader), expected=403)
        self.bridge_read(f'validate?job_id={reader}', token)

        delegation = self.admin_post('/api/improvements/governance/delegations', {
            'user_id': area_id, 'group_id': parent,
            'starts_on': datetime.now(ZoneInfo('Europe/Rome')).date().isoformat(), 'ends_on': '',
            'reason': 'Responsibility for child area', 'can_manage_jobs': 1,
            'can_manage_groups': 0, 'can_manage_users': 0, 'can_approve_corrections': 0})
        assert_scope({reader, common, managed})
        self.bridge_read(f'validate?job_id={managed}', token)
        created = self.delegated('POST', 'create', token, self.fields(key='area-managed-hours', job_id=managed), expected=201)
        self.assertEqual(len(created['ids']), 1)
        self.admin_post(f"/api/improvements/governance/delegations/{delegation['id']}/revoke",
                        {'revision': 1, 'reason': 'Responsibility ended'}, expected=200)
        assert_scope({reader, common})
        self.bridge_read(f'validate?job_id={managed}', token, expected=403)
        self.delegated('GET', f'entries?job_id={managed}', token, expected=403)
        self.delegated('POST', 'create', token, self.fields(key='after-area-revoke', job_id=managed), expected=403)

        admin = self.grant(time=True, cookie=self.manager_cookie)
        self.assertIs(self.bridge_read('scope', admin)['all_jobs'], True)
        self.assertEqual({job['id'] for job in self.bridge_read('jobs?limit=100', admin)['items']},
                         {self.job_id, managed, reader, common})
        self.bridge_read(f'validate?job_id={managed}', admin)
        admin_hours = self.delegated('POST', 'create', admin, self.fields(key='admin-global-hours', job_id=managed), expected=201)
        self.assertEqual(len(admin_hours['ids']), 1)

    def test_time_reading_is_personal_and_current_read_access_does_not_grant_historical_logging(self):
        today = datetime.now(ZoneInfo('Europe/Rome')).date()
        earlier = (today - timedelta(days=1)).isoformat()
        self.admin_post('/api/time-entries', self.entry(date=today.isoformat(), note='Admin private hours'))
        worker_token = self.grant(time=True)
        self.delegated('POST', 'create', worker_token,
                       self.fields(key='worker-personal-hours', date=today.isoformat()), expected=201)
        worker_hours = self.delegated('GET', f'entries?job_id={self.job_id}', worker_token)
        self.assertEqual([row['note'] for row in worker_hours['items']], ['Ticket #42: diagnosi effettiva'])
        admin_token = self.grant(time=True, cookie=self.manager_cookie)
        admin_hours = self.delegated('GET', f'entries?job_id={self.job_id}', admin_token)
        self.assertEqual([row['note'] for row in admin_hours['items']], ['Admin private hours'])

        self.admin_post(f'/api/jobs/{self.job_id}/assignments', {
            'user_id': self.worker_id, 'role_id': 2, 'starts_on': today.isoformat(), 'ends_on': ''}, expected=200)
        self.bridge_read(f'validate?job_id={self.job_id}', worker_token)
        self.delegated('POST', 'create', worker_token,
                       self.fields(key='before-grant-started', date=earlier), expected=403)
        self.admin_post(f'/api/jobs/{self.job_id}/assignments', {
            'user_id': self.worker_id, 'role_id': 1, 'starts_on': today.isoformat(), 'ends_on': ''}, expected=200)
        self.bridge_read(f'validate?job_id={self.job_id}', worker_token)
        self.delegated('GET', f'entries?job_id={self.job_id}', worker_token)
        self.delegated('POST', 'create', worker_token,
                       self.fields(key='reader-cannot-log', date=today.isoformat()), expected=403)
