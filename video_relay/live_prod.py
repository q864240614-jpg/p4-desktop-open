#!/usr/bin/env python3
"""Hit the deployed relay. Credentials come from env, never from a file."""
from __future__ import annotations

import io
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.request

from PIL import Image

for key in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"):
    os.environ.pop(key, None)
os.environ["NO_PROXY"] = "*"
os.environ["no_proxy"] = "*"

HOST = os.environ.get("VIDEO_RELAY_HOST", "127.0.0.1")
PORT = int(os.environ.get("VIDEO_RELAY_PORT", "2344"))
IP = os.environ.get("VIDEO_RELAY_LIVE_IP", "")
CODE = os.environ.get("VIDEO_RELAY_LIVE_CODE", "")
MODEL = os.environ.get("VIDEO_RELAY_LIVE_MODEL", "H2D")


OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def req(method, path, body=None, timeout=25):
    data = None if body is None else json.dumps(body).encode()
    headers = {"Content-Type": "application/json"} if data is not None else {}
    request = urllib.request.Request(f"http://{HOST}:{PORT}{path}", data=data, headers=headers, method=method)
    try:
        with OPENER.open(request, timeout=timeout) as res:
            raw = res.read()
            return res.status, raw, dict(res.headers)
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read(), dict(exc.headers)


def main():
    if not IP or not CODE:
        print("skip: VIDEO_RELAY_LIVE_IP / VIDEO_RELAY_LIVE_CODE not set")
        return 0
    status, raw, _ = req("GET", "/health", timeout=5)
    health = json.loads(raw.decode())
    assert status == 200 and health.get("ok"), health
    print("health", {k: health[k] for k in ("ok", "sessions", "live_printers")})

    status, raw, _ = req("POST", "/v1/session", {
        "ip": IP, "access_code": CODE, "model": MODEL, "width": 800, "height": 480, "fps": 4, "quality": 50
    })
    body = json.loads(raw.decode())
    if status != 201:
        print("FAIL open", status, body)
        return 1
    session = body["session"]
    print("session opened")

    status, jpeg, headers = req("GET", f"/v1/session/{session}/jpeg", timeout=25)
    if status != 200 or jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
        print("FAIL jpeg", status, len(jpeg), jpeg[:80])
        req("DELETE", f"/v1/session/{session}")
        return 1
    image = Image.open(io.BytesIO(jpeg))
    print("jpeg", image.size, image.format, "bytes", len(jpeg), "header", headers.get("X-Frame-Width"))
    if image.size != (800, 480):
        print("FAIL unexpected size")
        req("DELETE", f"/v1/session/{session}")
        return 1
    req("DELETE", f"/v1/session/{session}")
    time.sleep(0.4)

    sock = socket.create_connection((HOST, PORT), timeout=25)
    sock.settimeout(25)
    sock.sendall(json.dumps({
        "ip": IP, "access_code": CODE, "model": MODEL, "width": 800, "height": 480, "fps": 4
    }).encode() + b"\n")
    header = b""
    while len(header) < 16:
        chunk = sock.recv(16 - len(header))
        if not chunk:
            print("FAIL line protocol closed")
            sock.close()
            return 1
        header += chunk
    size = struct.unpack_from("<I", header, 0)[0]
    framed = b""
    while len(framed) < size:
        chunk = sock.recv(size - len(framed))
        if not chunk:
            break
        framed += chunk
    sock.close()
    ok_line = framed[:2] == b"\xff\xd8" and framed[-2:] == b"\xff\xd9"
    print("line protocol", "ok" if ok_line else "FAIL", "bytes", len(framed))

    req("DELETE", f"/v1/session/{session}")
    time.sleep(1.2)
    status, raw, _ = req("GET", "/health", timeout=5)
    health = json.loads(raw.decode())
    print("after stop", {k: health[k] for k in ("ok", "sessions", "live_printers")})
    if health.get("live_printers") != 0:
        print("FAIL printer still live")
        return 1
    if not ok_line:
        return 1
    print("live production check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
