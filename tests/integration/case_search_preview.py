# SPDX-License-Identifier: GPL-3.0-only
"""Search results carry no artwork; thumbnails come from the source console."""
import json
import sqlite3
import struct
import subprocess
import tempfile
from pathlib import Path
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()

ROOT = Path(__file__).resolve().parents[2]
SD = Path(tempfile.mkdtemp(prefix="search-preview-", dir=BUILD))
EXE = str(BUILD / "MainUI-dev")


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


def bmp(rgb, size=32):
    """A solid 24-bit BMP. The loader sniffs content, so a .png name works."""
    row = bytes((rgb[2], rgb[1], rgb[0])) * size
    pixels = row * size
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, size, size, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    return header + info + pixels


def pixel(path, x, y):
    data = Path(path).read_bytes()
    offset, = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    depth, = struct.unpack_from("<H", data, 28)
    step = depth // 8
    stride = (width * step + 3) & ~3
    row = y if height < 0 else height - 1 - y
    b, g, r = data[offset + row * stride + x * step:][:3]
    return r, g, b


for name, label, rom, images in (("SEARCH", "Search", "../../App/Search/data",
                                  "../../App/Search/data/Imgs"),
                                 ("ARCADE", "Arcade", "../../Roms/ARCADE",
                                  "../../Roms/ARCADE/Imgs")):
    write(SD / "Emu" / name / "config.json",
          json.dumps(dict(label=label, launch="launch.sh", rompath=rom, imgpath=images,
                          extlist="zip")))
    write(SD / "Emu" / name / "launch.sh", "#!/bin/sh\n")
write(SD / "Roms/ARCADE/first.zip", "")
write(SD / "Roms/ARCADE/second.zip", "")
write(SD / "Roms/ARCADE/Imgs/first.png", bmp((255, 0, 0)))
write(SD / "Roms/ARCADE/Imgs/second.png", bmp((0, 0, 255)))
write(SD / ".tmp_update/config/main-menu.json", '{"menu":{"games":true}}')

database = SD / "App/Search/data/data_cache6.db"
database.parent.mkdir(parents=True, exist_ok=True)
connection = sqlite3.connect(database)
connection.execute("CREATE TABLE 'data_roms' (id INTEGER PRIMARY KEY AUTOINCREMENT,disp TEXT "
                   "NOT NULL,path TEXT NOT NULL,imgpath TEXT NOT NULL,type INTEGER DEFAULT 0,"
                   "ppath TEXT NOT NULL,pinyin TEXT NOT NULL,cpinyin TEXT NOT NULL)")
rows = [("Arcade (2)", "/mnt/SDCARD/Roms/ARCADE", "/mnt/SDCARD/Roms/ARCADE", 1, "."),
        ("First", "/mnt/SDCARD/Emu/ARCADE/launch.sh:./first.zip", "", 0, "Arcade (2)"),
        ("Second", "/mnt/SDCARD/Emu/ARCADE/launch.sh:./second.zip", "", 0, "Arcade (2)")]
connection.executemany("INSERT INTO data_roms (disp,path,imgpath,type,ppath,pinyin,cpinyin) "
                       "VALUES (?,?,?,?,?,?,'')", [row + (row[0],) for row in rows])
connection.commit()
connection.close()


def capture(name, actions):
    shot = SD / f"{name}.bmp"
    subprocess.run([EXE, "--sd-root", str(SD), "--theme", str(ONION_THEME), "--system", "Search",
                    "--input", actions, "--snapshot", str(shot)],
                   cwd=ROOT, check=True, timeout=30, capture_output=True)
    return shot


# Enter the console folder, move past "..", then onto each result. The
# centered cover must be the source console's image, not Search's Imgs.
assert pixel(capture("first", "ED"), 515, 240) == (255, 0, 0)
assert pixel(capture("second", "EDD"), 515, 240) == (0, 0, 255)

# A later page of Search results that cannot be read (row 70 of a group of 80
# has no path) is reloaded once, then left with the Search message. It is
# never rebuilt or replaced by a scan of Search's data folder (review of
# 1.0.3), and the database is left as it was.
many = [("Arcade (80)", "/mnt/SDCARD/Roms/ARCADE", "/mnt/SDCARD/Roms/ARCADE", 1, ".")] + [
    (f"Game {i:02}", "" if i == 70 else "/mnt/SDCARD/Emu/ARCADE/launch.sh:./first.zip", "", 0,
     "Arcade (80)") for i in range(80)]
database.unlink()
connection = sqlite3.connect(database)
connection.execute("CREATE TABLE 'data_roms' (id INTEGER PRIMARY KEY AUTOINCREMENT,disp TEXT "
                   "NOT NULL,path TEXT NOT NULL,imgpath TEXT NOT NULL,type INTEGER DEFAULT 0,"
                   "ppath TEXT NOT NULL,pinyin TEXT NOT NULL,cpinyin TEXT NOT NULL)")
connection.executemany("INSERT INTO data_roms (disp,path,imgpath,type,ppath,pinyin,cpinyin) "
                       "VALUES (?,?,?,?,?,?,'')", [row + (row[0],) for row in many])
connection.commit()
connection.close()
before = database.read_bytes()
shot = SD / "search-recovery.bmp"
result = subprocess.run([EXE, "--sd-root", str(SD), "--theme", str(ONION_THEME), "--system",
                         "Search", "--input", "EU", "--snapshot", str(shot)],
                        cwd=ROOT, timeout=30, capture_output=True, text=True)
assert result.returncode == 0, result.stderr[-400:]
steps = [line.split(": ", 1)[1] for line in result.stderr.splitlines()
         if line.startswith("Recovering an unreadable list page")]
assert steps == ["reloading"], steps
assert database.read_bytes() == before
print("Search results use their source console's thumbnails; unreadable results stay Search's")
