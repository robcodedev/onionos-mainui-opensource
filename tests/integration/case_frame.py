# SPDX-License-Identifier: GPL-3.0-only
"""Pixel checks for shared headers, status state, footer colors and alpha."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT=Path(__file__).resolve().parents[2]
OUT=Path(tempfile.mkdtemp(prefix='frame-',dir=BUILD))
SD=OUT/'sd';THEME=OUT/'theme'
FALLBACK=ONION_THEME
for p in ['Emu/FC','Roms/FC']: (SD/p).mkdir(parents=True)
(THEME/'skin').mkdir(parents=True)
(SD/'Emu/FC/config.json').write_text(json.dumps(dict(label='NES',rompath='../../Roms/FC',extlist='nes')))
(SD/'Roms/FC/a.nes').write_bytes(b'')
config=json.loads((FALLBACK/'config.json').read_text())
config['currentpage']={'color':'#FF00FF'};config['total']={'color':'#00FFFF'}
(THEME/'config.json').write_text(json.dumps(config))
Image.new('RGB',(640,480),(10,20,30)).save(THEME/'skin/background.png')
Image.new('RGBA',(640,60),(0,0,0,0)).save(THEME/'skin/bg-title.png')
Image.new('RGBA',(640,60),(255,0,0,128)).save(THEME/'skin/tips-bar-bg.png')
Image.new('RGB',(20,20),(255,0,0)).save(THEME/'skin/miyoo-topbar.png')
for name,color in [('power-full-icon',(0,255,0)),('power-20%-icon',(0,0,255)),('ic-power-charge-100%',(255,255,0))]:
    Image.new('RGB',(48,48),color).save(THEME/'skin'/(name+'.png'))
def capture(name,battery='100',system=False):
    output=OUT/(name+'.bmp')
    args=[os.environ.get('MAINUI_TEST_EXE',str(BUILD / "MainUI-dev")),'--sd-root',str(SD),'--theme',str(THEME),
          '--fallback',str(FALLBACK),'--battery',battery,'--snapshot',str(output)]
    if system: args += ['--system','NES']
    subprocess.run(args,cwd=ROOT,check=True,timeout=20)
    return Image.open(output).convert('RGB')
home=capture('home')
assert home.getpixel((25,25))==(255,0,0)
assert home.getpixel((596,30))==(0,255,0)
assert capture('low','20').getpixel((596,30))==(0,0,255)
assert capture('charging','500').getpixel((596,30))==(255,255,0)
assert capture('hidden','-1').getpixel((596,30))==(10,20,30)
# miyoo-topbar.png is drawn unclipped at (20, (60-h)/2), as stock does: a tall
# logo shows below the header too (Super Onion Entertainment System's dots).
tall=Image.new('RGBA',(640,781),(0,0,0,0))
tall.paste((200,100,0,255),(100,750,111,759))
tall.save(THEME/'skin/miyoo-topbar.png')
dots=capture('tall-logo')
assert dots.getpixel((125,395))==(200,100,0) and dots.getpixel((125,399))!=(200,100,0)
Image.new('RGB',(20,20),(255,0,0)).save(THEME/'skin/miyoo-topbar.png')
list_image=capture('list',system=True)
# NES title is centered independently of the left logo slot and battery.
points=[(x,y) for y in range(60) for x in range(200,440) if min(list_image.getpixel((x,y)))>200]
assert points and abs((min(x for x,y in points)+max(x for x,y in points))/2-320)<12
assert all(abs(a-b)<=2 for a,b in zip(list_image.getpixel((400,450)),(132,10,15)))
colors=list_image.crop((520,420,640,480)).getdata()
assert any(r>220 and g<40 and b>220 for r,g,b in colors)
assert any(r<40 and g>220 and b>220 for r,g,b in colors)
print('Header logo/centering/battery states and footer per-counter colors/single alpha blend passed:',OUT)
