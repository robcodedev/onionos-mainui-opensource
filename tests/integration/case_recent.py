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
# A damaged list no longer stops Recents: the original is kept once.
path.write_text('{"broken":\n', encoding="utf-8")
before = path.read_bytes()
add(game(83))
assert read() == [game(83)]
assert (path.parent / "recentlist.json.damaged").read_bytes() == before
(path.parent / "recentlist.json.damaged").unlink()

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

# Restoring a selection (after a launch, or a reload of the list)
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
sidecar_backup = sidecar.parent / "favourite-folders.json.bak"
sidecar.unlink(missing_ok=True)
sidecar_backup.unlink(missing_ok=True)  # the browser would fall back to it
assert [restored("favorites", i) for i in range(2)] == [0, 1]
# One ROM as two Favorites (different label and launcher): both are listed,
# and each restores to its own row.
same = [dict(label=label, rompath="/mnt/SDCARD/Roms/FC/same.nes",
             launch=f"/mnt/SDCARD/Emu/{emu}/launch.sh", type=5)
        for label, emu in (("Label A", "FC-A"), ("Label B", "FC-B"))]
favourites.write_text("".join(json.dumps(game) + "\n" for game in same), encoding="utf-8")
assert [restored("favorites", i) for i in range(2)] == [0, 1]
# Removing either of them removes exactly that row (review of 1.0.3), and
# their shared folder assignment stays until no Favorite of that ROM is left.
def remove_favorite(label):
    return subprocess.run([str(BUILD / "fixture-recent"), str(sd), "remove-favorite", label],
                          timeout=20).returncode
lines = [json.dumps(game) + "\n" for game in same]
assignment = dict(kind="item", key="/mnt/SDCARD/Roms/FC/same.nes", type=5, folder="f",
                  order=0, note="kept")
def setup_same():
    favourites.write_text("".join(lines), encoding="utf-8")
    sidecar.write_text("".join(json.dumps(row) + "\n" for row in (
        dict(schema=1, generation=0),
        dict(kind="folder", id="f", parent="", name="Folder", order=0), assignment)),
        encoding="utf-8")
def assigned():
    return [row for row in map(json.loads, sidecar.read_text().splitlines())
            if row.get("kind") == "item"]
for keep, drop in ((0, 1), (1, 0)):
    setup_same()
    assert remove_favorite(same[drop]["label"]) == 0
    assert favourites.read_text(encoding="utf-8") == lines[keep], (keep, drop)
    assert assigned() == [assignment]  # still used by the survivor, unchanged
assert remove_favorite(same[1]["label"]) == 0  # the last one of that ROM
assert favourites.read_text(encoding="utf-8") == ""
assert assigned() == []
# A selected folder keeps its place even when an empty-ROM Favorite is listed.
favourites.write_text("".join(json.dumps(app) + "\n" for app in apps), encoding="utf-8")
sidecar.write_text(json.dumps(dict(schema=1, generation=0)) + "\n" +
                   json.dumps(dict(kind="folder", id="f", parent="", name="Folder",
                                   order=0)) + "\n", encoding="utf-8")
assert restored("favorites", 0) == 0  # Folder, then App 0, App 1
assert restored("favorites", 2) == 2
sidecar_backup.unlink(missing_ok=True)
for restored_file, saved in ((favourites, saved_favourites), (sidecar, saved_sidecar)):
    if saved is None:
        restored_file.unlink(missing_ok=True)
    else:
        restored_file.write_bytes(saved)
# Damaged Recents (review of 1.0.3): a confirmed clear always clears, and
# recording a launch skips unreadable lines after keeping the original once.
damaged_copy = path.parent / "recentlist.json.damaged"
for content in (json.dumps(a) + "\n{broken\n", "{broken\n", '[1,2]\n"text"\n',
                json.dumps(a) + "\n\x00\x01binary\n"):
    path.write_bytes(content.encode("utf-8"))
    assert subprocess.run([str(BUILD / "fixture-recent"), str(sd), "clear"],
                          timeout=20).returncode == 0, content
    assert path.read_bytes() == b"", content
damaged_copy.unlink(missing_ok=True)
original = (json.dumps(a) + "\n{broken\n").encode("utf-8")
path.write_bytes(original)
add(b)
assert read() == [b, a]  # the launch is recorded, the readable game kept
assert damaged_copy.read_bytes() == original
path.write_bytes((json.dumps(b) + "\n{other damage\n").encode("utf-8"))
add(a)
assert read() == [a, b] and damaged_copy.read_bytes() == original  # first copy kept
damaged_copy.unlink()

# One game, two spellings: a console list keeps the stock "Emu/FC/../../Roms"
# path while Search normalizes it (with a launcher prefix in its transport).
# Membership and Add must agree on both, so Search sees the Favorite and adding
# it again does not duplicate it.
stock = dict(label="Same", rompath="/mnt/SDCARD/Emu/FC/../../Roms/FC/same.nes",
             launch="/mnt/SDCARD/Emu/FC/launch.sh", type=5)
favourites.write_text(json.dumps(stock) + "\n", encoding="utf-8")
sidecar.unlink(missing_ok=True)
(sidecar.parent / "favourite-folders.json.bak").unlink(missing_ok=True)
def fixture_out(*args):
    return subprocess.run([str(BUILD / "fixture-recent"), str(sd)] + list(args), timeout=20,
                          capture_output=True, text=True)
for spelling in ("/mnt/SDCARD/Roms/FC/same.nes",
                 "/mnt/SDCARD/Emu/FC/launch.sh:/mnt/SDCARD/Roms//FC/./same.nes"):
    assert fixture_out("is-favorite", spelling).stdout.strip() == "yes", spelling
before = favourites.read_bytes()
assert fixture_out("add-favorite", json.dumps(dict(stock, rompath="/mnt/SDCARD/Roms/FC/same.nes"))).returncode == 0
assert favourites.read_bytes() == before
assert fixture_out("is-favorite", "/mnt/SDCARD/Roms/FC/other.nes").stdout.strip() == "no"
# The same game under another label, as a stale Add popup or missing markers
# would offer it, from either route and launcher: nothing is added and the
# file is not rewritten (audit A3). Aliases already listed stay as they are.
alias = dict(stock, label="Alias")
favourites.write_text(json.dumps(stock) + "\n" + json.dumps(alias) + "\n", encoding="utf-8")
before = favourites.read_bytes()
for added in (dict(stock, label="Other label", rompath="/mnt/SDCARD/Roms/FC/same.nes"),
              dict(stock, label="Searched", launch="/mnt/SDCARD/App/Search/launch.sh",
                   rompath="/mnt/SDCARD/Emu/FC/launch.sh:/mnt/SDCARD/Roms//FC/./same.nes"),
              dict(stock, label="Other emulator", launch="/mnt/SDCARD/Emu/FC2/launch.sh")):
    assert fixture_out("add-favorite", json.dumps(added)).returncode == 0, added
    assert favourites.read_bytes() == before, added
# A different game is still added; an App (no ROM) still goes by its label.
app = dict(label="Some App", rompath="", launch="/mnt/SDCARD/App/X/launch.sh", type=3)
for added in (dict(stock, label="Other", rompath="/mnt/SDCARD/Roms/FC/other.nes"), app,
              dict(app, label="Another App")):
    assert fixture_out("add-favorite", json.dumps(added)).returncode == 0, added
assert len(favourites.read_text().splitlines()) == 5
assert fixture_out("add-favorite", json.dumps(app)).returncode == 0
assert len(favourites.read_text().splitlines()) == 5
favourites.unlink()

# One launch, two routes: the console list records the stock spelling, Search
# its normalized ROM after the launcher. That is one Recent, in the writer
# and in the reader; another emulator still makes it a second Recent.
console = dict(label="Spelled", rompath="/mnt/SDCARD/Emu/FC/../../Roms/FC/spelled.nes",
               launch="/mnt/SDCARD/Emu/FC/launch.sh", type=5)
searched = dict(label="Spelled", launch="/mnt/SDCARD/App/Search/launch.sh", type=5,
                rompath="/mnt/SDCARD/Emu/FC/launch.sh:/mnt/SDCARD/Roms/FC/spelled.nes")
write([console])
add(searched)
assert read() == [searched]
other_emulator = dict(console, launch="/mnt/SDCARD/Emu/FC2/launch.sh")
add(other_emulator)
assert read() == [other_emulator, searched]
write([searched, console])  # both spellings already in the file
rows = [subprocess.run([str(BUILD / "fixture-recent"), str(sd), "restore", "recents", str(i)],
                       timeout=20, capture_output=True).returncode for i in range(2)]
assert rows == [0, 2], rows  # the list shows one row

# Preservation must be confirmed: a .damaged that is not a regular file
# (here a directory) leaves the damaged list unchanged.
damaged_copy.mkdir()
broken = (json.dumps(a) + "\n{broken\n").encode("utf-8")
path.write_bytes(broken)
add(b, success=False)
assert path.read_bytes() == broken
damaged_copy.rmdir()
path.unlink()
print("Recent writer scenarios passed")
