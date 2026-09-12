#!/bin/bash
set -euo pipefail
ROOT="${VIDEO_RELAY_ROOT:-$HOME/p4-video-relay}"
mkdir -p "$ROOT"
cd "$ROOT"
python3 -m venv --system-site-packages "$ROOT/venv"
"$ROOT/venv/bin/pip" install -q -r "$ROOT/requirements.txt"
"$ROOT/venv/bin/python" -c "import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())"
mkdir -p "$HOME/.config/systemd/user"
cp "$ROOT/p4-video-relay.service" "$HOME/.config/systemd/user/p4-video-relay.service"
systemctl --user daemon-reload
systemctl --user enable p4-video-relay
systemctl --user restart p4-video-relay
systemctl --user is-active p4-video-relay
for i in 1 2 3 4 5 6 7 8 9 10; do
  if curl -fsS "http://127.0.0.1:2344/health"; then
    echo
    exit 0
  fi
  sleep 0.2
done
echo "health check failed" >&2
systemctl --user status p4-video-relay --no-pager -l >&2 || true
exit 1
