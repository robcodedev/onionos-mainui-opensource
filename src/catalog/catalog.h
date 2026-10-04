/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_CATALOG_H
#define MAINUI_CATALOG_H
#include "cJSON.h"
#include "catalog/names.h"
#include "core/core.h"
#include "platform/files.h"
#define MAINUI_PATH_MAX 4096
#define MAINUI_SCAN_ENTRY_LIMIT 65536

/* Owned read-only directory model. Entries are invalidated on successful
 * navigation. Failed navigation retains the current screen and selection. */
typedef struct {
    char *label, *path, *extensions, *images;
    /* cache_key is the child ppath key for folders and the DB path for files;
     * artwork is a resolved host path.
     * Both are owned; scanned entries leave them NULL until artwork lookup exists.
     */
    char *cache_key, *artwork;
    /* Owned exact cache spelling; saved identity is independent of host paths. */
    char *stored_path, *stored_image;
    char *icon, *icon_selected, *description, *launch;
    bool directory, shortname;
    /* Raw config spelling is part of stock saved-record identity. */
    char *raw_rompath, *raw_imgpath;
    char *config;
} MainUIEntry;

typedef struct {
    MainUIEntry *entries;
    int count;                    /* Total rows, including rows not currently materialized. */
    int loaded, offset, capacity; /* Capacity is used only by directory scans. */
    int folder_count; /* Leading folder rows; excluded only from the displayed counter. */
    struct MainUICache *cache;
    bool cache_fallback;
    /* A console page scanned for this visit by recovery (see
     * mainui_catalog_scan_only()); session snapshots carry it, so folder
     * entry and other rebuilt catalogs in the same visit keep scanning. */
    bool scan_visit;
    char cache_file[MAINUI_PATH_MAX], cache_table[512];
    char title[256];
    char path[MAINUI_PATH_MAX], extensions[1024], images[MAINUI_PATH_MAX];
    MainUIViewport view;
} MainUICatalogPage;

typedef struct {
    MainUICatalogPage pages[MAINUI_STACK_MAX];
    int depth;
    bool case_sensitive;
    MainUINameLookup names;
    MainUICancel cancel;
    char sd[MAINUI_PATH_MAX];
    char error[256];
    /* Set by mainui_catalog_open() when Emu could not be enumerated (open, read
     * or stat failure, or the entry limit). The catalog is then a valid empty
     * Systems page; startup may continue with it. Not set for other failures. */
    bool unreadable;
} MainUICatalog;

/* Release all owned fields and clear the entry; entry must be non-NULL. */
void mainui_entry_close(MainUIEntry *entry);
/* Initialize a zeroed catalog from SD/Emu. Close even after failure. No ROM
 * content is opened and no caches/configs are written. Scan only one level;
 * cache rows load in windows when entering systems.
 */
bool mainui_catalog_open(MainUICatalog *catalog, const char *sd, bool case_sensitive);
/* Expert discovers both RApp ROM-list configs and standalone launchers.
 * An absent optional RApp directory is a successful empty catalog. */
bool mainui_catalog_expert(MainUICatalog *catalog, const char *sd, bool case_sensitive);
/* Discover app config.json entries; launch paths are data, never executed. */
bool mainui_catalog_apps(MainUICatalog *catalog, const char *sd, bool case_sensitive);
/* Create/rebuild one derived ROM cache transactionally from root XML when present,
 * otherwise from a filtered ROM-tree scan. Failed imports retain the old cache. */
bool mainui_catalog_build_cache(MainUICatalog *catalog, int system, bool replace);
/* Rebuild a damaged cache without being asked to: as replace, but an
 * unfinished ROM deletion is only recovered, never abandoned. */
bool mainui_catalog_repair_cache(MainUICatalog *catalog, int system);
/* The open console's cache has an unfinished ROM deletion (a journal). */
bool mainui_catalog_deletion_pending(const MainUICatalog *catalog);
/* Delete only one system's derived cache; ROMs and source XML are preserved. */
bool mainui_catalog_remove_cache(MainUICatalog *catalog, int system);
/* Enter a selected system/folder. Files are deliberately not executable here. */
bool mainui_catalog_enter(MainUICatalog *catalog, int index);
/* Browse the console whose ROM root is `root` by scanning its folder instead
 * of its cache on its next entry only (the visit that restores the list): the
 * last step of recovering a cache page that cannot be read. The entry after
 * that tries the cache again. False only when out of memory. */
bool mainui_catalog_scan_only(const char *root);
/* The same mark without a log line: a rebuilt catalog continuing a visit
 * that scans (a session snapshot with scan_visit) sets it before entering. */
bool mainui_catalog_keep_scanning(const char *root);
/* After the current page failed to load: true only if its cache content is
 * damaged (see mainui_cache_damaged()), which a rebuild can repair. */
bool mainui_catalog_page_damaged(const MainUICatalog *catalog);
/* The list shows Search's results: a virtual database that neither a rebuild
 * nor a folder scan can reproduce. */
bool mainui_catalog_search_results(const MainUICatalog *catalog);
/* The one test for Onion's Search: a console whose ROM root is the SD card's
 * App/Search/data, where Search writes its results database. Its label and
 * launcher are not used: a theme or a config can change those. */
bool mainui_search_root(const char *sd, const char *root);
/* Console `index` of the grid is the Search system (see mainui_search_root). */
bool mainui_catalog_search_system(const MainUICatalog *catalog, int index);
/* Borrow one row by absolute index. A later accessor may replace the current
 * 64-row cache window; never retain its pointer across accessor/navigation calls.
 * NULL reports an invalid index; cache read failures also set catalog->error.
 */
MainUIEntry *mainui_catalog_entry(MainUICatalog *catalog, int index);
/* Owned portable saved-list record for a file/app, or NULL for a folder. */
cJSON *mainui_catalog_record(MainUICatalog *catalog, int index);
bool mainui_catalog_back(MainUICatalog *catalog);
void mainui_catalog_close(MainUICatalog *catalog);
/* Resolve device SD paths or paths relative to base into a bounded host path.
 * Lexically normalizes . and ..; does not resolve symlinks. */
bool mainui_catalog_path(char out[MAINUI_PATH_MAX], const char *sd, const char *base,
                         const char *value);
#endif
