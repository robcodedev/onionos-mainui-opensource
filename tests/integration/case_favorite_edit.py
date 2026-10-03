# SPDX-License-Identifier: GPL-3.0-only
"""Exercise Favorite editing through models and the real SDL popup/name UI."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()

ROOT = Path(__file__).resolve().parents[2]
# On the real-vfat run (tests/vfat.sh) the fixtures live on the FAT image.
OUT = Path(tempfile.mkdtemp(prefix="favorite-edit-", dir=os.environ.get("MAINUI_FAT_ROOT", BUILD)))
THEME = ONION_THEME


def write_lines(path, rows):
    path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


def fixture(name):
    sd = OUT / name
    for directory in ("Emu", "Roms", ".tmp_update/config"):
        (sd / directory).mkdir(parents=True)
    (sd / ".tmp_update/config/main-menu.json").write_text('{"menu":["favorites"]}')
    games = [dict(label=label, rompath=label, type=5, keep="stock") for label in ("Zebra", "apple", "Apple", "Inside")]
    write_lines(sd / "Roms/favourite.json", games)
    write_lines(sd / "Roms/favourite-folders.json", [
        dict(schema=1, generation=4, keep="header"),
        dict(kind="folder", id="container", parent="", name="Container", order=0, keep="folder"),
        dict(kind="folder", id="nested", parent="container", name="Nested", order=0),
        dict(kind="item", key="Inside", type=5, folder="container", order=0, keep="assignment"),
        dict(kind="extension", keep="opaque"),
    ])
    return sd


def capture(sd, name, actions, text=None):
    path = OUT / (name + ".bmp")
    command = [str(BUILD / "MainUI-dev"),
               "--sd-root", str(sd), "--theme", str(THEME), "--input", actions, "--snapshot", str(path)]
    if text is not None:
        command += ["--text", text]
    subprocess.run(command, cwd=ROOT, check=True, timeout=30)
    return Image.open(path).convert("RGB").tobytes()


model = fixture("model")
stock = (model / "Roms/favourite.json").read_bytes()
subprocess.run([str(BUILD / "fixture-favorite_edit"), str(model)], cwd=ROOT, check=True)
assert (model / "Roms/favourite.json").read_bytes() == stock
records = [json.loads(line) for line in (model / "Roms/favourite-folders.json").read_text().splitlines() if line.strip()]
assert records[0]["keep"] == "header"
assert any(row.get("keep") == "assignment" for row in records)
assert any(row.get("keep") == "opaque" for row in records)
assert not any(row.get("id") == "container" for row in records)
assert any(row.get("id") == "nested" and row["parent"] == "" for row in records)

ui = fixture("ui")
sidecar = ui / "Roms/favourite-folders.json"
stock = (ui / "Roms/favourite.json").read_bytes()
baseline = sidecar.read_bytes()
# SELECT on the first folder -> Create -> name keyboard -> START saves.
capture(ui, "create-cancel", "ESDEB", "Cancelled")
assert sidecar.read_bytes() == baseline
capture(ui, "create", "ESDET", "New folder")
rows = [json.loads(line) for line in sidecar.read_text().splitlines()]
created = next(row for row in rows if row.get("name") == "New folder")
assert capture(ui, "new-selected", "ESDET", "Another") != capture(ui, "root", "E")
# First folder Rename is the third menu entry, Delete is fourth.
capture(ui, "rename", "ESDDET", "Renamed container")
rows = [json.loads(line) for line in sidecar.read_text().splitlines()]
assert next(row for row in rows if row.get("id") == "container")["name"] == "Renamed container"
assert next(row for row in rows if row.get("id") == "container")["keep"] == "folder"
# Moving the first folder changes only its marker until Move here executes.
before = sidecar.read_bytes()
assert capture(ui, "cut-marker", "ESE") != capture(ui, "uncut", "E")
assert sidecar.read_bytes() == before
capture(ui, "cut-exit", "ESEBE")
assert sidecar.read_bytes() == before
# Rename failure retains the input file and keeps the name keyboard open.
capture(ui, "duplicate", "ESDDET", "New folder")
assert sidecar.read_bytes() == before
(sidecar.parent / (sidecar.name + ".writing")).write_text("foreign writer")
capture(ui, "locked-create", "ESDET", "Locked")
assert sidecar.read_bytes() == before
(sidecar.parent / (sidecar.name + ".writing")).unlink()
# A damaged sidecar without a usable backup is refused and left as it is.
backup = sidecar.parent / (sidecar.name + ".bak")
good_backup = backup.read_bytes()
backup.unlink()
sidecar.write_bytes(b"{broken")
capture(ui, "malformed-create", "ESDET", "Unsafe")
assert sidecar.read_bytes() == b"{broken"
assert not (sidecar.parent / (sidecar.name + ".damaged")).exists()
# With a valid backup (what browsing shows), the edit works on the backup and
# the damaged bytes are kept as .damaged.
backup.write_bytes(good_backup)
capture(ui, "malformed-create-backup", "ESDET", "Unsafe")
assert (sidecar.parent / (sidecar.name + ".damaged")).read_bytes() == b"{broken"
assert any(json.loads(line).get("name") == "Unsafe" for line in sidecar.read_text().splitlines())
assert (ui / "Roms/favourite.json").read_bytes() == stock
print("Favorite editing persistence, UI name/cancel/cut flow, unknown fields and failed-save preservation passed:", OUT)

removal = fixture("remove-assignment")
assignment = removal / "Roms/favourite-folders.json"
with assignment.open("a", encoding="utf-8") as stream:
    stream.write(json.dumps(dict(kind="item", key="Zebra", type=5, folder="", order=0)) + "\n")
capture(removal, "remove-assignment", "EDSDDDE")
assert not any(json.loads(line).get("label") == "Zebra" for line in (removal / "Roms/favourite.json").read_text().splitlines())
assert not any(json.loads(line).get("key") == "Zebra" for line in assignment.read_text().splitlines())
print("Remove Favorite also removes its sidecar assignment")

keyboard = fixture("keyboard")
capture(keyboard, "keyboard-blank", "ESDE", "")
capture(keyboard, "keyboard-text", "ESDE", "Readable")
blank = Image.open(OUT / "keyboard-blank.bmp").convert("RGB")
filled = Image.open(OUT / "keyboard-text.bmp").convert("RGB")
assert blank.crop((40, 66, 600, 114)).tobytes() != filled.crop((40, 66, 600, 114)).tobytes()
assert filled.getpixel((80, 130)) == (0, 0, 0), "Eight-pixel gap after the first 52px key"
assert filled.getpixel((25, 127)) != filled.getpixel((80, 130)), "First key starts at (24,126)"
print("Recovered keyboard field contrast and key geometry passed")


# An edit saves the repair browsing shows, but keeps the damaged original
# once as .damaged, which later edits never replace (review of 1.0.2, finding 2).
def create_once(sd, name):
    return subprocess.run([str(BUILD / "fixture-favorite_edit"), str(sd), "create", name],
                          cwd=ROOT, capture_output=True, text=True, timeout=30)

def names_in(path):
    return [json.loads(line).get("name") for line in path.read_text().splitlines()]

header = dict(schema=1, generation=0)
damaged = {
    "missing-name": [dict(kind="folder", id="a", parent="", name="A", order=0),
                     dict(kind="folder", id="b", parent="", order=1)],
    "duplicate-id": [dict(kind="folder", id="a", parent="", name="A", order=0),
                     dict(kind="folder", id="a", parent="", name="Other", order=1)],
    "cycle": [dict(kind="folder", id="a", parent="b", name="A", order=0),
              dict(kind="folder", id="b", parent="a", name="B", order=0)],
    "too-deep": [dict(kind="folder", id=f"d{i}", parent=f"d{i - 1}" if i else "", name=f"D{i}",
                      order=0) for i in range(4)],
    "dangling-parent": [dict(kind="folder", id="a", parent="gone", name="A", order=0)],
    "numeric-parent": [dict(kind="folder", id="a", parent=7, name="A", order=0)],
    "dangling-assignment": [dict(kind="folder", id="a", parent="", name="A", order=0),
                            dict(kind="item", key="Zebra", type=5, folder="gone", order=0)],
    "conflicting-assignment": [dict(kind="item", key="Zebra", type=5, folder="", order=0),
                               dict(kind="item", key="Zebra", type=5, folder="", order=3)],
    # Same folder and order, but other fields differ: only the first survives.
    "duplicate-extra-fields": [
        dict(kind="item", key="Zebra", type=5, folder="", order=0, note="first"),
        dict(kind="item", key="Zebra", type=5, folder="", order=0, note="second")],
    "duplicate-field-in-second": [
        dict(kind="item", key="Zebra", type=5, folder="", order=0),
        dict(kind="item", key="Zebra", type=5, folder="", order=0, note="only here")],
}
for name, rows in damaged.items():
    sd = fixture("damaged-" + name)
    sidecar = sd / "Roms/favourite-folders.json"
    write_lines(sidecar, [header] + rows)
    original = sidecar.read_bytes()
    kept = sd / "Roms/favourite-folders.json.damaged"
    result = create_once(sd, "Unrelated")
    assert result.returncode == 0, (name, result.stdout)
    assert kept.read_bytes() == original, name
    assert "Unrelated" in names_in(sidecar), name
    # Now repaired: a later edit leaves the kept original alone.
    assert create_once(sd, "Later").returncode == 0
    assert kept.read_bytes() == original and "Later" in names_in(sidecar), name
    assert not list(sd.glob("Roms/*.damaged-*")), name

# Fresh damage with an older, different copy already kept: both originals survive.
sidecar.write_bytes(original.replace(b'"generation": 0', b'"generation": 7'))
second = sidecar.read_bytes()
assert create_once(sd, "Again").returncode == 0
assert kept.read_bytes() == original
assert (sd / "Roms/favourite-folders.json.damaged-7").read_bytes() == second

# If no copy can be kept, nothing is saved.
sd = fixture("damaged-unkeepable")
sidecar = sd / "Roms/favourite-folders.json"
write_lines(sidecar, [header] + damaged["cycle"])
before = sidecar.read_bytes()
(sd / "Roms/favourite-folders.json.damaged").mkdir()
result = create_once(sd, "Unrelated")
assert result.returncode == 1 and "copy of the damaged" in result.stdout, result.stdout
assert sidecar.read_bytes() == before

# A damaged sidecar whose .bak is valid (browsing already shows the backup):
# editing keeps the damaged bytes as .damaged, then works on the backup.
sd = fixture("damaged-with-backup")
sidecar = sd / "Roms/favourite-folders.json"
backup = sd / "Roms/favourite-folders.json.bak"
backup.write_bytes(sidecar.read_bytes())
broken = b'{"schema":1,"generation":3}\n{"kind":"folder",broken\n'
sidecar.write_bytes(broken)
assert create_once(sd, "Unrelated").returncode == 0
assert (sd / "Roms/favourite-folders.json.damaged").read_bytes() == broken
names = set(names_in(sidecar))
assert {"Container", "Nested", "Unrelated"} <= names, names
# A main file that cannot be read as text at all (NUL bytes, as a power cut on
# FAT can leave, or over the 8 MiB limit) is moved aside unchanged, never
# replacing a kept copy, and editing works on the valid backup. A read error
# is not damage: an unreadable main file (here a folder) still stops the edit.
for name, content in (("nul", b"\x00" * 4096), ("oversize", b" " * (8 * 1024 * 1024 + 1))):
    sd = fixture(f"unreadable-{name}")
    sidecar = sd / "Roms/favourite-folders.json"
    backup = sd / "Roms/favourite-folders.json.bak"
    backup.write_bytes(sidecar.read_bytes())
    sidecar.write_bytes(content)
    earlier = sd / "Roms/favourite-folders.json.damaged"
    earlier.write_bytes(b"an earlier copy")
    assert create_once(sd, "Unrelated").returncode == 0
    assert earlier.read_bytes() == b"an earlier copy"
    kept = [p for p in sd.glob("Roms/favourite-folders.json.damaged-*")]
    assert len(kept) == 1 and kept[0].read_bytes() == content, kept
    assert {"Container", "Nested", "Unrelated"} <= set(names_in(sidecar))
sd = fixture("unreadable-folder")
sidecar = sd / "Roms/favourite-folders.json"
(sd / "Roms/favourite-folders.json.bak").write_bytes(sidecar.read_bytes())
sidecar.unlink()
sidecar.mkdir()
assert create_once(sd, "Unrelated").returncode != 0
assert sidecar.is_dir() and not list(sd.glob("Roms/*.damaged*"))
# A newer schema is not damage: refused as before, nothing changed or kept.
sd = fixture("newer-schema-with-backup")
sidecar = sd / "Roms/favourite-folders.json"
(sd / "Roms/favourite-folders.json.bak").write_bytes(sidecar.read_bytes())
newer = b'{"schema":2,"generation":0}\n'
sidecar.write_bytes(newer)
assert create_once(sd, "Unrelated").returncode == 1
assert sidecar.read_bytes() == newer
assert not (sd / "Roms/favourite-folders.json.damaged").exists()

# Records the reader takes exactly as written stay editable: an agreeing
# repeated assignment, a folder without order, an over-long path (above).
sd = fixture("agreeing")
sidecar = sd / "Roms/favourite-folders.json"
write_lines(sidecar, [header, dict(kind="folder", id="a", parent="", name="A"),
                      dict(kind="item", key="Zebra", type=5, folder="a", order=0),
                      dict(kind="item", key="Zebra", type=5, folder="a", order=0)])
assert create_once(sd, "Unrelated").returncode == 0
assert any(json.loads(line).get("name") == "Unrelated" for line in sidecar.read_text().splitlines())
assert not list(sd.glob("Roms/*.damaged*"))

# With only the backup left, edits work on that whole document: its header,
# unknown fields, other record kinds, assignments of games no longer listed
# and its generation survive the first edit, which leaves the .bak as it is,
# and the second, which backs up the first (audit A1).
sd = fixture("backup-only")
sidecar = sd / "Roms/favourite-folders.json"
backup = sd / "Roms/favourite-folders.json.bak"
with sidecar.open("a", encoding="utf-8") as orphan:
    orphan.write(json.dumps(dict(kind="item", key="Gone", type=5, folder="container", order=3,
                                 keep="orphan")) + "\n")
sidecar.rename(backup)
saved = backup.read_bytes()


def kept(path):
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    return rows[0]["generation"], {row.get("keep") for row in rows}


for edit, name in enumerate(("Unrelated", "Second")):
    assert create_once(sd, name).returncode == 0
    names = {json.loads(line).get("name") for line in sidecar.read_text().splitlines()}
    assert {"Container", "Nested", "Unrelated", name} <= names, names
    generation, keeps = kept(sidecar)
    assert generation == 5 + edit, generation
    assert {"header", "folder", "assignment", "opaque", "orphan"} <= keeps, keeps
    if not edit:
        assert backup.read_bytes() == saved
generation, keeps = kept(backup)  # the first edit's result, metadata included
assert generation == 5 and "orphan" in keeps, (generation, keeps)
assert not list(sd.glob("Roms/*.damaged*"))
# A .bak that is no usable document is kept as .damaged before the first
# edit replaces what browsing shows; one that cannot be read stops the edit.
sd = fixture("backup-damaged")
sidecar = sd / "Roms/favourite-folders.json"
backup = sd / "Roms/favourite-folders.json.bak"
sidecar.unlink()
backup.write_bytes(b'{"schema":1,"generation":2}\n{broken\n')
assert create_once(sd, "Fresh").returncode == 0
assert (sd / "Roms/favourite-folders.json.damaged").read_bytes() == backup.read_bytes()
# Only a backup, of a newer schema: refused too, with no main file made from
# what MainUI could show and no .damaged copy (review C2).
sd = fixture("backup-newer-schema")
sidecar = sd / "Roms/favourite-folders.json"
backup = sd / "Roms/favourite-folders.json.bak"
sidecar.unlink()
backup.write_bytes(b'{"schema":2,"generation":0}\n{"kind":"folder","id":"x"}\n')
saved = backup.read_bytes()
assert create_once(sd, "Fresh").returncode == 1
assert not sidecar.exists() and backup.read_bytes() == saved
assert not list(sd.glob("Roms/*.damaged*"))
sd = fixture("backup-unreadable")
sidecar = sd / "Roms/favourite-folders.json"
backup = sd / "Roms/favourite-folders.json.bak"
sidecar.unlink()
backup.mkdir()
assert create_once(sd, "Fresh").returncode != 0
assert not sidecar.exists()
print("Repairs keep the damaged original once; agreeing and backup-only sidecars stay as they are")


# Removing a folder must not leave two same-named folders side by side
# (review of 1.0.2, finding 3). Ordinary promotion keeps working.
def delete_once(sd, folder_id):
    return subprocess.run([str(BUILD / "fixture-favorite_edit"), str(sd), "delete", folder_id],
                          cwd=ROOT, capture_output=True, text=True, timeout=30)

sd = fixture("promote-collision")
sidecar = sd / "Roms/favourite-folders.json"
write_lines(sidecar, [header,
                      dict(kind="folder", id="a", parent="", name="A", order=0),
                      dict(kind="folder", id="x", parent="", name="X", order=1),
                      dict(kind="folder", id="ax", parent="a", name="X", order=0),
                      dict(kind="item", key="Zebra", type=5, folder="ax", order=0)])
before = sidecar.read_bytes()
result = delete_once(sd, "a")
assert result.returncode == 1 and "same name" in result.stdout, result.stdout
assert sidecar.read_bytes() == before
sd = fixture("promote")
sidecar = sd / "Roms/favourite-folders.json"
write_lines(sidecar, [header,
                      dict(kind="folder", id="a", parent="", name="A", order=0),
                      dict(kind="folder", id="x", parent="", name="X", order=1),
                      dict(kind="folder", id="a1", parent="a", name="One", order=0),
                      dict(kind="folder", id="a2", parent="a", name="Two", order=1)])
assert delete_once(sd, "a").returncode == 0
rows = {row["id"]: row for row in map(json.loads, sidecar.read_text().splitlines()) if "id" in row}
assert set(rows) == {"x", "a1", "a2"} and rows["a1"]["parent"] == rows["a2"]["parent"] == ""
print("Folder removal refuses duplicate sibling names and promotes distinct ones")


# Paste keeps the visible window when the moved Favorite stays inside it and
# otherwise scrolls only as far as needed, like moving the cursor there.
def window_case(name, ups):
    sd = fixture(name)
    write_lines(sd / "Roms/favourite.json",
                [dict(label=f"Game {i:02}", rompath=f"/mnt/SDCARD/Roms/FC/{i:02}.nes", type=5)
                 for i in range(20)])
    (sd / "Roms/favourite-folders.json").unlink(missing_ok=True)
    # Wrap to the last game, Move it up `ups` rows, Paste.
    moved = capture(sd, name + "-moved", "EU" + "SE" + "U" * ups + "SE")
    # The same list and cursor, reached by navigation alone.
    assert capture(sd, name + "-navigated", "EU" + "U" * ups) == moved, name

window_case("paste-within-window", 3)
window_case("paste-above-window", 9)

# Between folders the list sizes change on both sides (review of v6). Root
# shows folders first, then games; a folder opens on its ".." row.
def folder_fixture(name, inside):
    sd = fixture(name)
    write_lines(sd / "Roms/favourite.json",
                [dict(label=f"Game {i:02}", rompath=f"/mnt/SDCARD/Roms/FC/{i:02}.nes", type=5)
                 for i in range(3 + inside)])
    write_lines(sd / "Roms/favourite-folders.json",
                [dict(schema=1, generation=0), dict(kind="folder", id="f", parent="", name="Folder",
                                                     order=0)] +
                [dict(kind="item", key=f"/mnt/SDCARD/Roms/FC/{i:02}.nes", type=5, folder="f",
                      order=i - 3) for i in range(3, 3 + inside)])
    return sd

def folder_case(name, inside, downs):
    # Move the last root game, open Folder (Down wraps to it), step down, Paste.
    paste = "EDDDSEDE" + "D" * downs + "SE"
    sd = folder_fixture(name, inside)
    moved = capture(sd, name + "-moved", paste)
    # The pasted game's row in the folder, after "..", from the saved order.
    rows = [json.loads(line) for line in
            (sd / "Roms/favourite-folders.json").read_text().splitlines()]
    inside_rows = sorted((row for row in rows if row.get("folder") == "f"),
                         key=lambda row: row["order"])
    row = 1 + [row["key"] for row in inside_rows].index("/mnt/SDCARD/Roms/FC/02.nes")
    assert len(inside_rows) == inside + 1 and row in (downs, downs + 1), (name, row)
    assert capture(sd, name + "-navigated", "EE" + "D" * row) == moved, name
    # Back at the root in the same session, one game fewer: as if freshly opened.
    sd = folder_fixture(name + "-back", inside)
    back = capture(sd, name + "-back", paste + "B")
    assert capture(sd, name + "-root", "E") == back, name

folder_case("paste-into-empty-folder", 0, 0)
folder_case("paste-into-long-folder", 10, 2)
folder_case("paste-into-long-folder-end", 10, 9)
print("Paste keeps the list window unless the moved Favorite leaves it")
