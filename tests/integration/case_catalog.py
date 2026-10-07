# SPDX-License-Identifier: GPL-3.0-only
"""Catalog tests use isolated generated ROMs; user additions to the demo are untouched."""
from pathlib import Path
import json
import subprocess
import tempfile
import sqlite3
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT=Path(__file__).resolve().parents[2]
sd=Path(tempfile.mkdtemp(prefix='catalog-check-',dir=BUILD))
for system,label,extensions in [('FC','NES','nes|zip'),('GB','Game Boy','gb'),('SFC','Super Nintendo','smc|sfc')]:
 emu=sd/'Emu'/system;emu.mkdir(parents=True)
 (emu/'config.json').write_text(json.dumps(dict(label=label,rompath=f'../../Roms/{system}',extlist=extensions,imgpath=f'../../Roms/{system}/Imgs')))
 (sd/'Roms'/system).mkdir(parents=True)
# Imgs and Manuals folders, in any case, are never listed as ROM folders.
for name in ['Adventure.nes','alpha.zip','Zebra.NES','ignored.txt','Imgs/cover.nes','Manuals/Guide.nes','Collections/Nested.nes','Collections/More/Deep.nes','Collections/manuals/Inner.nes']:
 p=sd/'Roms/FC'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'')
(sd/'Roms/FC/Empty').mkdir()
subprocess.run([str(BUILD / "fixture-catalog"),str(sd)],cwd=ROOT,check=True,timeout=30)
db=sd/'Roms/FC/FC_cache6.db'
with sqlite3.connect(db) as c:
 assert c.execute('pragma integrity_check').fetchone()==('ok',)
 assert [r[1] for r in c.execute('pragma table_info(FC_roms)')]==['id','disp','path','imgpath','type','ppath','pinyin','cpinyin']
 assert c.execute("select count(*) from FC_roms where ppath='.'").fetchone()==(5,)
 assert c.execute("select count(*) from sqlite_master where name='mainui_rom_browse'").fetchone()==(1,)
 assert c.execute("select path from FC_roms where ppath='Collections/More'").fetchone()==('/mnt/SDCARD/Emu/FC/../../Roms/FC/Collections/More/Deep.nes',)
 assert c.execute("select count(*) from FC_roms where path like '%anuals%'").fetchone()==(0,)
print('Created cache schema, index, nested keys and integrity verified')

xml_sd = Path(tempfile.mkdtemp(prefix='catalog-xml-record-', dir=BUILD))
(xml_sd / 'Emu/FC').mkdir(parents=True)
(xml_sd / 'Roms/FC').mkdir(parents=True)
(xml_sd / 'Emu/FC/config.json').write_text(json.dumps(dict(
    label='NES', rompath='../../Roms/FC', imgpath='../../Roms/FC/Imgs', extlist='nes')))
(xml_sd / 'Roms/FC/Actual.nes').write_bytes(b'')
(xml_sd / 'Roms/FC/miyoogamelist.xml').write_text(
    '<gameList><game><path>./Actual.nes</path><name>XML title</name>'
    '<image>./Other/cover.png</image></game></gameList>')
subprocess.run([str(BUILD / 'fixture-catalog'), str(xml_sd), 'xml'],
               cwd=ROOT, check=True, timeout=30)

# Absolute ROM/image prefixes are independent; neither is console-relative.
# Rebuild scanned rows for each prefix combination; cached identity is authoritative.
(xml_sd / 'Roms/FC/miyoogamelist.xml').unlink()
for mode, rompath, imgpath in (
    ('absolute-rom', '/mnt/SDCARD/Roms/FC', '../../Roms/FC/Imgs'),
    ('absolute-image', '../../Roms/FC', '/mnt/SDCARD/Roms/FC/Imgs'),
    ('absolute-both', '/mnt/SDCARD/Roms/FC', '/mnt/SDCARD/Roms/FC/Imgs'),
):
    (xml_sd / 'Emu/FC/config.json').write_text(json.dumps(dict(
        label='NES', rompath=rompath, imgpath=imgpath, extlist='nes')))
    (xml_sd / 'Roms/FC/FC_cache6.db').unlink()
    subprocess.run([str(BUILD / 'fixture-catalog'), str(xml_sd), mode],
                   cwd=ROOT, check=True, timeout=30)
