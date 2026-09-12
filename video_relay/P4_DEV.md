# ESP32-P4 client notes

The relay speaks HTTP and a one-line JSON TCP protocol. Printer IP and LAN access code are sent **per session** and must never be logged or written to flash by the host.

Default listen port is `2344`. Set `VIDEO_RELAY_IP` in firmware `main/video_relay.h` to the host running this service. Allow-list the board in `VIDEO_RELAY_ALLOW`.

## Recommended framing (same 16-byte header as A1 JPEG)

1. `socket()` to `VIDEO_RELAY_IP:2344` (plain TCP, LAN only).
2. Send one UTF-8 JSON line ending in `\n`:

```json
{"ip":"192.168.1.20","access_code":"YOURCODE","model":"H2D","width":800,"height":480,"fps":8,"quality":60}
```

3. Read frames:

| Offset | Length | Value |
| --- | --- | --- |
| 0 | 4 | JPEG size, little-endian |
| 4 | 4 | 0 |
| 8 | 4 | 1 |
| 12 | 4 | 0 |
| 16 | N | JPEG (`FF D8` … `FF D9`) |

4. Hardware-decode JPEG at 800×480. Close the socket on EXIT / sleep / fullscreen alert; the host then drops the printer connection.

Errors: a line starting with `ERR ` (first byte `E`). The body must not contain the access code.

HTTP alternative: `POST /v1/session` then `GET /v1/session/{id}/jpeg`. Browser preview is `GET /` on the same port; closing the page issues DELETE.

Health: `curl -s http://127.0.0.1:2344/health`.
