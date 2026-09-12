#!/usr/bin/env python3
"""Relay tests. Printer secrets never go to disk; live test reads them from env only."""
from __future__ import annotations

import sys
sys.dont_write_bytecode = True

import io
import json
import os
import socket
import struct
import tempfile
import threading
import time
import traceback
import urllib.error
import urllib.request
from http.client import HTTPConnection
from pathlib import Path

import server
from upstream import FakeSource, _safe_error, cover_crop_jpeg, extract_jpegs, find_ffmpeg, private_ipv4, validate_access_code
from PIL import Image

PASS = 0
FAIL = 0
HERE = Path(__file__).resolve().parent


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok  {name}")
    else:
        FAIL += 1
        print(f" FAIL {name} {detail}")


class Factory:
    def __init__(self):
        self.sources = []

    def __call__(self, ip, access_code, model, width, height, quality):
        source = FakeSource(ip, access_code, model, width, height, quality, interval=0.03)
        self.sources.append(source)
        return source


def start_server(factory=None):
    factory = factory or Factory()
    server.HUB = server.Hub(factory)
    httpd = server.DualServer(("127.0.0.1", 0), server.Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return httpd, httpd.server_address[1], factory


def stop_server(httpd):
    httpd.shutdown()
    httpd.server_close()
    server.HUB.shutdown()


def http_json(port, method, path, body=None, headers=None, timeout=5):
    conn = HTTPConnection("127.0.0.1", port, timeout=timeout)
    raw = None if body is None else json.dumps(body).encode()
    hdrs = {"Content-Type": "application/json"} if raw is not None else {}
    if headers:
        hdrs.update(headers)
    conn.request(method, path, body=raw, headers=hdrs)
    res = conn.getresponse()
    data = res.read()
    conn.close()
    try:
        parsed = json.loads(data.decode()) if data else {}
    except json.JSONDecodeError:
        parsed = {}
    return res.status, parsed, data


def jpeg_info(data: bytes):
    image = Image.open(io.BytesIO(data))
    return image.size, image.format


def test_validation():
    print("validation")
    check("lan ip", private_ipv4("192.168.1.48") == "192.168.1.48")
    try:
        private_ipv4("8.8.8.8")
        check("reject public ip", False)
    except ValueError:
        check("reject public ip", True)
    try:
        private_ipv4("127.0.0.1")
        check("reject loopback printer", False)
    except ValueError:
        check("reject loopback printer", True)
    try:
        validate_access_code("bad code")
        check("reject spaced code", False)
    except ValueError:
        check("reject spaced code", True)
    check("code ok", validate_access_code("240402ab") == "240402ab")
    check("timeout wording", _safe_error(TimeoutError("The read operation timed out")) == "printer camera timed out")


def test_jpeg_tools():
    print("jpeg tools")
    image = Image.new("RGB", (1680, 1080), (0, 174, 66))
    buf = io.BytesIO()
    image.save(buf, format="JPEG", quality=70)
    out = cover_crop_jpeg(buf.getvalue(), 800, 480, 60)
    size, fmt = jpeg_info(out)
    check("cover crop size", size == (800, 480), size)
    check("cover crop jpeg", fmt == "JPEG" and out[:2] == b"\xff\xd8" and out[-2:] == b"\xff\xd9")
    stream = out + out
    frames = list(extract_jpegs(bytearray(stream)))
    check("concat jpeg split", len(frames) == 2 and frames[0] == out)


def test_http_session():
    print("http session")
    logs = io.StringIO()
    old = server.log

    def capture(message):
        logs.write(message + "\n")
        old(message)

    server.log = capture
    httpd, port, factory = start_server()
    try:
        status, body, _ = http_json(port, "GET", "/health")
        check("health", status == 200 and body.get("ok") is True and body.get("live_printers") == 0, body)

        status, body, _ = http_json(port, "POST", "/v1/session", {
            "ip": "8.8.8.8", "access_code": "12345678", "model": "H2D"
        })
        check("post public ip rejected", status == 400, body)
        check("public ip did not connect", factory.sources == [])

        secret = "SecretCode99"
        status, body, _ = http_json(port, "POST", "/v1/session", {
            "ip": "192.168.1.48", "access_code": secret, "model": "H2D",
            "width": 800, "height": 480, "fps": 5, "quality": 55
        })
        check("post session", status == 201 and "session" in body, body)
        check("lazy connect after post", factory.sources == [])
        session = body["session"]

        conn = HTTPConnection("127.0.0.1", port, timeout=5)
        conn.request("GET", f"/v1/session/{session}/jpeg")
        res = conn.getresponse()
        jpeg = res.read()
        conn.close()
        check("jpeg http", res.status == 200 and jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9", res.status)
        check("jpeg size header", res.getheader("X-Frame-Width") == "800")
        size, _ = jpeg_info(jpeg)
        check("jpeg 800x480", size == (800, 480), size)
        check("connects on first viewer", len(factory.sources) == 1 and factory.sources[0].connects == 1)

        status, info, _ = http_json(port, "GET", f"/v1/session/{session}")
        check("info has no ip", status == 200 and "ip" not in info and "access_code" not in info, info)
        check("info live", info.get("state") in ("live", "waiting"), info)

        http_json(port, "DELETE", f"/v1/session/{session}")
        time.sleep(0.3)
        check("stopped after delete", factory.sources[0].frames.snapshot()[3] is True)
        text = logs.getvalue()
        check("logs hide access code", secret not in text)
        check("logs hide printer ip", "192.168.1.48" not in text)
    finally:
        server.log = old
        stop_server(httpd)


def test_idle_and_mjpeg():
    print("idle and mjpeg")
    prev = server.IDLE_SECONDS
    prev_first = server.FIRST_ACQUIRE_SECONDS
    server.IDLE_SECONDS = 0.5
    server.FIRST_ACQUIRE_SECONDS = 0.5
    httpd, port, factory = start_server()
    try:
        status, body, _ = http_json(port, "POST", "/v1/session", {
            "ip": "10.0.0.8", "access_code": "abcd1234", "model": "H2D"
        })
        session = body["session"]
        time.sleep(0.8)
        deadline = time.monotonic() + 2.0
        status = 200
        while time.monotonic() < deadline:
            status, _, _ = http_json(port, "GET", f"/v1/session/{session}")
            if status == 404:
                break
            time.sleep(0.1)
        check("idle session without viewer never connects", factory.sources == [] and status == 404, status)

        status, body, _ = http_json(port, "POST", "/v1/session", {
            "ip": "10.0.0.8", "access_code": "abcd1234", "model": "H2D"
        })
        session = body["session"]
        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock.sendall(
            f"GET /v1/session/{session}/mjpeg HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n".encode()
        )
        buf = b""
        while b"\r\n\r\n" not in buf or b"--frame" not in buf:
            chunk = sock.recv(128)
            if not chunk:
                break
            buf += chunk
            if len(buf) > 4096:
                break
        check("mjpeg status", b"200" in buf.split(b"\r\n", 1)[0], buf[:80])
        check("mjpeg boundary", b"--frame" in buf, buf[:80])
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()
        stopped = False
        for _ in range(20):
            time.sleep(0.1)
            if factory.sources and factory.sources[-1].frames.snapshot()[3]:
                stopped = True
                break
        check("printer dropped after idle", stopped)
    finally:
        server.IDLE_SECONDS = prev
        server.FIRST_ACQUIRE_SECONDS = prev_first
        stop_server(httpd)


def test_framed_and_line():
    print("framed and line protocol")
    httpd, port, factory = start_server()
    try:
        status, body, _ = http_json(port, "POST", "/v1/session", {
            "ip": "192.168.1.20", "access_code": "code9876", "model": "A1MINI"
        })
        session = body["session"]
        conn = HTTPConnection("127.0.0.1", port, timeout=5)
        conn.request("GET", f"/v1/session/{session}/framed")
        res = conn.getresponse()
        header = res.read(16)
        size = struct.unpack_from("<I", header, 0)[0]
        jpeg = res.read(size)
        conn.close()
        check("framed header flags", header[8] == 1 and header[4] == 0)
        check("framed jpeg", jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9" and size == len(jpeg))

        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock.sendall(b'{"ip":"192.168.1.21","access_code":"line1234","model":"H2D","fps":5}\n')
        header = b""
        while len(header) < 16:
            chunk = sock.recv(16 - len(header))
            if not chunk:
                break
            header += chunk
        size = struct.unpack_from("<I", header, 0)[0] if len(header) == 16 else 0
        jpeg = b""
        while size and len(jpeg) < size:
            chunk = sock.recv(size - len(jpeg))
            if not chunk:
                break
            jpeg += chunk
        sock.close()
        check("line protocol jpeg", len(header) == 16 and jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9", len(jpeg))
        time.sleep(0.2)
        check("line close stops source", any(src.frames.snapshot()[3] for src in factory.sources))
    finally:
        stop_server(httpd)


def test_share_upstream():
    print("shared upstream")
    httpd, port, factory = start_server()
    try:
        body = {"ip": "192.168.1.9", "access_code": "share0001", "model": "H2D"}
        _, a, _ = http_json(port, "POST", "/v1/session", body)
        _, b, _ = http_json(port, "POST", "/v1/session", body)
        for session in (a["session"], b["session"]):
            conn = HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("GET", f"/v1/session/{session}/jpeg")
            res = conn.getresponse()
            jpeg = res.read()
            conn.close()
            check("shared jpeg " + session[:8], res.status == 200 and jpeg[:2] == b"\xff\xd8")
        check("one printer connection", len(factory.sources) == 1, len(factory.sources))
        http_json(port, "DELETE", f"/v1/session/{a['session']}")
        time.sleep(0.2)
        check("source stays for other viewer", factory.sources[0].frames.snapshot()[3] is False)
        http_json(port, "DELETE", f"/v1/session/{b['session']}")
        time.sleep(0.4)
        check("source stops after last viewer", factory.sources[0].frames.snapshot()[3] is True)
    finally:
        stop_server(httpd)


def test_forbid_foreign_client():
    print("allowlist")
    check("localhost allowed", server.client_allowed("127.0.0.1"))
    check("lan allowed", server.client_allowed("192.168.1.50"))
    check("tailscale denied", not server.client_allowed("100.109.228.87"))
    check("public denied", not server.client_allowed("1.1.1.1"))


def test_no_disk_artifacts():
    print("no disk artifacts")
    tmp = Path(tempfile.gettempdir())
    before = {p.name for p in tmp.glob("*") if "h264" in p.name.lower() or "printer" in p.name.lower()}
    httpd, port, factory = start_server()
    try:
        status, body, _ = http_json(port, "POST", "/v1/session", {
            "ip": "192.168.1.7", "access_code": "NoSave01", "model": "H2D"
        })
        conn = HTTPConnection("127.0.0.1", port, timeout=5)
        conn.request("GET", f"/v1/session/{body['session']}/jpeg")
        conn.getresponse().read()
        conn.close()
    finally:
        stop_server(httpd)
    after = {p.name for p in tmp.glob("*") if "h264" in p.name.lower() or "printer" in p.name.lower()}
    extra = after - before
    check("tmp has no printer media", not extra, extra)
    leaked = []
    for path in HERE.glob("*"):
        if path.suffix in {".jpg", ".jpeg", ".h264", ".mp4"}:
            leaked.append(path.name)
    check("source dir has no media files", not leaked, leaked)


def test_ffmpeg_decode():
    print("ffmpeg decode")
    try:
        ffmpeg = find_ffmpeg()
    except FileNotFoundError:
        print("  skip ffmpeg not available")
        return
    from upstream import H264Decoder
    work = Path(tempfile.mkdtemp(prefix="vr-h264-"))
    clip = work / "src.h264"
    try:
        import subprocess
        subprocess.check_call([
            ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc=size=320x240:rate=6:duration=2",
            "-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p",
            "-g", "6", "-f", "h264", str(clip)
        ], timeout=20)
        decoder = H264Decoder(800, 480, 55)
        decoder.write(clip.read_bytes())
        decoder.proc.stdin.close()
        buffer = bytearray()
        deadline = time.monotonic() + 8
        frames = []
        while time.monotonic() < deadline and decoder.proc.stdout:
            chunk = decoder.proc.stdout.read(4096)
            if not chunk:
                break
            buffer.extend(chunk)
            frames.extend(extract_jpegs(buffer))
        err = decoder.error_text()
        decoder.close()
        check("decoded at least one jpeg", len(frames) >= 1, f"frames={len(frames)} ffmpeg={err}")
        if frames:
            size, fmt = jpeg_info(frames[0])
            check("decoded jpeg 800x480", size == (800, 480) and fmt == "JPEG", size)
    except Exception as exc:
        check("ffmpeg decode", False, f"{type(exc).__name__}: {exc}")
    finally:
        for path in work.glob("*"):
            path.unlink(missing_ok=True)
        work.rmdir()


def test_live_h2d():
    print("live H2D")
    ip = os.environ.get("VIDEO_RELAY_LIVE_IP", "")
    code = os.environ.get("VIDEO_RELAY_LIVE_CODE", "")
    if not ip or not code:
        print("  skip set VIDEO_RELAY_LIVE_IP / VIDEO_RELAY_LIVE_CODE to run")
        return
    httpd, port, _ = start_server(server.open_source)
    try:
        status, body, _ = http_json(port, "POST", "/v1/session", {
            "ip": ip, "access_code": code, "model": "H2D", "fps": 4, "quality": 50
        }, timeout=3)
        check("live session", status == 201, body)
        if status != 201:
            return
        conn = HTTPConnection("127.0.0.1", port, timeout=25)
        conn.request("GET", f"/v1/session/{body['session']}/jpeg")
        res = conn.getresponse()
        jpeg = res.read()
        conn.close()
        check("live jpeg", res.status == 200 and jpeg[:2] == b"\xff\xd8", f"status={res.status} bytes={len(jpeg)}")
        if res.status == 200:
            size, fmt = jpeg_info(jpeg)
            check("live jpeg size", size == (800, 480), size)
        http_json(port, "DELETE", f"/v1/session/{body['session']}")
        time.sleep(0.5)
        status, info, _ = http_json(port, "GET", "/health")
        check("live printers released", info.get("live_printers") == 0, info)
    finally:
        stop_server(httpd)


def main():
    os.chdir(HERE)
    tests = [
        test_validation,
        test_jpeg_tools,
        test_http_session,
        test_idle_and_mjpeg,
        test_framed_and_line,
        test_share_upstream,
        test_forbid_foreign_client,
        test_no_disk_artifacts,
        test_ffmpeg_decode,
        test_live_h2d,
    ]
    for fn in tests:
        try:
            fn()
        except Exception:
            global FAIL
            FAIL += 1
            print(f" FAIL {fn.__name__} crashed")
            traceback.print_exc()
    print(f"\n{PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
