"""Export the host renderer's main screens as PNGs and a README overview."""
from argparse import ArgumentParser
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SCREENS = (
    ("printer-h2d", "H2D printer"),
    ("printer-a1mini", "A1 mini printer"),
    ("standby-printers", "Standby clock"),
    ("codex-quota", "Codex quota"),
    ("todo", "Todo list"),
    ("printer-finished", "Print complete"),
)


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Directory containing smoke.exe PPM output")
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[1] / "assets/preview")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    overview = Image.new("RGB", (1648, 1632), "#151515")
    draw = ImageDraw.Draw(overview)
    font = ImageFont.load_default(size=24)
    for index, (name, label) in enumerate(SCREENS):
        with Image.open(args.source / f"{name}.ppm") as screen:
            assert screen.size == (800, 480), f"Unexpected screen size: {name}"
            screen.save(args.output / f"{name}.png")
            x, y = 16 + (index % 2) * 816, 16 + (index // 2) * 544
            draw.text((x, y), label, font=font, fill="#eeeeee")
            overview.paste(screen, (x, y + 36))
    overview.save(args.output / "overview.png")
    print(f"Exported {len(SCREENS)} screens and overview to {args.output}")


if __name__ == "__main__":
    main()
