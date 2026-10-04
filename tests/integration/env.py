# SPDX-License-Identifier: GPL-3.0-only
"""Paths shared by the integration cases, configured by run.py.

BUILD is the directory holding the binaries produced by the Makefile.
ONION_THEME is derived from ONION_ROOT, the same variable the Makefile uses, so
the Onion checkout is configured in one place (config.mk) for both builds and
tests. Cases needing it are skipped when it is absent, so a plain checkout can
still run the rest of the suite -- unless MAINUI_STRICT is set, which turns a
missing prerequisite into a failure. CI sets it so an all-skipped run cannot
pass as green.

The fixture SD tree is not in the repository, so under MAINUI_STRICT a missing
tree only fails when MAINUI_FIXTURE_SD names one explicitly. Otherwise those
cases still skip, and the skip is reported.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("MAINUI_BUILD_DIR", ROOT / "build"))
ONION_ROOT = Path(os.environ.get("ONION_ROOT", "/root/workspace/Onion"))
ONION_THEME = Path(os.environ.get(
    "ONION_THEME", ONION_ROOT / "static/build/miyoo/app"))
STRICT = bool(os.environ.get("MAINUI_STRICT"))


FIXTURE_SD_SET = "MAINUI_FIXTURE_SD" in os.environ
FIXTURE_SD = Path(os.environ.get("MAINUI_FIXTURE_SD", ROOT / "tests/fixtures/sdcard"))


def skip(reason):
    """Report a missing prerequisite. Fails the case when MAINUI_STRICT is set."""
    if STRICT:
        print(f"Missing prerequisite (MAINUI_STRICT): {reason}")
        raise SystemExit(1)
    print(f"Skip: {reason}")
    raise SystemExit(0)


def require_fixture_sd():
    if not FIXTURE_SD.is_dir():
        if STRICT and not FIXTURE_SD_SET:
            print(f"Skip: fixture SD tree not found at {FIXTURE_SD} (not in the repository; "
                  "set MAINUI_FIXTURE_SD to require it)")
            raise SystemExit(0)
        skip(f"fixture SD tree not found at {FIXTURE_SD}")
    return FIXTURE_SD


def require_onion_theme():
    try:
        found = ONION_THEME.is_dir()
    except OSError as error:  # for example no permission to look (Python 3.12 raises)
        found, reason = False, f" ({error.strerror})"
    else:
        reason = ""
    if not found:
        skip(f"Onion theme tree not found at {ONION_THEME}{reason}; set ONION_ROOT")
    return ONION_THEME


def unlink_if_exists(path):
    """Remove a file if present, including on Python 3.7."""
    try:
        path.unlink()
    except FileNotFoundError:
        pass
