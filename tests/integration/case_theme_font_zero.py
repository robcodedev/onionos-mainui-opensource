# SPDX-License-Identifier: GPL-3.0-only
"""Theme font size 0 hides that text, as in stock."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from PIL import Image
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="theme-font-zero-", dir=BUILD))
MARK = (255, 0, 255)  # a text colour no stock asset uses

SD = OUT / "sd"
for name in ("Emu/FC", "RApp/FC", "Roms/FC", "App", ".tmp_update/config"):
    (SD / name).mkdir(parents=True)
for console in ("Emu", "RApp"):
    (SD / console / "FC/config.json").write_text(json.dumps(dict(
        label="Famicom", rompath="../../Roms/FC", launch="launch.sh", extlist="nes")))
(SD / "Roms/FC/one.nes").write_bytes(b"rom")
(SD / ".tmp_update/config/main-menu.json").write_text(json.dumps(
    {"menu": dict(games=True, expert=True, apps=True, settings=True)}))


def theme(name, **sizes):
    directory = OUT / name
    shutil.copytree(ONION_THEME, directory)
    config = json.loads((directory / "config.json").read_text())
    config["title"]["color"] = config["hint"]["color"] = "#FF00FF"
    config["grid"]["color"] = config["grid"]["selectedcolor"] = "#FF00FF"
    config["list"]["color"] = "#FFFFFF"
    config["batteryPercentage"]["color"] = "#FFFFFF"
    config["currentpage"]["color"] = config["total"]["color"] = "#FF00FF"
    for key, value in sizes.items():
        section, field = key.split("__")
        config[section][field] = value
    (directory / "config.json").write_text(json.dumps(config))
    return directory


def marked(directory, actions, box):
    path = OUT / f"{directory.name}-{actions or 'home'}.bmp"
    subprocess.run([os.environ.get("MAINUI_TEST_EXE", str(BUILD / "MainUI-dev")),
                    "--sd-root", str(SD), "--theme", str(directory), "--input", actions,
                    "--snapshot", str(path)], cwd=ROOT, check=True, timeout=30)
    image = Image.open(path).convert("RGB").crop(box)
    return sum(1 for pixel in image.getdata() if pixel == MARK)


HEADER, FOOTER, HOME_LABELS, GRID = (0, 0, 640, 60), (0, 420, 640, 480), (0, 230, 640, 330), \
    (0, 60, 640, 420)
# Home is Games, Expert, Apps, Settings; Games and Expert open console grids
# and Apps opens a titled list with a page counter.
normal = theme("normal")
assert marked(normal, "", FOOTER) and marked(normal, "", HOME_LABELS)
assert marked(normal, "RRE", HEADER) and marked(normal, "RRE", FOOTER)
assert marked(normal, "E", GRID) and marked(normal, "RE", GRID)

hint = theme("hint", hint__size=0)
assert not marked(hint, "", FOOTER) and not marked(hint, "RRE", FOOTER)
assert marked(hint, "RRE", HEADER)

title = theme("title", title__size=0)
assert not marked(title, "RRE", HEADER)
assert marked(title, "RRE", FOOTER), "a hint size inherited from the title stays visible"

grid = theme("grid", grid__grid1x4=0)
assert not marked(grid, "", HOME_LABELS) and not marked(grid, "E", GRID)
assert marked(grid, "RE", GRID)

expert = theme("expert", grid__grid3x4=0)
assert not marked(expert, "RE", GRID)
assert marked(expert, "", HOME_LABELS) and marked(expert, "E", GRID)
print("Font size 0 hides hints, title, home and console labels:", OUT)
