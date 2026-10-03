#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the integration cases against the binaries built by the Makefile.

Each case_*.py in this directory is an independent script: it builds an SD tree
under the build directory, runs a fixture binary or the development preview
against it, and asserts on the result. Cases are run as subprocesses so one
crash cannot take the runner down, and so a case can still be run directly:

    python3 tests/integration/case_cache.py

Cases that need the Onion reference tree report themselves as skipped when it
is absent, which keeps a plain checkout usable. MAINUI_ALLOWED_SKIPS, a list of
case names separated by spaces or commas, turns any other skip into a failure;
CI sets it so a newly skipping case cannot pass unnoticed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=str(ROOT / "build"),
                        help="Directory holding the built binaries")
    parser.add_argument("--timeout", type=int, default=300,
                        help="Maximum seconds for a single case")
    parser.add_argument("--list", action="store_true", help="Print case names and exit")
    parser.add_argument("cases", nargs="*", help="Run only these cases")
    arguments = parser.parse_args()

    available = sorted(path.stem[len("case_"):] for path in HERE.glob("case_*.py"))
    if arguments.list:
        print("\n".join(available))
        return 0
    selected = arguments.cases or available
    unknown = [name for name in selected if name not in available]
    if unknown:
        parser.error(f"unknown case(s): {' '.join(unknown)}\navailable: {' '.join(available)}")

    environment = dict(os.environ)
    environment["MAINUI_BUILD_DIR"] = str(Path(arguments.build_dir).resolve())
    environment["PYTHONUNBUFFERED"] = "1"
    environment["PYTHONPATH"] = str(HERE) + os.pathsep + environment.get("PYTHONPATH", "")
    # Cases render frames through SDL; there is no display in CI.
    environment.setdefault("SDL_VIDEODRIVER", "dummy")
    environment.setdefault("SDL_AUDIODRIVER", "dummy")

    allowed = os.environ.get("MAINUI_ALLOWED_SKIPS")
    allowed = None if allowed is None else set(allowed.replace(",", " ").split())
    failures, skipped = [], []
    width = max(len(name) for name in selected)
    for name in selected:
        print(f"[ run  ] {name:<{width}}", end=" ", flush=True)
        started = time.monotonic()
        try:
            result = subprocess.run(
                [sys.executable, str(HERE / f"case_{name}.py")],
                cwd=ROOT, env=environment, timeout=arguments.timeout,
                capture_output=True, text=True)
        except subprocess.TimeoutExpired:
            print(f"TIMEOUT after {arguments.timeout}s")
            failures.append((name, f"timed out after {arguments.timeout}s"))
            continue
        elapsed = time.monotonic() - started
        output = (result.stdout or "") + (result.stderr or "")
        if "Skip:" in output and result.returncode == 0:
            reason = next(line for line in output.splitlines() if "Skip:" in line)
            print(f"skip  ({reason.split('Skip:', 1)[1].strip()})")
            if allowed is not None and name not in allowed:
                failures.append((name, f"unexpected skip (not in MAINUI_ALLOWED_SKIPS): {reason}"))
            else:
                skipped.append(name)
        elif result.returncode == 0:
            print(f"ok   {elapsed:5.1f}s")
        else:
            print(f"FAIL {elapsed:5.1f}s")
            failures.append((name, output.strip()[-4000:]))

    print(f"\n{len(selected)} case(s): {len(selected) - len(failures) - len(skipped)} passed, "
          f"{len(skipped)} skipped, {len(failures)} failed")
    for name, output in failures:
        print(f"\n----- {name} -----\n{output}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
