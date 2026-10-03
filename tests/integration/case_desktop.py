# SPDX-License-Identifier: GPL-3.0-only
"""Verify host button inputs, app discovery, and Settings rows."""
import json
import os
import stat
from pathlib import Path
import subprocess
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = BUILD / "desktop-checks"
SD = OUT/'sd'
CONFIG = SD/'.tmp_update/config'
CONFIG.mkdir(parents=True,exist_ok=True)
(SD/'Emu').mkdir(exist_ok=True)
(SD/'Roms').mkdir(exist_ok=True)
THEME = ONION_THEME
menu = dict(menu=dict(games=True,recent=False,apps=True,settings=True,recents=False),custom=dict(keep='unchanged'))
def reset():
    (CONFIG/'main-menu.json').write_text(json.dumps(menu))
    (CONFIG/'.romListRows').write_text('6')
reset()
for name,label,hidden in [('alpha','Alpha app',False),('zulu','Zulu app',False),('hidden','Hidden app',True)]:
    app = SD/'App'/name
    app.mkdir(parents=True,exist_ok=True)
    (app/'config.json').write_text(json.dumps(dict(label=label,hide=hidden,launch='launch.sh',description='Description for '+label,icon='../../Icons/Default/app/pacman.png')))
    (app/'launch.sh').write_text('# Fixture only; must never execute.\nexit 99\n')
bad = SD/'App/broken'
bad.mkdir(exist_ok=True)
(bad/'config.json').write_text('{broken')
subprocess.run([str(BUILD / "fixture-desktop"),str(SD),str(CONFIG)],cwd=ROOT,check=True,timeout=20)
def capture(name, actions):
    path = OUT/(name+'.bmp')
    subprocess.run([os.environ.get('MAINUI_TEST_EXE',str(BUILD / "MainUI-dev")),'--sd-root',str(SD),'--theme',str(THEME),
                    '--snapshot',str(path),'--input',actions],cwd=ROOT,check=True,timeout=20)
    return path.read_bytes()
home = capture('home','')
# SELECT retains its context menu; unassigned X/Y/START/MENU are silent.
assert capture('button-S', 'S') != home
for key in 'XYTM':
    assert capture('button-' + key, key) == home
assert capture('popup-start', 'ST') == capture('button-S', 'S')
assert capture('popup-select-close', 'SS') == home
for key in 'SXYTM': assert home == capture('button-return-'+key,key+'B')
apps = capture('apps','RE')
assert apps != home
assert apps != capture('app-details','REE')
assert apps == capture('app-return','REEB')
assert home == capture('apps-home','REBL')
settings = capture('settings','RRE')
assert settings != home
before = {p:p.read_bytes() for p in CONFIG.iterdir() if p.is_file()}
assert settings == capture('settings-no-preview-adjustment','RRER')
assert settings != capture('settings-down','RRED')
assert home == capture('settings-home','RREBLL')
assert before == {p:p.read_bytes() for p in CONFIG.iterdir() if p.is_file()}
(CONFIG/'main-menu.json').write_text('{"menu":{"games":true,"apps":true,"settings":true},"settings":["display","brightness","about"]}')
subprocess.run([str(BUILD / "fixture-stock_settings"),str(CONFIG),str(SD)],cwd=ROOT,check=True,timeout=20)
SOUNDS = OUT/'sounds-sd'
SOUNDS.mkdir(exist_ok=True)
subprocess.run([str(BUILD / "fixture-stock_settings"),str(CONFIG),str(SOUNDS),'sounds'],cwd=ROOT,check=True,timeout=20)
assert settings != capture('settings-whitelist','RRE')
print('Desktop checks passed: mapped buttons, Apps list, stock Settings navigation/whitelist, no invented settings writes')

for value in [[], {key:False for key in ['shutdown','brightness','wifi','display','themes','tweaks','language','sound','sleep','about']}, ['themes','tweaks']]:
    (CONFIG/'main-menu.json').write_text(json.dumps({'settings':value}))
    subprocess.run([str(BUILD / "fixture-stock_settings"),str(CONFIG),str(SD),'defaults'],cwd=ROOT,check=True,timeout=20)
