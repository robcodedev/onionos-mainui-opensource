# SPDX-License-Identifier: GPL-3.0-only
"""Exercise device adapters in an isolated runtime; never issue real hardware requests."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from env import BUILD, ONION_THEME, require_onion_theme  # noqa: E402

require_onion_theme()
ROOT = Path(__file__).resolve().parents[2]
ROOT_TEST = Path(tempfile.mkdtemp(prefix="devices-", dir=BUILD))
RUNTIME = ROOT_TEST / "runtime"
RUNTIME.mkdir()
subprocess.run([str(BUILD / "fixture-device"), str(RUNTIME)], check=True, timeout=30)
SD = ROOT_TEST / "sd"
(SD / "Emu").mkdir(parents=True)
THEME = ONION_THEME
EXE = str(BUILD / "MainUI-dev")
(RUNTIME / "percBat").write_text("63")

def capture(name, extra):
    output = ROOT_TEST / (name + ".bmp")
    result = subprocess.run([EXE, "--sd-root", str(SD), "--theme", str(THEME),
        "--snapshot", str(output), *extra], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    return output.read_bytes()
assert capture("simulated", ["--device-dir", str(RUNTIME)]) == capture("explicit", ["--battery", "63"])
assert capture("override", ["--device-dir", str(RUNTIME), "--battery", "25"]) == capture("quarter", ["--battery", "25"])
# --device real talks to the Onion runtime and is only meaningful on the
# device itself, so it is exercised by docs/BUILDING.md's device run instead of
# here. The old assertion checked a host guard that no longer exists.
(SD / ".tmp_update/config").mkdir(parents=True)
(SD / ".tmp_update/config/main-menu.json").write_text(json.dumps({"menu": {"settings": True}, "settings": ["wifi", "about"]}))
(SD / "system.json").write_text('{"wifi":1}')
(RUNTIME / "wifi-status.txt").write_text("wpa_state=COMPLETED\nssid=Host Wi-Fi\n")
connected = capture("wifi-connected", ["--device-dir", str(RUNTIME), "--input", "EE"])
(RUNTIME / "wifi-status.txt").write_text("wpa_state=DISCONNECTED\n")
assert connected != capture("wifi-disconnected", ["--device-dir", str(RUNTIME), "--input", "EE"])
capture("about", ["--device-dir", str(RUNTIME), "--input", "EDE"])
capture("scan-request", ["--device-dir", str(RUNTIME), "--input", "EEDDDDE"])
assert (RUNTIME / "mainui-wifi-request.sh").read_text() == "wpa_cli scan\nwpa_cli scan_results\n"
# The Wi-Fi icon sits in stock's second 48x48 status slot (x 504, y 6),
# centered in it: a 200x52 image, as Super Onion Entertainment System Remix
# ships, spans x 428-627 from y 4.
from PIL import Image  # noqa: E402
WIFI = (12, 201, 34)
wifi_theme = ROOT_TEST / "wifi-theme"
(wifi_theme / "skin").mkdir(parents=True)
(wifi_theme / "config.json").write_bytes((ONION_THEME / "config.json").read_bytes())
for level in ("02", "03", "04"):
    Image.new("RGB", (200, 52), WIFI).save(wifi_theme / f"skin/icon-wifi-signal-{level}.png")
(RUNTIME / "wifi-status.txt").write_text("wpa_state=COMPLETED\nssid=Host Wi-Fi\n")
shot = ROOT_TEST / "wifi-slot.bmp"
result = subprocess.run([EXE, "--sd-root", str(SD), "--theme", str(wifi_theme), "--fallback",
                         str(ONION_THEME), "--device-dir", str(RUNTIME), "--snapshot", str(shot)],
                        capture_output=True, text=True, timeout=30)
assert result.returncode == 0, result.stderr
with Image.open(shot) as image:
    header = image.convert("RGB")
assert header.getpixel((428, 10)) == WIFI and header.getpixel((427, 10)) != WIFI
assert header.getpixel((500, 4)) == WIFI and header.getpixel((500, 3)) != WIFI
print("Device adapter fixtures passed:", ROOT_TEST)
