# SPDX-License-Identifier: GPL-3.0-only
"""Exercise section contexts and saved-file preservation through real SDL input."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT=Path(__file__).resolve().parents[2]
OUT=Path(tempfile.mkdtemp(prefix='saved-actions-',dir=BUILD))
SD=OUT/'sd'
THEME=ONION_THEME
for name in ['Emu/FC','Roms/FC','App/Test','.tmp_update/config']: (SD/name).mkdir(parents=True)
(SD/'Emu/FC/config.json').write_text(json.dumps(dict(label='NES',rompath='../../Roms/FC',launch='launch.sh',extlist='nes')))
(SD/'App/Test/config.json').write_text(json.dumps(dict(label='App test',launch='launch.sh')))
(SD/'App/Test/launch.sh').write_text('# must not run')
(SD/'Roms/FC/one.nes').write_bytes(b'ROM must remain intact')
(SD/'.tmp_update/config/main-menu.json').write_text(json.dumps({'menu':dict(games=True,apps=True,recents=True,favorites=True,settings=True)}))
def capture(name,actions):
    path=OUT/(name+'.bmp')
    subprocess.run([os.environ.get('MAINUI_TEST_EXE',str(BUILD / "MainUI-dev")),'--sd-root',str(SD),'--theme',str(THEME),
        '--input',actions,'--snapshot',str(path)],cwd=ROOT,check=True,timeout=20)
    return path.read_bytes()
favorites=SD/'Roms/favourite.json'
capture('rom-add','EESDE')
first=favorites.read_bytes();record=json.loads(first)
assert record['rompath']=='/mnt/SDCARD/Emu/FC/../../Roms/FC/one.nes'
# Missing imgpath defaults to Imgs; the image need not exist.
assert record['imgpath']=='/mnt/SDCARD/Emu/FC/Imgs/one.png'
assert record['launch']=='/mnt/SDCARD/Emu/FC/launch.sh' and record['type']==5
capture('rom-remove-cancel','EESDB');assert favorites.read_bytes()==first
capture('app-add','RESDE')
records=[json.loads(line) for line in favorites.read_text().splitlines()]
assert len(records)==2 and records[1]['type']==3
assert records[1]['launch']=='/mnt/SDCARD/App/Test/launch.sh'
assert favorites.read_bytes().startswith(first)
capture('favorite-remove','RRRESDDDE')
assert len(favorites.read_text().splitlines())==1
assert json.loads(favorites.read_text())['label']=='App test'
recent=SD/'Roms/recentlist.json'
one={'label':'Search origin','rompath':'/mnt/SDCARD/Emu/FC/launch.sh:/mnt/SDCARD/Roms/FC/one.nes','launch':'/mnt/SDCARD/App/Search/launch.sh','type':5}
two=' {"label":"Keep exact bytes", "rompath":"/mnt/SDCARD/Roms/FC/two.nes", "launch":"/mnt/SDCARD/Emu/FC/launch.sh", "type":5, "extra":42}\r\n'
recent.write_bytes((json.dumps(one)+'\n'+two).encode())
capture('recent-remove','RRESDE')
assert recent.read_bytes()==two.encode()
before=recent.read_bytes()
capture('clear-dialog','RRESDDE')
capture('clear-cancel','RRESDDEB');assert recent.read_bytes()==before
capture('clear-held','RRESDDEEEE');assert recent.read_bytes()==before  # A held: repeats only
capture('clear-confirm','RRESDDE-EE');assert recent.read_bytes()==b''
assert (SD/'Roms/FC/one.nes').read_bytes()==b'ROM must remain intact'
# Bad existing data must never be replaced with just the newly selected Favorite.
for data in [b'{broken', b'{"label":"hidden"}\0trailing bytes']:
    favorites.write_bytes(data)
    capture('invalid-favorite','RESDE')
    assert favorites.read_bytes()==data
favorites.write_bytes(b'')
(SD/'Roms/favourite.json.writing').write_text('foreign writer')
capture('locked-favorite','RESDE')
assert favorites.read_bytes()==b''
assert (SD/'Roms/favourite.json.writing').read_text()=='foreign writer'
print('Apps/ROM Favorite addition, removal cancellation, removal, Search-origin Recent removal, clear confirmation and failure preservation passed:',OUT)
