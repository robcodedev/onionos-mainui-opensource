# SPDX-License-Identifier: GPL-3.0-only
"""A launch still happens when the ROM-list position cannot be saved.

Every other caller of mainui_positions_save() logs and continues; launching must
too. On the device the file is /appconfigs/romwinidx.json on internal flash, so
a full or read-only partition would otherwise block every launch from a list.
Here appconfigs is a regular file, which makes the save fail even as root.
"""
import json
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()

ROOT = Path(__file__).resolve().parents[2]
SD = Path(tempfile.mkdtemp(prefix="launch-position-", dir=BUILD))
EXE = str(BUILD / "MainUI-dev")


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def launch(name):
    """Launch the second ROM of Host/Collection; return (command, stderr).

    Each run gets its own handoff directory: with --system the startup does not
    consume the previous mainui-return.json, and publishing never overwrites it.
    """
    handoff = SD / ("handoff-" + name)
    handoff.mkdir()
    command = handoff / "cmd_to_run.sh"
    result = subprocess.run(
        [EXE, "--sd-root", str(SD), "--theme", str(ONION_THEME), "--input", "EDDE",
         "--snapshot", str(SD / (name + ".bmp")), "--system", "Host",
         "--handoff-dir", str(handoff)],
        cwd=ROOT, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert command.exists(), f"{name}: no launch published\n{result.stderr}"
    return command.read_text(encoding="utf-8"), result.stderr


write(SD / "Emu/FC/config.json",
      json.dumps(dict(label="Host", launch="launch.sh", rompath="../../Roms/FC", extlist="nes")))
write(SD / "Emu/FC/launch.sh", "#!/bin/sh\n")
(SD / "Emu/FC/launch.sh").chmod(0o755)
write(SD / "Roms/FC/Collection/It's first.nes", "")
write(SD / "Roms/FC/Collection/Two.nes", "")

# Control: with a writable appconfigs the position is saved and the ROM launches.
command, _ = launch("writable")
assert "Two.nes" in command, command
assert (SD / "appconfigs/romwinidx.json").is_file()

# Unwritable position file: the launch is still published, and the failure is logged.
(SD / "appconfigs/romwinidx.json").unlink()
(SD / "appconfigs").rmdir()
write(SD / "appconfigs", "not a directory")
command, stderr = launch("unwritable")
assert "Two.nes" in command, command
# Folder entry logs the same failure; this message is specific to the launch.
assert "launching anyway" in stderr, stderr
assert (SD / "appconfigs").read_text() == "not a directory"

# A damaged position file is moved aside once and replaced, so positions are
# saved again; its contents are kept for inspection.
(SD / "appconfigs").unlink()
write(SD / "appconfigs/romwinidx.json", "{broken")
command, stderr = launch("damaged")
assert "Two.nes" in command, command
assert "launching anyway" not in stderr, stderr
assert (SD / "appconfigs/romwinidx.json.bad").read_text() == "{broken"
saved = json.loads((SD / "appconfigs/romwinidx.json").read_text())
assert isinstance(saved.get("list"), list) and saved["list"], saved

# The same for content that cannot even be read as text: NUL bytes at the
# start, middle or end, or a file of zeros (review of 1.0.3, finding 5).
valid = b'{"list":[]}'
for name, damaged in (("nul-start", b"\x00" + valid), ("nul-middle", valid[:5] + b"\x00" + valid[5:]),
                      ("nul-end", valid + b"\x00"), ("zeros", b"\x00" * 512)):
    (SD / "appconfigs/romwinidx.json.bad").unlink(missing_ok=True)
    (SD / "appconfigs/romwinidx.json").write_bytes(damaged)
    command, stderr = launch(name)
    assert "Two.nes" in command and "launching anyway" not in stderr, (name, stderr)
    assert (SD / "appconfigs/romwinidx.json.bad").read_bytes() == damaged, name
    saved = json.loads((SD / "appconfigs/romwinidx.json").read_text())
    assert isinstance(saved.get("list"), list) and saved["list"], (name, saved)

# A record with "pos" written more than once (one of them the wrong type):
# one save leaves a single pos with the new value; other fields are kept.
(SD / "appconfigs/romwinidx.json.bad").unlink(missing_ok=True)
record = saved["list"][0]
(SD / "appconfigs/romwinidx.json").write_text(
    '{"list":[{"rompath":%s,"pos":0,"pos":"bad","pos":3,"start":0,"end":5,"note":"kept"}]}'
    % json.dumps(record["rompath"]))
command, stderr = launch("duplicate-pos")
assert "Two.nes" in command and "launching anyway" not in stderr, stderr
text = (SD / "appconfigs/romwinidx.json").read_text()
records = json.loads(text)["list"]
assert text.count('"pos"') == len(records) and '"note":"kept"' in text, text  # one per record
assert all(isinstance(row["pos"], int) for row in records), text
print("launch_position: ok")
