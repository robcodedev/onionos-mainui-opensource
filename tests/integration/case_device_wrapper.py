# SPDX-License-Identifier: GPL-3.0-only
"""The test wrapper always starts a launcher: logging problems never stop it,
and stock runs only when it is executable."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from env import BUILD

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="device-wrapper-", dir=BUILD))
SD = OUT / "sd"
TEST = SD / ".tmp_update/mainui-test"
LOGS = SD / ".tmp_update/logs"
(TEST / "stock").mkdir(parents=True)
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / "miyoo/app").mkdir(parents=True)
wrapper = OUT / "wrapper.sh"
# Map only the wrapper's own --handoff-dir /tmp, before the SD path goes in:
# a build directory under /tmp must not be rewritten a second time.
source = (ROOT / "device/MainUI-test-wrapper.sh").read_text()
assert source.count("--handoff-dir /tmp ") == 3, "update this case with the wrapper"
wrapper.write_text(source.replace("--handoff-dir /tmp ", f"--handoff-dir {OUT} ")
                   .replace("/mnt/SDCARD", str(SD)))
marker = OUT / "ran"


def fake(path, name):
    path.write_text(f"#!/bin/sh\necho {name} > '{marker}'\necho {name}-log\n")
    path.chmod(0o755)


fake(TEST / "MainUI", "open")
fake(TEST / "stock/MainUI", "stock")


def run():
    if marker.exists():
        marker.unlink()
    result = subprocess.run(["sh", str(wrapper)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    return marker.read_text().strip()


assert run() == "open"
(TEST / "DISABLED").touch()
assert run() == "stock"
(TEST / "stock/MainUI").chmod(0o644)
assert run() == "open"  # DISABLED but no runnable stock: better than nothing.
(TEST / "DISABLED").unlink()
(TEST / "stock/MainUI").chmod(0o755)

(SD / ".tmp_update/config/.logging").touch()
assert run() == "open"
assert "open-log" in (LOGS / "MainUI.log").read_text()
# The logs path cannot be a folder, or the log itself cannot be written.
shutil.rmtree(LOGS)
LOGS.write_text("not a folder")
assert run() == "open"
LOGS.unlink()
(LOGS / "MainUI.log").mkdir(parents=True)
assert run() == "open"
print("device_wrapper: ok")
