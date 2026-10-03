# Changelog

## 1.0.3 - 2026-10-03

### Themes and Settings

* Theme text with a font size of 0 is hidden, as in stock: footer hints and the page counter (`hint.size`), main-menu and Games console labels (`grid.grid1x4`), Expert console labels (`grid.grid3x4`) and the screen title (`title.size`). Themes such as DS XS and Game Boy Scouts use this; 1.0.2 drew the text at a default size. See [docs/THEMES.md](docs/THEMES.md).
* Brightness, Menu sound and Sleep timer play the change sound on every Left/Right press, also at the lowest and highest value, as in stock. Menu sound clicks at the new volume, so it is silent at 0 (#10, thanks to @Amiga500).
* Large console and main-menu icons no longer take unbounded memory: Expert icons keep only the part Expert shows, an icon used for both the normal and selected look is loaded once, and all menu icons together stay within 24 MiB; an icon beyond that is left out.
* A setting that cannot be saved no longer stays half applied. Onion's keymon keeps its own live copy of volume, brightness, sleep timer and display values and writes it back to `system.json`. MainUI changed that copy first, and when `system.json` then could not be written, the device kept the new value while Settings showed the old one. Now the values this save changed are put back, unless something else changed them in the meantime. If one cannot be put back, Settings says so, and Settings and Display show keymon's current values whenever they are opened.
* A damaged `system.json` is reported once in Settings instead of "Could not save system.json." on every press. MainUI never rewrites the file.
* A setting or ROM-list position that appears twice in its file is saved properly; before, the old copy could win when the file was read back.
* Connecting to a Wi-Fi network while another connection is still being made no longer closes the password keyboard without a word. It stays open with the password, and says that Wi-Fi is busy; an open network shows the same message.

### Buttons and screens

* Delete ROM, Clear Recents and Shutdown need a separate press of A to confirm. Before, holding A for half a second after choosing one of them confirmed it through key repeat, so one long press could delete a ROM. A held A that dismisses a message no longer acts on the screen below either. See [docs/NAVIGATION.md](docs/NAVIGATION.md).
* An error shown over game details takes the buttons first: A or Back dismisses it. Before, A launched the game behind the message and Back closed the details.
* Game details open at once while a cover is still being read; the cover appears when it is ready. Before, a slow cover held up the screen and the buttons.

### Favorites and Recents

* Removing a Recent removes the selected entry when the same game is in Recents twice, played with two different emulators. Before, the other entry could disappear instead, and launching one of them removed the other from Recents.
* Removing one of two Favorites of the same game (with different names or emulators) removes the one you selected, and the other keeps its folder.
* After returning from a game or when a list is reloaded, Recents and Favorites select the right entry when the same game is listed under two emulators or names, or when Favorites have no ROM path (such as apps). A selected folder no longer jumps to such a Favorite.
* A game launched once from its console list and once from Search results is one Recent, not two.
* A game favorited from its console list is shown as a Favorite in Search results too, and the other way round. Adding it again, from any list and under any name, changes nothing instead of listing the game twice; Favorites already listed twice stay as they are. Stock compared how the path was spelled, and only the name when adding.
* Favorite stars and Remove Favorite come back without a restart when `favourite.json` could not be read at startup: they are read again whenever a section or the menu opens. Before, they stayed missing until MainUI restarted.

### Favorites folders

* When editing Favorites folders saves a repair of a damaged `favourite-folders.json` (for example a folder without a name, a duplicate folder, or folders inside each other in a loop), the damaged original is first kept as `favourite-folders.json.damaged`, which later edits never replace. Before, the only copy was `.bak`, which the next edit overwrote.
* When `favourite-folders.json` is damaged and its backup is not, folders can be edited again: the damaged file is kept as `.damaged` and editing continues from the backup, which browsing already showed. This includes a file that cannot be read as text at all, such as one filled with zero bytes by a power cut, or one over 8 MiB: it is moved aside unchanged.
* When `favourite-folders.json` is missing and only its backup is left, editing works on the whole backup. Before, the first edit kept the folders but dropped everything else in it, such as extra fields from other tools and the placement of games no longer in Favorites, and the second edit replaced the backup. A backup that cannot be used is kept as `.damaged` first; one of a newer format is left alone and the edit refused.
* Removing a Favorites folder no longer leaves two folders of the same name side by side when a folder inside it has the same name as one next to it. Rename one of them first; the message says so.

### Lists and navigation

* MainUI no longer checks its files every few seconds while a screen is open. Opening Games, Expert, Apps, Favorites or Recents reads it again instead, keeping the selected console, so changes made outside MainUI (over Wi-Fi, for example) show up there. A console whose configuration could not be read, for example while it was being copied, is found the next time Games or Expert is opened; before, it stayed missing from Games until a restart.
* Moving a Favorite (Move, then Paste) keeps the list where it is when the Favorite stays on screen, and otherwise scrolls only as far as needed. Before, the list jumped so the Favorite was on the bottom row. Pasting into an empty or short folder also keeps its ".." row in view.
* Creating the first folder in an empty Favorites folder keeps its ".." row in view.
* Editing inside a Favorites folder (Create, Rename, Sort, Delete, removing a Favorite) no longer forgets your place in the folders above it: Back returns to the row and window you entered from.
* When Favorites folders were reordered while a game ran, Back after the return selects the folder you came from, not whichever folder took its old row.
* Removing the last Favorite, Recent or ROM of a long list moves the rows above into view instead of leaving an empty row at the bottom.

### Search

* After starting Search with the X button, MainUI opens Games → Search with the results, as stock does; starting it from Apps already worked. Before, MainUI returned to the main menu: Onion starts Search this way by closing MainUI, and MainUI did not follow the place Search leaves for it (#11, thanks to @Amiga500).
* Search is recognised by its data folder instead of its name, so a renamed Search console still behaves as Search.

### Damaged files and read errors

* A ROM list whose cache has an unreadable row no longer reloads endlessly when that row comes into view. MainUI now repairs it in the background, keeping the selected row: it reloads the list, then rebuilds that console's cache as Refresh roms does if the cache itself is damaged, and as a last resort lists the console by scanning its folder for the rest of the session. A temporary read error never replaces the cache, and an unfinished ROM deletion is never set aside by this: the list is then left with a message pointing to Refresh roms. Unreadable Search results are reloaded once; if that does not help, MainUI asks you to run Search again.
* A Recents list with an unreadable line can be cleared again, and new games are added to it again; the damaged original is kept once as `recentlist.json.damaged`.
* ROM-list positions are saved again after `romwinidx.json` was damaged with zero bytes; the damaged file is kept as `romwinidx.json.bad`.
* A damaged launch-return file (`mainui-return.json` with zero bytes, or over 256 KiB) no longer blocks every later game launch until a reboot. When no launch is pending, it is moved aside as `mainui-return.json.bad` and MainUI starts at the main menu.
* Refresh roms no longer reports a failure when it dropped a refused deletion journal but flushing the folder afterwards failed.

### Device and system

* Battery and Wi-Fi checks keep running if the system timer cannot be created.
* A Wi-Fi helper that hangs in the kernel can no longer hold up leaving MainUI or launching a game.

### Documentation and tests

* The 1.0.2 entry below said scroll speeds below 25 px/s stay exact. They do not: only 5, 6.25, 8.33 and 12.5 px/s are possible there. [docs/TIMING.md](docs/TIMING.md) lists the speed each setting gives.
* [docs/NAVIGATION.md](docs/NAVIGATION.md) describes how the cursor and the list window move, including where the console grid differs from stock on purpose: Right on the last console of a row goes to the next row, not to the next page.
* Tested on the Mini v4 and Mini Flip as well as the Mini Plus; the README no longer describes the other models as less tested.
* Tests can make one file read fail on purpose, to check recovery from read errors.
* CI also runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer, and fails when a test is skipped unexpectedly.

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
