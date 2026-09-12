# ESP32-P4 desktop UI

[中文说明](README_CN.md)

Landscape 800×480 LVGL dashboard for an ESP32-P4 board: Bambu Lab H2D / A1 mini status, a standby clock, a Codex quota page, and a Todo list. Optional LAN helpers live next to the firmware: a Python Todo/Feishu service and a printer-camera JPEG relay.

**This repository is intended exclusively for Agent-assisted development and hardware adaptation.** Give your coding Agent the repository, board specifications, wiring, and desired features. The Agent should read [AGENTS.md](AGENTS.md), assess compatibility, implement the changes, and validate the result against your hardware.

This is a source reference for development; it does not provide a ready-to-flash product or a manual build-and-flash tutorial.

## Develop with an Agent

1. Give your Agent the board/SoC model, display and touch specifications, wiring, and host environment.
2. Describe the features you need and any UI or integration changes.
3. Ask the Agent to read `AGENTS.md` and both READMEs, check hardware differences, and complete adaptation and validation in your local environment.

Example request:

> Read AGENTS.md and inspect this project. Adapt it to my board using the hardware specifications and wiring I provide. Implement the requested features, verify the UI and services, and report what has and has not been validated on hardware. Keep credentials local.

## Interface preview

![Main interface screens](assets/preview/overview.png)

Actual LVGL host renders at 800×480 using synthetic printer, quota, and task data. These are interface illustrations, not device photographs. Individual screens and reproduction steps: [preview gallery](assets/preview/README.md).

## Supported hardware

- **SoC:** ESP32-P4, 16 MB flash, PSRAM (200 MHz). The UI caches clock digits in PSRAM; plan on 16 MB PSRAM.
- **Wi-Fi:** onboard ESP32-C6 through ESP-Hosted over SDIO slot 1. Default pins in `sdkconfig.defaults`: CLK 18, CMD 19, D0 14, D1 15, D2 16, D3 17, C6 reset 54.
- **Panel:** ST7701 MIPI-DSI, native 480×800, RGB565, two DSI lanes. LVGL layout is 800×480; `p4_display.c` rotates with PPA. The BSP option `CONFIG_BSP_LCD_TYPE_1024_600` is the **name of the 480×800 ST7701 branch** in this BSP — do not change it to a 1024×600 panel unless you rewrite the display path.
- **Touch:** GT911, I2C SDA GPIO7, SCL GPIO8. Backlight GPIO23, LCD reset GPIO5.
- **Toolchain:** ESP-IDF **5.4.x** (reference version: 5.4.2; the manifest permits `>=5.4,<6.0`) and **LVGL 8.3.11**. Do not use IDF 6 or LVGL 9.

The Agent must assess hardware compatibility even for a matching board. For other boards, it can reuse `components/portable_ui`, `todo_service`, and `video_relay` while adapting the display, touch, and Wi-Fi interfaces.

## Project structure

| Piece | Role |
| --- | --- |
| `components/portable_ui/` | 800×480 LVGL UI (printer pages, clock, Codex, Todo, events) |
| `todo_service/` | LAN Todo HTTP/SQLite + optional Feishu sync |
| `video_relay/` | LAN JPEG relay so the P4 does not software-decode H2D High Profile |
| `main/bambu_config.c` | NVS printer settings + local config web page |
| `assets/` | fonts, icons, design previews (`assets/preview/`) |

Board-specific integration includes `main/p4_display.c`, `main/main.c` BSP bring-up, `vendor/` BSP/LCD, C6 SDIO pins, and most of `sdkconfig.defaults`.

## Local configuration boundaries

The Agent should use `main/wifi_credentials.example.h`, `main/todo_credentials.example.h`, `main/codex_credentials.example.h`, and `todo_service/deploy.env.example` as configuration templates. Keep real values in local ignored files. Todo device authentication must match the service's `TODO_DEVICE_TOKEN`; the relay address is defined in `main/video_relay.h`.

The three live credential headers and `todo_service/deploy.env` are gitignored. `main/video_relay.h` is tracked: keep its published address as a placeholder. Wi-Fi and service credentials are compiled into locally built firmware, so do not publish configured binaries.

Printer LAN IP / serial / access code are **not** compiled in. After Wi-Fi is up, the screen shows the board IP and a 6-digit PIN. Open `http://<board-ip>/` as `admin` / that PIN and save H2D and A1 mini settings. The PIN changes every reboot. MQTT is LAN port 8883, username `bblp`. This snapshot does not send pause/cancel/start commands.

## Host services

**Todo** (`todo_service/`): Python standard library + SQLite, with optional Feishu sync. Have the Agent configure it for your host environment using [the service reference](todo_service/README.md). The server reads process environment variables and does not load `deploy.env` itself.

**Video relay** (`video_relay/`): transcodes H2D RTSPS (and optionally A1 JPEG) to 800×480 JPEG for the board. The reference setup uses a relay for H2D video and supports direct JPEG for A1 mini. MQTT/liveview coexistence depends on printer firmware and LAN mode; verify both on your printer. Details: [video relay](video_relay/README.md) and [protocol](video_relay/P4_DEV.md).

## H2D video: relay and direct connection

H2D video has a relay path and a retained direct-decoding implementation:

- **Relay (the current firmware path):** a LAN host receives RTSPS/H.264, decodes it to 800×480 JPEG, and sends frames to the ESP32-P4. Run `video_relay/` and configure its address in `main/video_relay.h`.
- **Direct (requires firmware integration):** `main/bambu_video_rtsp.inc` and `components/openh264/` retain the implementation for receiving and decoding RTSPS/H.264 on the ESP32-P4. It decodes IDR keyframes only, not continuous P/B frames; refresh rate depends on keyframe intervals and on-board decoding performance.

The H2D branch in `main/bambu_video.c` currently calls `play_relay()` unconditionally. There is no web or screen mode selector, and no automatic direct fallback. Using the direct implementation requires code integration, a rebuild, and validation; changing the relay address alone does not enable it. Both paths handle live streams, not MP4 file playback. A1 mini direct video uses a JPEG image stream.

## Validation reference for the Agent

```sh
python video_relay/tests.py
python todo_service/test_feishu.py
python tools/check_open_package.py
python tools/secret_scan.py
```

`video_relay/tests.py` needs Pillow (and ffmpeg via `imageio-ffmpeg` for some paths). `tools/clock_dial_check.py` needs LVGL’s `lv_math.c` from a completed `idf.py` fetch. `tools/verify_bambu_config.py` needs `IDF_PATH` and a host C compiler.

## License

First-party code is MIT (`LICENSE`). Third-party notices: `NOTICE`. Cisco OpenH264 terms also sit in `components/openh264/upstream/LICENSE` and `firmware/OpenH264-LICENSE.txt`.
