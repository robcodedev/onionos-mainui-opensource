# Troubleshooting

Start here if Open MainUI misbehaves. Bug reports are most useful when they name the device model, describe the SD card layout, and include a log.

## Getting a log

Open MainUI writes its diagnostics to stderr. How much of that you can keep depends on how it was installed:

* **With the [optional wrapper](BUILDING.md#optional-wrapper-install-with-a-stock-switch-and-logging):** output is saved to `.tmp_update/logs/MainUI.log` only when `.tmp_update/config/.logging` exists on the SD card; otherwise it is discarded. Create that empty marker file, then reproduce the problem.
* **With the [simple method](BUILDING.md#simple-method-replace-the-binaries-directly):** Onion does not save MainUI's output. Install with the wrapper if you need a log.

A log of at least 1 MiB is moved to `MainUI.log.1` at the next start, so copy the log off the card soon after reproducing the problem. Onion itself deletes `MainUI.log` at every boot, so a problem seen after a restart needs the log copied before the next one.

## Reading a log

The first line of each start names the build: `[startup] Open MainUI <version>, device <model>, sd <path>, theme <path>`. Other lines start with what they are about, one line per event:

* `[message]`: every message shown on screen, as it appears.
* `[cache]`: each ROM list cache built (from `miyoogamelist.xml` or the ROM files, with its row count and time) or removed, and why: the cache was missing, it was damaged, or Refresh roms. A failed build gives its error.
* `[scan]`: a console listed by scanning its folder because its cache could not be built or read.
* `[job]`: a background read, search, refresh or repair that failed, with the console's `config.json`.
* `[return]` and `[restore]`: why the screen left for a game or app could not be shown again on return.
* `[write]`: a file that could not be saved, with the reason.
* `[settings]`: a setting that could not be saved to `system.json`.
* `[delete]`: an interrupted ROM deletion resolved automatically.
* `[timing]`: timing counters; see [TIMING.md](TIMING.md).

## A console lists file names instead of its gamelist titles

When a console's `miyoogamelist.xml` cannot be used at all (for example an empty file, or one with no `<gameList>`), the log says `miyoogamelist.xml is unusable (<reason>); listing the ROM files instead`, and the list is built from the file names. The XML is never changed. A file that cannot be read because of a card or permission error is different: the log says `could not be read`, and the previous list is kept. Most sloppy files are read as they are: unescaped `&` or `<` in titles and Windows-1252 accents work. Once the file is fixed, run **Refresh roms** for that console. See [CATALOG_CACHE.md](CATALOG_CACHE.md#xml-import-checkpoint).

## Resolving an interrupted ROM deletion

Deleting a ROM first renames it to a staged name, `<rom>.mainui-delete.<16 hex digits>`, and records this in a journal next to the console's cache (`<cache>.delete.json`). If the deletion is interrupted, for example by a power cut, Open MainUI finishes or undoes it automatically the next time you enter that console.

Automatic recovery refuses when it cannot tell which file is the right one, for example when a ROM with the original name has appeared again, the staged copy differs from the recorded size, or the journal is unreadable. Recovery never guesses, because a wrong guess could delete a ROM you want to keep. Until the journal is resolved:

* you can still browse and launch games in that console;
* Delete is blocked for that console, and its error names the staged file, or the journal if it cannot be read. Long paths show their trailing portion.

To continue, run **Refresh roms** for that console, from inside its list or from the console selector. Refresh roms removes only the journal and rebuilds the list from the files on the card. It never deletes or renames a ROM, so the worst case is an extra copy left on the card:

* the ROM you were deleting may still exist under its staged name. Staged files are not shown in the list.
* if a file with the original name exists, it stays and is listed normally.

Afterwards, look in that console's ROM folder for files ending in `.mainui-delete` or `.mainui-delete.` plus 16 hex digits. To keep one, rename it back to its original name, removing that suffix, and run Refresh roms again. Otherwise delete it with a file manager or on your computer. If both copies exist, compare them before removing either. With [logging enabled](#getting-a-log), the log names the staged file when recovery refuses and again when Refresh roms removes the journal.

How the journal, staging and automatic recovery work is described in [RESOURCE_LIMITS.md](RESOURCE_LIMITS.md#rom-deletion-recovery).
