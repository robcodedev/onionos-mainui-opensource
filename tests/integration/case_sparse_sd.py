# SPDX-License-Identifier: GPL-3.0-only
"""Focused sparse-card regressions; no Onion theme or SD fixture required."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from env import BUILD

sd = Path(tempfile.mkdtemp(prefix="sparse-sd-", dir=BUILD))

def run(mode, expected=0):
    result = subprocess.run([str(BUILD / "persistence-probe"), mode, str(sd)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == expected, (mode, result.returncode, result.stderr)
    return result

# Missing Emu and an empty Emu both produce an empty Systems list.
assert run("catalog-open").stdout.strip() == "0"
assert not (sd / "Emu").exists()
(sd / "Emu").mkdir()
assert run("catalog-open").stdout.strip() == "0"
# A non-directory is still a real error.
(sd / "Emu").rmdir()
(sd / "Emu").write_text("not a folder")
run("catalog-open", 3)
(sd / "Emu").unlink()
(sd / "Emu/Test").mkdir(parents=True)
config = sd / "Emu/Test/config.json"
for text in (None, "", "{broken"):
    if text is not None:
        config.write_text(text)
    assert run("catalog-open").stdout.strip() == "0"

# Absent and blank settings can be saved; nonempty corruption is preserved.
settings = sd / "system.json"
for text in (None, "", " \t\r\n"):
    if text is not None:
        settings.write_text(text)
    run("system-once")
    assert json.loads(settings.read_text()) == {"value": 1}
for content in (b"{broken", b"[]", b"null", b" \x00 "):
    settings.write_bytes(content)
    run("system-once", 3)
    assert settings.read_bytes() == content

config.write_text(json.dumps(dict(label="Test", rompath="../../Roms/Test", extlist="nes")))
assert run("catalog-open").stdout.strip() == "1"
assert "ROM folder is missing" in run("cache", 3).stderr
# Both normal Onion forms are accepted; absolute and relative escapes are ignored.
for rompath in ("/etc", "../../../../etc", str(sd) + "-outside/Roms"):
    config.write_text(json.dumps(dict(label="Test", rompath=rompath, extlist="nes")))
    assert run("catalog-open").stdout.strip() == "0"
for rompath in ("/mnt/SDCARD/Roms/Test", "../../Roms/Test"):
    config.write_text(json.dumps(dict(label="Test", rompath=rompath, extlist="nes")))
    assert run("catalog-open").stdout.strip() == "1"
roms = sd / "Roms/Test"
roms.mkdir(parents=True)
run("cache")
run("recover")
cache = roms / "Test_cache6.db"
with sqlite3.connect(cache) as db:
    assert db.execute("SELECT count(*) FROM Test_roms").fetchone() == (0,)

# An XML with no usable <gameList> is reported and not imported: the ROM
# files are listed instead, and the XML is left as it is.
(roms / "one.nes").write_bytes(b"ROM")
xml = roms / "miyoogamelist.xml"
for text in ("", "   ", "<gameList>"):
    xml.write_text(text)
    cache.unlink()
    assert "miyoogamelist.xml is unusable" in run("cache").stderr
    with sqlite3.connect(cache) as db:
        assert db.execute("SELECT disp FROM Test_roms").fetchall() == [("one",)]
    assert xml.read_text() == text
# Valid empty XML stays authoritative, even when a ROM exists.
xml.write_text("<gameList/>")
run("cache")
with sqlite3.connect(cache) as db:
    assert db.execute("SELECT count(*) FROM Test_roms").fetchone() == (0,)
assert not list(roms.glob("*.building.*"))
print("Sparse-card settings, discovery and cache diagnostics passed:", sd)
# A failed missing-cache write still allows ordinary filesystem browsing.
xml.unlink()
cache.unlink()
reservation = Path(str(cache) + ".building")
reservation.write_text("foreign")
run("recover")
assert not cache.exists() and reservation.read_text() == "foreign"
reservation.unlink()
# A present XML is never scanned around when the cache cannot be written,
# even an unusable one.
xml.write_text("")
reservation.write_text("foreign")
run("recover", 3)
assert not cache.exists()
reservation.unlink()
# Once it can be written, an unusable XML gives a cache of the ROM files.
assert "is unusable" in run("recover").stderr
assert cache.exists()
cache.unlink()

# Symlinked ROM roots must never scan, build, or remove an external cache.
xml.unlink()
outside = Path(tempfile.mkdtemp(prefix="outside-roms-", dir=BUILD))
(outside / "one.nes").write_bytes(b"external ROM")
(outside / "Test_cache6.db").write_bytes(b"external cache")
(outside / "Test_cache6.db.building.stale").write_bytes(b"external staging")
roms.rename(roms.with_name("Test-real"))
roms.symlink_to(outside.resolve(), target_is_directory=True)
before = {p.name: p.read_bytes() for p in outside.iterdir()}
assert run("catalog-open").stdout.strip() == "1"
for mode in ("recover", "cache", "remove-cache"):
    run(mode, 3)
assert {p.name: p.read_bytes() for p in outside.iterdir()} == before
roms.unlink()
roms.with_name("Test-real").rename(roms)

# A cached Delete must not open an external SQLite file read/write.
run("cache")
external_cache = outside / "external.db"
external_cache.write_bytes(cache.read_bytes())
cache.unlink()
cache.symlink_to(external_cache.resolve())
before = external_cache.read_bytes()
refused = run("delete", 3)
assert "ROM cache must be a regular file" in refused.stdout, refused.stdout
assert external_cache.read_bytes() == before
assert (roms / "one.nes").read_bytes() == b"ROM"
assert not list(outside.glob("external.db-*"))
cache.unlink()

# In scan fallback, unlink can succeed before the directory flush fails.
reservation.write_text("foreign")
env = dict(os.environ, MAINUI_TEST_SYNC_FAILURE="one.nes")
result = subprocess.run([str(BUILD / "persistence-probe"), "delete", str(sd)],
                        env=env, capture_output=True, text=True, timeout=10)
assert result.returncode == 3, result
assert "ROM removed, but saving the deletion" in result.stdout, result.stdout
assert "preserved" not in result.stdout
assert not (roms / "one.nes").exists()
reservation.unlink()

# Links inside the ROM tree are never followed: neither a linked folder that
# leaves the tree nor a linked file is listed. Real entries still are.
(roms / "kept.nes").write_bytes(b"ROM")
linked = Path(tempfile.mkdtemp(prefix="outside-linked-", dir=BUILD))
(linked / "secret.nes").write_bytes(b"external ROM")
(roms / "escape").symlink_to(linked.resolve(), target_is_directory=True)
(roms / "link.nes").symlink_to((linked / "secret.nes").resolve())
run("cache")
with sqlite3.connect(cache) as db:
    paths = [row[0] for row in db.execute("SELECT path FROM Test_roms")]
assert any(p.endswith("kept.nes") for p in paths), paths
assert not [p for p in paths if "secret" in p or "escape" in p or "link.nes" in p], paths
(roms / "escape").unlink()
(roms / "link.nes").unlink()

# The scanned folders themselves (Emu, App) are not followed when they are links.
outside_emu = Path(tempfile.mkdtemp(prefix="outside-emu-", dir=BUILD))
(outside_emu / "Test").mkdir()
(outside_emu / "Test/config.json").write_text(
    json.dumps(dict(label="Outside", rompath="../../Roms/Test", extlist="nes")))
(sd / "Emu").rename(sd / "Emu-real")
(sd / "Emu").symlink_to(outside_emu.resolve(), target_is_directory=True)
refused = run("catalog-open", 3)
assert refused.stdout.strip() == "0", refused.stdout
assert "symlink" in refused.stderr, refused.stderr
(sd / "Emu").unlink()
# A dangling Emu link is refused the same way, not treated as a missing folder.
(sd / "Emu").symlink_to(sd / "no-such-folder", target_is_directory=True)
refused = run("catalog-open", 3)
assert "symlink" in refused.stderr, refused.stderr
(sd / "Emu").unlink()
(sd / "Emu-real").rename(sd / "Emu")
(sd / "App").symlink_to(outside_emu.resolve(), target_is_directory=True)
refused = run("apps-open", 3)
assert refused.stdout.strip() == "0" and "symlink" in refused.stderr, refused
(sd / "App").unlink()
assert run("apps-open").stdout.strip() == "0"

# One unusable config hides only its own console or app, never the rest.
bad_sd = Path(tempfile.mkdtemp(prefix="bad-configs-", dir=BUILD))
good = dict(label="Good", rompath="../../Roms/Good", extlist="nes")
bad_configs = {
    "LongExt": dict(good, label="LongExt", extlist="|".join(["nes"] * 400)),
    "LongIcon": dict(good, label="LongIcon", icon="x" * 5000),
    "DriveIcon": dict(good, label="DriveIcon", icon="C:icon.png"),
    "LongImages": dict(good, label="LongImages", imgpath="y" * 5000),
}
for name, config in dict(Good=good, **bad_configs).items():
    (bad_sd / "Emu" / name).mkdir(parents=True)
    (bad_sd / "Emu" / name / "config.json").write_text(json.dumps(config))
    (bad_sd / "App" / name).mkdir(parents=True)
    app = dict(config, launch="launch.sh")
    app.pop("rompath")
    (bad_sd / "App" / name / "config.json").write_text(json.dumps(app))
(bad_sd / "Roms/Good").mkdir(parents=True)
for mode in ("catalog-open", "apps-open"):
    result = subprocess.run([str(BUILD / "persistence-probe"), mode, str(bad_sd)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (mode, result.stderr)
    assert result.stdout.strip() == "1", (mode, result.stdout, result.stderr)
    for name in bad_configs:
        if mode == "catalog-open" or name != "LongExt":
            assert f"/{name}: " in result.stderr, (mode, name, result.stderr)

# A host file name containing a backslash is skipped instead of being resolved
# as a path (FAT cannot store such names).
(roms / "odd\\name.nes").write_bytes(b"ROM")
run("cache")
with sqlite3.connect(cache) as db:
    paths = [row[0] for row in db.execute("SELECT path FROM Test_roms")]
assert any(p.endswith("kept.nes") for p in paths), paths
assert not [p for p in paths if "odd" in p or "name.nes" in p], paths
(roms / "odd\\name.nes").unlink()
