"""Offline integration tests: real HTTP/SQLite, simulated Feishu boundary."""
import sys
sys.dont_write_bytecode = True

import copy
import datetime as dt
import http.cookiejar
import http.client
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from unittest.mock import patch

TEMP = tempfile.TemporaryDirectory()
os.environ.update(TODO_DB=str(Path(TEMP.name) / 'test.db'), TODO_PASSWORD='test-password',
                  TODO_DEVICE_TOKEN='test-device', TODO_SESSION_SECRET='test-secret',
                  FEISHU_APP_ID='test-app', FEISHU_APP_SECRET='test-app-secret',
                  FEISHU_REDIRECT_URI='http://127.0.0.1/api/feishu/callback')
import server
import feishu


class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        server.initialize()
        cls.http = server.ThreadingHTTPServer(('127.0.0.1', 0), server.Handler)
        cls.base = 'http://127.0.0.1:' + str(cls.http.server_port)
        threading.Thread(target=cls.http.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown()
        cls.http.server_close()

    def setUp(self):
        with server.connect() as db:
            db.execute('DELETE FROM todos')
            db.execute('DELETE FROM feishu_auth')
            db.execute('DELETE FROM feishu_oauth')
        self.web = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.call('/api/login', {'password': 'test-password'})
        self.tasks = {}
        self.calls = []

    def call(self, path, body=None, device=False):
        req = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode(),
                                     headers={'Content-Type': 'application/json', **({'Authorization': 'Bearer test-device'} if device else {})})
        try:
            with self.web.open(req, timeout=5) as r:
                return r.status, json.load(r)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def test_device_today_and_global_preview(self):
        clock = dt.datetime(2026,9,10,12,tzinfo=feishu.TZ)
        midnight = int(clock.replace(hour=0).timestamp())
        with server.connect() as db:
            for stamp,title in [(midnight-1,'yesterday'),(midnight,'today'),(midnight+86400,'tomorrow')]:
                db.execute('INSERT INTO todos(title,done,created_at,completed_at) VALUES(?,1,?,?)',(title,midnight,stamp))
            first = db.execute('INSERT INTO todos(title,created_at,remind_minute,remind_date) VALUES(?,?,?,?)',
                ('global next',midnight,13*60,20260910)).lastrowid
            for i in range(12):db.execute('INSERT INTO todos(title,created_at) VALUES(?,?)',(str(i),midnight))
        with patch('server.now',return_value=clock),patch('feishu.now',return_value=clock):
            status,done=self.call('/api/device/todos?done=1',device=True)
            self.assertEqual(status,200)
            self.assertEqual([t['title'] for t in done['items']],['today'])
            self.assertEqual(done['total'],1)
            self.assertEqual(done['next']['id'],first)
            _,page=self.call('/api/device/todos?page=0',device=True)
            self.assertNotIn(first,[t['id'] for t in page['items']])
            self.assertEqual(page['next'],done['next'])
            self.assertEqual(page['pages'],2)
            self.assertEqual(self.call('/api/todos?done=1')[1]['total'],3)
            self.call(f'/api/todos/{first}/complete',{},device=True)
            self.assertIsNone(self.call('/api/device/todos',device=True)[1]['next'])

    def authorize(self):
        server.bridge.save_auth({'access_token': 'access', 'refresh_token': 'refresh', 'expires_at': time.time()+3600,
                                 'refresh_expires_at': time.time()+86400, 'open_id': 'owner', 'name': 'Test'})

    def remote(self, url, body=None, token=None, method=None):
        self.calls.append((url, copy.deepcopy(body), method))
        if url.endswith('/task/v2/tasks'):
            # Emulate Feishu's client_token idempotency contract.
            guid = body['client_token']
            self.tasks.setdefault(guid, {'guid': guid, 'created_at': str(int(time.time())*1000), **copy.deepcopy(body)})
            return {'data': {'task': copy.deepcopy(self.tasks[guid])}}
        if '/task/v2/tasks?' in url:
            return {'data': {'items': copy.deepcopy(list(self.tasks.values())), 'has_more': False, 'page_token': ''}}
        guid = url.rsplit('/', 1)[1]
        if guid not in self.tasks:
            raise feishu.TaskDeleted('deleted')
        if method == 'PATCH':
            self.tasks[guid].update(body['task'])
        return {'data': {'task': copy.deepcopy(self.tasks[guid])}}

    def sync(self):
        with patch('feishu.request', self.remote):
            server.bridge.sync()

    def test_local_stays_local_and_scope_requires_auth(self):
        self.assertEqual(self.call('/api/todos', {'title': '飞书', 'sync_feishu': True})[0], 409)
        for daily in (False, True):
            for remind in (None, '09:30'):
                self.assertEqual(self.call('/api/todos', {'title': '本地', 'daily': daily, 'remind': remind})[0], 201)
        self.authorize()
        self.sync()
        self.assertEqual(self.tasks, {})
        self.assertEqual(self.call('/api/todos')[1]['total'], 4)

    def test_four_categories_and_device_completion(self):
        self.authorize()
        for daily in (False, True):
            for remind in (None, '09:30'):
                self.call('/api/todos', {'title': '同步任务', 'daily': daily, 'remind': remind, 'sync_feishu': True})
        self.sync()
        self.assertEqual(len(self.tasks), 4)
        self.assertEqual(sum('due' in t for t in self.tasks.values()), 2)
        self.assertTrue(all('repeat_rule' not in t for t in self.tasks.values()))
        item = self.call('/api/todos')[1]['items'][0]
        self.assertEqual(self.call(f'/api/todos/{item["id"]}/complete', {}, True)[0], 200)
        self.assertEqual(self.call(f'/api/todos/{item["id"]}/complete', {}, True)[0], 200)
        self.sync()
        self.assertNotEqual(self.tasks[item['feishu_guid']]['completed_at'], '0')
        self.assertEqual(server.bridge.status()['pending'], 0)

    def test_remote_edits_complete_reopen_delete_and_future_reminder(self):
        self.authorize()
        due = feishu.now() + dt.timedelta(days=2)
        self.tasks['remote'] = {'guid': 'remote', 'summary': '飞书创建', 'completed_at': '0',
                                'created_at': str(int(time.time())*1000),
                                'due': {'timestamp': str(int(due.timestamp())*1000), 'is_all_day': False},
                                'reminders': [{'relative_fire_minute': 30}], 'repeat_rule': 'FREQ=WEEKLY;BYDAY=MO'}
        self.sync()
        item = self.call('/api/todos')[1]['items'][0]
        self.assertIsNone(item['remind_minute'])
        self.assertIsNotNone(item['scheduled_minute'])
        self.assertEqual(item['daily'], 0)
        self.tasks['remote'].update(summary='飞书修改', completed_at=str(int(time.time())*1000))
        self.sync()
        self.assertEqual(self.call('/api/todos?done=1')[1]['items'][0]['title'], '飞书修改')
        self.tasks['remote']['completed_at'] = '0'
        self.sync()
        self.assertEqual(self.call('/api/todos')[1]['total'], 1)
        del self.tasks['remote']
        self.sync()
        self.assertEqual(self.call('/api/todos')[1]['total'], 0)

    def test_daily_midnight_resets_both_sides_once(self):
        self.authorize()
        item_id = self.call('/api/todos', {'title': '每日', 'daily': True, 'remind': '09:30', 'sync_feishu': True})[1]['id']
        self.sync()
        self.call(f'/api/todos/{item_id}/complete', {}, True)
        self.sync()
        tomorrow = feishu.now() + dt.timedelta(days=1)
        with patch('feishu.now', return_value=tomorrow):
            self.sync()
            first = len([c for c in self.calls if c[2] == 'PATCH'])
            self.sync()
            self.assertEqual(len([c for c in self.calls if c[2] == 'PATCH']), first)
        self.assertEqual(next(iter(self.tasks.values()))['completed_at'], '0')

    def test_failure_preserves_outbox_and_concurrent_completion(self):
        self.authorize()
        item_id = self.call('/api/todos', {'title': '并发', 'sync_feishu': True})[1]['id']
        with patch('feishu.request', side_effect=feishu.FeishuError('offline')):
            with self.assertRaises(feishu.FeishuError):
                server.bridge.sync()
        self.assertEqual(server.bridge.status()['pending'], 1)
        def racing(url, body=None, token=None, method=None):
            result = self.remote(url, body, token, method)
            if body and 'client_token' in body:
                self.call(f'/api/todos/{item_id}/complete', {}, True)
            return result
        with patch('feishu.request', racing):
            server.bridge.sync()
        self.assertEqual(server.bridge.status()['pending'], 1)
        self.assertEqual(self.call('/api/todos?done=1')[1]['total'], 1)
        self.sync()
        self.assertEqual(server.bridge.status()['pending'], 0)
        self.assertEqual(len(self.tasks), 1)

    def test_oauth_state_and_scope_boundaries(self):
        url, state = server.bridge.authorization()
        self.assertIn('offline_access', url)
        with self.assertRaises(feishu.FeishuError):
            server.bridge.callback(state, 'wrong-browser', 'code')
        with patch.object(server.bridge, 'exchange', return_value={'access_token': 'token'}), \
             patch('feishu.request', return_value={'data': {'open_id': 'owner', 'name': 'Test'}}):
            server.bridge.callback(state, state, 'code')
            with self.assertRaises(feishu.FeishuError):
                server.bridge.callback(state, state, 'code')
        self.assertEqual(self.call('/api/todos?page=x')[0], 400)
        self.assertEqual(self.call('/api/todos', {'title': 'bad', 'daily': 'false'})[0], 400)

    def test_one_shot_past_time_schedules_tomorrow(self):
        evening = dt.datetime(2026, 9, 10, 21, 0, tzinfo=feishu.TZ)
        with patch('server.now', return_value=evening):
            self.assertEqual(self.call('/api/todos', {'title': 'tomorrow', 'remind': '09:30'})[0], 201)
            item = self.call('/api/todos')[1]['items'][0]
        self.assertEqual(item['remind_date'], 20260911)
        self.assertIsNone(item['remind_minute'])
        with server.connect() as db:
            row = db.execute('SELECT * FROM todos WHERE id=?', (item['id'],)).fetchone()
        due = server.bridge.due(row)
        self.assertEqual(int(due['timestamp'])//1000, int(evening.replace(day=11, hour=9, minute=30).timestamp()))
        with patch('server.now', return_value=evening.replace(day=11, hour=9, minute=30)):
            self.assertEqual(self.call('/api/todos')[1]['items'][0]['remind_minute'], 570)

    def test_invalid_content_length_returns_400_and_closes(self):
        conn = http.client.HTTPConnection('127.0.0.1', self.http.server_port, timeout=3)
        conn.request('POST', '/api/login', body=b'{}', headers={'Content-Type':'application/json', 'Content-Length':'bad'})
        response = conn.getresponse()
        self.assertEqual(response.status, 400)
        self.assertTrue(response.will_close)
        response.read(); conn.close()

    def test_bad_remote_json_is_reported(self):
        for payload in (b'not-json', b'{}', b'[]', b'{"code":0}', b'{"code":0,"data":{"items":[{}],"has_more":false}}'):
            with patch('urllib.request.urlopen', return_value=io.BytesIO(payload)):
                with self.assertRaises(feishu.FeishuError):
                    feishu.request(feishu.API + 'task/v2/tasks')

    def test_callback_does_not_wait_for_sync_network(self):
        self.authorize()
        _, state = server.bridge.authorization()
        entered, release = threading.Event(), threading.Event()
        def slow_pull(token):
            entered.set(); self.assertTrue(release.wait(3))
        with patch.object(server.bridge, 'push'), patch.object(server.bridge, 'pull', side_effect=slow_pull), \
             patch.object(server.bridge, 'exchange', return_value={'access_token':'new'}), \
             patch('feishu.request', return_value={'data':{'open_id':'owner','name':'Test'}}):
            thread = threading.Thread(target=server.bridge.sync)
            thread.start(); self.assertTrue(entered.wait(3))
            try:
                server.bridge.callback(state, state, 'code')
                self.assertEqual(server.bridge.auth()['access_token'], 'new')
            finally:
                release.set(); thread.join(3)

    def test_reauthorization_wins_over_inflight_refresh(self):
        self.authorize()
        old = server.bridge.auth(); old['expires_at'] = 0; server.bridge.save_auth(old)
        newer = {**old, 'access_token':'new-oauth', 'expires_at':time.time()+3600}
        def exchange(grant):
            server.bridge.save_auth(newer)
            return {**old, 'access_token':'stale-refresh'}
        with patch.object(server.bridge, 'exchange', side_effect=exchange):
            self.assertEqual(server.bridge.token(), 'new-oauth')
        self.assertEqual(server.bridge.auth()['access_token'], 'new-oauth')


if __name__ == '__main__':
    unittest.main()
