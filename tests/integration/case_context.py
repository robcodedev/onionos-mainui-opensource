# SPDX-License-Identifier: GPL-3.0-only
"""Exercise real SELECT shortcuts, refresh, and six-row popup asset expansion."""
from contextlib import closing
import json
import shutil
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from PIL import Image
from env import unlink_if_exists, BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix='context-', dir=BUILD))
SD=OUT/'sd'
THEME=OUT/'theme'
FALLBACK=ONION_THEME
for name in ['Emu/FC','Roms/FC','.tmp_update/config']: (SD/name).mkdir(parents=True)
(THEME/'skin').mkdir(parents=True)
(THEME/'config.json').write_bytes((FALLBACK/'config.json').read_bytes())
(SD/'Emu/FC/config.json').write_text(json.dumps(dict(label='NES',rompath='../../Roms/FC',extlist='nes')))
(SD/'Roms/FC/one.nes').write_bytes(b'fixture')
CONFIG=SD/'.tmp_update/config/main-menu.json'
menu=dict(games=True,apps=True,settings=True)
def setup(context=None):
    data={'menu':menu}
    if context is not None: data['context']=context
    CONFIG.write_text(json.dumps(data))
def capture(name,actions):
    unlink_if_exists((SD / 'appconfigs/romwinidx.json'))
    target=OUT/(name+'.bmp')
    subprocess.run([os.environ.get('MAINUI_TEST_EXE',str(BUILD / "MainUI-dev")),
        '--sd-root',str(SD),'--theme',str(THEME),'--fallback',str(FALLBACK),
        '--snapshot',str(target),'--input',actions],cwd=ROOT,check=True,timeout=20)
    return Image.open(target).convert('RGB')
setup()
home=capture('home','')
assert home.tobytes()==capture('select-back','SB').tobytes()
capture('refresh','SE')
cache=SD/'Roms/FC/FC_cache6.db'
assert not cache.exists()
capture('build-on-entry', 'EE')
assert cache.exists()
(SD/'Roms/FC/two.nes').write_bytes(b'fixture2')
capture('refresh-again','SE')
assert not cache.exists()
capture('rebuild-on-entry', 'EE')
with closing(sqlite3.connect(cache)) as db: assert db.execute('select count(*) from FC_roms').fetchone()[0]==2
assert (SD/'Roms/FC/one.nes').read_bytes()==b'fixture'
settings=capture('settings-direct','RRE')
menu['settings']=False
setup(['settings','games'])
assert settings.tobytes()==capture('hidden-settings-shortcut','SE').tobytes()
setup([])
assert capture('empty-context-home','').tobytes()==capture('empty-context-select','S').tobytes()
# Theme only provides three rows: six visible rows must be synthesized without resizing its trim.
bg=Image.new('RGBA',(400,190),(0,0,255,255));bg.paste((255,0,0,255),(0,180,400,190));bg.save(THEME/'skin/bg-pop-menu-3.png')
Image.new('RGBA',(400,60),(0,255,0,255)).save(THEME/'skin/bg-list-popup-s.png')
setup(['games','apps','settings','recents','favorites','expert','refresh'])
popup=capture('seven-entry-popup','S')
assert all(abs(a-b)<=2 for a,b in zip(popup.getpixel((5,10)),(0,255,0)))
assert all(abs(a-b)<=2 for a,b in zip(popup.getpixel((5,310)),(0,0,255)))
assert all(abs(a-b)<=2 for a,b in zip(popup.getpixel((5,365)),(255,0,0)))
assert popup.tobytes()!=capture('popup-scroll','S'+'D'*6).tobytes()
print('SELECT refresh, hidden shortcuts, explicit empty menu, popup expansion and scrolling passed:',OUT)

# An exact six-row image must beat the existing smaller active image.
Image.new('RGB', (400,370), (220,40,180)).save(THEME/'skin/bg-pop-menu-6.png')
exact = capture('exact-six', 'S')
assert exact.getpixel((5,310)) == (220,40,180)
(THEME/'skin/bg-list-popup-s.png').unlink()
Image.new('RGB', (640,60), (230,120,10)).save(THEME/'skin/bg-list-s.png')
ordinary = capture('ordinary-popup-selection', 'S')
assert ordinary.getpixel((5,10)) == (230,120,10)
(THEME/'skin/bg-pop-menu-6.png').write_bytes(b'not a PNG')
corrupt = capture('corrupt-exact-fallback', 'S')
assert all(abs(a-b)<=2 for a,b in zip(corrupt.getpixel((5,310)),(0,0,255)))
print('Exact popup precedence, corrupt-image fallback and ordinary selection fallback passed')

setup({'games': True, 'apps': False, 'settings': False})
revealed = capture('shoulder-reveal', '1234-1-2-3-4')
assert revealed.tobytes() == capture('shoulder-reveal-persists', '1234-1-2-3-4BS').tobytes()
assert revealed.tobytes() != capture('normal-hidden-popup', 'S').tobytes()
print('Four-shoulder main-menu reveal and same-process persistence passed')

# Refresh from a console list, folder row, parent row, and empty console.
setup()
(SD / 'Emu/OTHER').mkdir()
(SD / 'Roms/OTHER').mkdir()
(SD / 'Emu/OTHER/config.json').write_text(json.dumps(dict(
    label='Other', rompath='../../Roms/OTHER', extlist='nes')))
capture('build-other-cache', 'ER E'.replace(' ', ''))
other_cache = SD / 'Roms/OTHER/OTHER_cache6.db'
other_before = other_cache.read_bytes()
(SD / 'Roms/OTHER/new.nes').write_bytes(b'other')
(SD / 'Roms/FC/three.nes').write_bytes(b'new')
capture('refresh-from-rom', 'EESDDDDE')
with closing(sqlite3.connect(cache)) as db:
    assert db.execute('select count(*) from FC_roms').fetchone()[0] == 3
assert other_cache.read_bytes() == other_before
(SD / 'Roms/FC/Sub').mkdir()
(SD / 'Roms/FC/Sub/deep.nes').write_bytes(b'deep')
capture('refresh-add-folder', 'EESDDDDE')
(SD / 'Roms/FC/Sub/new.nes').write_bytes(b'new')
capture('refresh-from-parent', 'EEESDE')
with closing(sqlite3.connect(cache)) as db:
    assert db.execute("select count(*) from FC_roms where ppath='Sub'").fetchone()[0] == 2
capture('refresh-folder-row', 'EESDE')
assert other_cache.read_bytes() == other_before
# OTHER's cached empty list still exposes refresh as its first menu row.
capture('refresh-empty-console', 'ERESDE')
with closing(sqlite3.connect(other_cache)) as db:
    assert db.execute('select count(*) from OTHER_roms').fetchone()[0] == 1
print('Console-local refresh from ROM, folder, parent, and empty list passed')

# Shared console grid: invalidate only the selected console; rebuild on entry.
fc_before = cache.read_bytes()
(SD / 'Roms/OTHER/new.nes').unlink()
selected_grid = capture('selected-grid-before-refresh', 'ER')
refreshed_grid = capture('refresh-selected-grid-console', 'ERSDE')
assert not other_cache.exists()
assert cache.read_bytes() == fc_before
assert refreshed_grid.tobytes() == selected_grid.tobytes()
capture('rebuild-selected-grid-console-on-entry', 'ERE')
assert other_cache.exists()
with closing(sqlite3.connect(f'file:{other_cache}?mode=ro', uri=True)) as db:
    assert db.execute('select count(*) from OTHER_roms').fetchone()[0] == 0
assert cache.read_bytes() == fc_before

# The first grid action retains global scope across both consoles.
(SD / 'Roms/FC/global.nes').write_bytes(b'global')
(SD / 'Roms/OTHER/global.nes').write_bytes(b'global')
capture('refresh-all-grid-consoles', 'ERSE')
assert not cache.exists() and not other_cache.exists()
capture('rebuild-global-fc', 'EE')
capture('rebuild-global-other', 'ERE')
with closing(sqlite3.connect(cache)) as db:
    assert db.execute("select count(*) from FC_roms where path='/mnt/SDCARD/Emu/FC/../../Roms/FC/global.nes'").fetchone()[0] == 1
with closing(sqlite3.connect(other_cache)) as db:
    assert db.execute('select count(*) from OTHER_roms').fetchone()[0] == 1
fc_before = cache.read_bytes()

# The synthetic console retains only the global grid action. It is Search by
# its data folder, as Onion creates it; the label alone does not make it so.
# Its rows are FC's files, as when the fixture borrowed Roms/FC.
(SD / 'Emu/SEARCH').mkdir()
shutil.copytree(SD / 'Roms/FC', SD / 'App/Search/data',
                ignore=shutil.ignore_patterns('*_cache6.db*'))
(SD / 'Emu/SEARCH/config.json').write_text(json.dumps(dict(
    label=' Search ', rompath='../../App/Search/data', launch='../../App/Search/launch.sh',
    extlist='nes')))
search_grid = capture('synthetic-search-grid', 'E')
search_popup = capture('synthetic-search-global-popup', 'ES')
assert search_grid.tobytes() != search_popup.tobytes()
assert search_popup.tobytes() == capture('synthetic-search-global-wrap', 'ESD').tobytes()
assert search_grid.tobytes() == capture('synthetic-search-popup-back', 'ESB').tobytes()
search_folder = capture('synthetic-search-folder', 'EE')
assert search_folder.tobytes() != capture('synthetic-search-folder-popup', 'EES').tobytes()
assert capture('synthetic-search-folder-popup', 'EES').tobytes() == capture('synthetic-search-folder-wrap', 'EESD').tobytes()
# On a ROM, four Down presses wrap to Launch: Search is present, Refresh is absent.
assert capture('synthetic-search-rom-popup', 'EEDS').tobytes() == capture(
    'synthetic-search-rom-popup-wrap', 'EEDSDDDD').tobytes()
assert cache.read_bytes() == fc_before
print('Selected-console grid refresh, empty rebuild and synthetic Search suppression passed')
