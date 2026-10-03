# SPDX-License-Identifier: GPL-3.0-only
"""List windows stay put when Favorites change underneath them.

Every check compares a screenshot after an edit with one reached by plain
navigation to the same row of the same, already edited, files.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="favorite-views-", dir=os.environ.get("MAINUI_FAT_ROOT", BUILD)))
ROM = "/mnt/SDCARD/Roms/FC/{:02}.nes"


def write_lines(path, rows):
    path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


def fixture(name, games, folders=(), assignments=()):
    sd = OUT / name
    for directory in ("Emu", "Roms", ".tmp_update/config"):
        (sd / directory).mkdir(parents=True)
    (sd / ".tmp_update/config/main-menu.json").write_text('{"menu":["favorites"]}')
    write_lines(sd / "Roms/favourite.json",
                [dict(label=f"Game {i:02}", rompath=ROM.format(i), type=5) for i in range(games)])
    write_lines(sd / "Roms/favourite-folders.json",
                [dict(schema=1, generation=0)] +
                [dict(kind="folder", id=folder[0], parent=folder[1], name=folder[2],
                      order=folder[3] if len(folder) > 3 else order)
                 for order, folder in enumerate(folders)] +
                [dict(kind="item", key=ROM.format(game), type=5, folder=fid, order=order)
                 for order, (game, fid) in enumerate(assignments)])
    return sd


def capture(sd, name, actions, text=None):
    path = OUT / f"{sd.name}-{name}.bmp"
    command = [str(BUILD / "MainUI-dev"), "--sd-root", str(sd), "--theme", str(ONION_THEME),
               "--input", actions, "--snapshot", str(path)]
    if text is not None:
        command += ["--text", text]
    subprocess.run(command, cwd=ROOT, check=True, timeout=30)
    return path.read_bytes()


def sidecar(sd):
    return [json.loads(line) for line in
            (sd / "Roms/favourite-folders.json").read_text().splitlines()][1:]


# 1. Creating the first folder inside an empty folder keeps ".." in view.
sd = fixture("first-child", 2, [("f", "", "Folder")])
after = capture(sd, "created", "EESET", "Child")  # open Folder; on "..": Create, OK
assert any(row.get("name") == "Child" for row in sidecar(sd))
assert capture(sd, "navigated", "EED") == after

# 2. An edit inside a folder keeps the parent's place: Back returns to the row
# and window it was entered from. Root: twelve folders, the eleventh holds a
# subfolder "Kid" and two games; root's window has scrolled to reach it.
FOLDERS = [(f"f{i:02}", "", f"Folder {i:02}") for i in range(12)] + [("kid", "f10", "Kid")]
INSIDE = [(0, "f10"), (1, "f10")]
OPEN = "E" + "D" * 10 + "E"  # rows inside: "..", Kid, Game 00, Game 01
EDITS = {
    "create": ("SET", "New", lambda rows: any(r.get("name") == "New" for r in rows)),
    "rename": ("DSDDET", "Renamed", lambda rows: any(r.get("name") == "Renamed" for r in rows)),
    "delete": ("DSDDDE", None, lambda rows: not any(r.get("id") == "kid" for r in rows)),
    "sort": ("DDDSDDE", None, lambda rows: True),
    "remove": ("DDDSDDDE", None, lambda rows: True),
}
for name, (keys, text, done) in EDITS.items():
    sd = fixture("parent-" + name, 4, FOLDERS, INSIDE)
    before = (sd / "Roms/favourite-folders.json").read_bytes(), (sd / "Roms/favourite.json").read_bytes()
    back = capture(sd, "back", OPEN + keys + "B", text)
    after = (sd / "Roms/favourite-folders.json").read_bytes(), (sd / "Roms/favourite.json").read_bytes()
    assert after != before or name == "sort", (name, "the edit did not happen")
    assert done(sidecar(sd)), name
    assert capture(sd, "navigated", "E" + "D" * 10) == back, name

# The same two levels down: edit inside Kid, then Back twice.
sd = fixture("grandparent", 4, FOLDERS, INSIDE)
once = capture(sd, "back-once", OPEN + "DE" + "SET" + "B", "Deep")
assert any(row.get("name") == "Deep" for row in sidecar(sd))
assert capture(sd, "navigated-once", OPEN + "D") == once
sd = fixture("grandparent-twice", 4, FOLDERS, INSIDE)
twice = capture(sd, "back-twice", OPEN + "DE" + "SET" + "BB", "Deep")
assert capture(sd, "navigated-twice", "E" + "D" * 10) == twice

# The same with folders shown in another order than the sidecar stores them
# (after Move/Paste or Sort): Back must find the folder by its displayed row.
# Root: twelve folders with descending order values and a tie at 3 (stable,
# so Folder 09 shows before Folder 10). Folder 10 holds Zed, Kid, Mid in that
# file order, shown as Kid, Mid, Zed.
ROOT_ORDERS = [11, 10, 9, 8, 7, 6, 5, 4, 2, 3, 3, 0]
REORDERED = [(f"f{i:02}", "", f"Folder {i:02}", ROOT_ORDERS[i]) for i in range(12)] + [
    ("zed", "f10", "Zed", 2), ("kid", "f10", "Kid", 0), ("mid", "f10", "Mid", 1)]
shown = sorted(range(12), key=lambda i: (ROOT_ORDERS[i], i))
TARGET = shown.index(10)
assert TARGET != 10 and shown[TARGET - 1] == 9  # differs from file order; tie
TO_TARGET = "E" + "D" * TARGET
sd = fixture("reordered-parent", 0, REORDERED)
back = capture(sd, "back", TO_TARGET + "E" + "SET" + "B", "New")
assert any(row.get("name") == "New" for row in sidecar(sd))
assert capture(sd, "navigated", TO_TARGET) == back
# Two levels: edit inside Kid (row 1 after ".."; file position 2), Back twice.
sd = fixture("reordered-once", 0, REORDERED)
once = capture(sd, "back-once", TO_TARGET + "E" + "DE" + "SET" + "B", "Deep")
assert any(row.get("name") == "Deep" for row in sidecar(sd))
assert capture(sd, "navigated-once", TO_TARGET + "E" + "D") == once
sd = fixture("reordered-twice", 0, REORDERED)
twice = capture(sd, "back-twice", TO_TARGET + "E" + "DE" + "SET" + "BB", "Deep")
assert capture(sd, "navigated-twice", TO_TARGET) == twice

# 3. Removing the last Favorite of a long list closes the gap below it.
sd = fixture("remove-last", 20)
after = capture(sd, "removed", "EU" + "SDDDE")
assert len((sd / "Roms/favourite.json").read_text().splitlines()) == 19
assert capture(sd, "navigated", "EU") == after
print("Favorite edits keep list windows and parent positions:", OUT)
