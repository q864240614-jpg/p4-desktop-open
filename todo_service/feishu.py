"""Personal Feishu tasks. SQLite outbox keeps device requests independent of WAN."""
import datetime as dt
import json
import os
import secrets
import sqlite3
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

TZ = dt.timezone(dt.timedelta(hours=8))
SCOPE = 'task:task:write offline_access'
API = 'https://open.feishu.cn/open-apis/'


def now():
    return dt.datetime.now(TZ)


class FeishuError(Exception):
    pass


class TaskDeleted(FeishuError):
    pass


def request(url, body=None, token=None, method=None):
    headers = {'Content-Type': 'application/json'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    req = urllib.request.Request(url, data=None if body is None else json.dumps(body).encode(),
                                 headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as response:
            result = json.load(response)
    except urllib.error.HTTPError as error:
        if error.code == 404 and url.startswith(API + 'task/v2/tasks/'):
            raise TaskDeleted('飞书任务已删除') from error
        raise FeishuError(f'飞书 HTTP {error.code}，请检查权限或重新授权') from error
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise FeishuError('飞书网络连接失败，待同步操作仍保存在本机') from error
    except json.JSONDecodeError as error:
        raise FeishuError('飞书返回了无效 JSON') from error
    if not isinstance(result, dict) or type(result.get('code')) is not int:
        raise FeishuError('飞书响应缺少有效 code')
    if result['code'] != 0:
        message = result.get('msg', result.get('error_description', ''))
        raise FeishuError(f'飞书错误 {result["code"]}: {message}')
    try:
        if url.endswith('/oauth/v3/token'):
            for key in ('access_token','refresh_token','scope'):
                if not isinstance(result[key], str):raise ValueError(key)
            for key in ('expires_in','refresh_token_expires_in'):
                if type(result[key]) is not int:raise ValueError(key)
        elif method != 'PATCH':
            data = result['data']
            if not isinstance(data, dict):raise ValueError('data')
            if url.endswith('/authen/v1/user_info'):
                for key in ('open_id','name'):
                    if not isinstance(data[key], str):raise ValueError(key)
            if '/task/v2/tasks' in url:
                if 'task' in data:
                    tasks = [data['task']]
                else:
                    tasks = data['items']
                    if not isinstance(tasks, list) or type(data['has_more']) is not bool:raise ValueError('items')
                    if data['has_more'] and not isinstance(data['page_token'], str):raise ValueError('page_token')
                for task in tasks:
                    if body is not None:
                        if not isinstance(task['guid'], str):raise ValueError('guid')
                        continue
                    for key in ('guid','summary','created_at'):
                        if not isinstance(task[key], str):raise ValueError(key)
                    int(task['created_at']); int(task.get('completed_at', '0'))
                    due = task.get('due')
                    if due:int(due['timestamp'])
                    reminders = task.get('reminders', [])
                    if not isinstance(reminders, list):raise ValueError('reminders')
                    for reminder in reminders:
                        if type(reminder['relative_fire_minute']) is not int:raise ValueError('relative_fire_minute')
    except (KeyError, TypeError, ValueError) as error:
        raise FeishuError('飞书响应字段无效，请稍后重试') from error
    return result


class Bridge:
    def __init__(self, connect):
        self.connect = connect
        self.lock = threading.Lock()
        self.sync_lock = threading.Lock()
        self.wake = threading.Event()
        self.last_sync = None
        self.error = ''
        self.app_id = os.environ['FEISHU_APP_ID']
        self.secret = os.environ['FEISHU_APP_SECRET']
        self.redirect = os.environ['FEISHU_REDIRECT_URI']

    def initialize(self):
        with self.connect() as db:
            columns = {row[1] for row in db.execute('PRAGMA table_info(todos)')}
            for name, definition in {
                'sync_feishu': 'INTEGER NOT NULL DEFAULT 0',
                'feishu_guid': 'TEXT', 'client_token': 'TEXT',
                'dirty': 'INTEGER NOT NULL DEFAULT 0',
                'revision': 'INTEGER NOT NULL DEFAULT 0',
                'remind_date': 'INTEGER', 'remote_repeat': "TEXT NOT NULL DEFAULT ''",
                'deleted': 'INTEGER NOT NULL DEFAULT 0',
                'daily_date': 'INTEGER',
            }.items():
                if name not in columns:
                    db.execute(f'ALTER TABLE todos ADD COLUMN {name} {definition}')
            rows = db.execute('SELECT id,created_at,remind_minute FROM todos WHERE daily=0 AND remind_minute IS NOT NULL AND remind_date IS NULL AND feishu_guid IS NULL').fetchall()
            for row in rows:
                created = dt.datetime.fromtimestamp(row['created_at'], TZ)
                due = created.replace(hour=row['remind_minute']//60, minute=row['remind_minute']%60, second=0, microsecond=0)
                if due < created:
                    due += dt.timedelta(days=1)
                db.execute('UPDATE todos SET remind_date=? WHERE id=?', (int(due.strftime('%Y%m%d')), row['id']))
            db.execute('CREATE UNIQUE INDEX IF NOT EXISTS feishu_guid ON todos(feishu_guid)')
            db.execute('CREATE TABLE IF NOT EXISTS feishu_auth (id INTEGER PRIMARY KEY CHECK(id=1), payload TEXT NOT NULL)')
            db.execute('CREATE TABLE IF NOT EXISTS feishu_oauth (state TEXT PRIMARY KEY, expires INTEGER NOT NULL)')

    def auth(self):
        with self.connect() as db:
            row = db.execute('SELECT payload FROM feishu_auth WHERE id=1').fetchone()
        return json.loads(row[0]) if row else None

    def save_auth(self, data):
        with self.connect() as db:
            db.execute('INSERT OR REPLACE INTO feishu_auth VALUES(1,?)', (json.dumps(data),))

    def authorization(self):
        state = secrets.token_urlsafe(32)
        with self.connect() as db:
            db.execute('DELETE FROM feishu_oauth WHERE expires < ?', (int(time.time()),))
            db.execute('INSERT INTO feishu_oauth VALUES(?,?)', (state, int(time.time()) + 600))
        url = 'https://accounts.feishu.cn/open-apis/authen/v1/authorize?' + urllib.parse.urlencode({
            'client_id': self.app_id, 'redirect_uri': self.redirect, 'response_type': 'code',
            'scope': SCOPE, 'state': state, 'prompt': 'consent'})
        return url, state

    def exchange(self, grant):
        data = request('https://accounts.feishu.cn/oauth/v3/token', {
            'client_id': self.app_id, 'client_secret': self.secret, **grant})
        if not set(SCOPE.split()).issubset(data['scope'].split()):
            raise FeishuError('需要授予 task:task:write 和 offline_access 权限')
        data['expires_at'] = time.time() + data['expires_in']
        data['refresh_expires_at'] = time.time() + data['refresh_token_expires_in']
        return data

    def callback(self, state, cookie, code):
        if not state or not secrets.compare_digest(state, cookie):
            raise FeishuError('授权状态不匹配，请从原浏览器重新连接')
        with self.connect() as db:
            found = db.execute('DELETE FROM feishu_oauth WHERE state=? AND expires>=? RETURNING state',
                               (state, int(time.time()))).fetchone()
        if not found:
            raise FeishuError('授权已过期或已使用，请重新连接')
        data = self.exchange({'grant_type': 'authorization_code', 'code': code, 'redirect_uri': self.redirect})
        user = request(API + 'authen/v1/user_info', token=data['access_token'])['data']
        with self.lock:
            old = self.auth()
            if old and old['open_id'] != user['open_id']:
                raise FeishuError('当前数据已绑定另一飞书账号，请使用原账号重新授权')
            data['open_id'] = user['open_id']
            data['name'] = user['name']
            self.save_auth(data)
            self.error = ''
        self.wake.set()

    def token(self):
        with self.lock:
            data = self.auth()
        if data is None:
            raise FeishuError('尚未连接飞书，请先授权')
        if data['expires_at'] <= time.time() + 120:
            if data['refresh_expires_at'] <= time.time():
                raise FeishuError('飞书授权已过期，请重新连接')
            new = self.exchange({'grant_type': 'refresh_token', 'refresh_token': data['refresh_token']})
            new.update(open_id=data['open_id'], name=data['name'])
            with self.lock:
                current = self.auth()
                if current == data:
                    self.save_auth(new)
                    data = new
                else:
                    data = current # A completed OAuth callback supersedes the old refresh result.
        return data['access_token']

    def status(self):
        auth = self.auth()
        with self.connect() as db:
            pending = db.execute('SELECT count(*) FROM todos WHERE dirty=1 AND deleted=0').fetchone()[0]
        return {'connected': auth is not None, 'name': auth['name'] if auth else '',
                'last_sync': self.last_sync, 'error': self.error, 'pending': pending,
                'redirect_uri': self.redirect, 'scopes': SCOPE, 'app_id': self.app_id}

    def reset_daily(self):
        midnight = int(now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp())
        today = int(now().strftime('%Y%m%d'))
        with self.connect() as db:
            db.execute('UPDATE todos SET done=0, completed_at=NULL, dirty=sync_feishu, revision=revision+1 '
                       'WHERE daily=1 AND done=1 AND completed_at<? AND deleted=0', (midnight,))
            db.execute('UPDATE todos SET daily_date=?, dirty=sync_feishu, revision=revision+1 '
                       'WHERE daily=1 AND deleted=0 AND (daily_date IS NULL OR daily_date<>?)', (today, today))

    def due(self, row):
        if row['remind_minute'] is None:
            return None
        date = now() if row['daily'] else dt.datetime.strptime(str(row['remind_date']), '%Y%m%d').replace(tzinfo=TZ)
        date = date.replace(hour=row['remind_minute'] // 60, minute=row['remind_minute'] % 60, second=0, microsecond=0)
        return {'timestamp': str(int(date.timestamp()) * 1000), 'is_all_day': False}

    def push(self, token):
        with self.connect() as db:
            rows = db.execute('SELECT * FROM todos WHERE dirty=1 AND sync_feishu=1 AND deleted=0 ORDER BY id').fetchall()
        for row in rows:
            completion = str((row['completed_at'] or 0) * 1000)
            if row['feishu_guid'] is None:
                body = {'summary': row['title'], 'completed_at': completion, 'client_token': row['client_token'],
                        'members': [{'id': self.auth()['open_id'], 'type': 'user', 'role': 'assignee'}],
                        'description': '桌面助手每日任务：北京时间零点恢复待办。' if row['daily'] else '来自桌面助手',
                        'extra': json.dumps({'source': 'um-desk', 'daily': row['daily']})}
                due = self.due(row)
                if due:
                    body.update(due=due, reminders=[{'relative_fire_minute': 0}])
                task = request(API + 'task/v2/tasks', body, token)['data']['task']
                with self.connect() as db:
                    db.execute('UPDATE todos SET feishu_guid=?, dirty=CASE WHEN revision=? THEN 0 ELSE 1 END WHERE id=?',
                               (task['guid'], row['revision'], row['id']))
            else:
                task = {'completed_at': completion}
                if row['daily'] and row['remind_minute'] is not None:
                    task['due'] = self.due(row)
                try:
                    request(API + 'task/v2/tasks/' + row['feishu_guid'],
                            {'task': task, 'update_fields': list(task)}, token, 'PATCH')
                except TaskDeleted:
                    with self.connect() as db:
                        db.execute('UPDATE todos SET deleted=1,dirty=0 WHERE id=?', (row['id'],))
                    continue
                with self.connect() as db:
                    db.execute('UPDATE todos SET dirty=0 WHERE id=? AND revision=?', (row['id'], row['revision']))

    def pull(self, token):
        tasks = []
        page = ''
        while True:
            data = request(API + 'task/v2/tasks?' + urllib.parse.urlencode({
                'page_size': 100, 'type': 'my_tasks', 'page_token': page}), token=token)['data']
            tasks.extend(data['items'])
            if not data['has_more']:
                break
            page = data['page_token']
        seen = {task['guid'] for task in tasks}
        with self.connect() as db:
            linked = db.execute('SELECT feishu_guid FROM todos WHERE feishu_guid IS NOT NULL AND deleted=0').fetchall()
        for row in linked:
            if row['feishu_guid'] in seen:
                continue
            try:
                task = request(API + 'task/v2/tasks/' + row['feishu_guid'], token=token)['data']['task']
            except TaskDeleted:
                with self.connect() as db:
                    db.execute('UPDATE todos SET deleted=1,dirty=0 WHERE feishu_guid=?', (row['feishu_guid'],))
                continue
            tasks.append(task)
        for task in tasks:
            completed = int(task.get('completed_at', '0')) // 1000
            minute, date = None, None
            due, reminders = task.get('due'), task.get('reminders', [])
            if due and reminders:
                alarm = dt.datetime.fromtimestamp(int(due['timestamp']) / 1000, TZ) - dt.timedelta(minutes=reminders[0]['relative_fire_minute'])
                minute, date = alarm.hour * 60 + alarm.minute, int(alarm.strftime('%Y%m%d'))
            with self.connect() as db:
                row = db.execute('SELECT * FROM todos WHERE feishu_guid=?', (task['guid'],)).fetchone()
                if row is None:
                    db.execute('INSERT INTO todos(title,done,created_at,completed_at,daily,remind_minute,remind_date,'
                               'sync_feishu,feishu_guid,remote_repeat) VALUES(?,?,?,?,0,?,?,1,?,?)',
                               (task['summary'][:80], int(completed > 0), int(task['created_at']) // 1000,
                                completed or None, minute, date, task['guid'], task.get('repeat_rule', '')))
                elif not row['dirty']:
                    # Desk daily recurrence is owned by this service; native Feishu recurrence stays native.
                    if row['daily']:
                        minute, date = row['remind_minute'], None
                    db.execute('UPDATE todos SET title=?,done=?,completed_at=?,remind_minute=?,remind_date=?,remote_repeat=?,deleted=0 '
                               'WHERE id=? AND dirty=0',
                               (task['summary'][:80], int(completed > 0), completed or None, minute, date,
                                task.get('repeat_rule', ''), row['id']))

    def sync(self):
        with self.sync_lock:
            self.reset_daily()
            token = self.token()
            self.push(token)
            self.pull(token)
            self.last_sync = int(time.time())
            self.error = ''

    def worker(self):
        while True:
            self.wake.wait(15)
            self.wake.clear()
            try:
                self.reset_daily()
                if self.auth() is None:
                    continue
                self.sync()
            except (FeishuError, sqlite3.OperationalError) as error:
                self.error = str(error)
                print(self.error, flush=True)

    def start(self):
        threading.Thread(target=self.worker, name='feishu-sync', daemon=True).start()


def client_token():
    return str(uuid.uuid4())
