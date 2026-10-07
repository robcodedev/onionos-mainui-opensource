# SPDX-License-Identifier: GPL-3.0-only
"""A release of Menu opens the same context menu as Select, as stock does for
Onion's "Context menu" long press, where keymon sends only that release. A
second release (the physical one after keymon's) does not close it. On the
device the release is read from the input device (tests/test_input.c); here
it is a scripted key-up."""
import json
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme, unlink_if_exists  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="menu-context-", dir=BUILD))
SD = OUT / "sd"
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / ".tmp_update/config/main-menu.json").write_text(json.dumps({"menu": dict(games=True)}))
(SD / "Emu/C").mkdir(parents=True)
(SD / "Emu/C/config.json").write_text(json.dumps(dict(
    label="Console", rompath="../../Roms/C", launch="launch.sh", extlist="nes")))
(SD / "Roms/C").mkdir(parents=True)
for name in ("Alpha", "Beta"):
    (SD / f"Roms/C/{name}.nes").write_bytes(b"rom")


def shot(actions):
    target = OUT / "shot.bmp"
    subprocess.run([str(BUILD / "MainUI-dev"), "--sd-root", str(SD), "--theme", str(ONION_THEME),
                    "--input", actions, "--snapshot", str(target)],
                   cwd=ROOT, check=True, timeout=30, capture_output=True)
    unlink_if_exists(SD / "appconfigs/romwinidx.json")
    return target.read_bytes()


# Games grid (E), then the console's ROM list (EE). S is Select; M is Menu,
# -M its release.
for place in ("E", "EE"):
    by_select = shot(place + "S")
    assert by_select != shot(place), place + ": Select opened no context menu"
    assert shot(place + "-M") == by_select, place + ": a lone Menu release"
    assert shot(place + "M-M") == by_select, place + ": a Menu press and release"
    assert shot(place + "-M-M") == by_select, place + ": a second release closed the menu"
# Menu does not close an open menu either; B still does.
assert shot("EES-M") == shot("EES")
assert shot("EES-MC") == shot("EE")
print("A release of Menu opens the context menu as Select does, and a second one keeps it")
