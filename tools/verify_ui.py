"""Build/run actual LVGL UI on the host under AddressSanitizer; save native PNGs.
Requires cmake, C compiler, Pillow and ESP-IDF 5.x for cJSON (IDF_PATH).
"""
from pathlib import Path
import os
import subprocess
from PIL import Image
ROOT = Path(__file__).resolve().parents[1]
LVGL = ROOT / 'managed_components/lvgl__lvgl'
if not LVGL.is_dir():
    raise SystemExit('LVGL sources are fetched on first idf.py build into managed_components/lvgl__lvgl')
IDF = Path(os.environ['IDF_PATH'])
BUILD = ROOT / 'build/host'
BUILD.mkdir(parents=True, exist_ok=True)
OUT = ROOT / 'assets/preview'
OUT.mkdir(parents=True, exist_ok=True)
(BUILD / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(p4_ui_smoke C)
file(GLOB_RECURSE LVGL "{LVGL}/src/*.c")
file(GLOB FONTS "{ROOT}/components/portable_ui/lv_font_*.c")
add_executable(smoke "{ROOT}/tools/host/smoke.c" ${{LVGL}} ${{FONTS}}
 "{ROOT}/components/portable_ui/portable_icons.c" "{ROOT}/components/portable_ui/bambu_state.c"
 "{ROOT}/components/portable_ui/portable_ui_json.c" "{IDF}/components/json/cJSON/cJSON.c")
target_include_directories(smoke PRIVATE "{ROOT}/tools/host" "{ROOT}/components/portable_ui"
 "{LVGL}" "{IDF}/components/json/cJSON")
target_compile_definitions(smoke PRIVATE LV_CONF_SKIP=1 LV_KCONFIG_IGNORE=1 LV_COLOR_DEPTH=16
 LV_MEM_CUSTOM=1 LV_USE_FONT_COMPRESSED=1 LV_FONT_FMT_TXT_LARGE=1 LV_DISP_DEF_REFR_PERIOD=25)
target_compile_options(smoke PRIVATE -O1 -g -fsanitize=address -fno-omit-frame-pointer)
target_link_options(smoke PRIVATE -fsanitize=address)
''')
subprocess.run(['cmake', '-S', str(BUILD), '-B', str(BUILD / 'out')], check=True)
subprocess.run(['cmake', '--build', str(BUILD / 'out'), '-j', '8'], check=True)
subprocess.run([str(BUILD / 'out/smoke')], cwd=OUT, check=True,
               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
for path in OUT.glob('*.ppm'):
    with Image.open(path) as im:
        assert im.size == (800, 480)
        im.save(path.with_suffix('.png'))
    path.unlink()
print('Verified PNGs:', OUT)

for kind in ['start', 'pause', 'resume', 'warning', 'complete', 'offline']:
    paths = sorted(OUT.glob(f'event-{kind}-[0-9][0-9].png'))
    if paths:
        frames = [Image.open(p).convert('RGB') for p in paths]
        assert len({f.tobytes() for f in frames}) > 5
        frames[0].save(OUT / f'event-{kind}.gif', save_all=True, append_images=frames[1:], duration=100, loop=0)
        frames[20].save(OUT / f'event-{kind}.png')
        for frame in frames: frame.close()
        for path in paths: path.unlink()

from PIL import ImageSequence
combined = []
for kind in ['start', 'pause', 'resume', 'warning', 'complete', 'offline']:
    with Image.open(OUT / f'event-{kind}.gif') as animation:
        combined.extend(frame.convert('RGB').copy() for frame in ImageSequence.Iterator(animation))
combined[0].save(OUT / 'events-preview.gif', save_all=True, append_images=combined[1:], duration=100, loop=0)
for frame in combined: frame.close()
