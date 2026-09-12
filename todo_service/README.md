# Todo LAN service

Python 3 standard library + SQLite. Serves a browser UI and the ESP32 Todo page. Optional Feishu personal-task sync.

## Configure

```sh
cp deploy.env.example deploy.env
# edit TODO_PASSWORD, TODO_DEVICE_TOKEN, TODO_SESSION_SECRET
# optional: FEISHU_APP_ID, FEISHU_APP_SECRET, FEISHU_REDIRECT_URI, TZ
set -a
. ./deploy.env
set +a
python server.py
```

The commands above use Bash. The server reads process environment variables; it does not load `deploy.env` automatically. For PowerShell, set the corresponding `$env:` variables before starting Python.

`TODO_DEVICE_TOKEN` must match the bearer in firmware `main/todo_credentials.h`. `FEISHU_REDIRECT_URI` must be an exact URL you also add in the Feishu open-platform app (security settings), for example `http://192.168.1.10:2333/api/feishu/callback`.

systemd user unit `p4-todo.service` expects the files under `%h/p4-todo` (`$HOME/p4-todo`) and `EnvironmentFile=%h/p4-todo/deploy.env`.

## Feishu (optional)

1. Create your own Feishu app. Put its id/secret in `deploy.env`.
2. Enable user permissions `task:task:write` and `offline_access`, publish a version, add the redirect URL.
3. Open the web UI, sign in with `TODO_PASSWORD`, click connect, authorize with the same Feishu account you will keep using. Re-authorization cannot bind a different `open_id`.

App secrets stay in `deploy.env` (mode 600). User tokens stay in SQLite. The page console link is built from `FEISHU_APP_ID` at runtime; it is not hardcoded.

## API (short)

- Browser: cookie session after `POST /api/login`.
- Device: `Authorization: Bearer <TODO_DEVICE_TOKEN>`.
- `GET /api/device/todos?page=0&done=0` — 8 items/page; `done=1` is **today’s** completions (Asia/Shanghai). `next` is the next reminder across all open tasks.
- `POST /api/todos` — `{title, daily, remind, sync_feishu}`.
- `POST /api/todos/{id}/complete` — idempotent.
- Feishu: `/api/feishu/status|connect|callback|sync`.

## Tests

```sh
python test_feishu.py
```

Uses a temp SQLite file and a fake Feishu HTTP boundary. `check.py` talks to a **running** server (default `http://127.0.0.1:2333`) and **creates** a test task — not a read-only probe.

Official Feishu refs: [Task v2 create](https://open.feishu.cn/document/uAjLw4CM/ukTMukTMukTM/task-v2/task/create), [task list](https://open.feishu.cn/document/uAjLw4CM/ukTMukTMukTM/task-v2/task/list), [OAuth v3](https://open.feishu.cn/document/uAjLw4CM/ukTMukTMukTM/authentication-management/access-token/get-user-access-token-v3).
