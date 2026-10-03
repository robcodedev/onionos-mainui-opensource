# SPDX-License-Identifier: GPL-3.0-only
"""Games and Expert keep their console and window when left for Home and
re-entered, by every route (each loop pass saves the grid's view), and
opening them reads the consoles again."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
SD = Path(tempfile.mkdtemp(prefix="section-position-", dir=BUILD)) / "sd"
UI = os.environ.get("MAINUI_TEST_EXE", str(BUILD / "MainUI-dev"))
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / ".tmp_update/config/main-menu.json").write_text(
    json.dumps({"menu": dict(games=True, expert=True, apps=True, settings=True)}))
for root in ("Emu", "RApp"):
    for i in range(16):
        (SD / root / f"C{i:02}").mkdir(parents=True)
        (SD / root / f"C{i:02}/config.json").write_text(json.dumps(dict(
            label=f"Console {i:02}", rompath=f"../../Roms/C{i:02}", launch="launch.sh",
            extlist="nes")))
    # Two emulators for one ROM folder, listed right after Console 00.
    for name in ("A", "B"):
        (SD / root / f"Shared{name}").mkdir(parents=True)
        (SD / root / f"Shared{name}/config.json").write_text(json.dumps(dict(
            label=f"Console 00 {name}", rompath="../../Roms/C00", launch="launch.sh",
            extlist="nes")))
for i in range(16):
    (SD / f"Roms/C{i:02}").mkdir(parents=True, exist_ok=True)
    (SD / f"Roms/C{i:02}/Game.nes").write_bytes(b"rom")


def shot(actions, *options, env=None):
    out = SD.parent / f"{actions}{''.join(options)}.bmp"
    subprocess.run([UI, "--sd-root", str(SD), "--theme", str(ONION_THEME), "--input", actions,
                    "--snapshot", str(out), *options], cwd=ROOT, check=True, timeout=30,
                   capture_output=True, env=env)
    (SD / "appconfigs/romwinidx.json").unlink(missing_ok=True)
    return out.read_bytes()


# Home is Games, Expert, Apps, Settings; Games and Expert are 4x2 / 3x3 grids.
games, expert = shot("EDDDR"), shot("REDDR")
assert shot("EDDDRBE") == games            # Back to Home, re-enter
assert shot("EDDDRBREBLE") == games        # by way of Expert
assert shot("EDDDREBBE") == games          # from a ROM list, through the grid
assert shot("REDDRBE") == expert
assert shot("REDDREBBE") == expert

# A console whose config could not be read when MainUI started (here one read
# error) appears when Games is opened again: opening a section reads it
# again, while nothing is watched as a screen stays open.
fault = SD.parent / "fault"
libraries = subprocess.run(["ldd", UI], check=True, capture_output=True, text=True).stdout
asan = next((line.split("=>", 1)[1].split()[0] for line in libraries.splitlines()
             if "libasan.so" in line), "")
faulty = dict(os.environ, MAINUI_READ_FAULT=str(fault), LD_PRELOAD=" ".join(filter(
    None, (asan, os.environ.get("LD_PRELOAD"), str(BUILD / "read-fault.so")))))
grid = shot("", "--systems")
fault.write_text("Emu/C03/config.json eio")
assert shot("", "--systems", env=faulty) != grid  # skipped at startup
assert not fault.exists()
fault.write_text("Emu/C03/config.json eio")
assert shot("BE", "--systems", env=faulty) == grid  # Home, then Games again
fault.write_text("Emu/C03/config.json eio")
assert shot("E", env=faulty) == shot("E")  # started on Home
# Of two emulators sharing a ROM folder, the selected one stays selected:
# reopening matches the console by its config file, not its ROM folder.
for open_section, grid_shot in (("E", "ERR"), ("RE", "RERR")):
    selected = shot(grid_shot)
    assert selected != shot(open_section + "R")  # the other one looks different
    assert shot(grid_shot + "BE") == selected  # Home and back
    assert shot(grid_shot + "EBBE") == selected  # from its ROM list
# Favorite stars that could not be read at startup (one read error) are read
# when a section opens: the Favorite in Console 00 shows its star again.
(SD / "Roms/favourite.json").write_text(json.dumps(dict(
    label="Game", rompath="/mnt/SDCARD/Roms/C00/Game.nes",
    launch="/mnt/SDCARD/Emu/C00/launch.sh", type=5)) + "\n")
starred = shot("EE")
fault.write_text("Roms/favourite.json eio")
assert shot("E", "--systems", env=faulty) != shot("E", "--systems")  # no section opened
fault.write_text("Roms/favourite.json eio")
assert shot("EE", env=faulty) == starred
(SD / "Roms/favourite.json").unlink()
print("Games and Expert keep their position across Home and read their consoles and "
      "Favorite stars on opening")
