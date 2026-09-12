"""LAN Todo: Python standard library + SQLite, no third-party runtime dependencies."""
from contextlib import contextmanager
import datetime as dt
import hashlib
import hmac
import json
import os
from pathlib import Path
import sqlite3
import time
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from feishu import Bridge, FeishuError, client_token, now

ROOT = Path(__file__).resolve().parent
DB = os.environ.get('TODO_DB', str(ROOT / 'todos.sqlite3'))
PASSWORD = os.environ['TODO_PASSWORD']
DEVICE_TOKEN = os.environ['TODO_DEVICE_TOKEN']
SESSION_SECRET = os.environ['TODO_SESSION_SECRET'].encode()


@contextmanager
def connect():
    db = sqlite3.connect(DB)
    db.row_factory = sqlite3.Row
    try:
        with db:
            yield db
    finally:
        db.close()


bridge = Bridge(connect)


def initialize():
    with connect() as db:
        db.execute('CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY, title TEXT NOT NULL, done INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL, completed_at INTEGER)')
        if 'daily' not in [row[1] for row in db.execute('PRAGMA table_info(todos)')]:
            db.execute('ALTER TABLE todos ADD COLUMN daily INTEGER NOT NULL DEFAULT 0')
        if 'remind_minute' not in [row[1] for row in db.execute('PRAGMA table_info(todos)')]:
            db.execute('ALTER TABLE todos ADD COLUMN remind_minute INTEGER')
        if 'reminded_date' not in [row[1] for row in db.execute('PRAGMA table_info(todos)')]:
            db.execute('ALTER TABLE todos ADD COLUMN reminded_date INTEGER')
    bridge.initialize()


def refresh_daily():
    bridge.reset_daily()


def session():
    payload = str(int(time.time()) + 7 * 86400)
    return payload + '.' + hmac.new(SESSION_SECRET, payload.encode(), hashlib.sha256).hexdigest()


def valid_session(value):
    payload, _, signature = value.partition('.')
    return payload.isdigit() and int(payload) > time.time() and hmac.compare_digest(signature, hmac.new(SESSION_SECRET, payload.encode(), hashlib.sha256).hexdigest())


class Handler(BaseHTTPRequestHandler):
    timeout = 10

    def log_message(self, fmt, *args):
        # OAuth callback contains a one-use authorization code; never log its query.
        if urlparse(self.path).path == '/api/feishu/callback':
            return
        super().log_message(fmt, *args)

    def redirect(self, url, cookie=None):
        self.send_response(303)
        self.send_header('Location', url)
        self.send_header('Content-Length', '0')
        self.send_header('Cache-Control', 'no-store')
        if cookie:
            self.send_header('Set-Cookie', cookie)
        self.end_headers()

    def send(self, status, data, cookie=None):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        if cookie:
            self.send_header('Set-Cookie', cookie)
        self.end_headers()
        self.wfile.write(body)

    def authenticated(self):
        cookie = SimpleCookie(self.headers.get('Cookie', ''))
        value = cookie['todo'].value if 'todo' in cookie else ''
        return valid_session(value) or hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + DEVICE_TOKEN)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == '/api/feishu/callback':
            query = parse_qs(url.query)
            cookie = SimpleCookie(self.headers.get('Cookie', ''))
            if 'error' in query:
                return self.redirect('/?feishu=denied')
            if 'code' not in query or 'state' not in query or 'feishu_oauth' not in cookie:
                return self.send(400, {'error': '授权参数缺失，请从网页登录后重新连接飞书'})
            try:
                bridge.callback(query['state'][0], cookie['feishu_oauth'].value, query['code'][0])
            except FeishuError as error:
                return self.send(400, {'error': str(error)})
            return self.redirect('/?feishu=connected', 'feishu_oauth=; Path=/api/feishu; HttpOnly; SameSite=Lax; Max-Age=0')
        if url.path == '/':
            body = (ROOT / 'index.html').read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if not self.authenticated():
            return self.send(401, {'error': '请先登录'})
        if url.path == '/api/feishu/status':
            return self.send(200, bridge.status())
        if url.path in ('/api/todos', '/api/device/todos'):
            query = parse_qs(url.query)
            try:
                page = int(query.get('page', ['0'])[0])
                done = int(query.get('done', ['0'])[0])
            except ValueError:
                return self.send(400, {'error': '参数错误'})
            if page < 0 or done not in (0, 1):
                return self.send(400, {'error': '参数错误'})
            refresh_daily()
            device = url.path == '/api/device/todos'
            current = now()
            midnight = int(current.replace(hour=0, minute=0, second=0, microsecond=0).timestamp())
            next_midnight = int((current.replace(hour=0, minute=0, second=0, microsecond=0) + dt.timedelta(days=1)).timestamp())
            where = 'done=? AND deleted=0'
            params = [done]
            if device and done:
                where += ' AND completed_at>=? AND completed_at<?'
                params += [midnight, next_midnight]
            with connect() as db:
                count = db.execute('SELECT count(*) FROM todos WHERE '+where, params).fetchone()[0]
                rows = db.execute('SELECT * FROM todos WHERE '+where+' ORDER BY id DESC LIMIT 8 OFFSET ?', params+[page*8]).fetchall()
                next_task = None
                if device:
                    for task in db.execute('SELECT * FROM todos WHERE done=0 AND deleted=0 AND remind_minute IS NOT NULL'):
                        due = bridge.due(task)
                        at = int(due['timestamp'])//1000
                        if at < int(current.timestamp()):continue
                        if next_task is None or (at,task['id']) < (next_task['at'],next_task['id']):
                            next_task = {'id': task['id'], 'title': task['title'], 'at': at}
            items = []
            today = int(now().strftime('%Y%m%d'))
            for row in rows:
                item = {key: row[key] for key in ('id','title','done','created_at','completed_at','daily','remind_minute',
                         'reminded_date','remind_date','sync_feishu','dirty','feishu_guid','remote_repeat')}
                item['scheduled_minute'] = item['remind_minute']
                if item['remind_date'] and item['remind_date'] > today:
                    item['remind_minute'] = None
                items.append(item)
            result = {'items': items, 'page': page, 'pages': max(1, (count + 7) // 8), 'total': count}
            if device:result.update(done=done, next=next_task)
            return self.send(200, result)
        self.send(404, {'error': '不存在'})

    def do_POST(self):
        # The handler uses HTTP/1.0; explicitly close rejected/unread request bodies too.
        self.close_connection = True
        if self.headers.get('Content-Type', '').split(';')[0] != 'application/json':
            return self.send(415, {'error': '需要 JSON'})
        try:
            length = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            return self.send(400, {'error': '请求长度错误'})
        if not 0 < length <= 2048:
            return self.send(400, {'error': '请求长度错误'})
        try:
            data = json.loads(self.rfile.read(length))
        except (ValueError, UnicodeDecodeError):
            return self.send(400, {'error': 'JSON 格式错误'})
        if not isinstance(data, dict):
            return self.send(400, {'error': '需要 JSON 对象'})
        if self.path == '/api/login':
            if not isinstance(data.get('password'), str) or not hmac.compare_digest(data['password'].encode(), PASSWORD.encode()):
                return self.send(401, {'error': '密码错误'})
            return self.send(200, {'ok': True}, 'todo=' + session() + '; Path=/; HttpOnly; SameSite=Strict; Max-Age=604800')
        if not self.authenticated():
            return self.send(401, {'error': '请先登录'})
        if self.path == '/api/feishu/connect':
            url, state = bridge.authorization()
            return self.send(200, {'url': url}, 'feishu_oauth=' + state + '; Path=/api/feishu; HttpOnly; SameSite=Lax; Max-Age=600')
        if self.path == '/api/feishu/sync':
            if bridge.auth() is None:
                return self.send(409, {'error': '请先连接飞书'})
            bridge.wake.set()
            return self.send(202, {'queued': True})
        if self.path == '/api/logout':
            return self.send(200, {'ok': True}, 'todo=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0')
        if self.path == '/api/todos':
            title = data.get('title', '')
            if not isinstance(title, str) or not 1 <= len(title.strip()) <= 80:
                return self.send(400, {'error': '请输入 1–80 个字符'})
            if type(data.get('daily', False)) is not bool or type(data.get('sync_feishu', False)) is not bool:
                return self.send(400, {'error': '任务类型和同步范围必须为布尔值'})
            daily = int(data.get('daily', False))
            sync = int(data.get('sync_feishu', False))
            if sync and bridge.auth() is None:
                return self.send(409, {'error': '请先连接飞书，或选择仅本地'})
            remind = data.get('remind')
            minute = None
            if remind is not None and remind != '':
                if not isinstance(remind, str):
                    return self.send(400, {'error': '提醒时间格式错误'})
                parts = remind.split(':')
                if len(parts) != 2 or not all(p.isdigit() for p in parts):
                    return self.send(400, {'error': '提醒时间格式错误'})
                h, m = int(parts[0]), int(parts[1])
                if not (0 <= h < 24 and 0 <= m < 60):
                    return self.send(400, {'error': '提醒时间格式错误'})
                minute = h * 60 + m
            created = now()
            remind_date = None
            if minute is not None and not daily:
                due = created.replace(hour=minute//60, minute=minute%60, second=0, microsecond=0)
                if due < created:
                    due += dt.timedelta(days=1)
                remind_date = int(due.strftime('%Y%m%d'))
            with connect() as db:
                cur = db.execute('INSERT INTO todos(title,daily,remind_minute,remind_date,created_at,sync_feishu,dirty,client_token) VALUES(?,?,?,?,?,?,?,?)',
                                 (title.strip(), daily, minute, remind_date, int(created.timestamp()), sync, sync, client_token() if sync else None))
            if sync:
                bridge.wake.set()
            return self.send(201, {'id': cur.lastrowid, 'sync_pending': bool(sync)})
        if self.path.startswith('/api/todos/') and self.path.endswith('/notified'):
            part = self.path.split('/')[3]
            if not part.isdigit():
                return self.send(400, {'error': '任务编号错误'})
            today = int(now().strftime('%Y%m%d'))
            with connect() as db:
                cur = db.execute('UPDATE todos SET reminded_date=? WHERE id=?', (today, int(part)))
            return self.send(200 if cur.rowcount else 404, {'ok': bool(cur.rowcount)})
        if self.path.startswith('/api/todos/') and self.path.endswith('/complete'):
            part = self.path.split('/')[3]
            if not part.isdigit():
                return self.send(400, {'error': '任务编号错误'})
            with connect() as db:
                row = db.execute('SELECT done FROM todos WHERE id=? AND deleted=0', (int(part),)).fetchone()
                if row is None:
                    return self.send(404, {'error': '任务不存在'})
                cur = db.execute('UPDATE todos SET done=1, completed_at=?, dirty=sync_feishu, revision=revision+1 WHERE id=? AND done=0',
                                 (int(time.time()), int(part)))
            bridge.wake.set()
            return self.send(200, {'ok': True})
        self.send(404, {'error': '不存在'})


if __name__ == '__main__':
    initialize()
    bridge.start()
    ThreadingHTTPServer(('0.0.0.0', int(os.environ.get('PORT', '2333'))), Handler).serve_forever()
