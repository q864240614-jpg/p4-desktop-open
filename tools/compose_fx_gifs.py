#!/usr/bin/env python3
"""Turn LVGL card RGB dumps into looping GIFs of each printer effect."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = True
SRC = Path(__file__).resolve().parents[1] / "assets" / "preview-review"
OUT = SRC / "fx"
W, H, STEP_MS = 272, 316, 80
LABELS = [
    ("fx-water", "PRINTING"),
    ("fx-prep", "PREP"),
    ("fx-heat", "HEAT"),
    ("fx-level", "LEVEL"),
    ("fx-feed", "FILAMENT SWAP"),
    ("fx-unload", "UNLOADING"),
    ("fx-load", "LOADING"),
    ("fx-scan", "BED SCAN"),
    ("fx-clean", "NOZZLE WIPE"),
    ("fx-home", "HOMING"),
    ("fx-flow", "FLOW CAL"),
    ("fx-calibrate", "CALIBRATE"),
    ("fx-cool", "COOLING"),
    ("fx-tool", "HOTEND TEST"),
    ("fx-center", "CENTERING"),
    ("fx-camera", "CAMERA CAL"),
    ("fx-lidar", "LIDAR CAL"),
    ("fx-motor", "MOTOR CAL"),
    ("fx-motion", "MOTION CAL"),
    ("fx-offset", "OFFSET CAL"),
    ("fx-vibration", "VIBRATION CAL"),
    ("fx-vent", "AIR PURIFY"),
    ("fx-pause", "PAUSE"),
    ("fx-finish", "FINISH"),
    ("fx-error", "FAILED"),
]


def frames_of(name):
    data = (SRC / f"fx-anim-{name}.rgb").read_bytes()
    n = len(data) // (W * H * 3)
    out = []
    for i in range(n):
        raw = data[i * W * H * 3:(i + 1) * W * H * 3]
        out.append(Image.frombytes("RGB", (W, H), raw).quantize(colors=64, method=Image.Quantize.MEDIANCUT))
    return out


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    gifs = []
    for name, title in LABELS:
        frames = frames_of(name)
        path = OUT / f"{name}.gif"
        frames[0].save(path, save_all=True, append_images=frames[1:], duration=STEP_MS, loop=0, optimize=True)
        gifs.append((path.name, title, frames))
        print(path, len(frames))

    cols = 6
    rows = (len(gifs) + cols - 1) // cols
    tw, th = W // 2, H // 2
    mosaic = []
    count = min(len(gifs[0][2]), 40)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 12)
    except OSError:
        font = ImageFont.load_default()
    for t in range(count):
        sheet = Image.new("RGB", (cols * (tw + 8) + 8, rows * (th + 22) + 8), (3, 3, 3))
        draw = ImageDraw.Draw(sheet)
        for i, (filename, title, frames) in enumerate(gifs):
            x = 8 + (i % cols) * (tw + 8)
            y = 8 + (i // cols) * (th + 22)
            sheet.paste(frames[t].resize((tw, th), Image.Resampling.BILINEAR), (x, y))
            draw.text((x, y + th + 4), title, fill=(200, 204, 198), font=font)
        mosaic.append(sheet.quantize(colors=128, method=Image.Quantize.MEDIANCUT))
    mosaic_path = SRC / "all-fx.gif"
    mosaic[0].save(mosaic_path, save_all=True, append_images=mosaic[1:], duration=STEP_MS, loop=0, optimize=True)
    print(mosaic_path, mosaic[0].size)

    cards = "\n".join(
        f'<figure><img src="fx/{name}.gif" width="272" height="316" alt="{title}"><figcaption>{title}</figcaption></figure>'
        for name, title in LABELS
    )
    (SRC / "fx-live.html").write_text(
        f"""<!doctype html>
<html lang="zh-CN"><meta charset="utf-8">
<title>P4 · 动态特效</title>
<style>
body{{margin:0;background:#030303;color:#f5f5f2;font:14px/1.4 ui-sans-serif,system-ui}}
header{{padding:24px 32px 8px}} h1{{font-weight:550;margin:8px 0}}
p{{color:#949493;max-width:820px}}
main{{display:grid;grid-template-columns:repeat(auto-fill,minmax(272px,1fr));gap:22px;padding:8px 32px 48px}}
figure{{margin:0}} figcaption{{margin-top:8px;color:#8aa;letter-spacing:.06em;font:12px ui-monospace,Menlo,Consolas,monospace}}
img{{display:block;border-radius:16px;background:#121315}}
</style>
<header><h1>状态窗口动态特效</h1>
<p>实际 LVGL RGB565 左卡循环，80 ms / 帧，约 4 秒。粒子来自 portable_ui_stage_motion.inc，不是浏览器重写。</p></header>
<main>
{cards}
</main>
""",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
