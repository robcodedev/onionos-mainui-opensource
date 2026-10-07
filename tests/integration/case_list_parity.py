# SPDX-License-Identifier: GPL-3.0-only
"""Verify actual fixture configuration, list counters, empty art and shoulder navigation."""
from contextlib import closing
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile

from PIL import Image
from env import BUILD, ONION_THEME, require_fixture_sd, require_onion_theme  # noqa: E402

require_onion_theme()

FIXTURE_SD = require_fixture_sd()

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="list-parity-", dir=BUILD))
FALLBACK = ONION_THEME
THEME = OUT / "theme"
(THEME / "skin").mkdir(parents=True)
(THEME / "config.json").write_bytes((FALLBACK / "config.json").read_bytes())
Image.new("RGB", (640, 480), (0, 0, 0)).save(THEME / "skin/background.png")
Image.new("RGB", (640, 36), (180, 20, 50)).save(THEME / "skin/bg-list-s_10.png")
Image.new("RGB", (100, 80), (30, 210, 80)).save(THEME / "skin/Empty.png")


def make_sd(name, labels, folder=False, fixture_config=False):
    sd = OUT / name
    for directory in ("Emu/FC", "Roms/FC", ".tmp_update/config"):
        (sd / directory).mkdir(parents=True)
    emulator = dict(label="NES", rompath="../../Roms/FC", extlist="nes")
    (sd / "Emu/FC/config.json").write_text(json.dumps(emulator))
    for label in labels:
        (sd / "Roms/FC" / (label + ".nes")).write_bytes(b"")
    if folder:
        (sd / "Roms/FC/Folder").mkdir()
        (sd / "Roms/FC/Folder/Inner.nes").write_bytes(b"")
    if fixture_config:
        source = FIXTURE_SD / ".tmp_update/config"
        for filename in (".romListRows", ".romListTitleScroll", ".romListFontSize"):
            shutil.copyfile(source / filename, sd / ".tmp_update/config" / filename)
    return sd


def capture(sd, name, actions="", elapsed=0):
    output = OUT / (name + ".bmp")
    executable = str(BUILD / "MainUI-dev")
    subprocess.run([
        executable, "--sd-root", str(sd), "--theme", str(THEME),
        "--fallback", str(FALLBACK), "--system", "NES", "--input", actions,
        "--elapsed", str(elapsed), "--snapshot", str(output),
    ], cwd=ROOT, check=True, timeout=20)
    return Image.open(output).convert("RGB")


labels = ["Alpha", "Alpine", "Beta", "Bravo", "Charlie"]
plain = make_sd("plain", labels)
folders = make_sd("folders", labels, folder=True)
# The first ROM after a physical folder must still display 1/5.
first = capture(plain, "first")
folder_first = capture(folders, "folder-first-rom", "D")
assert first.crop((500, 420, 640, 480)).tobytes() == folder_first.crop((500, 420, 640, 480)).tobytes()
last = capture(plain, "last", "U")
folder_last = capture(folders, "folder-last-rom", "U")
assert last.crop((500, 420, 640, 480)).tobytes() == folder_last.crop((500, 420, 640, 480)).tobytes()
# A folder with nothing below it is no longer added to a cache, but one can
# still be in a cache built by an older version: its page shows Empty.png.
with closing(sqlite3.connect(folders / "Roms/FC/FC_cache6.db")) as database:
    database.execute("DELETE FROM FC_roms WHERE ppath='Folder'")
    database.commit()
empty = capture(folders, "empty-folder", "E")
assert empty.getpixel((270, 200)) == (30, 210, 80)
assert empty.getpixel((369, 279)) == (30, 210, 80)
assert empty.getpixel((269, 199)) == (0, 0, 0)
# 1/2 are L1/R1. 3/4 are L2/R2. Chords press the direction first.
assert capture(plain, "next-letter", "2").tobytes() == capture(plain, "beta", "DD").tobytes()
assert capture(plain, "previous-wrap", "1").tobytes() == last.tobytes()
assert capture(plain, "end-chord", "D4").tobytes() == last.tobytes()
assert capture(plain, "start-chord", "U3").tobytes() == first.tobytes()
assert capture(plain, "page-last", "4").tobytes() == last.tobytes()
long_labels = ["M" * 70] + [f"Z game {i:02}" for i in range(14)]
configured = make_sd("configured", long_labels, fixture_config=True)
idle = capture(configured, "fixture-idle")
assert idle.getpixel((639, 70)) == (180, 20, 50)
assert idle.getpixel((639, 97)) == (0, 0, 0)
assert idle.crop((0, 60, 640, 96)).tobytes() != capture(configured, "fixture-scroll", elapsed=1100).crop((0, 60, 640, 96)).tobytes()
assert capture(configured, "ten-row-page", "4").tobytes() == capture(configured, "ten-down", "D" * 10).tobytes()
print("Fixture rows/scroll, folder-free counters, Empty.png and shoulder navigation passed:", OUT)

assert capture(plain, "release-repress", "D4-4U3").tobytes() == first.tobytes()
print("Scripted key release/repress preserved modifier-first navigation")
