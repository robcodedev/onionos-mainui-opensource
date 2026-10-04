# SPDX-License-Identifier: GPL-3.0-only
"""A message too long for one line wraps instead of being cut at the panel's
edge. Here a console whose cache cannot be built (another build's
reservation is in the way) and whose gamelist must not be scanned around
shows "Catalog unavailable" with a two-line explanation."""
import json
from pathlib import Path
import subprocess
import tempfile

from PIL import Image
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="message-wrap-", dir=BUILD))
SD = OUT / "sd"
(SD / "Emu/GG").mkdir(parents=True)
(SD / "Roms/GG").mkdir(parents=True)
(SD / "Emu/GG/config.json").write_text(json.dumps(dict(
    label="Game Gear", rompath="../../Roms/GG", launch="launch.sh", extlist="gg")))
(SD / "Roms/GG/game.gg").write_bytes(b"rom")
(SD / "Roms/GG/miyoogamelist.xml").write_text("<gameList/>")
(SD / "Roms/GG/GG_cache6.db.building").write_text("another build")

target = OUT / "message.bmp"
result = subprocess.run([str(BUILD / "MainUI-dev"), "--sd-root", str(SD), "--theme",
                         str(ONION_THEME), "--systems", "--input", "E", "--snapshot", str(target)],
                        cwd=ROOT, timeout=30, capture_output=True, text=True)
assert result.returncode == 0, result.stderr
image = Image.open(target).convert("RGB")


def text_rows(top, bottom, left=35, right=605):
    """Rows in the band holding light text pixels on the dark panel."""
    return [y for y in range(top, bottom)
            if any(sum(image.getpixel((x, y))) > 450 for x in range(left, right))]


# The first body line starts at y 215; the second follows it, above the hint.
first = text_rows(215, 245)
assert first, "no first line"
second = text_rows(max(first) + 2, max(first) + 40)
assert second, "the explanation was not wrapped onto a second line"
# Nothing reaches the clipped right edge of the panel: no word is cut off.
assert not text_rows(215, max(second) + 1, left=595), "text runs into the panel's edge"
print("A long message wraps onto more lines instead of being cut off")
