#!/usr/bin/env python3
"""Fail if this public snapshot still contains known private strings.

Needles are one-way SHA-256 hashes of UTF-8 secret bytes. Matching uses
token extraction plus fixed-length sliding windows. Hashes avoid storing
plaintext values, but low-entropy inputs remain vulnerable to guessing.
This checks only the known values; use a general scanner for new secrets.
"""
from __future__ import annotations

import sys
sys.dont_write_bytecode = True

import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (UTF-8 byte length, SHA-256 digest). Never add plaintext values here.
HASHES: list[tuple[int, str]] = [
    (13, "4b1555943fdfd70fd18d296c6526cf16bef1f2046168d4e1841d3887b724823e"),
    (9, "e94569dce44f93d536c62e15757367bfd763abf3f885c494e1438040ce799728"),
    (20, "b6c5f05dc5f1d30b9cc799ac5eaf5dd8256bd862c6de54f3c8ac3e72c270fe3c"),
    (28, "abb6b6cb68a2ba5072693b0371a7ba18c3ac211d6f846530b4b1c133cb1611bc"),
    (15, "c6326581c2d74fb13bfd3d30c577e8624a65a1222231ca555ab563e8dde90c32"),
    (8, "1db26a954eaef052538f8a2c643af9201980c8dcd29dc934b89bc520eebbc946"),
    (12, "a01ca6ed51a3a6413200a1c28d7d24dac0fd55183606b364774418ba220af137"),
    (13, "7b6b994d9a5407c9fe1fec411663be13501cdccd125def55591df04b7285ddd6"),
    (48, "ec16b31ad69566d15c3c7945832ae91e64bfc9359e2711b1bd2bca88f55aff69"),
    (48, "9ff8cc59ef2cb60fb657e5e51922638bf29ebb5a6de4421872833c2118359552"),
    (64, "40471420c35800b63e9571857c3442f3938399f5571e95e231be37f1692ba9fb"),
    (20, "463c8ca07d8fc4779dc053d9103c5debd214dc72fb683095fb00ee580253976d"),
    (6, "f3c8d857001dd6215cb5e663191c824c318ec4c1318fdc4971e006e91b9ad98b"),
    (16, "a7e74c7de9ca9ea7548cde3c2f64894a90853a4f036ba7ec9242451c08087615"),
    (16, "e9f6ef422c7c8f8b12618245715e330734127055fa42614fa16cefa13465014d"),
]

IPV4 = re.compile(rb"(?:\d{1,3}\.){3}\d{1,3}")
HEX = re.compile(rb"\b[0-9a-fA-F]{48,64}\b")
TOKEN = re.compile(rb"[A-Za-z0-9_@+./\\:-]{6,80}")
QUOTED = re.compile(rb"[\"']([^\"']{6,80})[\"']")
WINPATH = re.compile(rb"[A-Za-z]:\\[^\n\"']{3,80}")
SKIP_DIR_NAMES = {"__pycache__", ".git"}
SKIP_SUFFIXES = {".pyc", ".pyo"}
SLIDE_LIMIT = 256


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def index_hashes(entries: list[tuple[int, str]] | None = None) -> dict[int, set[str]]:
    by_len: dict[int, set[str]] = {}
    for length, hashed in entries or HASHES:
        by_len.setdefault(length, set()).add(hashed)
    return by_len


def iter_files(root: Path):
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if any(part in SKIP_DIR_NAMES for part in path.parts):
            continue
        if path.suffix.lower() in SKIP_SUFFIXES:
            continue
        yield path


def match_blob(blob: bytes, by_len: dict[int, set[str]]) -> list[int]:
    length = len(blob)
    hashes = by_len.get(length)
    if hashes and digest(blob) in hashes:
        return [length]
    return []


def slide_run(run: bytes, by_len: dict[int, set[str]]) -> list[int]:
    hits = []
    n = len(run)
    hits.extend(match_blob(run, by_len))
    if n > SLIDE_LIMIT:
        return hits
    for length, hashes in by_len.items():
        if n < length or length in hits:
            continue
        for i in range(0, n - length + 1):
            if digest(run[i:i + length]) in hashes:
                hits.append(length)
                break
    return hits


def record(hits: list[str], seen: set, rel: str, lengths: list[int]) -> None:
    for length in lengths:
        key = (rel, length)
        if key in seen:
            continue
        seen.add(key)
        hits.append(f"{rel}: hashed-secret length {length}")


def scan(root: Path | None = None, hashes: list[tuple[int, str]] | None = None) -> list[str]:
    root = (root or ROOT).resolve()
    by_len = index_hashes(hashes)
    hits: list[str] = []
    for path in iter_files(root):
        data = path.read_bytes()
        rel = path.relative_to(root).as_posix()
        seen: set = set()
        exact = []
        exact.extend(IPV4.findall(data))
        exact.extend(HEX.findall(data))
        exact.extend(TOKEN.findall(data))
        for blob in exact:
            record(hits, seen, rel, match_blob(blob, by_len))
        sliding = []
        sliding.extend(QUOTED.findall(data))
        sliding.extend(WINPATH.findall(data))
        for blob in sliding:
            record(hits, seen, rel, slide_run(blob, by_len))
    return hits


def main() -> int:
    hits = scan()
    if hits:
        print("secret-scan FAIL")
        for hit in hits:
            print(" ", hit)
        return 1
    print("secret-scan PASS", len(HASHES), "hashes", ROOT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
