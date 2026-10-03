# SPDX-License-Identifier: GPL-3.0-only
"""Qualify cancellable catalog jobs and coherent external edits on isolated data."""
import json
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()

ROOT = Path(__file__).resolve().parents[2]
SD = Path(tempfile.mkdtemp(prefix="catalog-jobs-", dir=BUILD))
THEME = ONION_THEME
EXE = str(BUILD / "MainUI-dev")
(SD / "Emu/Host").mkdir(parents=True)
(SD / "Roms/Host").mkdir(parents=True)
(SD / "Emu/Host/config.json").write_text(json.dumps(dict(label="Host", rompath="../../Roms/Host", launch="launch.sh", extlist="nes")))
for i in range(200):
    (SD / f"Roms/Host/game{i:03}.nes").write_bytes(b"")
subprocess.run([str(BUILD / "fixture-catalog_job"), str(SD)], check=True, timeout=30)
# A cancelled XML import must retain the previous complete cache too.
(SD / "Roms/Host/miyoogamelist.xml").write_text("<gameList>" + "".join(
    f"<game><path>game{i:03}.nes</path><name>game{i:03}</name></game>" for i in range(200)) + "</gameList>")
# Use a fresh scan cache for the same native selection/order fixture.
(SD / "Roms/Host/Host_cache6.db").unlink()
subprocess.run([str(BUILD / "fixture-catalog_job"), str(SD)], check=True, timeout=30)
# Two consoles; refresh and repair of A must never touch B when A cannot be
# found again (read-fault.so makes one config read fail on request).
TARGET = SD.parent / "target-sd"
shutil.rmtree(TARGET, ignore_errors=True)
for name in ("A", "B"):
    (TARGET / f"Emu/{name}").mkdir(parents=True)
    (TARGET / f"Emu/{name}/config.json").write_text(json.dumps(dict(
        label=name, rompath=f"../../Roms/{name}", launch="launch.sh", extlist="nes")))
    (TARGET / f"Roms/{name}").mkdir(parents=True)
    for i in range(3):
        (TARGET / f"Roms/{name}/{name.lower()}{i}.nes").write_bytes(b"rom")
libraries = subprocess.run(["ldd", str(BUILD / "fixture-catalog_job")], check=True,
                           capture_output=True, text=True).stdout
asan = next((line.split("=>", 1)[1].split()[0] for line in libraries.splitlines()
             if "libasan.so" in line), "")
subprocess.run([str(BUILD / "fixture-catalog_job"), str(TARGET), "target"], check=True,
               timeout=30, env=dict(os.environ, MAINUI_READ_FAULT=str(TARGET / "fault"),
                                    LD_PRELOAD=" ".join(filter(None, (
                                        asan, os.environ.get("LD_PRELOAD"),
                                        str(BUILD / "read-fault.so"))))))
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / ".tmp_update/config/main-menu.json").write_text('{"menu":{"games":true}}')

def capture(name, actions):
    path = SD / (name + ".bmp")
    subprocess.run([EXE, "--sd-root", str(SD), "--theme", str(THEME), "--input", actions,
                    "--snapshot", str(path)], check=True, timeout=30)
    return path.read_bytes()

assert capture("home", "") == capture("home-back", "B")
assert capture("home", "") == capture("home-start", "T")
assert capture("home", "") == capture("home-menu", "M")
assert capture("systems", "E") == capture("cancelled-enter", "EEC")
assert capture("games", "EE") == capture("reload", "EET")
print("Catalog job fixtures passed:", SD)
