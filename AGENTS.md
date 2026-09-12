# Agent adapter notes

This snapshot is a **reference implementation** for one ESP32-P4 + ST7701 + GT911 + ESP32-C6 board. Another agent should **reuse modules**, not treat the tree as a drop-in for a different SoC, LCD, or radio.

Constraints that are easy to break:

- ESP-IDF **5.4.x only** (`main/idf_component.yml`: `idf: ">=5.4,<6.0"`). IDF 5.3 and IDF 6 are out of scope.
- LVGL 8.3.11 with `esp_lvgl_port` **2.6.0**. LVGL 9 APIs are not used. `CMakeLists.txt` exports `LVGL_VERSION=8.3.11` for the port.
- Logical UI size is **800×480**. The panel in this BSP is physically **480×800**; rotation is in `main/p4_display.c` (PPA + MIPI DPI copy), not in `portable_ui`.

## Module map

| Path | What it is | Reuse on another board? |
| --- | --- | --- |
| `components/portable_ui/` | LVGL widgets, fonts, icons, printer/Codex/Todo/clock/events | **Yes** if you can present an 800×480 `lv_disp_t` and call the C API on the LVGL task |
| `components/portable_ui/bambu_state.*` | Bambu MQTT JSON → `BambuState` | **Yes** (host or firmware) |
| `components/portable_ui/todo_state.h` | Todo snapshot struct | **Yes** |
| `todo_service/` | HTTP + SQLite + optional Feishu | **Yes** (any LAN host) |
| `video_relay/` | H2D/A1 → 800×480 JPEG | **Yes** (any LAN host; P4 client is optional) |
| `main/bambu_config.*` + `printer_settings.html` | NVS + PIN-gated settings HTTP | Mostly yes; bind to whatever netif you have |
| `main/bambu_app.c` | Dual MQTT clients, pushall, UI poll | Yes if you have TLS MQTT and the same topics |
| `main/todo_app.c` / `main/codex_app.c` | HTTP clients into the UI | Yes; only the credential headers change |
| `main/bambu_video.c` + `bambu_video_rtsp.inc` | Direct A1 JPEG / optional H2D path + relay client | Partial: JPEG + sockets travel; P4 JPEG decoder, PPA, and OpenH264 wrapper do not |
| `components/openh264/` | PSRAM-backed IDR-only decoder | P4-oriented; other chips need a different decoder |
| `main/p4_display.c` | PPA 90° rotate + panel `draw_bitmap` | **No** — rewrite for your LCD |
| `main/main.c` | `bsp_display_new_with_handles`, GT911, brightness, timers | **No** — rewrite bring-up |
| `vendor/espressif__esp32_p4_function_ev_board/` | Espressif P4 function EV BSP | **No** unless you are on that BSP |
| `vendor/espressif__esp_lcd_st7701/` | ST7701 MIPI/RGB driver | Only if your panel is ST7701 |
| `sdkconfig.defaults` | P4 flash/PSRAM, C6 SDIO pins, LVGL 16-bit | Copy only matching Kconfig symbols |
| `patches/esp-hosted-c6-start.patch` | Ignore repeated `WIFI_EVENT_STA_START` from this C6 firmware | Apply after IDF fetches `espressif__esp_hosted` |

Public C API for the UI is `components/portable_ui/portable_ui.h`. Create under an LVGL lock, then pump `PortableUI_Process()` next to `lv_timer_handler()` (this firmware uses `esp_lvgl_port`’s task and a 25 ms timer in `main.c`).

## Same board, different agent

If the hardware matches (P4 + this ST7701 + GT911 + C6 SDIO pins above):

1. Copy example headers to `wifi_credentials.h`, `todo_credentials.h`, `codex_credentials.h` and edit `video_relay.h`.
2. Export ESP-IDF 5.4, run `idf.py build` from this folder (BSP path is already `../vendor/...`).
3. After `managed_components/espressif__esp_hosted` appears, apply `patches/esp-hosted-c6-start.patch`.
4. Flash without erasing NVS unless you want to drop printer settings.
5. Do not reintroduce secrets into git; `.gitignore` already lists the live files.

Do not point `idf_component.yml` back at a sibling `common_components` tree. That path was private-layout specific.

## Different board

Keep these units and **replace the glue**:

1. **Display.** Implement an `lv_disp_t` that is 800×480 RGB565 (or adapt `portable_ui` constants). Do not ship `p4_display.c` to a panel that is already landscape or uses RGB/8080/SPI instead of MIPI.
2. **Touch.** Feed LVGL an input device. This tree maps GT911 through `esp_lvgl_port` using the display’s rotation. If you rotate in hardware, do not also rotate in software.
3. **Wi-Fi.** This firmware uses `esp_wifi_remote` + `esp_hosted` onto a C6. A P4 with native Wi-Fi, an S3, or a C6-as-main-SoC should drop those components and call `esp_wifi` directly. SDIO pin numbers in `sdkconfig.defaults` are this board only.
4. **Video.** A1 mini path: TLS :6000 JPEG, hardware decode on P4. H2D path: prefer `video_relay` (TCP JSON line + 16-byte JPEG header) so the MCU does not run High Profile OpenH264. On a faster host SoC you may skip the relay; still keep printer credentials in RAM only.
5. **Credentials.** Always examples + gitignore. Never bake a site SSID, bearer token, or Feishu app id into source you will publish.

Suggested order for a port: get LVGL flushing a color on the new panel → attach `PortableUI_Create` → Wi-Fi + SNTP → MQTT/Todo HTTP → video last.

## What not to copy

- `build-win/`, `managed_components/`, `*.bin` / `*.elf` — generated.
- `assets/preview-review/` — not shipped; it is a dump directory for host UI review.
- Historical session markdown from the private tree (COM-port logs, LAN IPs, local IDF paths). This `README.md` and this file replace those.
- Live `deploy.env` and `*_credentials.h`.

## Host tests (no board)

```sh
python todo_service/test_feishu.py
python video_relay/tests.py
python tools/check_open_package.py
python tools/secret_scan.py
```

`tools/verify_bambu_config.py` compiles `main/bambu_config.c` against IDF’s cJSON + mbedTLS base64; needs `IDF_PATH` and `cc`. `tools/clock_dial_check.py` reads LVGL `sin0_90_table` from `managed_components/` after the first firmware configure.
