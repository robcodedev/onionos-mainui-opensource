# SPDX-License-Identifier: GPL-3.0-only
"""Games and Expert keep their console and window when left for Home and
re-entered, by every route (each loop pass saves the grid's view)."""
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
for i in range(16):
    (SD / f"Roms/C{i:02}").mkdir(parents=True, exist_ok=True)
    (SD / f"Roms/C{i:02}/Game.nes").write_bytes(b"rom")


def shot(actions):
    out = SD.parent / f"{actions}.bmp"
    subprocess.run([UI, "--sd-root", str(SD), "--theme", str(ONION_THEME), "--input", actions,
                    "--snapshot", str(out)], cwd=ROOT, check=True, timeout=30,
                   capture_output=True)
    (SD / "appconfigs/romwinidx.json").unlink(missing_ok=True)
    return out.read_bytes()


# Home is Games, Expert, Apps, Settings; Games and Expert are 4x2 / 3x3 grids.
games, expert = shot("EDDDR"), shot("REDDR")
assert shot("EDDDRBE") == games            # Back to Home, re-enter
assert shot("EDDDRBREBLE") == games        # by way of Expert
assert shot("EDDDREBBE") == games          # from a ROM list, through the grid
assert shot("REDDRBE") == expert
assert shot("REDDREBBE") == expert
print("Games and Expert keep their position across Home")
