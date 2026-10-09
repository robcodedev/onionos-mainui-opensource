# ROM cache browsing and creation

MainUI reads existing ROM caches and creates missing ones. This document records the on-disk format and query behaviour, so other Onion tools reading the same databases stay compatible.

## Input contract and sources

`src/randomGamePicker/randomGamePicker.c` and `src/renameRom/renameRom.c` in the Onion source derive the filename and table from the ROM root basename: `<basename>_cache6.db` in that directory and `<basename>_roms` inside the database. Root rows use `ppath='.'`; folders are type 1 and games type 0. A child query uses the folder row's unchanged `path` as its `ppath` value. `disp` is authoritative for the displayed name.

The reader selects `disp,path,imgpath,type`, filters by a bound `ppath`, and orders by type descending and `disp` with the configured BINARY/NOCASE collation. `id` breaks exact name ties deterministically; stock tie equivalence needs more fixtures. Identifiers are double-quoted and escaped; values are bound, including folder keys containing apostrophes. No SQL is assembled from an unescaped filename or value.

ROM/art paths beginning `/mnt/SDCARD/` map to the chosen host SD root. Relative cached paths resolve from the system ROM root, including in nested folders. The original folder key is kept separately from the resolved host path. Empty image paths fall back to the configured image directory and ROM basename for thumbnails.

## Ownership, paging and failures

`src/catalog/cache.c` owns the read-only SQLite connection and prepared row query for each open folder. Each connection has one owner. Workers transfer models to the UI only after joining; changed generations cannot be paged into an older model. `src/catalog/catalog.c` owns decoded entries; `mainui_catalog_entry` returns a borrowed pointer whose lifetime ends on a window replacement or navigation. Callers use it immediately rather than storing UI pointers.

There are at most 64 decoded rows per open cached folder. A full replacement is decoded before the old window is freed. The total count drives the viewport, so libraries larger than the filesystem scanner's 10,000-row cap can be browsed. SQL LIMIT/OFFSET queries have a VM-work budget and a short lock timeout. Decoded-row memory is bounded; deep OFFSET queries are not guaranteed constant-time. Entry/Search/rebuild worker I/O is implemented; matched device performance remains open.

A missing cache is built transactionally on first system entry. If opening an existing cache fails, confirmed SQLite corruption or a missing ROM table triggers one atomic replacement attempt at the system root, then a retry. Failed repair preserves scan fallback. The same repair runs when the cache opens but its first page holds damaged content (a row the reader rejects, or SQLite corruption): it is rebuilt once and read again. Unsupported WAL, permission/locking, busy, I/O and memory errors never trigger replacement; they produce a diagnostic and a scan of the folder for that visit. A later window that cannot be read while browsing is recovered in the background, each step once: the list is reloaded (it may have changed outside MainUI); only if the cache content itself is damaged (a row the reader rejects, or SQLite corruption) the console's cache is rebuilt, as Refresh roms does, while a busy, I/O or memory failure never replaces it; then the console is browsed by scanning its folder for this visit, including folders opened and lists reloaded during it; an explicit Refresh roms ends it. The next time the console is entered, its cache is read again, so a passing failure does not turn the cache off. Each step keeps the selected row and window. Refresh roms and this repair act only on the console they were started for, identified by its configuration file: if it cannot be found again, they fail before changing any cache. While a ROM deletion is unfinished (its journal cannot be recovered), the rebuild only tries to recover it and no scan bypasses it: the list is then left with a message pointing to Refresh roms, which as an explicit request may set the journal aside. Only if the scan fails too, the list is left with the message "This list cannot be read."; Back during recovery leaves the list. A valid empty cache remains empty even when files exist on disk: its data is authoritative. A later window failure retains the old model and returns an error, which starts that recovery rather than showing an incomplete list.

No long read transaction is held. Generation checks reject changed cache windows; active-source polling and focus return schedule a reload, including same-count SQL edits. See [catalog contracts](RESPONSIVENESS_DURABILITY_DEVICES.md). This does not turn existing caches into automatic recursive ROM scanners: new files still need Refresh roms.

## Verification

`tools/test_cache.py` creates synthetic databases only in `build/cache-fixtures/`. It runs `tests/test_cache.c` against ten scenarios, checks input hashes and absence of extra database files, and renders five SDL scenarios. Coverage includes 13,002 rows, nested/empty folders, BINARY/NOCASE sorting, quoted identifiers and parent keys, authoritative titles, artwork path remapping, malformed data and failed-window recovery. Existing filesystem catalog and ten browser SDL scenarios also pass.

No reference database or real ROM content was used. Full stock fixture equivalence, Parent rows are projected by the browser controller.

## WAL-mode boundary

WAL caches are rejected by inspecting the database header before opening them with SQLite, rather than relying on a `SQLITE_OMIT_WAL` build. This keeps the rejection behaviour consistent across host and device SQLite builds. These caches fall back to filesystem browsing because read-only WAL connections may still need to create or update auxiliary shared-memory (`-shm`) files. See SQLite's official [read-only WAL documentation](https://www.sqlite.org/wal.html#read_only_databases). A WAL fixture verifies that no sidecar appears. Full WAL reading would require a separate no-write design; do not enable it silently in this development preview.

## Missing caches and explicit rebuild

Opening a system without its cache creates `<basename>_cache6.db` in its ROM root. The builder uses the reference eight-column schema, relative `path`/`ppath` keys, folder/game types, a transaction and a deferred browsing index. An exclusively reserved `.building` database is published only after successful commit/close. Existing caches are preserved unless `--refresh-caches` is explicitly requested. The main-menu SELECT action Refresh all roms invokes the same builder. Confirmed corrupt caches also rebuild automatically on system entry; failed repair uses scan fallback.

Without XML, only names and directory metadata are scanned, and folders named `Imgs` or `Manuals` (in any case) or the console's image folder are never listed, and a folder is listed only when a file matching the console's `extlist` lies somewhere below it, so empty folders and game data folders (a ScummVM game's `AUDIO` or `DRIVERS`, a port's data) are left out; ROM contents are not read and `disp` strips the extension. An empty or missing `extlist` matches every file with an extension, as in stock, except `miyoogamelist.xml`, `gamelist.xml`, ROM list caches (`*_cache*.db` and their build, journal and deletion files) and the copy of a ROM held while it is deleted. As in stock, a file ending in `.bin` is left out when the `.cue` of the same name is in the same folder, but only when the `extlist` lists `.cue` files too (or is empty): stock also hides it when the `.cue` is not listed, which leaves no entry for the game. With XML, `disp` comes from `<name>`. Shortname filesystem rebuilds now resolve Arcade labels from the Onion name file. Both pinyin fields now store [reference-style initials](SEARCH_METADATA_THEME.md). `tools/test_cache_build.py` verifies nested paths, quoted system names, integrity, explicit refresh and failed publication.


## XML import checkpoint

Missing-cache creation and explicit refresh now prefer `miyoogamelist.xml` in the ROM root. A present usable XML file is authoritative, including an empty `gameList`; unlisted filesystem games are not merged into it. When the file is absent, or present but unusable (see below), the existing filtered directory scan runs. `gamelist.xml` details remain independent.

The importer reads direct `game` children with nonempty `path` and `name`, checks that each ROM exists as a file, and imports `name` and `image`. Missing or empty images store an empty string, never the previous record's image. Paths resolve from the ROM root, with `/mnt/SDCARD/` remapped to the host SD root in development builds. Navigable ancestor folder rows are deduplicated and use the existing relative path/parent keys.

The XML is read leniently, as stock reads it, since many gamelists are written without escaping (Onion's own `miyoogamelist_gen.sh` writes names as they are):

- UTF-8 (optional BOM), predefined and numeric entities, comments and CDATA are decoded. A bare `&`, an unknown entity or an invalid reference stays as written. A byte that does not start valid UTF-8 is read as Windows-1252, so a hand-edited list in that encoding shows its accents and quotes; control characters are dropped.
- Inside an imported field only an end tag of an open element is markup: `A < B` and `A <b>x</b>` are text. An end tag closes up to its open element, so a missing `</name>` is closed by `</game>`; a stray end tag is ignored. Attributes are skipped, and `<!DOCTYPE ...>` and other declarations are skipped without expanding anything, so XML cannot trigger file reads.
- A repeated field keeps its first value. Anything outside the `<gameList>` root is ignored, including a second root. A list cut off after a whole game keeps the games before the cut.
- A long title, genre, rating or description is cut at a character boundary once it reaches 4,092 UTF-8 bytes (room is kept for a four-byte character within the 4,095-byte field). A path or image that long is never cut, since its first part could name a different file: the game is skipped (with a log line), or keeps no image. A game whose path cannot be stored is skipped too.

A file is unusable when its content cannot be used: it is larger than 16 MiB, nests deeper than 32 element levels, exceeds one million game records, or has no `<gameList>` that was either closed or held a game (for example an empty file or `<gameList>` alone). An unusable file is reported on the log (`miyoogamelist.xml is unusable (<reason>); listing the ROM files instead`), left unchanged, and the cache is built from the ROM files as if it were absent. A dry pass checks the whole file first, so one found unusable halfway leaves no rows behind. To use a fixed file, run Refresh roms.

A file that is present but cannot be opened or read (an I/O or permission error) is different: its content may be fine, so the build fails, the log says `could not be read (<reason>); the previous cache is kept`, and the previous cache stays as it was. A transient card error therefore never replaces a gamelist-based cache with one made from file names.

Existing navigation depth bounds also apply. The parser keeps one record and a bounded input buffer; folder deduplication uses a temporary SQLite table. SQL, allocation and cancellation errors abort the build; the existing transaction and exclusive `.building` publication keep the previous database unchanged. Source XML and ROM contents are not written.

Current compatibility boundaries requiring further reference acceptance:

- Only the ROM-root `miyoogamelist.xml` is selected; emulator `gamelist` overrides are not implemented. Encodings other than UTF-8 and Windows-1252 are not recognized.
- This is a lenient, bounded XML subset, not a general XML library: DTDs and external entities are never read.
- Missing paths/names, missing files, directories and paths outside the ROM root are skipped. Folder rows are derived from imported games; explicit empty XML folder records are not imported. Exact stock edge-case equivalence remains open.
- Each ROM uses a file-status check. The bounded root filename set optimization and matched device performance remain map is applied separately. Scanning runs on the worker thread and is cancellable.

Run `tests/integration/case_gamelist.py` after `make`. It verifies names, artwork strings, nested folder keys, entities, CDATA, Unicode, missing/empty images, invalid records, lenient reading of unescaped and Windows-1252 text, unusable lists falling back to the ROM files, unchanged XML, authoritative empty lists, scan fallback, and 10,050-row import plus SDL navigation. Existing core, catalog, cache-build and 13,002-row cache-reader checks also pass.

The [screen contracts](SCREEN_CONTRACTS.md) establish name lookup precedence, parent-row/cache offset separation and independent detail-metadata formats.

## Console-list refresh and detail lookup

Every real console ROM-list context menu ends with **Refresh roms** (language ID 27), including folder, parent, and empty-list selections. It rebuilds only that console's cache and restores the open folder against the rebuilt cache. The shared console grid offers **Refresh all roms** followed by **Refresh roms** for the selected console. The synthetic ` Search ` console omits only the console-specific action; its grid popup retains **Refresh all roms**. Main-menu **Refresh all roms** keeps its existing scope.

Game details for `Roms/SNES/Sub/Game.sfc` first read `Sub/gamelist.xml` using `Game.sfc`. Missing local XML or no matching game triggers a second lookup in `SNES/gamelist.xml` using `Sub/Game.sfc`. A matching local record wins in full, even when its metadata fields are empty. Details use the same lenient reader; a local `gamelist.xml` that is unusable gives no details. This detail lookup is independent of `miyoogamelist.xml` cache import.

**Sort A-Z** in Favorites retains the selected numeric index and viewport instead of following the previously selected game to its new sorted position.
