#!/usr/bin/env python3
"""Contact sheet of LVGL printer effect / state snapshots."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SRC = ROOT / "assets" / "preview-review"
OUT = SRC / "all-states.png"
CARD = (24, 148, 24 + 272, 148 + 316)
LABELS = [
    ("fx-offline", "OFFLINE"),
    ("fx-idle", "IDLE"),
    ("fx-prep", "PREP"),
    ("fx-water", "PRINTING"),
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
    ("fx-pause", "PAUSE"),
    ("fx-finish", "FINISH"),
    ("fx-error", "FAILED"),
    ("fx-stopped", "STOPPED"),
    ("fx-disconnected", "DISCONNECTED"),
]


def main():
    cols = 7
    rows = (len(LABELS) + cols - 1) // cols
    cw, ch = 272, 316
    pad, caption = 12, 28
    sheet = Image.new("RGB", (cols * (cw + pad) + pad, rows * (ch + caption + pad) + pad), (3, 3, 3))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 14)
    except OSError:
        font = ImageFont.load_default()
    for i, (name, title) in enumerate(LABELS):
        path = SRC / f"{name}.ppm"
        if not path.exists():
            raise SystemExit(f"missing {path}")
        crop = Image.open(path).crop(CARD)
        x = pad + (i % cols) * (cw + pad)
        y = pad + (i // cols) * (ch + caption + pad)
        sheet.paste(crop, (x, y))
        draw.text((x, y + ch + 6), title, fill=(245, 245, 242), font=font)
    SRC.mkdir(parents=True, exist_ok=True)
    sheet.save(OUT)
    print(OUT, sheet.size)


if __name__ == "__main__":
    main()
