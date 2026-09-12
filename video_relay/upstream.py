"""On-demand Bambu LAN camera pull + JPEG transcode. Nothing is written to disk."""
from __future__ import annotations

import io
import os
import re
import ssl
import struct
import socket
import hashlib
import base64
import shutil
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from typing import Optional
from urllib.request import parse_http_list, parse_keqv_list

from PIL import Image

START = b"\x00\x00\x00\x01"
JPEG_CAP = 256 * 1024
NAL_LIMIT = 1024 * 1024
AU_LIMIT = 2 * 1024 * 1024
KEEPALIVE = 15.0
READ_TIMEOUT = 20.0
FIRST_READ_TIMEOUT = 60.0

H264_MODELS = {"h2d", "h2s", "h2c", "h2", "x1", "x1c", "x1e", "p2s"}
JPEG_MODELS = {"a1", "a1mini", "a1_mini", "a1-mini", "p1", "p1p", "p1s"}


def log(message: str) -> None:
    sys.stderr.write(time.strftime("%Y-%m-%d %H:%M:%S ") + message + "\n")
    sys.stderr.flush()


def normalize_model(model: str) -> str:
    name = (model or "h2d").strip().lower().replace(" ", "")
    if name in JPEG_MODELS:
        return "a1"
    if name in H264_MODELS or name.startswith("h2") or name.startswith("x1"):
        return "h2d"
    raise ValueError("unsupported printer model")


def private_ipv4(text: str) -> str:
    try:
        packed = socket.inet_aton(text)
    except OSError as exc:
        raise ValueError("printer ip must be IPv4") from exc
    ip = socket.inet_ntoa(packed)
    n = struct.unpack("!I", packed)[0]
    if ip in ("0.0.0.0", "255.255.255.255", "127.0.0.1"):
        raise ValueError("printer ip is not a LAN address")
    if (n >> 24) == 10:
        return ip
    if (n >> 20) == 0xAC1:
        return ip
    if (n >> 16) == 0xC0A8:
        return ip
    raise ValueError("printer ip must be RFC1918")


def validate_access_code(code: str) -> str:
    if not isinstance(code, str) or not 4 <= len(code) <= 32 or not re.fullmatch(r"[A-Za-z0-9]+", code):
        raise ValueError("access code must be 4-32 alphanumeric characters")
    return code


def clamp_output(width: int, height: int, fps: int, quality: int) -> tuple[int, int, int, int]:
    width = int(width or 800)
    height = int(height or 480)
    fps = int(fps or 8)
    quality = int(quality or 60)
    if width % 2 or height % 2 or not 320 <= width <= 800 or not 192 <= height <= 480:
        raise ValueError("output size must be even, 320-800 x 192-480")
    if not 1 <= fps <= 15:
        raise ValueError("fps must be 1-15")
    if not 20 <= quality <= 90:
        raise ValueError("jpeg quality must be 20-90")
    return width, height, fps, quality


def cover_crop_jpeg(jpeg: bytes, width: int, height: int, quality: int) -> bytes:
    image = Image.open(io.BytesIO(jpeg)).convert("RGB")
    src_w, src_h = image.size
    if not src_w or not src_h:
        raise ValueError("empty jpeg")
    if src_w * height > src_h * width:
        crop_w = src_h * width // height
        left = (src_w - crop_w) // 2
        image = image.crop((left, 0, left + crop_w, src_h))
    else:
        crop_h = src_w * height // width
        top = (src_h - crop_h) // 2
        image = image.crop((0, top, src_w, top + crop_h))
    if image.size != (width, height):
        image = image.resize((width, height), Image.BILINEAR)
    out = io.BytesIO()
    image.save(out, format="JPEG", quality=quality, optimize=False)
    data = out.getvalue()
    if len(data) > JPEG_CAP:
        out = io.BytesIO()
        image.save(out, format="JPEG", quality=max(20, quality - 20), optimize=True)
        data = out.getvalue()
    if len(data) > JPEG_CAP or data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise ValueError("jpeg encode failed")
    return data


def find_ffmpeg() -> str:
    env = os.environ.get("VIDEO_RELAY_FFMPEG")
    if env and os.path.isfile(env) and os.access(env, os.X_OK):
        return env
    local = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", "ffmpeg")
    if os.path.isfile(local) and os.access(local, os.X_OK):
        return local
    try:
        import imageio_ffmpeg
        path = imageio_ffmpeg.get_ffmpeg_exe()
        if path and os.path.isfile(path):
            return path
    except Exception:
        pass
    found = shutil.which("ffmpeg")
    if found:
        return found
    raise FileNotFoundError("ffmpeg is not installed")


def extract_jpegs(buffer: bytearray):
    """Yield complete JPEG images from a concatenated MJPEG byte stream."""
    while True:
        start = buffer.find(b"\xff\xd8")
        if start < 0:
            buffer.clear()
            return
        if start:
            del buffer[:start]
        i = 2
        while i + 1 < len(buffer):
            if buffer[i] != 0xFF:
                i += 1
                continue
            marker = buffer[i + 1]
            if marker == 0xD9:
                frame = bytes(buffer[: i + 2])
                del buffer[: i + 2]
                yield frame
                break
            if marker == 0x00 or marker == 0xD8 or (0xD0 <= marker <= 0xD7):
                i += 2
                continue
            if marker == 0xFF:
                i += 1
                continue
            if i + 3 >= len(buffer):
                return
            length = (buffer[i + 2] << 8) | buffer[i + 3]
            i += 2 + length
        else:
            return


class FrameBuffer:
    def __init__(self):
        self._cv = threading.Condition()
        self._jpeg: Optional[bytes] = None
        self._seq = 0
        self._error: Optional[str] = None
        self._stopped = False

    def publish(self, jpeg: bytes) -> None:
        with self._cv:
            self._jpeg = jpeg
            self._seq += 1
            self._cv.notify_all()

    def fail(self, message: str) -> None:
        with self._cv:
            self._error = message
            self._stopped = True
            self._cv.notify_all()

    def stop(self) -> None:
        with self._cv:
            self._stopped = True
            self._cv.notify_all()

    def snapshot(self) -> tuple[Optional[bytes], int, Optional[str], bool]:
        with self._cv:
            return self._jpeg, self._seq, self._error, self._stopped

    def wait(self, last_seq: int, timeout: float) -> tuple[Optional[bytes], int, Optional[str], bool]:
        deadline = time.monotonic() + timeout
        with self._cv:
            while self._seq == last_seq and not self._stopped and not self._error:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self._cv.wait(remaining)
            return self._jpeg, self._seq, self._error, self._stopped


class TLSConn:
    def __init__(self, ip: str, port: int, timeout: float = FIRST_READ_TIMEOUT):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        raw = socket.create_connection((ip, port), timeout=timeout)
        raw.settimeout(timeout)
        self.sock = ctx.wrap_socket(raw, server_hostname=ip)
        self.buf = bytearray()

    def settimeout(self, timeout: float) -> None:
        self.sock.settimeout(timeout)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def sendall(self, data: bytes) -> None:
        self.sock.sendall(data)

    def read_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("printer closed the camera connection")
            self.buf.extend(chunk)
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def read_until(self, token: bytes, limit: int) -> bytes:
        while token not in self.buf:
            if len(self.buf) >= limit:
                raise ValueError("printer response too large")
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("printer closed the camera connection")
            self.buf.extend(chunk)
        idx = self.buf.index(token) + len(token)
        out = bytes(self.buf[:idx])
        del self.buf[:idx]
        return out


class H264Assembler:
    def __init__(self):
        self.fragment = bytearray()
        self.fragment_ts = None
        self.au = bytearray()
        self.au_ts = None
        self.seq = None
        self.sps = b""
        self.pps = b""
        self.discard_ts = None

    def load_sprop(self, value: str) -> None:
        for part in value.split(","):
            nal = base64.b64decode(part.strip())
            if not nal:
                continue
            self._keep_parameter(nal)
            self.au.extend(START + nal)

    def push(self, packet: bytes, payload_type: int) -> Optional[bytes]:
        parsed = _rtp(packet, payload_type)
        if parsed is None:
            return None
        seq, timestamp, marker, payload = parsed
        if self.seq is not None and seq != (self.seq + 1) & 0xFFFF:
            self.fragment.clear()
            self.au.clear()
            self.au_ts = None
            self.discard_ts = timestamp
        self.seq = seq
        if self.discard_ts is not None:
            if timestamp == self.discard_ts:
                return None
            self.discard_ts = None
        if self.au_ts is not None and timestamp != self.au_ts and self.au:
            picture = self._finish()
            self._append_payload(payload, timestamp)
            if marker:
                later = self._finish()
                return picture if later is None else later
            return picture
        self._append_payload(payload, timestamp)
        if marker:
            return self._finish()
        return None

    def _keep_parameter(self, nal: bytes) -> None:
        ntype = nal[0] & 31
        if ntype == 7:
            self.sps = nal
        elif ntype == 8:
            self.pps = nal

    def _nal(self, nal: bytes, timestamp: int) -> None:
        if not nal or nal[0] & 0x80:
            return
        ntype = nal[0] & 31
        if ntype == 0 or ntype > 23:
            return
        self._keep_parameter(nal)
        if ntype in (6, 9):
            return
        if self.au_ts is None:
            self.au_ts = timestamp
        if len(self.au) + 4 + len(nal) > AU_LIMIT:
            self.au.clear()
            return
        self.au.extend(START + nal)

    def _append_payload(self, payload: bytes, timestamp: int) -> None:
        if not payload:
            return
        ntype = payload[0] & 31
        if 1 <= ntype <= 23:
            self._nal(payload, timestamp)
            return
        if ntype == 24:
            offset = 1
            while offset + 2 <= len(payload):
                size = int.from_bytes(payload[offset:offset + 2], "big")
                offset += 2
                if size <= 0 or offset + size > len(payload):
                    return
                self._nal(payload[offset:offset + size], timestamp)
                offset += size
            return
        if ntype != 28 or len(payload) < 3 or payload[1] & 0x20:
            return
        start, end = payload[1] & 0x80, payload[1] & 0x40
        reconstructed = (payload[0] & 0xE0) | (payload[1] & 31)
        if start:
            self.fragment = bytearray([reconstructed])
            self.fragment_ts = timestamp
        elif not self.fragment or self.fragment_ts != timestamp or self.fragment[0] != reconstructed:
            self.fragment.clear()
            return
        if len(self.fragment) + len(payload) - 2 > NAL_LIMIT:
            self.fragment.clear()
            return
        self.fragment.extend(payload[2:])
        if end:
            self._nal(bytes(self.fragment), timestamp)
            self.fragment.clear()

    def _finish(self) -> Optional[bytes]:
        picture = bytes(self.au)
        self.au.clear()
        self.au_ts = None
        if len(picture) < 8:
            return None
        if self.sps and START + self.sps not in picture:
            picture = START + self.sps + (START + self.pps if self.pps else b"") + picture
        return picture


def _rtp(packet: bytes, payload_type: int):
    if len(packet) < 12 or packet[0] >> 6 != 2:
        return None
    if (packet[1] & 127) != payload_type:
        return None
    seq = int.from_bytes(packet[2:4], "big")
    timestamp = int.from_bytes(packet[4:8], "big")
    marker = bool(packet[1] & 128)
    start = 12 + 4 * (packet[0] & 15)
    if start > len(packet):
        return None
    if packet[0] & 16:
        if start + 4 > len(packet):
            return None
        ext = int.from_bytes(packet[start + 2:start + 4], "big")
        start += 4 + 4 * ext
    if start > len(packet):
        return None
    payload = packet[start:]
    if packet[0] & 32:
        pad = payload[-1] if payload else 0
        if not pad or pad > len(payload):
            return None
        payload = payload[:-pad]
    return seq, timestamp, marker, payload


class H264Decoder:
    def __init__(self, width: int, height: int, quality: int):
        ffmpeg = find_ffmpeg()
        vf = (
            f"scale={width}:{height}:force_original_aspect_ratio=increase:flags=fast_bilinear,"
            f"crop={width}:{height}"
        )
        # Quality 2-31 for mjpeg qscale; map 20-90 JPEG quality onto that range.
        qscale = str(max(2, min(16, round((100 - quality) / 5))))
        self.proc = subprocess.Popen(
            [
                ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "h264",
                "-i",
                "pipe:0",
                "-an",
                "-vf",
                vf,
                "-q:v",
                qscale,
                "-f",
                "mjpeg",
                "pipe:1",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self._err = bytearray()
        threading.Thread(target=self._drain_stderr, name="ffmpeg-err", daemon=True).start()

    def _drain_stderr(self) -> None:
        try:
            while True:
                chunk = self.proc.stderr.read(256)
                if not chunk:
                    return
                if len(self._err) < 512:
                    self._err.extend(chunk[: 512 - len(self._err)])
        except OSError:
            return

    def write(self, access_unit: bytes) -> None:
        if not self.proc.stdin:
            raise RuntimeError("decoder stdin closed")
        self.proc.stdin.write(access_unit)
        self.proc.stdin.flush()

    def close(self) -> None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.kill()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=1)
        except Exception:
            pass

    def error_text(self) -> str:
        text = bytes(self._err).decode("utf-8", "replace").strip()
        text = re.sub(r"\d{1,3}(?:\.\d{1,3}){3}", "<ip>", text)
        return text[:160] if text else "decoder exited"


class Source:
    """Live transcode session. start() launches threads; stop() drops the printer."""

    def __init__(self, ip: str, access_code: str, model: str, width: int, height: int, quality: int):
        self.model = normalize_model(model)
        self.width = width
        self.height = height
        self.quality = quality
        self.frames = FrameBuffer()
        self.connected = False
        self.produced = 0
        self.stage = "idle"
        self._ip = private_ipv4(ip)
        self._code = validate_access_code(access_code)
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._tls: Optional[TLSConn] = None
        self._decoder: Optional[H264Decoder] = None
        self._rtsp: Optional[RtspClient] = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="printer-source", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self.frames.stop()
        tls, decoder, rtsp = self._tls, self._decoder, self._rtsp
        self._tls = None
        self._decoder = None
        self._rtsp = None
        self._code = ""
        self._ip = ""
        if rtsp and tls:
            try:
                tls.settimeout(2)
                rtsp.teardown()
            except Exception:
                pass
            rtsp.code = ""
        if tls:
            tls.close()
        if decoder:
            decoder.close()

    def join(self, timeout: float = 2.0) -> None:
        if self._thread:
            self._thread.join(timeout)

    def _run(self) -> None:
        try:
            if self.model == "a1":
                self._run_jpeg()
            else:
                self._run_h264()
        except Exception as exc:
            if not self._stop.is_set():
                self.stage = "error"
                err = _safe_error(exc)
                self.frames.fail(err)
                log(f"{self.model} error {err}")
        finally:
            self.connected = False
            tls, decoder, rtsp = self._tls, self._decoder, self._rtsp
            self._tls = None
            self._decoder = None
            self._rtsp = None
            if rtsp and tls and not self._stop.is_set():
                try:
                    tls.settimeout(2)
                    rtsp.teardown()
                except Exception:
                    pass
            if rtsp:
                rtsp.code = ""
            if tls:
                tls.close()
            if decoder:
                decoder.close()
            self._code = ""
            if not self._stop.is_set() and self.frames.snapshot()[2] is None:
                self.stage = "ended"
                self.frames.fail("printer stream ended")

    def _run_jpeg(self) -> None:
        self.stage = "tls"
        log("a1 stage=tls")
        tls = TLSConn(self._ip, 6000)
        self._tls = tls
        packet = bytearray(80)
        struct.pack_into("<I", packet, 0, 0x40)
        struct.pack_into("<I", packet, 4, 0x3000)
        packet[16:20] = b"bblp"
        packet[48:48 + len(self._code)] = self._code.encode()
        tls.sendall(bytes(packet))
        self.connected = True
        self.stage = "playing"
        log("a1 stage=playing")
        while not self._stop.is_set():
            header = tls.read_exact(16)
            size = struct.unpack_from("<I", header, 0)[0]
            if size < 4 or size > 512 * 1024:
                raise ValueError("printer jpeg frame size invalid")
            jpeg = tls.read_exact(size)
            if jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
                raise ValueError("printer jpeg is incomplete")
            out = cover_crop_jpeg(jpeg, self.width, self.height, self.quality)
            self.produced += 1
            if self.produced == 1:
                self.stage = "live"
                tls.settimeout(READ_TIMEOUT)
            self.frames.publish(out)

    def _run_h264(self) -> None:
        self.stage = "tls"
        log("h2d stage=tls")
        tls = TLSConn(self._ip, 322)
        self._tls = tls
        self.stage = "rtsp"
        log("h2d stage=rtsp")
        session = RtspClient(tls, self._ip, self._code)
        self._rtsp = session
        payload_type, sprop = session.play()
        self.connected = True  # RTSP PLAY ok; first JPEG may still take a few seconds.
        self.stage = "playing"
        log("h2d stage=playing")
        decoder = H264Decoder(self.width, self.height, self.quality)
        self._decoder = decoder
        assembler = H264Assembler()
        if sprop:
            assembler.load_sprop(sprop)
            if assembler.au:
                decoder.write(bytes(assembler.au))
                assembler.au.clear()
        reader = threading.Thread(target=self._read_jpegs, args=(decoder,), name="jpeg-out", daemon=True)
        reader.start()
        last_keep = time.monotonic()
        while not self._stop.is_set():
            channel, packet = session.read_interleaved()
            if channel == 0:
                access_unit = assembler.push(packet, payload_type)
                if access_unit:
                    decoder.write(access_unit)
            now = time.monotonic()
            if now - last_keep >= KEEPALIVE:
                session.options()
                last_keep = now
            if decoder.proc.poll() is not None:
                raise RuntimeError(decoder.error_text())

    def _read_jpegs(self, decoder: H264Decoder) -> None:
        buffer = bytearray()
        try:
            while not self._stop.is_set() and decoder.proc.stdout:
                chunk = decoder.proc.stdout.read(4096)
                if not chunk:
                    return
                buffer.extend(chunk)
                for jpeg in extract_jpegs(buffer):
                    if jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9" and len(jpeg) <= JPEG_CAP:
                        self.produced += 1
                        if self.produced == 1:
                            self.stage = "live"
                            log("h2d stage=live")
                        self.frames.publish(jpeg)
                    if len(buffer) > 2 * JPEG_CAP:
                        del buffer[:-2]
        except OSError:
            return


def _safe_error(exc: BaseException) -> str:
    text = str(exc) or exc.__class__.__name__
    if isinstance(exc, TimeoutError) or "timed out" in text.lower() or "timeout" in text.lower():
        return "printer camera timed out"
    text = re.sub(r"\d{1,3}(?:\.\d{1,3}){3}", "<ip>", text)
    text = re.sub(r"[A-Za-z0-9]{4,32}", lambda m: m.group(0) if m.group(0).isalpha() else "<id>", text)
    if len(text) > 160:
        text = text[:157] + "..."
    return text or "printer stream failed"


class RtspClient:
    def __init__(self, tls: TLSConn, ip: str, code: str):
        self.tls = tls
        self.url = f"rtsps://{ip}:322/streaming/live/1"
        self.code = code
        self.cseq = 0
        self.session = ""
        self.challenge = None
        self.nc = 0
        self.track = ""
        self.payload_type = 96
        self.sprop = ""

    def play(self) -> tuple[int, str]:
        headers, body = self.request("DESCRIBE", self.url, "Accept: application/sdp\r\n")
        self._parse_sdp(headers, body)
        setup_headers, _ = self.request(
            "SETUP", self.track, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        )
        session = setup_headers.get("session", "")
        self.session = session.split(";", 1)[0].strip()
        if not self.session:
            raise ValueError("printer RTSP session missing")
        self.request("PLAY", self.url, "Range: npt=0.000-\r\n")
        return self.payload_type, self.sprop

    def teardown(self) -> None:
        if not self.session:
            return
        try:
            self.request("TEARDOWN", self.url, "")
        except Exception:
            pass

    def options(self) -> None:
        try:
            self.request("OPTIONS", self.url, "")
        except Exception:
            pass

    def request(self, method: str, target: str, extra: str) -> tuple[dict, str]:
        self.cseq += 1
        for _attempt in range(2):
            auth = self._authorization(method, target)
            session = f"Session: {self.session}\r\n" if self.session else ""
            message = (
                f"{method} {target} RTSP/1.0\r\n"
                f"CSeq: {self.cseq}\r\n"
                f"User-Agent: P4-VideoRelay\r\n"
                f"{auth}{session}{extra}\r\n"
            )
            self.tls.sendall(message.encode())
            headers, body, status = self._read_response()
            if status == 401 and self.challenge is None and "www-authenticate" in headers:
                self.challenge = headers["www-authenticate"]
                self.cseq += 1
                continue
            if status != 200:
                raise ValueError(f"printer RTSP {method} failed ({status})")
            return headers, body
        raise ValueError("printer RTSP authentication failed")

    def _authorization(self, method: str, target: str) -> str:
        if not self.challenge:
            return ""
        scheme, _, value = self.challenge.partition(" ")
        if scheme.lower() == "basic":
            token = base64.b64encode(f"bblp:{self.code}".encode()).decode()
            return f"Authorization: Basic {token}\r\n"
        fields = parse_keqv_list(parse_http_list(value))
        realm = fields.get("realm", "")
        nonce = fields.get("nonce", "")
        opaque = fields.get("opaque", "")
        qop_list = fields.get("qop", "")
        md5 = lambda text: hashlib.md5(text.encode()).hexdigest()
        ha1 = md5(f"bblp:{realm}:{self.code}")
        ha2 = md5(f"{method}:{target}")
        if "auth" in {item.strip() for item in qop_list.split(",") if item.strip()} or qop_list == "auth":
            self.nc += 1
            cnonce = hashlib.md5(os.urandom(8)).hexdigest()[:16]
            response = md5(f"{ha1}:{nonce}:{self.nc:08x}:{cnonce}:auth:{ha2}")
            extra = f', qop=auth, nc={self.nc:08x}, cnonce="{cnonce}"'
        else:
            response = md5(f"{ha1}:{nonce}:{ha2}")
            extra = ""
        opaque_part = f', opaque="{opaque}"' if opaque else ""
        return (
            f'Authorization: Digest username="bblp", realm="{realm}", '
            f'nonce="{nonce}", uri="{target}", response="{response}"{extra}{opaque_part}\r\n'
        )

    def _read_response(self) -> tuple[dict, str, int]:
        while True:
            first = self.tls.read_exact(1)
            if first == b"$":
                channel = self.tls.read_exact(1)[0]
                length = int.from_bytes(self.tls.read_exact(2), "big")
                packet = self.tls.read_exact(length) if length else b""
                if channel == 0:
                    # RTP before the PLAY reply is processed after handshake.
                    continue
                continue
            header = first + self.tls.read_until(b"\r\n\r\n", 8192)
            return self._parse_headers(header)

    def read_interleaved(self) -> tuple[int, bytes]:
        while True:
            first = self.tls.read_exact(1)
            if first != b"$":
                # Unexpected RTSP text; consume a line to resync.
                self.tls.buf[:0] = first
                self.tls.read_until(b"\r\n", 4096)
                continue
            channel = self.tls.read_exact(1)[0]
            length = int.from_bytes(self.tls.read_exact(2), "big")
            packet = self.tls.read_exact(length) if length else b""
            return channel, packet

    def _parse_headers(self, blob: bytes) -> tuple[dict, str, int]:
        text = blob.decode("utf-8", "replace")
        line, _, rest = text.partition("\r\n")
        parts = line.split()
        if len(parts) < 2 or not parts[1].isdigit():
            raise ValueError("malformed RTSP response")
        status = int(parts[1])
        headers = {}
        body_header, _, _ = rest.partition("\r\n\r\n")
        for raw in body_header.split("\r\n"):
            if ":" in raw:
                key, value = raw.split(":", 1)
                headers[key.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0") or 0)
        body = self.tls.read_exact(length).decode("utf-8", "replace") if length else ""
        return headers, body, status

    def _parse_sdp(self, headers: dict, body: str) -> None:
        host = self.url.split("/")[2]
        base = headers.get("content-base", self.url).rstrip("/")
        if base.startswith("rtsps://") or base.startswith("rtsp://"):
            base = re.sub(r"^rtsp[s]?://[^/]+", f"rtsps://{host}", base)
        else:
            base = self.url
        control = ""
        payload = None
        sprop = ""
        video = False
        for line in body.splitlines():
            if line.startswith("m=video"):
                video = True
                parts = line.split()
                if len(parts) >= 4 and parts[3].isdigit():
                    payload = int(parts[3])
            elif not video:
                continue
            elif line.startswith("m="):
                break
            elif line.startswith("a=control:"):
                control = line.split(":", 1)[1].strip()
            elif line.startswith("a=fmtp:"):
                match = re.search(r"sprop-parameter-sets=([^;\s]+)", line)
                if match:
                    sprop = match.group(1)
                match = re.search(r"a=fmtp:(\d+)", line)
                if match and payload is None:
                    payload = int(match.group(1))
            elif line.startswith("a=rtpmap:"):
                match = re.match(r"a=rtpmap:(\d+)\s+H264", line, re.I)
                if match:
                    payload = int(match.group(1))
        if payload is None:
            raise ValueError("printer SDP has no H264 payload")
        if not control:
            raise ValueError("printer SDP has no video track")
        if control.startswith("rtsp://") or control.startswith("rtsps://"):
            track = control
        else:
            track = base.rstrip("/") + "/" + control.lstrip("/")
        track = re.sub(r"rtsps://([^/:]+)/", r"rtsps://\1:322/", track)
        self.track = track
        self.payload_type = payload
        self.sprop = sprop


class FakeSource(Source):
    """In-memory JPEG generator used by tests. Never touches a printer."""

    def __init__(self, ip: str, access_code: str, model: str, width: int, height: int, quality: int,
                 color=(0, 174, 66), interval: float = 0.05, hold: Optional[threading.Event] = None):
        super().__init__(ip, access_code, model, width, height, quality)
        self.color = color
        self.interval = interval
        self.hold = hold
        self.connects = 0

    def _run(self) -> None:
        self.connects += 1
        self.connected = True
        self.stage = "live"
        try:
            if self.hold:
                self.hold.wait(timeout=5)
            n = 0
            while not self._stop.is_set():
                image = Image.new("RGB", (self.width, self.height), self.color)
                buf = io.BytesIO()
                image.save(buf, format="JPEG", quality=self.quality)
                self.produced += 1
                self.frames.publish(buf.getvalue())
                n += 1
                if self._stop.wait(self.interval):
                    return
        finally:
            self.connected = False
            self._code = ""
            self._ip = ""
            if not self._stop.is_set():
                self.frames.fail("fake source ended")


SourceFactory = Callable[..., Source]


def open_source(ip: str, access_code: str, model: str, width: int, height: int, quality: int) -> Source:
    return Source(ip, access_code, model, width, height, quality)
