# SPDX-License-Identifier: GPL-3.0-only
"""Committed-game Recent persistence without a theme or display."""
import json
import subprocess
from env import BUILD

sd = BUILD / "recent-write-checks"
roms = sd / "Roms"
roms.mkdir(parents=True, exist_ok=True)
path = roms / "recentlist.json"

def game(index, **extra):
    return dict(label=f"Game {index}", rompath=f"/mnt/SDCARD/Roms/FC/{index}.nes",
                launch="/mnt/SDCARD/Emu/FC/launch.sh", type=5, **extra)

def write(rows):
    path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")

def read():
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]

def add(record, success=True):
    result = subprocess.run([str(BUILD / "fixture-recent"), str(sd), json.dumps(record)],
                            check=False, timeout=20)
    assert result.returncode == (0 if success else 1)

path.unlink(missing_ok=True)
add(game(1, imgpath="/mnt/SDCARD/Roms/missing.png"))
assert read() == [game(1, imgpath="/mnt/SDCARD/Roms/missing.png")]
write([game(i) for i in range(70)])
add(game(30))
rows = read()
assert len(rows) == 50 and rows[0] == game(30)
assert sum(row["rompath"] == game(30)["rompath"] for row in rows) == 1
assert rows[1] == game(0)
before = path.read_bytes()
add(dict(label="App", rompath="/mnt/SDCARD/App/Test/launch.sh",
         launch="/mnt/SDCARD/App/Test/launch.sh", type=3))
add(dict(game(1), app_action=True))
assert path.read_bytes() == before
search = game(30)
search["rompath"] = search["launch"] + ":" + search["rompath"]
search["launch"] = "/mnt/SDCARD/App/Search/launch.sh"
add(search)
assert read()[0] == search and len(read()) == 50
art = roms / "art.png"
art.write_bytes(b"artwork-existence-fixture")
add(game(80, imgpath="/mnt/SDCARD/Roms/art.png"))
assert read()[0]["imgpath"] == "/mnt/SDCARD/Roms/art.png"
spelling = dict(game(81), rompath="/mnt/SDCARD/Roms/FC/../FC/Mixed Case.nes")
add(spelling)
assert read()[0]["rompath"] == spelling["rompath"]
stock = dict(game(90), rompath='/mnt/SDCARD/Emu/FC/../../Roms/FC/90.nes',
             imgpath='/mnt/SDCARD/Emu/FC/../../Roms/FC/Imgs/90.png', extra='preserved')
write([game(91), stock])
add(stock)
assert read() == [stock, game(91)]
write([dict(game(i), type=3) for i in range(200)] + [game(999)])
add(game(82))
assert read() == [game(82)]
path.write_text('{"broken":\n', encoding="utf-8")
before = path.read_bytes()
add(game(83), success=False)
assert path.read_bytes() == before

# One ROM under two emulators is two Recents. Removing or relaunching one
# keeps the other (review of 1.0.2, finding 1).
def variant(name, label):
    return dict(label=label, rompath="/mnt/SDCARD/Roms/PS/game.pbp",
                launch=f"/mnt/SDCARD/Emu/{name}/launch.sh", type=5)

def remove(index, success=True):
    result = subprocess.run([str(BUILD / "fixture-recent"), str(sd), "remove", str(index)],
                            check=False, timeout=20)
    assert result.returncode == (0 if success else 1)

a, b = variant("PS-A", "Game A"), variant("PS-B", "Game B")
first = json.dumps(a) + "\n"
path.write_text(first + json.dumps(b) + "\n", encoding="utf-8")
remove(1)
assert path.read_text(encoding="utf-8") == first
path.write_text(first + json.dumps(b) + "\n", encoding="utf-8")
remove(0)
assert read() == [b]
write([a, b])
add(b)
assert read() == [b, a]
add(a)
assert read() == [a, b]
# Search's prefix names the same launcher, so it still replaces that variant only.
searched = dict(a, label="Searched", launch="/mnt/SDCARD/App/Search/launch.sh",
                rompath=a["launch"] + ":" + a["rompath"])
add(searched)
assert read() == [searched, b]
write([searched, b])
remove(0)
assert read() == [b]
write([b, searched])
remove(1)
assert read() == [b]

# Restoring a selection (after a launch, or a background reload of the list)
# finds the selected entry by the list's own identity (review of 1.0.3).
def restored(section, index):
    result = subprocess.run([str(BUILD / "fixture-recent"), str(sd), "restore", section,
                             str(index)], check=True, timeout=20, capture_output=True, text=True)
    return int(result.stdout)

write([a, b])
assert [restored("recents", i) for i in range(2)] == [0, 1]
write([b, searched])  # Search's spelling of A's launcher stays distinct from B
assert [restored("recents", i) for i in range(2)] == [0, 1]

favourites = sd / "Roms/favourite.json"
saved_favourites = favourites.read_bytes() if favourites.exists() else None
sidecar = sd / "Roms/favourite-folders.json"
saved_sidecar = sidecar.read_bytes() if sidecar.exists() else None
# Two Favorites without a ROM (apps), told apart by launcher and label.
apps = [dict(label=f"App {i}", rompath="", launch=f"/mnt/SDCARD/App/a{i}/launch.sh", type=5)
        for i in range(2)]
favourites.write_text("".join(json.dumps(app) + "\n" for app in apps), encoding="utf-8")
sidecar.unlink(missing_ok=True)
assert [restored("favorites", i) for i in range(2)] == [0, 1]
# One ROM as two Favorites (different label and launcher): both are listed,
# and each restores to its own row.
same = [dict(label=label, rompath="/mnt/SDCARD/Roms/FC/same.nes",
             launch=f"/mnt/SDCARD/Emu/{emu}/launch.sh", type=5)
        for label, emu in (("Label A", "FC-A"), ("Label B", "FC-B"))]
favourites.write_text("".join(json.dumps(game) + "\n" for game in same), encoding="utf-8")
assert [restored("favorites", i) for i in range(2)] == [0, 1]
# A selected folder keeps its place even when an empty-ROM Favorite is listed.
favourites.write_text("".join(json.dumps(app) + "\n" for app in apps), encoding="utf-8")
sidecar.write_text(json.dumps(dict(schema=1, generation=0)) + "\n" +
                   json.dumps(dict(kind="folder", id="f", parent="", name="Folder",
                                   order=0)) + "\n", encoding="utf-8")
assert restored("favorites", 0) == 0  # Folder, then App 0, App 1
assert restored("favorites", 2) == 2
for path, saved in ((favourites, saved_favourites), (sidecar, saved_sidecar)):
    if saved is None:
        path.unlink(missing_ok=True)
    else:
        path.write_bytes(saved)
print("Recent writer scenarios passed")
