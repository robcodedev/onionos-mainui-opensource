# SPDX-License-Identifier: GPL-3.0-only
"""Verify screen/catalog boundaries on isolated SD data and actual SDL frames."""
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import shutil
import tempfile
from env import unlink_if_exists, BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()

ROOT = Path(__file__).resolve().parents[2]
SD = Path(tempfile.mkdtemp(prefix="boundaries-", dir=BUILD))
THEME = ONION_THEME
EXE = str(BUILD / "MainUI-dev")


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def config(directory, label, rom, **extra):
    write(SD / directory / "config.json", json.dumps(dict(
        label=label, rompath=rom, launch="launch.sh", extlist="nes", **extra)))


def capture(name, actions="", *options, theme=THEME):
    unlink_if_exists((SD / "appconfigs/romwinidx.json"))
    target = SD / (name + ".bmp")
    subprocess.run([EXE, "--sd-root", str(SD), "--theme", str(theme), "--fallback", str(THEME),
                    "--snapshot", str(target), "--input", actions, *options],
                   cwd=ROOT, check=True, timeout=30)
    return target.read_bytes()


(SD / "miyoo/app").mkdir(parents=True)
shutil.copyfile(THEME / "py.dat", SD / "miyoo/app/py.dat")

config("Emu/Normal", "Normal", "../../Roms/Normal", shortname=1)
for name in ("alpha.nes", "zulu.nes"):
    write(SD / "Roms/Normal" / name, "")
(SD / "Roms/Normal/Empty").mkdir()
for index in range(70):
    write(SD / f"Roms/Normal/Folder/nested{index:02}.nes", "")
for index in range(10):
    config(f"RApp/{index:02}", f"Expert {index:02}", f"../../Roms/Expert{index:02}")
    write(SD / f"Roms/Expert{index:02}/one.nes", "")
config("RApp/Hidden", "Hidden", "../../Roms/Normal", hide=1)
write(SD / "RApp/NoConfig/ignored", "")
write(SD / "RApp/Invalid/config.json", "{broken")
write(SD / "BIOS/arcade_lists/arcade-rom-names.txt",
      'alpha "Old"\n\nafterblank "After blank"\nalpha "Alpha final"\nALPHA "Upper"\nempty ""\n')
write(SD / ".tmp_update/config/main-menu.json", json.dumps({"menu": {
    "games": True, "expert": True, "favorites": True, "settings": True}}))
write(SD / "Roms/Normal/gamelist.xml",
      '<gameList><game id="1"><path>./ALPHA.nes</path><genre>Action &amp; Arcade</genre>'
      '<rating>0.45</rating><desc><![CDATA[CafÃ© <test> ' + 'Description text. ' * 160 +
      ']]></desc></game><game><path>zulu.nes</path></game></gameList>')
write(SD / "Roms/Bad/gamelist.xml", '<gameList><game><path>bad.nes</path><genre>Stale</genre></game>')
(SD / "MapMissing/BIOS/arcade_lists").mkdir(parents=True)
write(SD / "MapLarge/BIOS/arcade_lists/arcade-rom-names.txt",
      ''.join(f'key{i} "' + 'Long name ' * 7 + '"\n' for i in range(66000)) +
      'tail "Large map tail"\n  leading "Whitespace"\n')
for i in range(17):
    records = 32768 if i < 6 else 2
    write(SD / f"Metadata/{i}/gamelist.xml", '<gameList>' +
          '<game><path>target.nes</path><genre>Indexed</genre></game>' +
          ''.join(f'<game><path>other{j}.nes</path></game>' for j in range(records - 1)) +
          '</gameList>')
write(SD / "Metadata/TooMany/gamelist.xml", '<gameList>' +
      '<game><path>target.nes</path></game>' * 32769 + '</gameList>')
write(SD / "Metadata/LargeRecord/gamelist.xml", '<gameList><game><path>target.nes</path>' +
      '<ignored>' + 'x' * 33000 + '</ignored></game></gameList>')
subprocess.run([str(BUILD / "fixture-boundaries"), str(SD)], cwd=ROOT, check=True)
normal = capture("normal", "EEDD")
assert normal == capture("parent-roundtrip", "EEDEDBD")
# Root has two folders; details must wrap over games and clear the old metadata.
detail = capture("detail", "EEDDR")
assert detail != normal
assert detail != capture("detail-next", "EEDDRD")
assert detail == capture("detail-wrap", "EEDDRDD")
assert normal == capture("detail-back", "EEDDRB")
assert detail != capture("description-page", "EEDDR4")
assert detail == capture("description-return", "EEDDR43")
expert = capture("expert", "RE")
assert expert != capture("games-grid", "E")
assert expert != capture("expert-last-page", "REDD")
assert capture("expert-last-page", "REDD") == capture("expert-return", "REDDEB")
assert capture("games-grid", "E") == capture("section-return", "REB LE".replace(" ", ""))
record = dict(label="Different saved label", rompath="/mnt/SDCARD/Emu/Normal/../../Roms/Normal/alpha.nes",
              launch="/mnt/SDCARD/Emu/Normal/launch.sh", type=5)
write(SD / "Roms/favourite.json", json.dumps(record) + "\n")
marked = capture("marked", "EEDD")
assert marked != normal
stock_path = record["rompath"]
record["rompath"] = "/mnt/SDCARD/Roms/Normal/alpha.nes"
write(SD / "Roms/favourite.json", json.dumps(record) + "\n")
# One game, one identity: the tidied spelling of the same ROM (as Search
# records it) marks the console row too. Stock compared the spelling.
assert capture("tidied-path-match", "EEDD") == marked
record["rompath"] = stock_path
write(SD / "Roms/favourite.json", json.dumps(record) + "\n")
assert capture("marked-parent", "EEDE") == capture("marked-parent-again", "EEDE")
# Missing ROMs in the saved list cannot mark another game's equal display label.
record["rompath"] = "/mnt/SDCARD/Emu/Normal/../../Roms/Normal/not-alpha.nes"
write(SD / "Roms/favourite.json", json.dumps(record) + "\n")
assert capture("wrong-path", "EEDD") == normal
write(SD / "Roms/favourite.json", "")
added = capture("add-marker", "EEDDSDE")
assert added != normal
assert capture("marker-reopen", "EEDD") == added
assert capture("remove-marker", "EEDDSDE") == normal
assert capture("marker-removed-reopen", "EEDD") == normal
with closing(sqlite3.connect(SD / "Roms/Normal/Normal_cache6.db")) as database:
    assert database.execute("select disp,pinyin,cpinyin from Normal_roms where path='/mnt/SDCARD/Emu/Normal/../../Roms/Normal/alpha.nes'").fetchone() == (
        "Alpha final", "Alpha final", "Alpha final")
    assert {row[1] for row in database.execute("pragma table_info(Normal_roms)")} == {
        "id", "disp", "path", "imgpath", "type", "ppath", "pinyin", "cpinyin"}
# Explicit XML labels override name lookup during a later rebuild.
write(SD / "Roms/Normal/miyoogamelist.xml", '<gameList><game><path>alpha.nes</path><name>XML title</name></game></gameList>')
capture("xml-precedence", "", "--refresh-caches", "--system", "Normal")
with closing(sqlite3.connect(SD / "Roms/Normal/Normal_cache6.db")) as database:
    assert database.execute("select disp from Normal_roms").fetchall() == [("XML title",)]
# Both scan and XML rebuilds store stock initials, leaving display labels intact.
config("Emu/Pinyin", "Pinyin", "../../Roms/Pinyin", shortname=0)
write(SD / "Roms/Pinyin/超级马里奥.nes", "")
capture("pinyin-scan", "", "--refresh-caches", "--system", "Pinyin")
def pinyin_row():
    with closing(sqlite3.connect(SD / "Roms/Pinyin/Pinyin_cache6.db")) as database:
        return database.execute("select disp,pinyin,cpinyin from Pinyin_roms").fetchone()
assert pinyin_row() == ("超级马里奥", "cjmla", "cjmla")
write(SD / "Roms/Pinyin/miyoogamelist.xml", '<gameList><game>'
      '<path>超级马里奥.nes</path><name>拳皇97 &amp; 重行乐</name></game></gameList>')
capture("pinyin-xml", "", "--refresh-caches", "--system", "Pinyin")
assert pinyin_row() == ("拳皇97 & 重行乐", "qh97 & zxl", "qh97 & zxl")
(SD / "miyoo/app/py.dat").unlink()
capture("pinyin-missing-dictionary", "", "--refresh-caches", "--system", "Pinyin")
assert pinyin_row() == ("拳皇97 & 重行乐", "  97 &    ", "  97 &    ")
shutil.copyfile(THEME / "py.dat", SD / "miyoo/app/py.dat")
# Child style fallbacks must equal spelling out the inherited title fields.
active = SD / "Theme"
style = {"font": str(THEME / "Exo-2-Bold-Italic.ttf"), "size": 27, "color": "#22cc44"}
write(active / "config.json", json.dumps({"title": style, "hideIconTitle": True, "hideLabels": {"icons": False}}))
inherited = capture("theme-inherited", "EE", theme=active)
write(active / "config.json", json.dumps({"title": style, "list": style, "hint": style, "hideLabels": {"icons": False, "hints": False}}))
assert inherited == capture("theme-explicit", "EE", theme=active)
print("Boundary fixtures passed:", SD)
