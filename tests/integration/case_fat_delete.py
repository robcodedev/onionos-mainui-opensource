# SPDX-License-Identifier: GPL-3.0-only
"""Delete/recover without hard links; optionally remount a real vfat fixture."""
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import time

from env import BUILD

SD = Path(tempfile.mkdtemp(prefix="fat-delete-", dir=os.environ.get("MAINUI_FAT_ROOT", BUILD)))
PROBE = BUILD / "persistence-probe"
ENV = dict(os.environ)
ENV.pop("MAINUI_TEST_FAULT", None)
shim = BUILD / "no-hardlinks.so"
# Set by the no-hardlinks make target; also used when this case runs normally.
if shim.exists():
    # ASan must precede the shim in sanitized executables.
    libraries = subprocess.run(["ldd", str(PROBE)], check=True, capture_output=True, text=True)
    asan = next((line.split("=>", 1)[1].split()[0]
                 for line in libraries.stdout.splitlines() if "libasan.so" in line), "")
    ENV["LD_PRELOAD"] = " ".join(filter(None, (asan, ENV.get("LD_PRELOAD"), str(shim))))

def run(mode, fault=None):
    env = dict(ENV)
    if fault:
        env["MAINUI_TEST_FAULT"] = fault
    return subprocess.run([str(PROBE), mode, str(SD)], env=env,
                          capture_output=True, text=True, timeout=5)

def require(mode, code=0, fault=None):
    result = run(mode, fault)
    assert result.returncode == code, (mode, result.returncode, result.stdout, result.stderr)

def remount():
    mount = os.environ.get("MAINUI_FAT_ROOT")
    if mount:
        # Desktop indexers can hold a fresh mount briefly; retry, then name the holder.
        for _ in range(10):
            if subprocess.run(["sudo", "-n", "umount", mount]).returncode == 0:
                break
            time.sleep(0.2)
        else:
            holders = subprocess.run(["sudo", "-n", "fuser", "-vm", mount],
                                     capture_output=True, text=True)
            raise RuntimeError(f"cannot unmount {mount}:\n{holders.stdout}{holders.stderr}")
        subprocess.run(["sudo", "-n", "mount", "-o", f"loop,uid={os.getuid()},gid={os.getgid()}",
                        os.environ["MAINUI_FAT_IMAGE"], mount], check=True)

(SD / "Emu/Test").mkdir(parents=True)
(SD / "Roms/Test").mkdir(parents=True)
(SD / "Emu/Test/config.json").write_text(json.dumps(dict(
    label="Test", launch="launch.sh", rompath="../../Roms/Test", extlist="nes")))
rom = SD / "Roms/Test/one.nes"
cache = rom.parent / "Test_cache6.db"
journal = Path(str(cache) + ".delete.json")

def prepare():
    rom.write_bytes(b"ROM payload")
    require("cache")

def check(retained):
    assert rom.exists() == retained
    if retained:
        assert rom.read_bytes() == b"ROM payload"
    assert not journal.exists()
    assert not list(rom.parent.glob("*.mainui-delete*"))
    # sqlite3's own context manager only commits; it never closes, and an open
    # database keeps the vfat mount busy at remount().
    with closing(sqlite3.connect(cache)) as db:
        assert bool(db.execute("SELECT 1 FROM Test_roms WHERE path='/mnt/SDCARD/Emu/Test/../../Roms/Test/one.nes'").fetchone()) == retained

# Locked publication must never truncate an existing destination or reservation.
locked = SD / "locked.bin"
reservation = SD / "locked.bin.writing"
reservation.write_bytes(b"foreign")
require("locked-write", 3)
assert not locked.exists() and reservation.read_bytes() == b"foreign"
reservation.unlink()
require("locked-write")
assert locked.read_bytes() == b"value"
locked.write_bytes(b"preserve")
require("locked-write", 3)
assert locked.read_bytes() == b"preserve"

prepare()
require("delete")
check(False)
for phase, retained in (("delete-moved", True), ("delete-committed", False)):
    for legacy in (False, True):
        prepare()
        require("delete", 77, phase)
        intent = json.loads(journal.read_text())
        staged = Path(intent["staged"])
        assert staged.is_file() and not rom.exists()
        assert len(staged.name.rsplit(".", 1)[1]) == 16
        assert "identity" not in intent and int(intent["size"]) == 11
        if legacy:
            fixed = Path(str(rom) + ".mainui-delete")
            staged.rename(fixed)
            intent.pop("staged")
            intent.pop("size")
            # Deliberately wrong inode and subsecond mtime: legacy uses size only.
            intent["identity"] = "999999999:11:123456789"
            journal.write_text(json.dumps(intent))
        remount()
        require("recover")
        check(retained)

# Same catalog instance skips a busy lock, then retries on the next entry.
prepare()
require("delete", 77, "delete-moved")
require("recover-busy")
check(True)

# Malformed/incomplete journals without staging are safe to discard.
for text in ("{", "{}", '{"original": "missing"}'):
    journal.write_text(text)
    require("recover")
    assert not journal.exists() and rom.exists()

# Preserve unreadable journals whenever legacy or random staging remains.
for suffix in (".mainui-delete", ".mainui-delete.0123456789abcdef"):
    staged = Path(str(rom) + suffix)
    rom.rename(staged)
    journal.write_text("{")
    require("recover-only", 3)
    assert journal.read_text() == "{" and staged.read_bytes() == b"ROM payload"
    staged.rename(rom)
    journal.unlink()

# A mismatched size must never destroy the staged payload.
prepare()
require("delete", 77, "delete-moved")
intent = json.loads(journal.read_text())
staged = Path(intent["staged"])
staged.write_bytes(b"different payload")
require("recover-only", 3)
assert staged.read_bytes() == b"different payload" and journal.exists()
staged.write_bytes(b"ROM payload")
require("recover")
check(True)

# A journal naming any other file as "staged" must never remove it, even when
# the size matches and the database row is already gone.
prepare()
require("delete", 77, "delete-committed")
intent = json.loads(journal.read_text())
bystander = rom.parent / "other.nes"
bystander.write_bytes(b"ROM payload")
journal.write_text(json.dumps(dict(intent, staged=str(bystander))))
require("recover-only", 3)
assert bystander.read_bytes() == b"ROM payload" and journal.exists()
journal.write_text(json.dumps(intent))
require("recover")
check(False)
assert bystander.read_bytes() == b"ROM payload"
bystander.unlink()
def listed():
    with closing(sqlite3.connect(cache)) as db:
        return [row[0].rsplit("/", 1)[1] for row in db.execute("SELECT path FROM Test_roms")]

# A recreated original is a conflict regardless of size, staged presence or commit state.
# Browsing continues and Delete stays blocked, but Refresh roms (in the list: "cache";
# from the selector: "remove-cache") drops only the journal and keeps both ROM files.
for phase, refresh in (("delete-moved", "cache"), ("delete-committed", "remove-cache")):
    prepare()
    require("delete", 77, phase)
    staged = Path(json.loads(journal.read_text())["staged"])
    rom.write_bytes(b"new payload")
    require("recover-only", 3)
    assert rom.read_bytes() == b"new payload" and staged.read_bytes() == b"ROM payload"
    assert journal.exists()
    preserved = (rom.read_bytes(), staged.read_bytes(), journal.read_bytes(), cache.read_bytes())
    result = run("recover")
    assert result.returncode == 0 and str(staged) in result.stderr, result.stderr
    refused = run("delete")
    assert refused.returncode == 3 and str(staged) in refused.stdout, refused.stdout
    assert "Refresh roms" in refused.stdout, refused.stdout
    assert (rom.read_bytes(), staged.read_bytes(), journal.read_bytes(), cache.read_bytes()) == preserved
    result = run(refresh)
    assert result.returncode == 0 and str(staged) in result.stderr, result.stderr
    assert rom.read_bytes() == b"new payload" and staged.read_bytes() == b"ROM payload"
    assert not journal.exists()
    remount()
    require("recover")  # re-entering rebuilds a removed cache; staged names are not listed
    assert listed() == ["one.nes"]
    require("delete")  # Delete works again
    assert not rom.exists() and staged.read_bytes() == b"ROM payload"
    staged.unlink()  # the extra copy is the user's to keep or remove
    check(False)

# A folder flush that fails after the journal is gone does not fail Refresh roms
# (review of 1.0.2, finding 4); a journal that cannot be removed still does.
for refresh in ("cache", "remove-cache"):
    prepare()
    require("delete", 77, "delete-committed")
    staged = Path(json.loads(journal.read_text())["staged"])
    rom.write_bytes(b"new payload")
    env = dict(ENV, MAINUI_TEST_SYNC_FAILURE=journal.name)
    result = subprocess.run([str(PROBE), refresh, str(SD)], env=env, capture_output=True,
                            text=True, timeout=5)
    assert result.returncode == 0, (refresh, result.stdout, result.stderr)
    assert "flushing its folder failed" in result.stderr, result.stderr
    assert not journal.exists()
    assert rom.read_bytes() == b"new payload" and staged.read_bytes() == b"ROM payload"
    staged.unlink()
    rom.unlink()
if not os.environ.get("MAINUI_FAT_ROOT") and os.geteuid() != 0:
    prepare()
    require("delete", 77, "delete-committed")
    staged = Path(json.loads(journal.read_text())["staged"])
    rom.write_bytes(b"new payload")
    cache.parent.chmod(0o555)
    try:
        assert run("cache").returncode == 3
    finally:
        cache.parent.chmod(0o755)
    assert journal.exists()
    require("cache")
    staged.unlink()
    rom.unlink()

# A journal left without its cache is still dropped by Refresh roms from the selector.
prepare()
require("delete", 77, "delete-committed")
staged = Path(json.loads(journal.read_text())["staged"])
rom.write_bytes(b"new payload")
cache.unlink()
require("remove-cache")
assert not journal.exists() and rom.read_bytes() == b"new payload"
assert staged.read_bytes() == b"ROM payload"
staged.unlink()
rom.unlink()
prepare()
require("delete", 77, "delete-committed")
staged = Path(json.loads(journal.read_text())["staged"])
staged.unlink()
rom.write_bytes(b"new payload")
require("recover-only", 3)
assert rom.read_bytes() == b"new payload" and journal.exists()
rom.unlink()
require("recover")
check(False)

# Recovery must bound card-provided SQL, just like the regular cache reader.
prepare()
require("delete", 77, "delete-committed")
staged = Path(json.loads(journal.read_text())["staged"])
before = cache.read_bytes()
with closing(sqlite3.connect(cache)) as db:
    db.execute("DROP TABLE Test_roms")
    db.execute("CREATE VIEW Test_roms AS WITH RECURSIVE n(x) AS "
               "(VALUES(1) UNION ALL SELECT x+1 FROM n) "
               "SELECT 0 AS type, CAST(x AS TEXT) AS path FROM n")
    db.commit()
require("recover-only", 3)
assert journal.exists() and staged.read_bytes() == b"ROM payload"
cache.write_bytes(before)
require("recover")
check(False)

# VFAT cannot represent symlinks; exercise these host-only cases on ordinary Linux filesystems.
if not os.environ.get("MAINUI_FAT_ROOT"):
    # A dangling journal symlink is an entry, not an absence: both Refresh roms
    # routes remove the link itself, and a following Delete can journal again.
    target = SD.parent / (SD.name + "-missing-journal")
    for refresh in ("cache", "remove-cache"):
        prepare()
        journal.symlink_to(target)
        # Recovery, console entry and the Delete guard all see the entry.
        require("recover-only", 3)
        refused = run("delete")
        assert refused.returncode == 3 and "Refresh roms" in refused.stdout, refused.stdout
        assert os.path.lexists(journal) and not target.exists()
        assert rom.read_bytes() == b"ROM payload"
        require(refresh)
        assert not os.path.lexists(journal) and not target.exists()
        assert rom.read_bytes() == b"ROM payload"
    require("recover")
    require("delete")
    check(False)

    prepare()
    require("delete", 77, "delete-committed")
    outside = SD.parent / (SD.name + "-outside")
    (SD / "Roms").rename(outside)
    try:
        (SD / "Roms").symlink_to(outside.resolve(), target_is_directory=True)
        require("recover-only", 3)
        assert journal.exists()
        assert len(list((outside / "Test").glob("*.mainui-delete.*"))) == 1
    finally:
        if (SD / "Roms").is_symlink():
            (SD / "Roms").unlink()
        outside.rename(SD / "Roms")
    require("recover")
    check(False)

print("FAT-safe delete, remount recovery, legacy journals, staged-name validation, nonblocking retry "
      "and journal-only refresh passed:", SD)
