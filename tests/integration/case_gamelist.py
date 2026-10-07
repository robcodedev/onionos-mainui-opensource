# SPDX-License-Identifier: GPL-3.0-only
"""Exercise XML cache publication on isolated, empty ROM fixtures."""
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()

ROOT = Path(__file__).resolve().parents[2]
SD = Path(tempfile.mkdtemp(prefix="xml-import-", dir=BUILD))
ROM = SD / "Roms/Jeux"
EMU = SD / "Emu/Test"
THEME = ONION_THEME
EXE = str(BUILD / "MainUI-dev")
XML = ROM / "miyoogamelist.xml"
DB = ROM / "Jeux_cache6.db"


def run(*options, code=0):
    result = subprocess.run(
        ([str(BUILD / "fixture-cache"), str(SD), "rebuild"]
         if options == ("--refresh-caches",) else
         [EXE, "--sd-root", str(SD), "--theme", str(THEME),
          "--snapshot", str(SD / "capture.bmp"), *options]),
        cwd=ROOT, timeout=30, capture_output=True,
    )
    assert result.returncode == code, result.stderr.decode(errors="replace")
    return result.stderr.decode(errors="replace")


def rows():
    with closing(sqlite3.connect(DB)) as connection:
        assert connection.execute("pragma integrity_check").fetchone() == ("ok",)
        return connection.execute(
            "select disp,path,imgpath,type,ppath from Jeux_roms order by id"
        ).fetchall()


def game(path, name, image=""):
    return f"<game><path>{path}</path><name>{name}</name>{image}</game>"


EMU.mkdir(parents=True)
(ROM / "Sets/Deep").mkdir(parents=True)
(EMU / "config.json").write_text(json.dumps({
    "label": "XML test", "rompath": "../../Roms/Jeux", "extlist": "nes",
}), encoding="utf-8")
for name in ("first.nes", "second.nes", "third.nes", "unlisted.nes", "café.nes",
             "Sets/Deep/a.nes", "Sets/Deep/b.nes"):
    (ROM / name).write_bytes(b"")

document = (
    '\ufeff<?xml version="1.0" encoding="UTF-8"?>\n<gameList>\n'
    '<!-- ignored <game> markup -->'
    '<game id="one"><path>./first.nes</path><name>A &amp; B &#x1F3AE;</name>'
    '<image>./Imgs/one.png</image><desc><extra>Ignored</extra></desc></game>'
    + game("./second.nes", "Missing image")
    + game("./third.nes", "Empty image", "<image/>")
    + game("./café.nes", "<![CDATA[Café <demo>]]>", "<image></image>")
    + game("./Sets/Deep/a.nes", "Nested A")
    + game("/mnt/SDCARD/Roms/Jeux/Sets/Deep/b.nes", "Nested B")
    + game("./missing.nes", "Skip missing")
    + game("./Sets", "Skip directory")
    + game("./first.nes", "")
    + '<game><name>Missing path</name></game></gameList>'
)
XML.write_text(document, encoding="utf-8")
run("--system", "XML test")
actual = rows()
stock_prefix = "/mnt/SDCARD/Emu/Test/../../Roms/Jeux/"
assert actual == [
    ("A & B " + chr(0x1f3ae), stock_prefix + "./first.nes", stock_prefix + "./Imgs/one.png", 0, "."),
    ("Missing image", stock_prefix + "./second.nes", "", 0, "."),
    ("Empty image", stock_prefix + "./third.nes", "", 0, "."),
    ("Café <demo>", stock_prefix + "./café.nes", "", 0, "."),
    ("Sets", stock_prefix + "Sets", stock_prefix + "Sets", 1, "."),
    ("Deep", stock_prefix + "Sets/Deep", stock_prefix + "Sets/Deep", 1, "Sets"),
    ("Nested A", stock_prefix + "./Sets/Deep/a.nes", "", 0, "Sets/Deep"),
    ("Nested B", "/mnt/SDCARD/Roms/Jeux/Sets/Deep/b.nes", "", 0, "Sets/Deep"),
], actual
run("--system", "XML test", "--input", "EDE")
assert XML.read_text(encoding="utf-8") == document

# Gamelists are read leniently, as stock reads them: Onion's own generator
# writes names without escaping. Each of these imports its first game.
def first(name):
    return [(name, stock_prefix + "./first.nes", "", 0, ".")]


prefix = '<gameList>' + game('./first.nes', 'Kept')
lenient = [
    (game('./first.nes', 'Sonic & Tails'), "Sonic & Tails"),
    (game('./first.nes', 'A &unknown; &#0; &#xD800; &amp'), "A &unknown; &#0; &#xD800; &amp"),
    (game('./first.nes', 'A < B'), "A < B"),
    (game('./first.nes', 'A <b>bold</b>'), "A <b>bold</b>"),
    (game('./first.nes', ']]> stray'), "]]> stray"),
    ('<game><path>./first.nes</path><name>x</name><name>y</name></game>', "x"),
    ('<game><path>./first.nes</path><name>Unclosed name</game>', "Unclosed name"),
    ('<game bad attr><path>./first.nes</path><name>Attributes</name></wrong></game>',
     "Attributes"),
    (game('./first.nes', 'x' * 4096), "x" * 4092),
]
for body, name in lenient:
    XML.write_text('<gameList>' + body + '</gameList>', encoding="utf-8")
    run("--refresh-caches")
    assert rows() == first(name), (body[:60], [row[0][:60] for row in rows()])
documents = [
    ('<!DOCTYPE gameList [<!ENTITY x SYSTEM "file:///ignored">]><gameList>'
     + game('./first.nes', '&x;') + '</gameList>', first("&x;")),
    (prefix, first("Kept")),  # cut off after a whole game
    (prefix + '</gameList>trailing <junk>', first("Kept")),
    ('junk <before/>' + prefix + '</gameList>', first("Kept")),
    ('<gameList/><gameList>' + game('./first.nes', 'Second root') + '</gameList>', []),
    ('<gameList><!-- invalid -- comment --></gameList>', []),
]
for text, expected in documents:
    XML.write_text(text, encoding="utf-8")
    run("--refresh-caches")
    assert rows() == expected, (text[:60], rows())
# Windows-1252 bytes, the usual encoding of a hand-edited list.
XML.write_bytes(b'<gameList><game><path>./first.nes</path><name>Caf\xe9 \x93Q\x94\x81</name>'
                b'</game></gameList>')
run("--refresh-caches")
assert rows() == first("Caf\u00e9 \u201cQ\u201d\u0081"), rows()

# A list with no usable <gameList> is not imported: the ROM files are listed
# and the cache is saved, so the console still opens. The XML is untouched.
for data in (b'', b'   ', b'<gameList>', b'<gameList><game><path>broken', b'not xml',
             b'<gameList>\x00</gameList>', b'<gameList><game attr="unfinished',
             b'<gameList>' + b'<unknown>' * 33 + b'</unknown>' * 33 + b'</gameList>',
             # Found unusable after a game: that game is not imported either.
             (prefix + '<unknown>' * 33).encode(),
             b'x' * (16 * 1024 * 1024 + 1)):
    XML.write_bytes(data)
    assert "is unusable" in run("--refresh-caches"), data[:40]
    assert len(rows()) == 9, data[:40]
    assert XML.read_bytes() == data
    assert not Path(str(DB) + ".building").exists()

# A path longer than a field holds is never cut to a shorter one: here the
# first 4,092 bytes name an existing ROM (repeated slashes normalize away),
# so a cut would import a different file. The game is skipped and the rest of
# the list still imports. An image that long is dropped, not cut.
def overlong(target, tail):
    # The host ROM path is used as is: /mnt/SDCARD/ would be remapped to the
    # longer host root here, making the cut path too long to resolve at all,
    # so the check would pass even without the fix.
    head = str(ROM)
    head += "/" * (4092 - len(head.encode()) - len(target.encode())) + target
    assert len(head.encode()) == 4092
    return head + tail


for path in (overlong("first.nes", "extra"), overlong("café.nes", "é")):
    XML.write_text('<gameList>' + game(path, 'Overlong') + game('./second.nes', 'Second')
                   + '</gameList>', encoding="utf-8")
    assert "skipped a game whose path is longer" in run("--refresh-caches")
    assert rows() == [("Second", stock_prefix + "./second.nes", "", 0, ".")], rows()
XML.write_text('<gameList>' + game('./first.nes', 'Long image',
                                   '<image>' + '/' * 5000 + 'x.png</image>')
               + '</gameList>', encoding="utf-8")
run("--refresh-caches")
assert rows() == first("Long image"), rows()

# A list that cannot be read (an I/O error, injected by read-fault.so) may
# be fine: the build fails and the previous cache is kept, not replaced by one
# made from the ROM files.
XML.write_text('<gameList>' + game('./first.nes', 'Kept') + '</gameList>', encoding="utf-8")
run("--refresh-caches")
kept = DB.read_bytes()
fault = Path(tempfile.mkdtemp(prefix="xml-fault-", dir=BUILD)) / "fault"
fault.write_text("miyoogamelist.xml eio\n")
libraries = subprocess.run(["ldd", str(BUILD / "fixture-cache")], check=True,
                           capture_output=True, text=True).stdout
asan = next((line.split("=>", 1)[1].split()[0] for line in libraries.splitlines()
             if "libasan.so" in line), "")
result = subprocess.run(
    [str(BUILD / "fixture-cache"), str(SD), "rebuild"], cwd=ROOT, timeout=30,
    capture_output=True, text=True,
    env=dict(os.environ, MAINUI_READ_FAULT=str(fault), LD_PRELOAD=" ".join(filter(
        None, (asan, os.environ.get("LD_PRELOAD"), str(BUILD / "read-fault.so"))))))
assert result.returncode == 4, (result.returncode, result.stderr)
assert "could not be read" in result.stderr and "is unusable" not in result.stderr, result.stderr
assert not fault.exists(), "the read fault was not used"
assert DB.read_bytes() == kept and rows() == first("Kept")

# A valid empty list is authoritative; removing XML restores filesystem scanning.
XML.write_text('<gameList/>', encoding="utf-8")
run("--refresh-caches")
assert rows() == []
XML.unlink()
run("--refresh-caches")
assert len(rows()) == 9

# Index a large directory once, then reuse it for distinct XML ROM records.
for index in range(10050):
    (ROM / f"bulk{index:05d}.nes").write_bytes(b"")
XML.write_text('<gameList>' + ''.join(
    game(f'./bulk{i:05d}.nes', f'Title {i:05d}') for i in range(10050)
) + '</gameList>', encoding="utf-8")
run("--refresh-caches")
assert len(rows()) == 10050
run("--system", "XML test", "--input", "U")
# Interleaved folders, symlink targets, and nonregular entries retain stat semantics.
(ROM / "Alias.nes").symlink_to("first.nes")
(ROM / "Dangling.nes").symlink_to("absent.nes")
(ROM / "Directory.nes").symlink_to("Sets", target_is_directory=True)
os.mkfifo(ROM / "Pipe.nes")
paths = ["./first.nes", "./Sets/Deep/a.nes", "./second.nes",
         "./Sets/Deep/b.nes", "./Alias.nes", "./Dangling.nes",
         "./Directory.nes", "./Pipe.nes", "./missing.nes", "./FIRST.NES",
         "./MissingFolder/no.nes", "./café.nes"]
expected = {path for path in paths if (ROM / path).is_file()}
XML.write_text("<gameList>" + "".join(game(path, path) for path in paths)
               + "</gameList>", encoding="utf-8")
run("--refresh-caches")
assert {row[1] for row in rows() if row[3] == 0} == {stock_prefix + path for path in expected}
# Directory snapshots are import-local: additions/removals appear next time.
(ROM / "second.nes").unlink()
(ROM / "missing.nes").write_bytes(b"")
expected = {path for path in paths if (ROM / path).is_file()}
run("--refresh-caches")
assert {row[1] for row in rows() if row[3] == 0} == {stock_prefix + path for path in expected}

print("XML import checks passed: names/art, empty images, Unicode/entities/CDATA, nested "
      "folders, invalid records, lenient reading, unusable lists, empty list, scan fallback, "
      "10,050 rows")
