# SPDX-License-Identifier: GPL-3.0-only
"""Returning from Onion's Search opens Games -> Search with its results (#11).

Onion's X shortcut (keymon) starts Search by killing MainUI, so no return
envelope exists; Search then leaves Games -> Search in the stock state.json.
Starting Search from MainUI's own menus writes an envelope instead.
"""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="search-return-", dir=BUILD))
UI = os.environ.get("MAINUI_TEST_EXE", str(BUILD / "MainUI-dev"))
# Onion's res/search_state.json, as Search writes it to /tmp/state.json.
SEARCH_STATE = {"list": [
    {"title": 157, "type": 0, "currpos": 1, "pagestart": 0, "pageend": 3},
    {"title": 2, "type": 1, "currpos": 0, "pagestart": 0, "pageend": 7},
    {"title": -1, "type": 5, "currpos": 0, "pagestart": 0, "pageend": 5, "emuname": " Search "}]}
MAIN_MENU = {"list": [{"title": 0, "type": 0, "currpos": 1, "pagestart": 0, "pageend": 3}]}


def sd_card(name):
    sd = OUT / name / "sd"
    (sd / "App/Search").mkdir(parents=True)
    (sd / "App/Search/config.json").write_text(json.dumps(dict(label="Search", launch="launch.sh")))
    (sd / "App/Search/launch.sh").write_text("#!/bin/sh\n")
    for i in range(3):
        (sd / f"Emu/C{i}").mkdir(parents=True)
        (sd / f"Emu/C{i}/config.json").write_text(json.dumps(dict(
            label=f"Console {i}", rompath=f"../../Roms/C{i}", launch="launch.sh", extlist="nes")))
        (sd / f"Roms/C{i}").mkdir(parents=True)
        (sd / f"Roms/C{i}/Game.nes").write_bytes(b"rom")
    (sd.parent / "handoff").mkdir()
    return sd


def search_ran(sd):
    """What Search leaves behind: its console and a results database."""
    (sd / "Emu/SEARCH").mkdir(parents=True, exist_ok=True)
    (sd / "Emu/SEARCH/config.json").write_text(json.dumps({
        "label": " Search ", "launch": "../../App/Search/launch.sh",
        "rompath": "../../App/Search/data", "imgpath": "../../App/Search/data/Imgs",
        "extlist": "miyoocmd"}))
    (sd / "App/Search/data").mkdir(parents=True, exist_ok=True)
    database = sd / "App/Search/data/data_cache6.db"
    database.unlink(missing_ok=True)
    rows = [("Console 0 (1)", "/mnt/SDCARD/Roms/C0", 1, "."),
            ("Game", "/mnt/SDCARD/Emu/C0/launch.sh:/mnt/SDCARD/Roms/C0/Game.nes", 0,
             "Console 0 (1)"),
            ("Clear search", "clear", 0, "."), ("Search: game", "search", 0, ".")]
    connection = sqlite3.connect(database)
    connection.execute("CREATE TABLE data_roms (id INTEGER PRIMARY KEY AUTOINCREMENT, disp TEXT "
                       "NOT NULL, path TEXT NOT NULL, imgpath TEXT NOT NULL, type INTEGER DEFAULT "
                       "0, ppath TEXT NOT NULL, pinyin TEXT NOT NULL, cpinyin TEXT NOT NULL)")
    connection.executemany("INSERT INTO data_roms (disp,path,imgpath,type,ppath,pinyin,cpinyin) "
                           "VALUES (?,?,'',?,?,?,'')", [row + (row[0],) for row in rows])
    connection.commit()
    connection.close()


def run(sd, name, *options):
    shot = sd.parent / f"{name}.bmp"
    result = subprocess.run([UI, "--sd-root", str(sd), "--theme", str(ONION_THEME),
                             "--snapshot", str(shot)] + list(options),
                            cwd=ROOT, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (name, result.stderr[-400:])
    return shot.read_bytes()


def restart(sd, name):
    return run(sd, name, "--handoff-dir", str(sd.parent / "handoff"))


def search_list(sd):
    return run(sd, "reference", "--systems", "--input", "E")  # Search sorts first


# 1. X shortcut (keymon killed MainUI): no envelope, Search's stock state.
sd = sd_card("keymon")
home = restart(sd, "home")
search_ran(sd)
(sd.parent / "handoff/state.json").write_text(json.dumps(SEARCH_STATE))
assert restart(sd, "after-search") == search_list(sd)
# Clear search: Search removes its console and restores the earlier state.
(sd / "Emu/SEARCH/config.json").unlink()
(sd.parent / "handoff/state.json").write_text(json.dumps(MAIN_MENU))
assert restart(sd, "after-clear") == home

# 2. A stock state naming another console is not taken for Search.
sd = sd_card("other-console")
search_ran(sd)
other = dict(SEARCH_STATE, list=SEARCH_STATE["list"][:2] + [dict(SEARCH_STATE["list"][2],
                                                                  emuname="Console 1")])
(sd.parent / "handoff/state.json").write_text(json.dumps(other))
assert restart(sd, "other") == run(sd, "plain")

# 3. Search started from MainUI's menu (an envelope), once before any search
# and once with Search already present: Games -> Search either way.
for name, present in (("menu-first", False), ("menu-again", True)):
    sd = sd_card(name)
    if present:
        search_ran(sd)
    handoff = sd.parent / "handoff"
    run(sd, "launch", "--handoff-dir", str(handoff), "--input", "SDE")  # Home menu: Search
    assert (handoff / "mainui-return.json").exists(), name
    (handoff / "cmd_to_run.sh").unlink()  # the runtime ran it
    search_ran(sd)
    (handoff / "state.json").write_text(json.dumps(SEARCH_STATE))
    assert restart(sd, "after") == search_list(sd), name

# 4. Search is recognised by its data folder, not its label: a renamed
# Search console works the same, as long as the state names it.
sd = sd_card("renamed")
search_ran(sd)
config = json.loads((sd / "Emu/SEARCH/config.json").read_text())
(sd / "Emu/SEARCH/config.json").write_text(json.dumps(dict(config, label="Results")))
renamed = dict(SEARCH_STATE, list=SEARCH_STATE["list"][:2] + [dict(SEARCH_STATE["list"][2],
                                                                    emuname="Results")])
(sd.parent / "handoff/state.json").write_text(json.dumps(renamed))
reference = run(sd, "reference-renamed", "--systems", "--input", "RRRE")  # after Console 0-2
assert restart(sd, "after-renamed") == reference
print("Search returns to Games -> Search by every route; Clear search does not")
