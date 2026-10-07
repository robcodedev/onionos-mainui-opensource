# SPDX-License-Identifier: GPL-3.0-only
"""Isolated SDL checks for thumbnail transitions and deferred custom language selection."""
import os
import json
from pathlib import Path
import subprocess
import tempfile
from PIL import Image
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix='preview-language-', dir=BUILD))
SD = OUT/'sd'
THEME = ONION_THEME
for name in ['Emu/FC', 'Roms/FC/Folder', 'Roms/FC/Imgs', '.tmp_update/config', 'miyoo/app/lang']:
    (SD/name).mkdir(parents=True)
(SD/'Emu/FC/config.json').write_text(json.dumps(dict(label='NES',rompath='../../Roms/FC',imgpath='../../Roms/FC/Imgs',extlist='nes')))
for name in ['A.nes','B.nes','Folder/C.nes']: (SD/'Roms/FC'/name).write_bytes(b'')
# Indexed PNG exercises palette normalization as well as downscaling.
Image.new('RGB',(500,400),(255,0,0)).convert('P').save(SD/'Roms/FC/Imgs/A.png')
CONFIG=SD/'.tmp_update/config/main-menu.json'
CONFIG.write_text('{"menu":{"games":true,"apps":true,"settings":true},"settings":["language"]}')
(SD/'system.json').write_text('{"language":"en.lang","preserve":123}')
(SD/'miyoo/app/lang/en.lang').write_text('\ufeff'+json.dumps({'lang':'Custom test','15':'Custom settings','23':'Custom language','407':'Custom tweaks'}),encoding='utf-8')
(SD/'miyoo/app/lang/bad.lang').write_text('{broken')
subprocess.run([str(BUILD / "fixture-language"),str(SD),str(OUT/'missing-fallback')],check=True,cwd=ROOT)
# The saved language is loaded at startup; start in German (from the Onion
# fallback) so choosing the custom en.lang in Settings visibly changes labels.
(SD/'system.json').write_text('{"language":"de.lang","preserve":123}')
def capture(name,actions,system=False):
    target=OUT/(name+'.bmp')
    args=[os.environ.get('MAINUI_TEST_EXE',str(BUILD / "MainUI-dev")),'--sd-root',str(SD),'--theme',str(THEME),'--input',actions,'--snapshot',str(target)]
    if system: args += ['--system','NES']
    subprocess.run(args,check=True,cwd=ROOT,timeout=20)
    return Image.open(target).convert('RGB')
folder=capture('folder','',True)
thumb=capture('thumb','D',True)
missing=capture('no-thumb','DD',True)
restored=capture('restored','DU',True)
assert thumb.getpixel((500,200)) == (255,0,0)
assert folder.tobytes()==restored.tobytes()
assert missing.getpixel((500,200)) != (255,0,0)
# Language discovery/selection is reached only after A on the Settings language row.
stock=capture('settings','RRE')
# In the name-sorted list, "Custom test" (en.lang) sits directly above "Deutsch".
changed=capture('custom-language','RREEUE')
assert stock.tobytes()!=changed.tobytes()
saved=json.loads((SD/'system.json').read_text())
assert saved['language']=='en.lang' and saved['preserve']==123
# A foreign transaction file must not be overwritten; the active language stays unchanged.
(SD/'system.json.writing').write_text('reserved')
old=(SD/'system.json').read_bytes()
capture('blocked-language-save','RREEE')
assert (SD/'system.json').read_bytes()==old
assert (SD/'system.json.writing').read_text()=='reserved'
print('Thumbnail placement, palette scaling, stale-image clearing, language UI and atomic-save checks passed:',OUT)

# Stock numeric settings use their device keys; adjustment never discards unknown fields.
for setting,key,initial,expected in [('brightness','brightness',7,8),('sound','bgmvol',19,20),('sleep','hibernate',5,15)]:
    CONFIG.write_text(json.dumps({'menu':{'games':True,'apps':True,'settings':True},'settings':[setting]}))
    (SD/'system.json').write_text(json.dumps({key:initial,'preserve':123}))
    # Release only the reservation created by this isolated test.
    reservation=SD/'system.json.writing'
    if reservation.exists(): reservation.unlink()
    capture(setting+'-adjust','RRER')
    saved=json.loads((SD/'system.json').read_text())
    assert saved[key]==expected and saved['preserve']==123
    if setting=='sleep':
        capture('sleep-full-cycle','RRERRRR')
        assert json.loads((SD/'system.json').read_text())[key]==15
        for expected in [5,0,30,15]:
            capture('sleep-reverse-'+str(expected),'RREL')
            assert json.loads((SD/'system.json').read_text())[key]==expected
    else:
        capture(setting+'-minimum','RRE'+'L'*35)
        assert json.loads((SD/'system.json').read_text())[key]==0
# Malformed original settings must survive an attempted edit byte-for-byte.
(SD/'system.json').write_text('{broken')
capture('malformed-settings-save','RRER')
assert (SD/'system.json').read_text()=='{broken'
print('Stock numeric keys, limits and malformed-file preservation passed')
