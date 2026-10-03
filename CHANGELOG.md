# Changelog

## 1.0.2 - 2026-09-28

### Fixes

* Deleting a ROM whose cache entry was changed or removed after the list was read (for example by an external rebuild) no longer removes the ROM file while reporting it as preserved. Nothing is deleted, and the message asks to reopen the console (#6).
* A long title waiting out a long scroll delay no longer holds up battery, Wi-Fi and list checks until it starts to scroll.

### Performance

* Idle CPU use is lower: static screens are no longer redrawn every 500 ms when nothing on them has changed (#9).
* Cover images are scaled on the background decoder, so scrolling and opening lists no longer do this work on the UI thread (#8).
* Scrolling titles are much cheaper: only the selected row is redrawn and sent to the display, instead of the whole screen.
* Scrolling titles move smoothly: frames are paced on even 40 ms intervals with a constant whole-pixel step, and no longer pause for periodic full-screen repaints or Wi-Fi status checks. The `.romListTitleScroll` speed is rounded to a whole number of pixels per frame, that is to a multiple of 25 px/s (for example 120 px/s scrolls at 125 px/s). Below 25 px/s only 5, 6.25, 8.33 and 12.5 px/s are possible (for example 20 px/s scrolls at 25 px/s and 15 px/s at 12.5 px/s); see [docs/TIMING.md](docs/TIMING.md). This entry first said slow speeds stay exact.

### Other

* With logging enabled, timing figures are also written once a minute while MainUI runs, so sessions ended by the game switcher still leave them in the log. See [docs/TIMING.md](docs/TIMING.md).
* `--help` now lists `--refresh-caches` and `--version`, and neither option prints a timing report any more.
* CI no longer runs a second time for release tags.

## 1.0.1 - 2026-09-27

### Fixes

* Theme backgrounds are shown the right way up. Onion stores `skin/background.png` upside down, and stock rotates it on load; 1.0 showed it as stored (#1).
* An unreadable `Emu` (for example a file instead of a folder) no longer stops the launcher. It starts with an empty Games list and logs the reason (#5).
* When no theme font can be opened, Onion's built-in font on internal flash is used. If that fails too, the log names the missing font (#5).
* ROMs larger than 2 GiB (for example multi-disc PBP files) are no longer treated as missing (#2).
* A failure to save the ROM-list position no longer blocks launching a game (#3).
* A saved setting, list position, favourite or device request is no longer reported as failed when the file was written but flushing its folder failed. The same applies to rebuilt and removed ROM caches.
* Returning from a game keeps its list position even if removing the handoff file only partly succeeds.
* An unusable theme path in `system.json` falls back to the stock theme instead of stopping the launcher.
* Filesystem directory scans no longer follow symlink entries or other non-regular filesystem objects, and the scanned `Emu`, `App` and `RApp` folders must not be symlinks themselves. FAT cards are unaffected.
* The log names the failing step when theme audio is unavailable.
* A console or app whose `config.json` has an unusable value (for example an overlong path or `extlist`) is skipped and logged, instead of hiding the whole Games or Apps list or stopping the launcher.
* An unreadable `favourite.json` or `recentlist.json` shows a message instead of closing the launcher.
* A damaged `romwinidx.json` is moved aside to `romwinidx.json.bad` and replaced, so list positions are saved again.
* On non-FAT disks, file names containing a backslash are skipped rather than read as a path.
* When a theme font cannot be opened, the fallback font follows the language as in stock: `wqy-microhei.ttc` for non-English languages, Exo 2 for English. When a theme relies on that fallback, changing between English and another language restarts MainUI and returns to Settings, so the matching font is used at once.
* New [docs/THEMES.md](docs/THEMES.md) for theme authors: optional settings and assets, list colours, background orientation, fonts and languages, and safe margins.
* The language saved in `system.json` is used from startup. In 1.0, labels stayed in English until a language was chosen again in Settings, and after every game.
* A list row that cannot be read while a background task is running no longer closes the launcher; the list reloads when the task ends.
* Test wrapper: logging problems no longer prevent startup, and stock is only run when its backup is executable.

## 1.0 - 2026-09-26

Initial open-source release of an independent MainUI implementation for Onion on the Miyoo Mini, Mini Plus and Mini Flip.

Reimplements features from the [patched MainUI project](https://github.com/robcodedev/onionos-mainui-patcher) in an open-source launcher.

### What it does

* Console and ROM browsing against the existing `*_cache6.db` SQLite caches, including cache creation, refresh and folder navigation.
* Recents and Favorites, with folder creation, renaming, reordering and removal.
* Search, the Apps list, game launch and return, and the stock settings screens.
* Wi-Fi settings with signal strength, a marker on the connected network, and automatic rescanning while the menu is open, as in stock.
* Theme loading with fallback, and `miyoogamelist.xml` metadata for display names and box art.

### Beyond the stock launcher

* Auto-scrolling game titles.
* Configurable row count in game lists.
* Genre, rating and description from `gamelist.xml` in game details.
* Configurable main menu through `main-menu.json`, including hidden Recents and Expert sections.
* Letter jump in game lists, and a leading `..` row for going up a folder.
* Recents marks favourite games with the theme's favourite icon, as game lists do. Stock shows no icon in Recents. Display only: `recentlist.json` and `favourite.json` are unchanged.
* Sort A-Z for Favorites folders.
* Custom context menus.
* Special characters and spaces in Wi-Fi SSIDs and passwords, except double quotes in SSIDs.
* Weak networks in the Wi-Fi list show the correct signal icon; stock shows full bars for them.
* Preloads thumbnails for the two games before and after the selection.
* The selected game's thumbnail appears together with the list when opening a console, Recents or Favorites, or when returning from a game. An image that takes longer than 80 ms to load never delays opening; it appears when ready.
* Much faster ROM cache rebuilds. The ROM folder is read without checking each file individually, and the image folder is never scanned. A 5,000-ROM arcade folder with 16,000 images rebuilds in under a second, against over 5 minutes in stock MainUI and about a minute in the patched MainUI.
* Faster list navigation.
* Lower CPU use for better battery life: scrolling titles redraw at about 30 fps, the screen is not redrawn while background work such as a ROM refresh or Wi-Fi scan runs, battery and Wi-Fi status are polled every 5 s outside Settings, and each frame is presented in a single pass.
* Basic device information in Settings / About device.

### Reliability and recovery

* Missing emulator directories and blank settings files are handled gracefully. Nonempty malformed settings and existing caches are preserved on failure.
* ROM lists remain browsable when a missing cache cannot be written, with clearer errors for missing ROM folders and invalid gamelists.
* Wi-Fi power changes finish during shutdown, within at most 5 seconds. Background Wi-Fi services cannot retain MainUI's file locks and can be stopped normally.
* Memory limits for theme artwork (32 MiB) and fonts (64 MiB per file) protect against excessively large assets; rejected assets are logged.
* Long Favorites names and folder paths no longer prevent game launches.
* A console with an unreadable `config.json` is skipped instead of hiding every console.
* FAT32-compatible ROM deletion, with recovery after interrupted deletes across reboots and SD-card remounts. If recovery cannot tell which file to keep, it keeps both, and Refresh roms clears the pending state.
* Console ROM paths are confined to the SD card, with additional checks before deleting files.

### Implementation notes

* Links Onion's SQLite on the device rather than bundling a copy, which is a large part of why the binary is around 200 KB against the original's 1.4 MB.
* GPL-3.0-only, matching Onion. See [third-party notices](THIRD_PARTY_NOTICES.md) for cJSON and SQLite.

### Known limitations

* Invalid UTF-8 in miyoogamelist.xml prevents importing the file; save it as UTF-8 before rebuilding the cache.
* ROM paths containing dollar signs ($) or backticks are currently rejected for launch.
* The Max resolution row in About device can read "unknown" on some devices.
* The status bar doesn't show the hotspot icon while Wi-Fi hotspot mode is active (stock does). Onion mostly uses the hotspot during netplay, so this rarely shows.
* A single ROM folder can hold at most 65,536 entries; split larger folders into subfolders. Consoles have been tested with around 5,000 ROMs; much larger libraries (tens of thousands) may run out of memory while the cache is built.
* Testing has covered the Mini Plus more thoroughly than the Mini or Mini Flip.
* Newer labels (folder actions, Tweaks, the About device rows) appear in English whatever language is selected, because their translation IDs do not exist in Onion's language files yet. See [lang/README.md](lang/README.md).
