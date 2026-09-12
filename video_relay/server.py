#!/usr/bin/env python3
"""LAN video relay for ESP32-P4. Printer credentials stay in RAM for the request only."""
from __future__ import annotations

import ipaddress
import json
import os
import re
import select
import signal
import socket
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

from upstream import (
    Source,
    _safe_error,
    clamp_output,
    normalize_model,
    open_source,
    private_ipv4,
    validate_access_code,
)

BIND = os.environ.get("VIDEO_RELAY_BIND", "0.0.0.0")
PORT = int(os.environ.get("VIDEO_RELAY_PORT", "2344"))
TOKEN = os.environ.get("VIDEO_RELAY_TOKEN", "")
ALLOW = os.environ.get("VIDEO_RELAY_ALLOW", "127.0.0.1/32,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16")
IDLE_SECONDS = float(os.environ.get("VIDEO_RELAY_IDLE", "2"))
FIRST_ACQUIRE_SECONDS = float(os.environ.get("VIDEO_RELAY_FIRST_WAIT", "15"))
MAX_SESSIONS = 8
MAX_UPSTREAMS = 2
HERE = os.path.dirname(os.path.abspath(__file__))


def log(message: str) -> None:
    sys.stderr.write(time.strftime("%Y-%m-%d %H:%M:%S ") + message + "\n")
    sys.stderr.flush()


def allowed_networks():
    nets = []
    for item in ALLOW.split(","):
        item = item.strip()
        if item:
            nets.append(ipaddress.ip_network(item, strict=False))
    return nets


ALLOWED = allowed_networks()


def client_allowed(addr: str) -> bool:
    try:
        ip = ipaddress.ip_address(addr)
    except ValueError:
        return False
    return any(ip in net for net in ALLOWED)


def connection_closed(sock: socket.socket) -> bool:
    try:
        readable, _, exceptional = select.select([sock], [], [sock], 0)
        if exceptional:
            return True
        if readable:
            return sock.recv(1, socket.MSG_PEEK) == b""
    except OSError:
        return True
    return False


def framed_header(size: int) -> bytes:
    return (
        size.to_bytes(4, "little")
        + (0).to_bytes(4, "little")
        + (1).to_bytes(4, "little")
        + (0).to_bytes(4, "little")
    )


def printer_key(session: "Session") -> tuple:
    return (
        session._params.get("ip", ""),
        session._params.get("access_code", ""),
        session.model,
        session.width,
        session.height,
        session.quality,
    )


class Session:
    def __init__(self, params: dict, factory):
        self.id = uuid.uuid4().hex
        self.model = params["model"]
        self.width = params["width"]
        self.height = params["height"]
        self.fps = params["fps"]
        self.quality = params["quality"]
        self.created = time.monotonic()
        self.touched = self.created
        self.consumers = 0
        self.source: Source | None = None
        self.error: str | None = None
        self._params = params
        self._factory = factory

    def touch(self) -> None:
        self.touched = time.monotonic()

    def acquire(self) -> Source:
        return HUB.checkout(self)

    def release(self) -> None:
        HUB.checkin(self)

    def stop_upstream(self) -> None:
        HUB.detach(self)


class Hub:
    def __init__(self, factory=open_source):
        self.factory = factory
        self.lock = threading.RLock()
        self.sessions: dict[str, Session] = {}
        self._stop = threading.Event()
        self._reaper = threading.Thread(target=self._reap, name="session-reaper", daemon=True)
        self._reaper.start()

    def create(self, body: dict) -> Session:
        params = parse_printer(body)
        with self.lock:
            self._purge_locked()
            if len(self.sessions) >= MAX_SESSIONS:
                raise ValueError("too many idle sessions")
            session = Session(params, self.factory)
            self.sessions[session.id] = session
        log(f"session created model={session.model}")
        return session

    def checkout(self, session: Session) -> Source:
        with self.lock:
            session.touch()
            session.consumers += 1
            if session.source:
                return session.source
            key = printer_key(session)
            for other in self.sessions.values():
                if other is not session and other.source and printer_key(other) == key:
                    session.source = other.source
                    return session.source
            unique = {id(item.source) for item in self.sessions.values() if item.source}
            if len(unique) >= MAX_UPSTREAMS:
                session.consumers -= 1
                raise ValueError("too many live printer connections")
            session.source = self.factory(
                session._params["ip"],
                session._params["access_code"],
                session.model,
                session.width,
                session.height,
                session.quality,
            )
            session.source.start()
            return session.source

    def checkin(self, session: Session) -> None:
        with self.lock:
            session.consumers = max(0, session.consumers - 1)
            session.touch()

    def detach(self, session: Session) -> None:
        with self.lock:
            source = session.source
            session.source = None
            session._params["access_code"] = ""
            session._params["ip"] = ""
            shared = source is not None and any(item.source is source for item in self.sessions.values())
        if source and not shared:
            source.stop()
            source.join(1.5)

    def get(self, session_id: str) -> Session:
        with self.lock:
            session = self.sessions.get(session_id)
        if not session:
            raise KeyError("unknown session")
        session.touch()
        return session

    def drop(self, session_id: str) -> None:
        with self.lock:
            session = self.sessions.pop(session_id, None)
        if session:
            src = session.source
            produced = src.produced if src else 0
            stage = src.stage if src else "none"
            connected = src.connected if src else False
            error = src.frames.snapshot()[2] if src else None
            session.stop_upstream()
            extra = f" error={error}" if error else ""
            log(f"session dropped model={session.model} frames={produced} stage={stage} connected={int(connected)}{extra}")

    def stats(self) -> dict:
        with self.lock:
            sessions = list(self.sessions.values())
        live = len({id(s.source) for s in sessions if s.source and s.source.connected})
        return {
            "ok": True,
            "service": "p4-video-relay",
            "sessions": len(sessions),
            "live_printers": live,
            "idle_seconds": IDLE_SECONDS,
        }

    def shutdown(self) -> None:
        self._stop.set()
        with self.lock:
            ids = list(self.sessions)
        for session_id in ids:
            self.drop(session_id)

    def _reap(self) -> None:
        while not self._stop.wait(0.2):
            try:
                self._purge()
            except Exception as exc:
                log(f"reaper error {exc.__class__.__name__}")

    def _purge(self) -> None:
        with self.lock:
            self._purge_locked()

    def _purge_locked(self) -> None:
        now = time.monotonic()
        dead = []
        for session_id, session in self.sessions.items():
            if session.consumers != 0:
                continue
            limit = IDLE_SECONDS if session.source else FIRST_ACQUIRE_SECONDS
            if now - session.touched >= limit:
                dead.append(session_id)
        for session_id in dead:
            session = self.sessions.pop(session_id, None)
            if session:
                produced = session.source.produced if session.source else 0
                session.stop_upstream()
                log(f"session reaped model={session.model} frames={produced}")


def parse_printer(body: dict) -> dict:
    ip = private_ipv4(str(body.get("ip", "")))
    access_code = validate_access_code(str(body.get("access_code", body.get("code", ""))))
    model = normalize_model(str(body.get("model", "H2D")))
    width, height, fps, quality = clamp_output(
        body.get("width", 800), body.get("height", 480), body.get("fps", 8), body.get("quality", 60)
    )
    return {
        "ip": ip,
        "access_code": access_code,
        "model": model,
        "width": width,
        "height": height,
        "fps": fps,
        "quality": quality,
    }


HUB = Hub()
INDEX = os.path.join(HERE, "index.html")


class Handler(BaseHTTPRequestHandler):
    timeout = 30
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        path = urlparse(self.path).path
        path = re.sub(r"/v1/session/[0-9a-f]+", "/v1/session/<id>", path)
        log(f"http {self.command} {path}")

    def handle_one_request(self):
        if not client_allowed(self.client_address[0]):
            try:
                self.request.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            except OSError:
                pass
            self.close_connection = True
            return
        super().handle_one_request()

    def _auth(self) -> bool:
        if not TOKEN:
            return True
        header = self.headers.get("Authorization", "")
        if header == f"Bearer {TOKEN}" or self.headers.get("X-Relay-Token") == TOKEN:
            return True
        self._json(401, {"error": "relay token required"})
        return False

    def _read_json(self) -> dict:
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length < 2 or length > 4096:
            raise ValueError("json body must be 2-4096 bytes")
        raw = self.rfile.read(length)
        data = json.loads(raw.decode("utf-8"))
        if not isinstance(data, dict):
            raise ValueError("json object required")
        return data

    def _json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _headers_session(self) -> dict:
        ip = self.headers.get("X-Printer-IP", "")
        code = self.headers.get("X-Printer-Access-Code", self.headers.get("X-Printer-Code", ""))
        model = self.headers.get("X-Printer-Model", "H2D")
        return {
            "ip": ip,
            "access_code": code,
            "model": model,
            "width": int(self.headers.get("X-Width", "800")),
            "height": int(self.headers.get("X-Height", "480")),
            "fps": int(self.headers.get("X-Fps", "8")),
            "quality": int(self.headers.get("X-Quality", "60")),
        }

    def do_GET(self):
        path = urlparse(self.path).path
        if path not in ("/health", "/v1/health", "/") and not self._auth():
            return
        if path in ("/health", "/v1/health"):
            self._json(200, HUB.stats())
            return
        if path == "/":
            self._index()
            return
        if path == "/v1/mjpeg":
            self._oneshot_stream("mjpeg")
            return
        if path == "/v1/framed":
            self._oneshot_stream("framed")
            return
        match = re.fullmatch(r"/v1/session/([0-9a-f]{32})/(mjpeg|jpeg|framed)", path)
        if match:
            self._session_stream(match.group(1), match.group(2))
            return
        match = re.fullmatch(r"/v1/session/([0-9a-f]{32})", path)
        if match:
            self._session_info(match.group(1))
            return
        self._json(404, {"error": "not found"})

    def do_POST(self):
        if not self._auth():
            return
        path = urlparse(self.path).path
        if path != "/v1/session":
            self._json(404, {"error": "not found"})
            return
        try:
            session = HUB.create(self._read_json())
        except ValueError as exc:
            self._json(400, {"error": str(exc)})
            return
        except json.JSONDecodeError:
            self._json(400, {"error": "invalid json"})
            return
        self._json(201, session_payload(session))

    def do_DELETE(self):
        if not self._auth():
            return
        path = urlparse(self.path).path
        match = re.fullmatch(r"/v1/session/([0-9a-f]{32})", path)
        if not match:
            self._json(404, {"error": "not found"})
            return
        HUB.drop(match.group(1))
        self._json(200, {"ok": True})

    def _index(self):
        try:
            body = open(INDEX, "rb").read()
        except OSError:
            body = b"<html><body>p4-video-relay</body></html>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _session_info(self, session_id: str) -> None:
        try:
            session = HUB.get(session_id)
        except KeyError:
            self._json(404, {"error": "unknown session"})
            return
        jpeg, seq, error, stopped = (session.source.frames.snapshot() if session.source else (None, 0, None, False))
        self._json(200, {
            "session": session.id,
            "model": session.model,
            "width": session.width,
            "height": session.height,
            "fps": session.fps,
            "state": "error" if error else ("live" if session.source and session.source.connected else ("waiting" if session.source else "idle")),
            "frames": session.source.produced if session.source else 0,
            "seq": seq,
            "error": error,
        })

    def _session_stream(self, session_id: str, kind: str) -> None:
        try:
            session = HUB.get(session_id)
        except KeyError:
            self._json(404, {"error": "unknown session"})
            return
        try:
            source = session.acquire()
        except ValueError as exc:
            self._json(503, {"error": str(exc)})
            return
        try:
            if kind == "jpeg":
                self._write_one_jpeg(source, session.fps)
            elif kind == "framed":
                self._write_framed(source, session.fps)
            else:
                self._write_mjpeg(source, session.fps)
        except BrokenPipeError:
            pass
        except ConnectionResetError:
            pass
        finally:
            session.release()

    def _oneshot_stream(self, kind: str) -> None:
        try:
            session = HUB.create(self._headers_session())
        except ValueError as exc:
            self._json(400, {"error": str(exc)})
            return
        try:
            source = session.acquire()
        except ValueError as exc:
            HUB.drop(session.id)
            self._json(503, {"error": str(exc)})
            return
        try:
            if kind == "framed":
                self._write_framed(source, session.fps)
            else:
                self._write_mjpeg(source, session.fps)
        except BrokenPipeError:
            pass
        except ConnectionResetError:
            pass
        finally:
            HUB.drop(session.id)

    def _write_one_jpeg(self, source: Source, fps: int) -> None:
        jpeg = wait_frame(source, timeout=FIRST_WAIT)
        if jpeg is None:
            error = source.frames.snapshot()[2] or "no picture yet"
            self._json(504, {"error": error})
            return
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(jpeg)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Frame-Width", str(source.width))
        self.send_header("X-Frame-Height", str(source.height))
        self.end_headers()
        self.wfile.write(jpeg)

    def _write_mjpeg(self, source: Source, fps: int) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Pragma", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        last = -1
        interval = 1.0 / max(1, fps)
        while not connection_closed(self.connection):
            jpeg, seq, error, stopped = source.frames.wait(last, min(0.25, interval))
            if error and jpeg is None:
                return
            if jpeg is None:
                if stopped:
                    return
                continue
            if seq == last:
                continue
            last = seq
            header = (
                b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                + str(len(jpeg)).encode()
                + b"\r\n\r\n"
            )
            self.wfile.write(header + jpeg + b"\r\n")
            try:
                self.wfile.flush()
            except OSError:
                return

    def _write_framed(self, source: Source, fps: int) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.send_header("X-Frame-Format", "bambu-jpeg-16")
        self.end_headers()
        last = -1
        interval = 1.0 / max(1, fps)
        while not connection_closed(self.connection):
            jpeg, seq, error, stopped = source.frames.wait(last, min(0.25, interval))
            if error and jpeg is None:
                return
            if jpeg is None:
                if stopped:
                    return
                continue
            if seq == last:
                continue
            last = seq
            self.wfile.write(framed_header(len(jpeg)) + jpeg)
            try:
                self.wfile.flush()
            except OSError:
                return


FIRST_WAIT = 60.0


def wait_frame(source: Source, timeout: float) -> bytes | None:
    deadline = time.monotonic() + timeout
    last = -1
    while time.monotonic() < deadline:
        jpeg, seq, error, stopped = source.frames.wait(last, min(1.0, deadline - time.monotonic()))
        if jpeg:
            return jpeg
        if error or stopped:
            return None
    return None


def session_payload(session: Session) -> dict:
    return {
        "session": session.id,
        "format": "jpeg",
        "width": session.width,
        "height": session.height,
        "fps": session.fps,
        "stream": f"/v1/session/{session.id}/mjpeg",
        "frame": f"/v1/session/{session.id}/jpeg",
        "framed": f"/v1/session/{session.id}/framed",
    }


def read_line(sock: socket.socket, limit: int = 4096) -> bytes:
    data = bytearray()
    while b"\n" not in data:
        if len(data) >= limit:
            raise ValueError("line too long")
        chunk = sock.recv(1)
        if not chunk:
            raise EOFError
        data.extend(chunk)
    return bytes(data)


def handle_line_client(sock: socket.socket, addr) -> None:
    session = None
    try:
        sock.settimeout(None)
        raw = read_line(sock)
        body = json.loads(raw.decode("utf-8"))
        if TOKEN and body.get("token") != TOKEN:
            sock.sendall(b"ERR relay token required\n")
            return
        params = parse_printer(body)
        session = HUB.create(params)
        source = session.acquire()
        last = -1
        interval = 1.0 / max(1, session.fps)
        first_deadline = time.monotonic() + 60.0
        sent = False
        started = time.monotonic()
        while not connection_closed(sock):
            wait = min(0.25, interval)
            if not sent:
                remain = first_deadline - time.monotonic()
                if remain <= 0:
                    _, _, error, _ = source.frames.snapshot()
                    stage = source.stage
                    log(
                        f"line first jpeg timed out model={session.model} "
                        f"stage={stage} connected={int(source.connected)} "
                        f"frames={source.produced} error={error or '-'}"
                    )
                    msg = error or f"no picture yet ({stage})"
                    sock.sendall(b"ERR " + msg.encode("utf-8", "replace")[:80] + b"\n")
                    return
                wait = min(wait, max(0.05, remain))
            jpeg, seq, error, stopped = source.frames.wait(last, wait)
            if error and jpeg is None:
                log(f"line stream error {error}")
                sock.sendall(b"ERR " + error.encode("utf-8", "replace")[:80] + b"\n")
                return
            if jpeg is None:
                if stopped:
                    return
                continue
            if seq == last:
                continue
            last = seq
            if not sent:
                log(f"line first jpeg model={session.model} wait={time.monotonic()-started:.1f}s")
            sock.sendall(framed_header(len(jpeg)) + jpeg)
            sent = True
    except Exception as exc:
        try:
            sock.sendall(b"ERR " + _safe_error(exc).encode("utf-8", "replace")[:80] + b"\n")
        except OSError:
            pass
        log(f"line client {_safe_error(exc)}")
    finally:
        try:
            sock.close()
        except OSError:
            pass
        if session:
            HUB.drop(session.id)


class DualServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 16

    def process_request(self, request, client_address):
        if not client_allowed(client_address[0]):
            try:
                request.close()
            except OSError:
                pass
            return
        try:
            request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            request.settimeout(30)
            first = request.recv(1, socket.MSG_PEEK)
        except OSError:
            try:
                request.close()
            except OSError:
                pass
            return
        if first == b"{":
            thread = threading.Thread(target=handle_line_client, args=(request, client_address), daemon=True)
            thread.start()
            return
        super().process_request(request, client_address)


def serve(bind=BIND, port=PORT, factory=open_source):
    global HUB
    HUB = Hub(factory)
    server = DualServer((bind, port), Handler)
    log(f"listening {bind}:{port}")

    def stop(*_args):
        log("stopping")
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    if hasattr(signal, "SIGINT"):
        signal.signal(signal.SIGINT, stop)
    try:
        server.serve_forever()
    finally:
        HUB.shutdown()
        server.server_close()
        log("stopped")


def main():
    try:
        from upstream import find_ffmpeg
        log("decoder " + find_ffmpeg())
    except FileNotFoundError:
        log("warning: ffmpeg missing; H2D sessions will fail, A1 JPEG still works")
    serve()


if __name__ == "__main__":
    main()
