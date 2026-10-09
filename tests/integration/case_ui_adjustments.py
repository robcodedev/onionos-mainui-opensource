# SPDX-License-Identifier: GPL-3.0-only
"""Regression captures for the September UI parity fixes, on an isolated SD tree."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image
from env import unlink_if_exists, BUILD, ONION_THEME, require_fixture_sd, require_onion_theme  # noqa: E402

require_onion_theme()

FIXTURE_SD = require_fixture_sd()

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(tempfile.mkdtemp(prefix="ui-adjustments-", dir=BUILD))
SD, THEME, RUNTIME = OUT / "sd", OUT / "theme", OUT / "runtime"
FALLBACK = ONION_THEME
EXE = str(BUILD / "MainUI-dev")
MARKER = (17, 231, 93)
TITLE = (240, 32, 48)
DESCRIPTION = (31, 63, 239)
DIVIDER = (23, 197, 201)


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def menu(sections, settings=None):
    value = {"menu": {section: True for section in sections}}
    if settings:
        value["settings"] = settings
    write(SD / ".tmp_update/config/main-menu.json", json.dumps(value))


def capture(name, actions="", *options):
    # Each action sequence starts from a fresh list position; persistence has
    # separate coverage and must not leak between these presentation captures.
    unlink_if_exists((SD / "appconfigs/romwinidx.json"))
    target = OUT / (name + ".bmp")
    subprocess.run([EXE, "--sd-root", str(SD), "--theme", str(THEME),
                    "--fallback", str(FALLBACK), "--handoff-dir", str(RUNTIME),
                    "--input", actions, "--snapshot", str(target), *options],
                   cwd=ROOT, check=True, timeout=30)
    with Image.open(target) as image:
        return image.convert("RGB")


def pixels(image, box):
    return image.crop(box).tobytes()


def has_color(image, color, box=(0, 60, 640, 420)):
    return color in image.crop(box).getdata()


RUNTIME.mkdir()
(THEME / "skin").mkdir(parents=True)
style = json.loads((FALLBACK / "config.json").read_text())
style["title"]["color"] = "#f02030"
style["list"]["color"] = "#eeeeee"
style["grid"]["color"] = "#1f3fef"
style["grid"]["selectedcolor"] = "#7f7f7f"
write(THEME / "config.json", json.dumps(style))
Image.new("RGB", (640, 480), (0, 0, 0)).save(THEME / "skin/background.png")
Image.new("RGBA", (640, 60)).save(THEME / "skin/bg-title.png")
Image.new("RGB", (640, 2), DIVIDER).save(THEME / "skin/div-line-h.png")
Image.new("RGBA", (640, 60)).save(THEME / "skin/tips-bar-bg.png")
Image.new("RGB", (12, 12), MARKER).save(THEME / "skin/ic-favorite-mark.png")
Image.new("RGB", (60, 30), (70, 71, 72)).save(THEME / "skin/num-bg.png")
digits = Image.new("RGB", (100, 18))
for i in range(10):
    digits.paste((100 + i * 10, 20, 200), (i * 10, 0, (i + 1) * 10, 18))
digits.save(THEME / "skin/list-num.png")
write(SD / "Emu/Console/config.json", json.dumps(dict(
    label="Console", rompath="../../Roms/Console", launch="launch.sh", extlist="nes")))
write(SD / "Roms/Console/Folder/nested.nes", "")
write(SD / "Roms/Console/Folder/Deeper/deep.nes", "")
write(SD / "Roms/Console/alpha.nes", "")
write(SD / "Roms/Console/gamelist.xml", '<gameList><game><path>alpha.nes</path>'
      '<genre>Action</genre><desc>Description text.</desc></game></gameList>')
write(SD / "Roms/Console/miyoogamelist.xml", '<gameList>'
      '<game><path>./Folder/nested.nes</path><name>nested</name></game>'
      '<game><path>./Folder/Deeper/deep.nes</path><name>deep</name></game>'
      '<game><path>./alpha.nes</path><name>alpha</name>'
      '<image>./Imgs/XML cover.png</image></game></gameList>')
record = dict(label="alpha", rompath="/mnt/SDCARD/Emu/Console/../../Roms/Console/./alpha.nes",
              launch="/mnt/SDCARD/Emu/Console/launch.sh",
              imgpath="/mnt/SDCARD/Emu/Console/../../Roms/Console/./Imgs/XML cover.png", type=5)
write(SD / "Roms/favourite.json", json.dumps(record) + "\n")
write(SD / "Roms/recentlist.json", json.dumps(record) + "\n")
menu(["games"])
root = capture("rom-root", "EE")
assert has_color(root, TITLE, (0, 0, 640, 60))
assert root.tobytes() == capture("left-no-back", "EEL").tobytes()
assert root.tobytes() == capture("right-no-folder", "EER").tobytes()
folder = capture("folder", "EEE")
assert has_color(folder, TITLE, (0, 0, 640, 60))
# Stock uses the emulator label, including nested folders and returning upward.
for actions in ("EEE", "EEEDE", "EEEDEE"):
    assert pixels(root, (0, 0, 640, 60)) == pixels(capture("rom-header-" + actions, actions), (0, 0, 640, 60))
assert folder.tobytes() == capture("folder-left", "EEEL").tobytes()
assert folder.tobytes() == capture("parent-right", "EEER").tobytes()
assert root.tobytes() == capture("parent-enter", "EEEE").tobytes()
game = capture("game", "EED")
assert has_color(game, MARKER)
detail = capture("detail", "EEDR")
# First game is 0001 despite the preceding folder; no total is drawn.
assert detail.getpixel((276, 72)) == (100, 20, 200)
assert detail.getpixel((287, 72)) == (100, 20, 200)
assert detail.getpixel((298, 72)) == (100, 20, 200)
assert detail.getpixel((309, 72)) == (110, 20, 200)
assert pixels(detail, (0, 420, 480, 480)) == pixels(game, (0, 420, 480, 480))
assert game.tobytes() == capture("details-left", "EEDRL").tobytes()

for section in ("favorites", "recents"):
    menu([section])
    saved = capture(section, "E")
    assert has_color(saved, MARKER) == (section == "recents")
    assert has_color(saved, TITLE, (0, 0, 640, 60))
    assert saved.tobytes() == capture(section + "-left", "EL").tobytes()
# Membership must still work when the favorite is assigned to a folder.
sidecar = [dict(schema=1, generation=1),
           dict(kind="folder", id="f", parent="", name="Saved folder", order=0),
           dict(kind="folder", id="g", parent="f", name="Deeper favorite", order=0),
           dict(kind="item", key=record["rompath"], type=5, folder="f", order=0)]
write(SD / "Roms/favourite-folders.json", "".join(json.dumps(row) + "\n" for row in sidecar))
menu(["favorites"])
saved_folder = capture("saved-folder", "EE")
saved_root = capture("saved-root", "E")
for actions in ("EE", "EEDE", "EEDEE"):
    assert pixels(saved_root, (0, 0, 640, 60)) == pixels(capture("favorite-header-" + actions, actions), (0, 0, 640, 60))
assert has_color(saved_folder, TITLE, (0, 0, 640, 60))
assert not has_color(saved_folder, MARKER)
assert saved_folder.tobytes() == capture("saved-folder-arrows", "EELR").tobytes()
menu(["games"])
remove_popup = capture("remove-popup", "EEDS")
capture("remove-favorite", "EEDSDE")
assert not (SD / "Roms/favourite.json").read_text().strip()
add_popup = capture("add-popup", "EEDS")
assert pixels(remove_popup, (0, 60, 400, 120)) != pixels(add_popup, (0, 60, 400, 120))
capture("add-favorite", "EEDSDE")
assert json.loads((SD / "Roms/favourite.json").read_text())["rompath"] == record["rompath"]

write(SD / "App/Example/config.json", json.dumps(dict(label="Example", description="App description", launch="launch.sh")))
menu(["apps"])
apps = capture("apps", "E")
assert apps.getpixel((639, 240)) == DIVIDER
assert has_color(apps, DESCRIPTION, (0, 100, 640, 150))
# Binary 0x20090..0x203dc: fixed icon lane, text x111, row top 62, desc y107,
# as a stock screenshot confirms.
icon_color = (203, 71, 9)
icon_path = SD / "App/Example/icon.png"
Image.new("RGB", (64, 64), icon_color).save(icon_path)
write(SD / "App/Example/config.json", json.dumps(dict(
    label="Example", description="App description", launch="launch.sh", icon="icon.png")))
with_icon = capture("apps-fixed-icon", "E")
assert with_icon.getpixel((20, 75)) == icon_color
assert with_icon.getpixel((83, 138)) == icon_color
assert with_icon.getpixel((83, 139)) != icon_color
assert with_icon.getpixel((19, 75)) != icon_color
assert not has_color(with_icon, DESCRIPTION, (0, 60, 111, 150))
assert has_color(with_icon, DESCRIPTION, (111, 107, 640, 150))
# Both oversized dimensions use the stock centered 71px source crop at row top.
Image.new("RGB", (120, 120), icon_color).save(icon_path)
oversized = capture("apps-cropped-icon", "E")
assert oversized.getpixel((20, 62)) == icon_color
assert oversized.getpixel((20, 61)) != icon_color
assert oversized.getpixel((90, 132)) == icon_color
assert oversized.getpixel((91, 132)) != icon_color
# The fourth row is drawn in full: its highlight covers y 420-421, over the
# footer, as stock draws it. Clipping at 420 cut off a framed highlight.
SELECTED = (201, 23, 97)
Image.new("RGB", (640, 90), SELECTED).save(THEME / "skin/bg-list-l.png")
for name in ("Second", "Third", "Fourth"):
    write(SD / f"App/{name}/config.json", json.dumps(dict(label=name, launch="launch.sh")))
fourth = capture("apps-fourth-row", "EDDD")
assert fourth.getpixel((320, 332)) == SELECTED
assert fourth.getpixel((320, 421)) == SELECTED
assert fourth.getpixel((320, 422)) != SELECTED
(THEME / "skin/bg-list-l.png").unlink()


menu(["settings"], ["display", "brightness", "sound", "sleep", "about"])
write(SD / "system.json", json.dumps(dict(brightness=1, vol=1, bgmvol=1, hibernate=5,
      lumination=1, hue=1, saturation=1, contrast=1)))
settings = capture("settings", "E")
display = capture("display", "EE")
assert pixels(display, (0, 420, 640, 480)) != pixels(settings, (0, 420, 640, 480))
assert not any(pixels(display, (0, 350, 640, 420)))
capture("display-zero", "EE" + "LLD" * 3 + "LLE")
values = json.loads((SD / "system.json").read_text())
assert all(values[key] == 0 for key in ("lumination", "hue", "saturation", "contrast"))
zero = capture("display-zero-reopen", "EE")
assert pixels(zero, (0, 60, 640, 350)) == pixels(capture("display-zero-clamped", "EEL"), (0, 60, 640, 350))
for name, down, key in (("brightness", 1, "brightness"), ("sound", 2, "bgmvol"), ("sleep", 3, "hibernate")):
    capture(name + "-zero", "E" + "D" * down + "L")
    assert json.loads((SD / "system.json").read_text())[key] == 0
    if name != "sleep":
        before = capture(name + "-reopen", "E" + "D" * down)
        assert before.tobytes() == capture(name + "-clamped", "E" + "D" * down + "L").tobytes()

for name, value in {"deviceModel": "354", "serial-number.txt": "ABC123",
                    "firmware-version.txt": "20260910", "cpu-frequency.txt": "1200MHz",
                    "memory-size.txt": "128M", "storage-usage.txt": "1G/32G"}.items():
    write(RUNTIME / name, value)
about = capture("about", "EDDDDE")
assert about.getpixel((639, 360)) == DIVIDER
last = capture("about-last", "EDDDDE" + "D" * 6)
assert about.tobytes() != last.tobytes()
assert about.tobytes() == capture("about-wrap", "EDDDDE" + "D" * 10).tobytes()
write(RUNTIME / "serial-number.txt", "Changed serial")
changed = capture("about-serial", "EDDDDE")
assert pixels(about, (0, 242, 640, 302)) != pixels(changed, (0, 242, 640, 302))
assert pixels(about, (0, 302, 640, 420)) == pixels(changed, (0, 302, 640, 420))
menu(["games"])
style["total"] = {"color": "#03f5fd"}
style["currentpage"] = {"color": "#fc03ed"}
write(THEME / "config.json", json.dumps(style))
for name, count in (("Small", 99), ("Large", 100), ("Empty", 0)):
    write(SD / f"Emu/{name}/config.json", json.dumps(dict(
        label=name, rompath=f"../../Roms/{name}", launch="launch.sh", extlist="nes")))
    (SD / f"Roms/{name}").mkdir(parents=True, exist_ok=True)
    for i in range(count):
        write(SD / f"Roms/{name}/{i:03}.nes", "")
    frame = capture("counter-" + name, "", "--system", name)
    points = [(x, y) for y in range(420, 480) for x in range(480, 640)
              if frame.getpixel((x, y)) == (3, 245, 253)]
    if count:
        assert points
        right = max(x for x, _ in points)
        assert (610 <= right < 620) if count == 99 else (590 <= right < 600)
    else:
        assert not points
        assert not has_color(frame, (252, 3, 237), (480, 420, 640, 480))
print("UI adjustment navigation, markers, popup, colors, detail digits, Settings zero persistence and About passed:", OUT)


# Repeat boundary navigation and details round trips with two fixture themes.
# Compare states within each theme rather than using brittle cross-theme pixels.
theme_root = FIXTURE_SD / "Themes"
extra_themes = [theme_root / "Silky by DiMo",
                next(path.parent for path in theme_root.rglob("config.json")
                     if path.parent.name == "Material White by tenlevels")]
original_theme = THEME
menu(["games"])
write(SD / ".tmp_update/config/.romListRows", "6")
for count in (0, 1, 6, 7):
    name = f"Boundary{count}"
    write(SD / f"Emu/{name}/config.json", json.dumps(dict(
        label=name, rompath=f"../../Roms/{name}", launch="launch.sh", extlist="nes")))
    (SD / f"Roms/{name}").mkdir(parents=True, exist_ok=True)
    for index in range(count):
        write(SD / f"Roms/{name}/{index:02d} Long game title for wrapping across the detail panel.nes", "")
    for theme_index, THEME in enumerate(extra_themes):
        tag = f"theme{theme_index}-boundary{count}"
        first = capture(tag, "", "--system", name)
        assert first.tobytes() == capture(tag + "-wrap", "UD", "--system", name).tobytes()
        if count:
            details = capture(tag + "-details", "R", "--system", name)
            assert details.tobytes() == capture(tag + "-detail-wrap", "RDU", "--system", name).tobytes()
            assert first.tobytes() == capture(tag + "-back", "RL", "--system", name).tobytes()
        else:
            assert first.tobytes() == capture(tag + "-no-detail", "R", "--system", name).tobytes()
THEME = original_theme

# Long translations must change text, not paint into the unused Display rows.
menu(["settings"], ["language", "display"])
lang_path = SD / "miyoo/app/lang/en.lang"
write(lang_path, json.dumps({"lang": "Test", "119": "Short luminance"}))
short = capture("short-translation", "EEEDE")
write(lang_path, json.dumps({"lang": "Test", "119": "Long translated luminance " * 16,
                            "124": "Apply translated changes " * 12,
                            "89": "Translated back " * 12}))
long = capture("long-translation", "EEEDE")
assert pixels(short, (0, 60, 640, 120)) != pixels(long, (0, 60, 640, 120))
assert pixels(short, (0, 350, 640, 420)) == pixels(long, (0, 350, 640, 420))
assert pixels(short, (0, 420, 640, 480)) != pixels(long, (0, 420, 640, 480))
print("Additional themes, 0/1/6/7-row boundaries, long titles/translations and details returns passed")
