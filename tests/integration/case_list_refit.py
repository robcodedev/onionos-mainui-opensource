# SPDX-License-Identifier: GPL-3.0-only
"""A list left at its bottom and found shorter when it is shown again fills its
window, as if navigated to the same row, instead of leaving an empty row: on
reopening Favorites after removing its last game from a console list, and on
returning from a game to Favorites or a ROM list."""
import json
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme, unlink_if_exists  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="list-refit-", dir=BUILD))
SD = OUT / "sd"
HANDOFF = OUT / "handoff"
HANDOFF.mkdir()
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / ".tmp_update/config/main-menu.json").write_text(
    json.dumps({"menu": dict(favorites=True, games=True)}))
(SD / "Emu/C").mkdir(parents=True)
(SD / "Emu/C/config.json").write_text(json.dumps(dict(
    label="Console", rompath="../../Roms/C", launch="launch.sh", extlist="nes")))
(SD / "Roms/C").mkdir(parents=True)


def games(count):
    for file in (SD / "Roms/C").iterdir():
        file.unlink()
    for i in range(count):
        (SD / f"Roms/C/G{i:02}.nes").write_bytes(b"rom")


def favorites(count):
    (SD / "Roms/favourite.json").write_text("".join(json.dumps(dict(
        label=f"G{i:02}", rompath=f"/mnt/SDCARD/Roms/C/G{i:02}.nes",
        launch="/mnt/SDCARD/Emu/C/launch.sh", type=5)) + "\n" for i in range(count)))


def shot(actions, handoff=False):
    target = OUT / "shot.bmp"
    options = ["--handoff-dir", str(HANDOFF)] if handoff else []
    subprocess.run([str(BUILD / "MainUI-dev"), "--sd-root", str(SD), "--theme", str(ONION_THEME),
                    "--input", actions, "--snapshot", str(target), *options],
                   cwd=ROOT, check=True, timeout=30, capture_output=True)
    unlink_if_exists(SD / "appconfigs/romwinidx.json")
    unlink_if_exists(HANDOFF / "cmd_to_run.sh")
    return target.read_bytes()


# Home is Favorites, Games; Favorites lists G00..G09 in six rows. U from the
# top wraps to the last row.
games(10)
favorites(10)
# Favorites at its bottom, Home, Games, the console, its last game: Select,
# Remove Favorite. Then back to Home and Favorites again.
removed = shot("EUBREEUSDABBLE")
assert (SD / "Roms/favourite.json").read_text().count("\n") == 9
favorites(9)
bottom = shot("EU")
assert removed == bottom

# Leave Favorites by launching G08, the row above the last; G09's Favorite goes
# while away. The return keeps G08, now the last row, in a full window.
favorites(10)
shot("EUUA", handoff=True)
favorites(9)
assert shot("", handoff=True) == bottom

# Launch the last game of the ROM list; it is deleted while away and the cache
# rebuilt without it. The return selects the new last game in a full window.
favorites(0)
shot("REEUA", handoff=True)
unlink_if_exists(SD / "Roms/C/G09.nes")
unlink_if_exists(SD / "Roms/C/C_cache6.db")
assert shot("", handoff=True) == shot("REEU")
print("Favorites and ROM lists found shorter keep a full window at their bottom")
