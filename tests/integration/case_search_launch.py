# SPDX-License-Identifier: GPL-3.0-only
"""A game opened from Onion's Search console is launched, and saved to
Recents, with the ROM as Search spells it (Favorites take the same record),
/mnt/SDCARD/Emu/C/../../Roms/C/..., the spelling ROM lists use. Onion's Game
List Options recognizes a game only by that spelling; the tidied
/mnt/SDCARD/Roms/C/... hid its game options."""
import json
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme, unlink_if_exists  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="search-launch-", dir=BUILD))
SD = OUT / "sd"
HANDOFF = OUT / "handoff"
HANDOFF.mkdir()
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / ".tmp_update/config/main-menu.json").write_text(json.dumps({"menu": dict(games=True)}))
(SD / "Emu/C").mkdir(parents=True)
(SD / "Emu/C/config.json").write_text(json.dumps(dict(
    label="Zed", rompath="../../Roms/C", launch="launch.sh", extlist="nes")))
(SD / "Roms/C").mkdir(parents=True)
(SD / "Roms/C/G00.nes").write_bytes(b"rom")
(SD / "Emu/SEARCH").mkdir(parents=True)
(SD / "Emu/SEARCH/config.json").write_text(json.dumps(dict(
    label=" Search ", rompath="../../App/Search/data", launch="../../App/Search/launch.sh",
    extlist="nes")))
(SD / "App/Search/data").mkdir(parents=True)
ROM = "/mnt/SDCARD/Emu/C/../../Roms/C/G00.nes"
with sqlite3.connect(SD / "App/Search/data/data_cache6.db") as db:
    db.execute("CREATE TABLE 'data_roms' (id INTEGER PRIMARY KEY AUTOINCREMENT,disp TEXT NOT "
               "NULL,path TEXT NOT NULL,imgpath TEXT NOT NULL,type INTEGER DEFAULT 0,ppath TEXT "
               "NOT NULL,pinyin TEXT NOT NULL,cpinyin TEXT NOT NULL)")
    db.execute("INSERT INTO data_roms (disp,path,imgpath,type,ppath,pinyin,cpinyin) VALUES "
               "('Zed (1)','/mnt/SDCARD/Roms/C','/mnt/SDCARD/Roms/C',1,'.','Zed (1)','')")
    db.execute("INSERT INTO data_roms (disp,path,imgpath,type,ppath,pinyin,cpinyin) VALUES "
               "('G00',?,'/mnt/SDCARD/Emu/C/../../Roms/C/Imgs/G00.png',0,'Zed (1)','G00','')",
               ("/mnt/SDCARD/Emu/C/launch.sh:" + ROM,))
RECENT = SD / "Roms/recentlist.json"


def launch(actions):
    unlink_if_exists(HANDOFF / "cmd_to_run.sh")
    unlink_if_exists(HANDOFF / "mainui-return.json")
    unlink_if_exists(RECENT)
    subprocess.run([str(BUILD / "MainUI-dev"), "--sd-root", str(SD), "--theme", str(ONION_THEME),
                    "--system", " Search ", "--input", actions, "--snapshot", str(OUT / "shot.bmp"),
                    "--handoff-dir", str(HANDOFF)],
                   cwd=ROOT, check=True, timeout=30, capture_output=True)
    unlink_if_exists(SD / "appconfigs/romwinidx.json")
    command = (HANDOFF / "cmd_to_run.sh").read_text()
    return command, json.loads(RECENT.read_text().splitlines()[0])


# In the Search console: its console folder, then the game below "..".
for key in "AY":
    command, recent = launch("ED" + key)
    assert f'"/mnt/SDCARD/Emu/C/launch.sh" "{ROM}"' in command, command
    assert recent["rompath"] == ROM and recent["launch"] == "/mnt/SDCARD/Emu/C/launch.sh", recent
print("Search results launch and save the ROM as Search spells it")
