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
# A folder is listed only with a ROM somewhere below it: Sets is (through
# Inner), the empty Empty and the game data folder Data (no nes or zip in it,
# as a ScummVM game's AUDIO or DRIVERS) are not.
for name in ['Sets/Inner/Game.nes','Data/readme.txt','Data/AUDIO/track.wav']:
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
 assert c.execute("select count(*) from FC_roms where path like '%/Empty' or path like '%/Data%'").fetchone()==(0,)
 assert c.execute("select disp,type from FC_roms where ppath='Sets'").fetchall()==[('Inner',1)]
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

# An empty or missing extlist lists every file, except the game lists, ROM
# list caches and deletion copies kept beside the ROMs, and hidden files.
for config in (dict(label='All', rompath='../../Roms/ALL', extlist=''),
               dict(label='All', rompath='../../Roms/ALL')):
    all_sd = Path(tempfile.mkdtemp(prefix='catalog-all-', dir=BUILD))
    (all_sd / 'Emu/ALL').mkdir(parents=True)
    (all_sd / 'Emu/ALL/config.json').write_text(json.dumps(config))
    for name in ('Game.bin', 'NOEXT', 'Disc/Track.iso', 'Data/readme.txt', 'gamelist.xml',
                 'Imgs/Game.png', 'Manuals/Game.pdf', '.hidden', 'Empty/.keep',
                 'Old_cache2.db', 'Game.bin.mainui-delete.0000000000000001'):
        file = all_sd / 'Roms/ALL' / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(b'')
    subprocess.run([str(BUILD / 'MainUI-dev'), '--sd-root', str(all_sd), '--theme', str(ONION_THEME),
                    '--refresh-caches', '--system', 'All', '--snapshot', str(all_sd / 'shot.bmp')],
                   cwd=ROOT, check=True, timeout=30, capture_output=True)
    with sqlite3.connect(all_sd / 'Roms/ALL/ALL_cache6.db') as c:
        rows = sorted(c.execute('select disp,type,ppath from ALL_roms'))
    assert rows == [('Data', 1, '.'), ('Disc', 1, '.'), ('Game', 0, '.'), ('NOEXT', 0, '.'),
                    ('Track', 0, 'Disc'), ('readme', 0, 'Data')], (config, rows)
print('An empty or missing extlist lists every file but MainUI and Onion files')
