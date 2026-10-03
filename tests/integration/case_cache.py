# SPDX-License-Identifier: GPL-3.0-only
import os
#!/usr/bin/env python3
"""Generate cache fixtures in build/ and verify compiled C plus actual SDL paging.
All ROMs are empty filename fixtures. Valid/unsupported caches remain byte-identical.
"""
from contextlib import closing
import hashlib
import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
from env import unlink_if_exists, BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
BASE = BUILD / "cache-fixtures"
SYSTEM = "Odd'System"
TABLE = SYSTEM + '_roms'
def quoted(name):
    return '"' + name.replace('"', '""') + '"' 
def make(name, rows=(), corrupt=False, wrong=False, wal=False):
    sd = BASE / name
    emu = sd / 'Emu/FC'
    rom = sd / 'Roms' / SYSTEM
    emu.mkdir(parents=True, exist_ok=True)
    rom.mkdir(parents=True, exist_ok=True)
    (emu/'config.json').write_text(json.dumps(dict(label='Cache test', rompath=f'../../Roms/{SYSTEM}',
                                                 imgpath=f'../../Roms/{SYSTEM}/Imgs', extlist='nes')))
    (rom/'fallback.nes').write_bytes(b'')
    db = rom / (SYSTEM + '_cache6.db')
    if db.exists():
        # Exact generated fixture file, never an SD-card input.
        db.unlink()
    if corrupt:
        db.write_bytes(b'not a database')
        return sd, db
    with sqlite3.connect(db) as conn:
        if wrong:
            conn.execute('CREATE TABLE unrelated (x)')
        else:
            schema = '(id INTEGER PRIMARY KEY, disp TEXT NOT NULL, path TEXT NOT NULL, imgpath TEXT NOT NULL, type INTEGER DEFAULT 0, ppath TEXT NOT NULL, pinyin TEXT NOT NULL, cpinyin TEXT NOT NULL)'
            conn.execute(f'CREATE TABLE {quoted(TABLE)} {schema}')
            conn.executemany(f'INSERT INTO {quoted(TABLE)} VALUES (?,?,?,?,?,?,?,?)', rows)
            conn.execute(f'CREATE INDEX browse ON {quoted(TABLE)} (ppath,type DESC,disp COLLATE NOCASE)')
            quoted_table = quoted('quoted"table')
            conn.execute(f'CREATE TABLE {quoted_table} {schema}')
            conn.execute('INSERT INTO "quoted""table" VALUES (1,\'Quoted identifier\',\'./q.nes\',\'\',0,\'.\',\'\',\'\')')
    conn.close()
    if wal:
        connection = sqlite3.connect(db)
        connection.execute('PRAGMA journal_mode=WAL')
        connection.close()
    return sd, db
def row(i, label, path, kind=0, parent='.', art=''):
    return (i,label,path,art,kind,parent,'','')
def run(scenario, name, rows=(), **kwargs):
    sd, db = make(name, rows, **kwargs)
    before = hashlib.sha256(db.read_bytes()).digest()
    files = sorted(p.relative_to(sd).as_posix() for p in sd.rglob('*') if p.is_file())
    subprocess.run([str(BUILD / "fixture-cache"), str(sd), scenario], check=True, cwd=ROOT, timeout=30)
    if scenario == "repaired":
        assert hashlib.sha256(db.read_bytes()).digest() != before
        with sqlite3.connect(db) as connection:
            assert connection.execute("pragma integrity_check").fetchone() == ("ok",)
        repaired = db.read_bytes()
        subprocess.run([str(BUILD / "fixture-cache"), str(sd), scenario], check=True, cwd=ROOT, timeout=30)
        assert db.read_bytes() == repaired
    else:
        assert hashlib.sha256(db.read_bytes()).digest() == before
    after_files = {p.relative_to(sd).as_posix() for p in sd.rglob('*') if p.is_file()}
    expected_files = set(files)

    assert expected_files == after_files
    return sd
large = [row(1,'Collections',"./Sets/O'Brien",1),row(2,'Empty','./Empty',1)]
large += [row(i+3,f'Game {i:05d}',f'./raw{i}.nes') for i in range(13000)]
large += [row(14000,'A proper cached title',f"/mnt/SDCARD/Roms/{SYSTEM}/Sets/O'Brien/raw.nes",0,"./Sets/O'Brien",f'/mnt/SDCARD/Roms/{SYSTEM}/Imgs/cover.png')]
sd = run('large','large',large)
run('empty','empty')
run('repaired','corrupt',corrupt=True)
run('repaired','wrong-schema',wrong=True)
run('fallback', 'wal', wal=True)
run('fallback','invalid-row',[row(1,'Invalid','./x.nes',9)])
run('fallback','nul-row',[row(1,'bad\0label','./x.nes')])
run('fallback','long-row',[row(1,'x'*4096,'./x.nes')])
run('bad-later','bad-later',[row(i+1,f'Game {i:03d}','./x.nes' if i<69 else '') for i in range(70)])
run('sort','sort',[row(i+1,label,f'./{i}.nes') for i,label in enumerate(['alpha','ALPHA','beta','Zebra'])])
# Real SDL rendering exercises the former fixed-size list boundary and cross-window navigation.
ui = BUILD / "MainUI-dev"
theme = ONION_THEME
def capture(name, actions=''):
    unlink_if_exists((sd / "appconfigs/romwinidx.json"))
    out = BASE/(name+'.bmp')
    command = [str(ui),'--sd-root',str(sd),'--theme',str(theme),'--system','Cache test','--snapshot',str(out)]
    if actions:
        command += ['--input', actions]
    subprocess.run(command, check=True, cwd=ROOT, timeout=30)
    return out.read_bytes()
first = capture('first')
assert capture('folder-return','EB') == first
assert capture('last','U') != first
assert capture('last-return','UD') == first
assert capture('window-boundary','D'*65) != first
# A cache page that cannot be read is recovered in the background, keeping the
# selected row and window: reload, then rebuild the cache, then scan the folder.
# Each SD holds 80 real ROMs whose cache MainUI built; row 69 is then damaged.
RECOVERY = BASE / 'recovery'
def recovery_sd(name, cache=True, xml=None):
    sd = RECOVERY / name
    for directory in ('Emu/REC', 'Roms/REC'):
        (sd / directory).mkdir(parents=True)
    (sd / 'Emu/REC/config.json').write_text(json.dumps(dict(
        label='Recovery', rompath='../../Roms/REC', launch='launch.sh', extlist='nes')))
    for i in range(80):
        (sd / f'Roms/REC/Game {i:03}.nes').write_bytes(b'rom')
    if cache:
        recovery_run(sd, 'build', '')
        with closing(sqlite3.connect(sd / 'Roms/REC/REC_cache6.db')) as conn, conn:
            conn.execute("UPDATE REC_roms SET path='' WHERE disp='Game 069'")
    if xml is not None:
        (sd / 'Roms/REC/miyoogamelist.xml').write_text(xml)
    return sd
def recovery_run(sd, name, actions, env=None):
    unlink_if_exists(sd / 'appconfigs/romwinidx.json')
    out = RECOVERY / f'{sd.name}-{name}.bmp'
    command = [str(ui), '--sd-root', str(sd), '--theme', str(theme), '--system', 'Recovery',
               '--snapshot', str(out)] + (['--input', actions] if actions else [])
    result = subprocess.run(command, cwd=ROOT, timeout=30, capture_output=True, text=True,
                            env=env)
    assert result.returncode == 0, (name, result.returncode, result.stderr[-400:])
    assert 'cannot be read' not in result.stderr
    steps = [line.split(': ', 1)[1] for line in result.stderr.splitlines()
             if line.startswith('Recovering an unreadable list page')]
    return steps, out.read_bytes()
shutil.rmtree(RECOVERY, ignore_errors=True)
# The rebuilt cache reads: same row and window as the clean list, no message.
sd = recovery_sd('rebuild')
steps, recovered = recovery_run(sd, 'recovered', 'U')
assert steps == ['reloading', 'rebuilding the cache'], steps
assert recovery_run(sd, 'clean', 'U') == ([], recovered)
# The rebuild fails (a malformed gamelist): the folder is scanned for the
# session, still at the same row, and the damaged cache is left as it was.
sd = recovery_sd('scan', xml='<gameList><game><path>broken')
cache = (sd / 'Roms/REC/REC_cache6.db').read_bytes()
steps, scanned = recovery_run(sd, 'scanned', 'U')
assert steps == ['reloading', 'rebuilding the cache', 'scanning the folder'], steps
assert (sd / 'Roms/REC/REC_cache6.db').read_bytes() == cache
plain = recovery_sd('plain', cache=False)
assert recovery_run(plain, 'plain', 'U')[1] == scanned
# A failure that is not damaged content (here SQLITE_IOERR for every later
# page) never replaces the cache: reload, then scan, at the same row.
sd = recovery_sd('io-error', cache=True)
with closing(sqlite3.connect(sd / 'Roms/REC/REC_cache6.db')) as conn, conn:
    conn.execute("UPDATE REC_roms SET path='./Game 069.nes' WHERE disp='Game 069'")
cache = (sd / 'Roms/REC/REC_cache6.db').read_bytes()
libraries = subprocess.run(['ldd', str(ui)], check=True, capture_output=True, text=True)
asan = next((line.split('=>', 1)[1].split()[0] for line in libraries.stdout.splitlines()
             if 'libasan.so' in line), '')
io_env = dict(os.environ, LD_PRELOAD=' '.join(filter(
    None, (asan, os.environ.get('LD_PRELOAD'), str(BUILD / 'sqlite-ioerr.so')))))
steps, scanned = recovery_run(sd, 'scanned', 'U', io_env)
assert steps == ['reloading', 'scanning the folder'], steps
assert (sd / 'Roms/REC/REC_cache6.db').read_bytes() == cache
assert recovery_run(plain, 'plain-io', 'U')[1] == scanned
# An unfinished ROM deletion (here a journal that cannot be read beside a
# staged ROM, so it cannot be recovered) is never set aside by this automatic
# recovery: the rebuild only tries to recover it, no scan bypasses it, and the
# list is left with a message. Refresh roms, asked for, may still abandon it.
sd = recovery_sd('pending-delete')
journal = sd / 'Roms/REC/REC_cache6.db.delete.json'
journal.write_text('{')
staged = sd / 'Roms/REC/Game 001.nes.mainui-delete'
staged.write_bytes(b'rom')
cache = (sd / 'Roms/REC/REC_cache6.db').read_bytes()
steps, left = recovery_run(sd, 'pending', 'U')
assert steps == ['reloading', 'rebuilding the cache'], steps
assert journal.read_text() == '{'
assert (sd / 'Roms/REC/REC_cache6.db').read_bytes() == cache
assert left != recovery_run(sd, 'pending-list', '')[1]  # left the list
assert journal.read_text() == '{' and staged.exists()
# Back (C cancels the running step) leaves the list; it reopens at its top.
sd = recovery_sd('cancel')
steps, cancelled = recovery_run(sd, 'cancelled', 'UC')
assert steps == ['reloading'], steps
assert recovery_run(sd, 'grid', 'B')[1] == cancelled
assert recovery_run(sd, 'reopened', 'UCE')[1] == recovery_run(sd, 'top', '')[1]
print('Cache checks passed: 10 C scenarios, 13002-row paging, 5 SDL captures, page recovery, unchanged input databases')
