"""Rasterize LED geometry at P4-native sizes; export matching C alpha masks/PNGs.
Requires Pillow. Run from any directory: python3 tools/export_icons.py.
"""
from pathlib import Path
import json
import math
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
DATA = json.loads((ROOT / 'assets/icons.json').read_text())
DEST = ROOT / 'assets/png'
DEST.mkdir(exist_ok=True)
ICONS = []


def led(mask, size, x, y, core):
    """Analytic native-pixel core and soft halo, no resizing source bitmaps."""
    for py in range(max(0, y - 2), min(size, y + core + 2)):
        for px in range(max(0, x - 2), min(size, x + core + 2)):
            dx = max(x - px, 0, px - (x + core - 1))
            dy = max(y - py, 0, py - (y + core - 1))
            alpha = 255 if dx == dy == 0 else round(40 * math.exp(-(dx*dx + dy*dy) / 3))
            k = py * size + px
            mask[k] = max(mask[k], alpha)


def pattern(rows, size, symbol='#'):
    mask = bytearray(size * size)
    pitch, core = (6, 4) if size == 64 else (4, 2)
    for row, text in enumerate(rows):
        for col, char in enumerate(text):
            if char == symbol:
                led(mask, size, 4 + col * pitch, 4 + row * pitch, core)
    return mask


def save(name, size, mask, color):
    im = Image.new('RGBA', (size, size), '#' + color)
    im.putalpha(Image.frombytes('L', im.size, bytes(mask)))
    im.save(DEST / (name + '.png'))
    return im


for name, rows in DATA['controls'].items():
    mask = pattern(rows, 64)
    accent = pattern(rows, 64, 'R') if name == 'speaker' else None
    ICONS.append((name, 64, mask, accent))
    for suffix, color in (('white', 'F5F5F2'), ('red', 'E64A43')):
        im = save(name + '_' + suffix, 64, mask, color)
        if accent:
            im.alpha_composite(save(name + '_accent', 64, accent, 'E64A43'))
            im.save(DEST / (name + '_' + suffix + '.png'))
mask = [0.] * (36 * 36)
for n in range(4):
    for y in range(26):
        for x in range(26):
            dx, dy = (x - 12.5) / 2, (y - 12.5) / 2
            alpha = (255 if abs(dx) <= 1 and abs(dy) <= 1 else int(52 * math.exp(-(dx*dx + dy*dy) / 10))) / 255
            k = (y + n // 2 * 10) * 36 + x + n % 2 * 10
            mask[k] = 1 - (1-mask[k]) * (1-alpha)
mask = bytearray(round(v * 255) for v in mask)
ICONS.append(('status_cluster', 36, mask, None))
for name, color in DATA['status_colors'].items():
    save('status_cluster_' + name, 36, mask, color)
header = ['#pragma once', '#include "lvgl.h"', '#ifdef __cplusplus', 'extern "C" {', '#endif', 'typedef enum {']
header += ['    PORTABLE_ICON_' + name.upper() + ',' for name, *_ in ICONS]
header += ['    PORTABLE_ICON_COUNT', '} PortableIcon;', 'const lv_img_dsc_t *PortableIcons_Get(PortableIcon icon);', 'const lv_img_dsc_t *PortableIcons_GetAccent(PortableIcon icon);', '#ifdef __cplusplus', '}', '#endif']
(ROOT / 'components/portable_ui/portable_icons.h').write_text('\n'.join(header) + '\n')
c = ['#include "portable_icons.h"']
descs = []

def array(name, mask):
    c.append('static const uint8_t ' + name + '[] = {\n' + '\n'.join('    '+','.join(map(str, mask[i:i+32]))+',' for i in range(0, len(mask), 32)) + '\n};')

for name, size, mask, accent in ICONS:
    assert len(mask) == size * size
    array('mask_' + name, mask)
    descs.append('    {.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w=%d,.h=%d},.data_size=%d,.data=mask_%s},' % (size, size, len(mask), name))
    if accent:
        array('accent_' + name, accent)
        c.append('static const lv_img_dsc_t accent_%s_dsc = {.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w=%d,.h=%d},.data_size=%d,.data=accent_%s};' % (name, size, size, len(accent), name))
c += ['static const lv_img_dsc_t icons[] = {\n' + '\n'.join(descs) + '\n};',
      'const lv_img_dsc_t *PortableIcons_Get(PortableIcon icon) { return (unsigned)icon < PORTABLE_ICON_COUNT ? &icons[icon] : NULL; }',
      'const lv_img_dsc_t *PortableIcons_GetAccent(PortableIcon icon) { return icon == PORTABLE_ICON_SPEAKER ? &accent_speaker_dsc : NULL; }']
(ROOT / 'components/portable_ui/portable_icons.c').write_text('\n'.join(c)+'\n')
files = sorted(DEST.glob('*.png'))
sheet = Image.new('RGB', (900, math.ceil(len(files)/5)*112), '#030303')
draw = ImageDraw.Draw(sheet)
for i, file in enumerate(files):
    im = Image.open(file)
    assert im.mode == 'RGBA'
    x, y = i % 5 * 180 + 8, i // 5 * 112 + 6
    sheet.paste(im, (x, y), im)
    draw.text((x, y+72), file.stem, fill='#949493')
sheet.save(ROOT / 'assets/icons-sheet.png')
(ROOT / 'assets/manifest.json').write_text(json.dumps([dict(id=n, width=s, height=s, format='LV_IMG_CF_ALPHA_8BIT', accent=a is not None) for n,s,m,a in ICONS], indent=2)+'\n')
print(f'Exported {len(ICONS)} native C masks and {len(files)} PNG variants')
