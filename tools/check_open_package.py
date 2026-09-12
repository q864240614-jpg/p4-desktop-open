#!/usr/bin/env python3
"""Prove the public snapshot layout, examples, and docs match what we ship."""
from __future__ import annotations

import sys
sys.dont_write_bytecode = True

import hashlib
import os
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import secret_scan  # noqa: E402


REQUIRED_PATHS = (
    "main/main.c",
    "main/p4_display.c",
    "main/bambu_app.c",
    "main/idf_component.yml",
    "main/wifi_credentials.example.h",
    "main/todo_credentials.example.h",
    "main/codex_credentials.example.h",
    "components/portable_ui/portable_ui.c",
    "components/portable_ui/portable_ui.h",
    "components/openh264/p4_h264.cpp",
    "vendor/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c",
    "vendor/espressif__esp_lcd_st7701/esp_lcd_st7701.c",
    "todo_service/server.py",
    "todo_service/feishu.py",
    "todo_service/test_feishu.py",
    "todo_service/deploy.env.example",
    "video_relay/server.py",
    "video_relay/tests.py",
    "tools/build.ps1",
    "tools/flash.ps1",
    "CMakeLists.txt",
    "partitions.csv",
    "sdkconfig.defaults",
    "patches/esp-hosted-c6-start.patch",
    "LICENSE",
    "NOTICE",
    "README.md",
    "README_CN.md",
    "AGENTS.md",
    ".gitignore",
)

ABSENT_PATHS = (
    "main/wifi_credentials.h",
    "main/todo_credentials.h",
    "main/codex_credentials.h",
    "todo_service/deploy.env",
    "build-win",
    "managed_components",
    "assets/preview-review",
    "firmware/p4-bambu-800x480-merged.bin",
    "firmware/p4-ui-800x480-merged.bin",
)

ABSENT_SUFFIXES = {".elf"}


class OpenPackage(unittest.TestCase):
    def test_secret_scan_clean(self):
        hits = secret_scan.scan(ROOT)
        self.assertEqual(hits, [], msg="\n".join(hits))

    def test_scan_detects_synthetic_window(self):
        payload = b"scan-probe-window-9f3c2a1b"
        hashed = hashlib.sha256(payload).hexdigest()
        extra = list(secret_scan.HASHES) + [(len(payload), hashed)]
        probe = ROOT / ".scan-probe.txt"
        probe.write_bytes(b"head " + payload + b" tail")
        try:
            hits = secret_scan.scan(ROOT, hashes=extra)
            self.assertTrue(any(".scan-probe.txt" in row for row in hits), hits)
        finally:
            probe.unlink(missing_ok=True)

    def test_scan_flags_private_credentials_if_present(self):
        private_main = ROOT.parent / "lvgl_demo_v8_relay" / "main"
        if not (private_main / "wifi_credentials.h").is_file():
            self.skipTest("private working copy is not adjacent")
        hits = secret_scan.scan(private_main)
        self.assertTrue(hits, "scanner must flag live credential headers in the private tree")

    def test_hashes_cover_private_tree_if_present(self):
        private = ROOT.parent / "lvgl_demo_v8_relay"
        wifi = private / "main" / "wifi_credentials.h"
        if not wifi.is_file():
            self.skipTest("private working copy is not adjacent")
        known = {item[1] for item in secret_scan.HASHES}
        files = [
            wifi,
            private / "main" / "todo_credentials.h",
            private / "main" / "codex_credentials.h",
            private / "todo_service" / "deploy.env",
        ]
        values = []
        for path in files:
            text = path.read_text(encoding="utf-8")
            for quoted in re.findall(r'"([^"]+)"', text):
                if quoted.startswith("http://") or quoted.startswith("Bearer "):
                    continue
                values.append(quoted)
            values.extend(re.findall(r"Bearer ([A-Za-z0-9]+)", text))
            values.extend(re.findall(r"(?m)^[A-Z_]+=(\S+)$", text))
        missing = 0
        for value in values:
            if hashlib.sha256(value.encode()).hexdigest() not in known:
                missing += 1
        self.assertEqual(missing, 0)

    def test_scanner_is_one_way(self):
        source = (ROOT / "tools" / "secret_scan.py").read_text(encoding="utf-8")
        self.assertNotIn("base64", source)
        self.assertIn("sha256", source)
        self.assertNotIn("b64decode", source)

    def test_required_sources_present(self):
        missing = [rel for rel in REQUIRED_PATHS if not (ROOT / rel).exists()]
        self.assertEqual(missing, [])

    def test_no_pycache(self):
        caches = [p.relative_to(ROOT).as_posix() for p in ROOT.rglob("__pycache__")]
        pycs = [p.relative_to(ROOT).as_posix() for p in ROOT.rglob("*.pyc")]
        self.assertEqual(caches + pycs, [])

    def test_scanners_disable_bytecode(self):
        for rel in ("tools/secret_scan.py", "tools/check_open_package.py"):
            text = (ROOT / rel).read_text(encoding="utf-8")
            self.assertIn("sys.dont_write_bytecode = True", text)
            self.assertLess(text.find("sys.dont_write_bytecode = True"), text.find("import hashlib"))

    def test_private_and_build_artifacts_absent(self):
        present = [rel for rel in ABSENT_PATHS if (ROOT / rel).exists()]
        self.assertEqual(present, [])
        bins = [p.relative_to(ROOT).as_posix() for p in ROOT.rglob("*.bin") if "upstream" not in p.parts]
        self.assertEqual(bins, [])
        elves = [p.relative_to(ROOT).as_posix() for p in ROOT.rglob("*") if p.suffix in ABSENT_SUFFIXES]
        self.assertEqual(elves, [])

    def test_example_credentials_are_placeholders(self):
        wifi = (ROOT / "main/wifi_credentials.example.h").read_text(encoding="utf-8")
        todo = (ROOT / "main/todo_credentials.example.h").read_text(encoding="utf-8")
        codex = (ROOT / "main/codex_credentials.example.h").read_text(encoding="utf-8")
        env = (ROOT / "todo_service/deploy.env.example").read_text(encoding="utf-8")
        self.assertIn("your-ssid", wifi)
        self.assertIn("replace-with-device-token", todo)
        self.assertIn("replace-with-status-token", codex)
        self.assertIn("replace-with-web-password", env)
        self.assertIn("cli_your_app_id", env)
        yml = (ROOT / "main/idf_component.yml").read_text(encoding="utf-8")
        self.assertIn("../vendor/espressif__esp32_p4_function_ev_board", yml)
        self.assertNotIn("common_components", yml)

    def test_human_doc_covers_hardware_build_and_local_config(self):
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertGreater(len(text.strip()), 400)
        for needle in (
            "ST7701",
            "GT911",
            "idf.py",
            "wifi_credentials.example.h",
            "todo_credentials.example.h",
            "deploy.env.example",
            "ESP-IDF 5.4",
        ):
            self.assertIn(needle, text)

    def test_ai_doc_names_reuse_vs_board_glue_and_versions(self):
        text = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        self.assertGreater(len(text.strip()), 400)
        for needle in (
            "portable_ui",
            "p4_display",
            "ESP-IDF 5.4",
            "LVGL 8.3.11",
            "C6",
            "todo_service",
            "video_relay",
        ):
            self.assertIn(needle, text)


if __name__ == "__main__":
    unittest.main()
