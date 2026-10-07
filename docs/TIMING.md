# Timing diagnostics

Keep the installed wrapper's existing Onion logging switch: `.tmp_update/config/.logging`. When it exists, the wrapper appends stdout and stderr to `.tmp_update/logs/MainUI.log`. The wrapper was not changed for timing support. Without that marker stdout is /dev/null: startup detects this once and subsequent marks/counters return without reading the clock or updating counters. The one-time detection still has a cost; no fixed nanosecond claim is made.

Marks use CLOCK_MONOTONIC and integer timespec arithmetic. Only the UI thread records timestamps. Catalog counters use atomic longs, since scans can run on a worker. While the UI runs, at most one interim line per minute is written, and only when counters changed: `[timing] interim uptime-ms N` followed by the same counters as the final report. Onion stops MainUI with SIGKILL when the Menu button opens the game switcher, so that session never reaches teardown; its last interim line is then the figure to use. After workers stop, teardown emits one block to stdout, including peak RSS from /proc/self/status (VmHWM, in KiB). A direct host run also reports when stdout is a terminal or captured file.

## Reading the report

- session: run entry through session setup.
- video: video setup, including SDL/MI initialization, TTF and theme loading.
- restore: session restoration.
- ready: render-resource setup, including audio initialization.
- first-frame: entry to the first successful SDL_Flip, recorded once. This is a presentation boundary, not proof that physical scanout has finished.
- launch-ms: latest dispatched key-down to launch handoff, handoff to exit mark, and their sum. Timer/decoder/key-up events cannot replace the key mark. Input queue delay before dispatch is not included.
- peak-rss: the process's peak resident memory; it includes earlier decode spikes.
- frames: successful UI frame flips.
- draw-ms: accumulated wall-clock milliseconds around mainui_draw_frame, including drawing, rotation, blitting and SDL_Flip. Uses the existing timing helper, which calls mainui_count_add and skips clock reads when logging is disabled. Each draw duration is truncated to integer milliseconds. For ordinary runs with successful flips, draw-ms / frames gives average measured milliseconds per frame; skip the division when frames is zero. Snapshot saves and failed presentations also contribute draw time but do not increment frames.
- gap-under35, gap-40, gap-50-60, gap-over65: spacing between consecutive animation frames (marquee and letter jump), in buckets of under 35, 35-45, 46-65 and over 65 ms. A scrolling title should land almost entirely in gap-40; letter jump counts under gap-under35. A few entries at startup or around the scroll delay are normal.
- roms: games in the most recently entered catalog folder, excluding folders; zero if no ROM folder has been entered. It is not a whole-card count.
- cache: most recent successful catalog entry: 1 for database, 0 for scan, -1 if no catalog folder has been entered.
- cache-hits/cache-scans: successful database-backed/scanned folder entries. A rebuilt cache can be a hit; scan-entries reveals the build work.
- scan-entries: entries materialized during cache construction and successful scan fallback, accumulated across the session (including folders).
- cache-build-ms: cumulative time inside the locked cache-build operation, including failed/cancelled attempts and existing-cache checks; excludes lock acquisition.
- scan-ms: cumulative directory enumeration and sorting time, including discovery, fallback and scans nested inside cache builds. These totals overlap and must not be added together. Each operation is rounded down to milliseconds, so very short operations can report zero.
- discover-ms: cumulative time reading the console, app and Expert folders with their config.json files: at startup for Games, and each time Games, Expert or Apps is opened. A slow first entry after boot with a large discover-ms points to slow card reads.
- icon-ms: cumulative time loading and decoding console and app icons for the Games and Expert grids and the Apps list (not home icons or theme images). A slow first entry with a large icon-ms points to large icons or slow icon reads.
- -1 means unavailable or an unreached boundary, not zero elapsed time.

Snapshot-only runs do not flip a frame or reach the launch handoff mark, so those boundaries remain unavailable. Reports also cover early setup failures where possible. Ordinary exits omit the launch line.

EXIT is recorded immediately before report generation. The launch numbers exclude report formatting, the stdout flush and the final process-exit syscall; they are not exact keypress-to-process-death measurements. Reporting itself can write the SD card when logging is enabled.

## Time away

On a logged real-device run, /tmp/mainui-exit exchanges the prior launch's exit mark with the next entry mark. It is a small tmpfs diagnostic, closed before exit without an SD write or fsync. The record includes Linux's boot ID; malformed, future and different-boot records are ignored. The next logged entry consumes it once. Host and snapshot runs do not configure this path.

The away interval includes reporting overhead, emulator startup, gameplay, emulator shutdown and anything else before MainUI starts again. It does not isolate return latency or establish whether MI initialization in another process was slow. Measuring keypress-to-emulator-ready requires instrumentation in that process.

## Checks

The timing unit suite checks silent collection, one report, concurrent counter increments, reset behavior, missing boundaries, /dev/null gating and same-boot handoff consumption:

```sh
make build/unit-tests
build/unit-tests timing
```

## Comparing frame cost on the device

Use separate logged sessions with the same theme, brightness, ROM list and network
conditions before and after the change. In each session, spend 60 seconds in one
of these scenarios, then leave by launching a game from the list, which emits the
final report (leaving through the game switcher kills MainUI; use its last interim
line instead):

1. Idle on a short title.
2. Idle on a long title with marquee scrolling enabled.
3. Sit in the Wi-Fi menu with Wi-Fi enabled.
4. Run Refresh roms, recording its completion time as well as the 60-second window.

Record frames, draw-ms and draw-ms / frames for each run. Counters cover the whole
session, including navigation, so use the same entry and exit steps. Compare
cache-build-ms and scan-ms for Refresh roms too; if it takes longer than 60 seconds,
let it finish and record the actual session duration.

While a title scrolls, frames are paced on fixed 40 ms deadlines (four 10 ms kernel ticks) and the title moves one whole-pixel step per frame. This departs from exact `.romListTitleScroll` speed parity on purpose: an averaging step would keep the exact speed but bring back uneven motion. From 25 px/s up, the configured speed is rounded to a whole number of pixels per frame, that is to a multiple of 25 px/s (120 px/s scrolls at 125 px/s). Below that, the title moves one pixel every 2, 3, 4 or 5 frames, so only five slow speeds exist. Of the 396 accepted speeds (5 to 400 px/s), the 17 exact ones are 5 px/s and the multiples of 25.

| Configured px/s | Actual px/s | Motion |
| --- | --- | --- |
| 5 | 5 | 1 px every 5 frames |
| 6-7 | 6.25 | 1 px every 4 frames |
| 8-10 | 8.33 | 1 px every 3 frames |
| 11-16 | 12.5 | 1 px every 2 frames |
| 17-37 | 25 | 1 px per frame |
| 38-62 | 50 | 2 px per frame |
| 25n - 12 to 25n + 12 | 25n | n px per frame, up to 400 px/s (16 px) |

During the scroll delay (up to 30 s)
the normal maintenance ticks continue; the loop wakes when the title starts to
move and only then switches to paced frames. Letter-jump work retains 17 ms wakes. Workers wake the UI
on completion, and catalog work has a wake scheduled for the 500 ms Loading panel
deadline. Status polling is every second in Settings and every five seconds
elsewhere, with no periodic supplicant requests when system.json has Wi-Fi off.
On the home, console, list and apps screens, a maintenance tick that would repaint
an identical frame is skipped: nothing may be animating or in flight, input must
have settled for a second, and battery and Wi-Fi must be unchanged. A full repaint
still happens at least every five seconds, or every 30 seconds while a title
scrolls, when those ticks recompose and present only the selected row. The
periodic Wi-Fi status refresh does not force full frames; its result repaints
the screen only if the header changes. On the host,
`MAINUI_VERIFY_PARTIAL=1` checks every such row frame against a full frame and
aborts on the first differing pixel.
