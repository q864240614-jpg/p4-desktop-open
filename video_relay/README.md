# P4 printer video relay

LAN host service: when the ESP32-P4 asks, pull H2D RTSPS (:322) or A1 TLS JPEG (:6000), scale to 800×480 JPEG, and send it to the board. The host does **not** store printer IP, access code, or frames on disk.

Protocol notes: [Doridian/OpenBambuAPI video.md](https://github.com/Doridian/OpenBambuAPI/blob/main/video.md). Board-side framing: [P4_DEV.md](P4_DEV.md).

## Why a relay

H2D High Profile 1680×1080 software decode on the P4 fights the UI. Opening H2D liveview **and** LAN MQTT at once also drops the printer’s MQTT session. This snapshot therefore:

- **H2D:** board talks only to this relay.
- **A1 mini:** board may still connect directly (hardware JPEG).

Turn on LAN Only Liveview / Local RTSP on the printer.

## Run locally

```sh
python -m pip install -r requirements.txt
python tests.py
python server.py
```

Environment (all optional):

| Variable | Default |
| --- | --- |
| `VIDEO_RELAY_BIND` | `0.0.0.0` |
| `VIDEO_RELAY_PORT` | `2344` |
| `VIDEO_RELAY_ALLOW` | loopback + RFC1918 |
| `VIDEO_RELAY_TOKEN` | empty (no extra auth) |
| `VIDEO_RELAY_IDLE` | `2` seconds after last viewer |

Tighten `VIDEO_RELAY_ALLOW` to your LAN. Live printer credentials for `live_prod.py` come from `VIDEO_RELAY_LIVE_IP` / `VIDEO_RELAY_LIVE_CODE` in the environment, never from a file.

## Deploy

Copy this directory to the host (for example `$HOME/p4-video-relay`) and run `bash install.sh`. Override the directory with `VIDEO_RELAY_ROOT`.

From Windows, `deploy.ps1` needs:

```text
P4_DEPLOY_HOST   relay hostname or IP
P4_DEPLOY_USER   SSH user
P4_DEPLOY_KEY    path to a private key
P4_DEPLOY_DIR    optional remote directory
```

systemd unit: `p4-video-relay.service` (`WorkingDirectory=%h/p4-video-relay`).

## Behaviour

- `POST /v1/session` stores parameters in RAM only.
- The first JPEG GET or TCP JSON-line viewer opens the printer.
- About two seconds after the last viewer, the upstream is torn down and the access code is dropped.
- Logs must not contain printer IP or access code. `LimitCORE=0`.
- Printer IP must be RFC1918.
