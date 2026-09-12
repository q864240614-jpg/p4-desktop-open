"""Generate UI font assets. Run only after the user approves font/firmware generation.
Uses the existing Inter and Noto Sans CJK sources plus lv_font_conv 1.5.3.
"""
from pathlib import Path
import re
import subprocess
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
ROOT = Path(__file__).resolve().parents[1]
converter = [sys.argv[1]] if len(sys.argv) > 1 else ['npx', '--yes', 'lv_font_conv@1.5.3']
medium = ROOT / 'assets/fonts/Inter-Medium.ttf'
instantiateVariableFont(TTFont(ROOT / 'assets/fonts/Inter-variable.ttf'), {'wght': 500, 'opsz': 24}, inplace=True).save(medium)
for size in (24, 28, 32, 40, 48, 108, 120, 164):
    # Keep the established clock numerals; medium weight for controls and metrics.
    source = ROOT / 'assets/fonts/Inter-variable.ttf' if size >= 108 else medium
    subprocess.run(converter + ['--size', str(size), '--bpp', '4', '--format', 'lvgl',
        '--font', str(source), '--range', '45,48-57' if size >= 108 else '32-126,176',
        '--no-compress', '--no-kerning', '--lv-include', 'lvgl.h',
        '--lv-font-name', f'lv_font_inter_{size}',
        '-o', str(ROOT / f'components/portable_ui/lv_font_inter_{size}.c')], check=True)
# One font for every 24px control/title: consistent Latin weight and Chinese baseline.
output = ROOT / 'components/portable_ui/lv_font_cjk_24.c'
subprocess.run(converter + ['--size', '24', '--bpp', '4', '--format', 'lvgl',
    '--font', str(medium), '--range', '32-126,176',
    '--font', str(ROOT / 'assets/fonts/NotoSansCJKsc-Regular.otf'),
    '--range', '0x3000-0x303F,0x4E00-0x9FFF,0xFF00-0xFFEF',
    '--no-kerning', '--lv-include', 'lvgl.h', '--lv-font-name', 'lv_font_cjk_24',
    '-o', str(output)], check=True)
# Rare fullwidth glyphs have taller extents; common UI text uses a 32px line box.
text = output.read_text()
text = re.sub(r'\.line_height = \d+,', '.line_height = 32,', text)
text = re.sub(r'\.base_line = \d+,', '.base_line = 6,', text)
output.write_text(text)
print('Generated medium UI fonts and merged 4bpp Chinese/Latin font. Firmware not compiled.')
