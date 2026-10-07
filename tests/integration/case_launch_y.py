# SPDX-License-Identifier: GPL-3.0-only
"""Y on a game launches it as A does, as stock does, so Onion's keymon flag
(/tmp/launch_alt, set on Y) makes runtime.sh open Game List Options for it
(#14). Y on a folder launches nothing. Game List Options removes the first
line of Recents, taken to be the launch, so after Y Recents is as before."""
import json
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme, unlink_if_exists  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="launch-y-", dir=BUILD))
SD = OUT / "sd"
HANDOFF = OUT / "handoff"
HANDOFF.mkdir()
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / ".tmp_update/config/main-menu.json").write_text(
    json.dumps({"menu": dict(favorites=True, games=True)}))
(SD / "Emu/C").mkdir(parents=True)
(SD / "Emu/C/config.json").write_text(json.dumps(dict(
    label="Console", rompath="../../Roms/C", launch="launch.sh", extlist="nes")))
(SD / "Roms/C/Sub").mkdir(parents=True)
(SD / "Roms/C/Sub/Inner.nes").write_bytes(b"rom")
for i in range(3):
    (SD / f"Roms/C/G{i:02}.nes").write_bytes(b"rom")
(SD / "Roms/favourite.json").write_text(json.dumps(dict(
    label="G01", rompath="/mnt/SDCARD/Roms/C/G01.nes",
    launch="/mnt/SDCARD/Emu/C/launch.sh", type=5)) + "\n")
COMMAND = HANDOFF / "cmd_to_run.sh"


def launched(actions):
    """The command the actions left for runtime.sh, or None."""
    unlink_if_exists(COMMAND)
    unlink_if_exists(HANDOFF / "mainui-return.json")
    subprocess.run([str(BUILD / "MainUI-dev"), "--sd-root", str(SD), "--theme", str(ONION_THEME),
                    "--input", actions, "--snapshot", str(OUT / "shot.bmp"),
                    "--handoff-dir", str(HANDOFF)],
                   cwd=ROOT, check=True, timeout=30, capture_output=True)
    unlink_if_exists(SD / "appconfigs/romwinidx.json")
    return COMMAND.read_bytes() if COMMAND.exists() else None


# Home is Favorites, Games. In the console's list the Sub folder comes first.
by_a = launched("REEDA")
assert by_a and b"G00.nes" in by_a, by_a
assert launched("REEDY") == by_a
assert launched("REEY") is None, "Y on a folder launched something"
# keymon arms its Y flag on every Y press and clears it only on B or X. Y on
# the folder is ignored; A on the game below must then start the game, not
# open Game List Options, which would also remove its Recent.
FLAG = HANDOFF / "launch_alt"
FLAG.write_text("")
assert launched("REEYDA") == by_a and not FLAG.exists()
assert json.loads((SD / "Roms/recentlist.json").read_text().splitlines()[0])["label"] == "G00"
# Game List Options tells the list a game came from by the second "type" in
# state.json, read line by line as below (game_list_options.sh): 1 for Games,
# 2 for Favorites. On a single line it read nothing.
TAB = OUT / "tab.sh"
TAB.write_text('grep "\\"type\\":" "$1" | sed -e \'s/^.*:\\s*//g\' | sed -e \'s/\\s*,$//g\' '
               '| xargs | awk \'{ print $2 }\'\n')


def tab():
    return subprocess.run(["sh", str(TAB), str(HANDOFF / "state.json")], check=True,
                          capture_output=True, text=True).stdout.strip()


assert launched("REEDY") and tab() == "1", tab()
# Favorites: the same launch as A.
favorite = launched("EA")
assert favorite and b"G01.nes" in favorite, favorite
assert launched("EY") == favorite and tab() == "2", tab()

# Recents: Y puts the game's line first and leaves the list's lines as they
# were, so removing that line (as Game List Options does) gives the list back.
(SD / ".tmp_update/config/main-menu.json").write_text(json.dumps({"menu": dict(recents=True)}))
RECENT = SD / "Roms/recentlist.json"
original = "".join(json.dumps(dict(
    label=f"G{i:02}", rompath=f"/mnt/SDCARD/Roms/C/G{i:02}.nes",
    launch="/mnt/SDCARD/Emu/C/launch.sh", type=5)) + "\n" for i in range(3))
RECENT.write_text(original)
command = launched("EDY")
assert command and b"G01.nes" in command, command
first, rest = RECENT.read_text().split("\n", 1)
assert json.loads(first)["rompath"].endswith("/G01.nes") and rest == original, RECENT.read_text()
# A Y launch that cannot be published (here a launch is already pending)
# clears keymon's Y flag, which A would not, so the next launch is a normal
# one. Its Recents line stays above the list's unchanged lines.
RECENT.write_text(original)
COMMAND.write_text("pending\n")
FLAG.write_text("")
subprocess.run([str(BUILD / "MainUI-dev"), "--sd-root", str(SD), "--theme", str(ONION_THEME),
                "--input", "EDY", "--snapshot", str(OUT / "shot.bmp"),
                "--handoff-dir", str(HANDOFF)], cwd=ROOT, check=True, timeout=30,
               capture_output=True)
assert COMMAND.read_text() == "pending\n" and RECENT.read_text().split("\n", 1)[1] == original
assert not FLAG.exists()
unlink_if_exists(COMMAND)
# Recents that cannot be read: Y launches nothing from a ROM list, for Game
# List Options would remove another game's line. A still launches.
(SD / ".tmp_update/config/main-menu.json").write_text(
    json.dumps({"menu": dict(favorites=True, games=True)}))
RECENT.unlink()
RECENT.mkdir()
FLAG.write_text("")
assert launched("REEDY") is None and RECENT.is_dir() and not FLAG.exists()
assert launched("REEDA") == by_a
RECENT.rmdir()
(SD / ".tmp_update/config/main-menu.json").write_text(json.dumps({"menu": dict(recents=True)}))
# A still moves the game to the top once.
RECENT.write_text(original)
launched("EDA")
assert [json.loads(line)["label"] for line in RECENT.read_text().splitlines()] == ["G01", "G00", "G02"]
print("Y launches the selected game as A does, nothing on a folder, and keeps Recents")
