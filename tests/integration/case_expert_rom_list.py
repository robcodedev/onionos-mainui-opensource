# SPDX-License-Identifier: GPL-3.0-only
"""An Expert config with a rompath is a ROM list even when its extlist is
empty, as fMSX's is: entering it lists every file and A launches the selected
one with its launch.sh. Only a config without a rompath launches directly."""
import json
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme, unlink_if_exists  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="expert-rom-list-", dir=BUILD))
SD = OUT / "sd"
HANDOFF = OUT / "handoff"
HANDOFF.mkdir()
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / ".tmp_update/config/main-menu.json").write_text(json.dumps({"menu": dict(expert=True)}))
(SD / "RApp/fMSX").mkdir(parents=True)
(SD / "RApp/fMSX/config.json").write_text(json.dumps(dict(
    label="fMSX", rompath="../../Roms/MSX", launch="launch.sh", extlist="")))
(SD / "Roms/MSX").mkdir(parents=True)
(SD / "Roms/MSX/game.rom").write_bytes(b"rom")
COMMAND = HANDOFF / "cmd_to_run.sh"
unlink_if_exists(COMMAND)
# Home, Expert, fMSX, its first file.
subprocess.run([str(BUILD / "MainUI-dev"), "--sd-root", str(SD), "--theme", str(ONION_THEME),
                "--input", "EEA", "--snapshot", str(OUT / "shot.bmp"),
                "--handoff-dir", str(HANDOFF)],
               cwd=ROOT, check=True, timeout=30, capture_output=True)
command = COMMAND.read_text()
assert '"/mnt/SDCARD/RApp/fMSX/launch.sh"' in command and "Roms/MSX/game.rom" in command, command
print("An Expert config with a rompath and an empty extlist is a ROM list")
