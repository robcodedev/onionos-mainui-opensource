# SPDX-License-Identifier: GPL-3.0-only
"""Maintenance wakes continue when SDL_AddTimer fails."""
import os
import re
import subprocess
from env import BUILD

PROBE = BUILD / "persistence-probe"


def ticks(fail):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    env.pop("MAINUI_TEST_TIMER_FAILURE", None)
    if fail:
        env["MAINUI_TEST_TIMER_FAILURE"] = "1"
    result = subprocess.run([str(PROBE), "ticks", str(BUILD)], env=env, capture_output=True,
                            text=True, timeout=20)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    found = re.search(r"ticks=3 elapsed=(\d+) timer=(\d)", result.stdout)
    assert found, result.stdout
    return int(found.group(1)), found.group(2) == "1", result.stderr


# Device builds wake every 500 ms, so three ticks take about 1.5 s either way.
elapsed, timer, log = ticks(False)
assert timer and 1300 <= elapsed <= 3000, (elapsed, timer)
assert "SDL_AddTimer failed" not in log
elapsed, timer, log = ticks(True)
assert not timer and 1300 <= elapsed <= 3000, (elapsed, timer)
assert log.count("SDL_AddTimer failed") == 1, log
print("Maintenance ticks continue on a deadline when SDL_AddTimer fails")
