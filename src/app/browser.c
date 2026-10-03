/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/browser.h"
#include "app/positions.h"
#include "catalog/delete.h"
#include "menus/menu.h"
#include "platform/files.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mainui_browser_grid_restore(const MainUICatalog *catalog, MainUIViewport *view, int selected)
{
    bool expert = !strcmp(catalog->pages[0].title, "Expert");
    mainui_grid_restore(view, catalog->pages[0].count, selected, expert ? 3 : 4, expert ? 3 : 2);
}

int mainui_browser_count(const MainUICatalog *catalog)
{
    return catalog->pages[catalog->depth].count + (catalog->depth > 1);
}

int mainui_browser_index(const MainUICatalog *catalog, int row)
{
    return row - (catalog->depth > 1);
}

const char *mainui_browser_label(MainUICatalog *catalog, int row)
{
    if (row < 0 || row >= mainui_browser_count(catalog)) {
        return NULL;
    }
    int index = mainui_browser_index(catalog, row);
    if (index < 0) {
        return "..";
    }
    MainUIEntry *entry = mainui_catalog_entry(catalog, index);
    return entry ? entry->label : NULL;
}

bool mainui_browser_folder(MainUICatalog *catalog, int row)
{
    if (row < 0 || row >= mainui_browser_count(catalog)) {
        return false;
    }
    int index = mainui_browser_index(catalog, row);
    if (index < 0) {
        return true;
    }
    MainUIEntry *entry = mainui_catalog_entry(catalog, index);
    return entry && entry->directory;
}

bool mainui_browser_back(MainUICatalog *catalog, MainUIViewport *view)
{
    if (!mainui_positions_save(catalog, view)) {
        fprintf(stderr, "Could not save ROM-list position.\n");
    }
    if (!mainui_catalog_back(catalog)) {
        return false;
    }
    *view = catalog->pages[catalog->depth].view;
    if (!catalog->depth) {
        mainui_browser_grid_restore(catalog, view, view->selected);
    }
    return true;
}

bool mainui_browser_enter(MainUICatalog *catalog, MainUIViewport *view, int rows)
{
    if (view->selected < 0 || view->selected >= mainui_browser_count(catalog)) {
        return false;
    }
    int index = mainui_browser_index(catalog, view->selected);
    if (index < 0) {
        return mainui_browser_back(catalog, view);
    }
    catalog->pages[catalog->depth].view = *view;
    if (!mainui_catalog_enter(catalog, index)) {
        return false;
    }
    mainui_viewport_restore(view, mainui_browser_count(catalog), rows, 0, 0, rows - 1);
    mainui_positions_restore(catalog, view, rows);
    return true;
}

bool mainui_browser_refresh_control(const char *sd, bool sensitive, char error[256],
                                    MainUICancel cancel)
{
    for (int kind = 0; kind < 2; kind++) {
        MainUICatalog *catalog = calloc(1, sizeof *catalog);
        if (catalog) {
            catalog->cancel = cancel;
        }
        bool ok = catalog && (kind ? mainui_catalog_expert(catalog, sd, sensitive)
                                   : mainui_catalog_open(catalog, sd, sensitive));
        for (int i = 0; ok && i < catalog->pages[0].count; i++) {
            ok = mainui_catalog_remove_cache(catalog, i);
        }
        if (!ok) {
            snprintf(error, 256, "%s",
                     catalog && *catalog->error ? catalog->error
                                                : "Cannot discover systems for refresh");
        }
        if (catalog) {
            mainui_catalog_close(catalog);
        }
        free(catalog);
        if (!ok) {
            return false;
        }
    }
    error[0] = 0;
    return true;
}

static bool delete_locked(MainUICatalog *catalog, MainUIViewport *view, int rows)
{
    if (!catalog || !catalog->depth) {
        return false;
    }
    /* A pending journal blocks this whole console, even with no selected row. */
    char journal[4096];
    MainUICatalogPage *page = &catalog->pages[catalog->depth];
    int n = snprintf(journal, sizeof journal, "%s.delete.json", page->cache_file);
    if (n < 0 || n >= (int)sizeof journal) {
        return false;
    }
    if (mainui_delete_journal_present(page->cache_file)) {
        mainui_delete_pending_error(page->cache_file, catalog->sd, catalog->error);
        return false;
    }
    MainUIEntry *entry =
        mainui_catalog_entry(catalog, mainui_browser_index(catalog, view->selected));
    if (!entry || entry->directory || !mainui_regular_file_within(entry->path, catalog->sd) ||
        !mainui_regular_file_within(entry->path, catalog->pages[1].path)) {
        snprintf(catalog->error, sizeof catalog->error,
                 "Only a regular ROM within this console can be deleted.");
        return false;
    }
    char original[4096], temporary[4096] = "";
    snprintf(original, sizeof original, "%s", entry->path);
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    bool cached = page->cache != NULL;
    if (cached && !mainui_regular_file_within(page->cache_file, catalog->sd)) {
        snprintf(catalog->error, sizeof catalog->error,
                 "ROM cache must be a regular file within the SD card.");
        return false;
    }
    bool ok =
        !cached || (entry->cache_key && sqlite3_open_v2(page->cache_file, &database,
                                                        SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    if (ok && cached) {
        sqlite3_busy_timeout(database, 100);
        ok = sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
        char *sql =
            sqlite3_mprintf("DELETE FROM \"%w\" WHERE type=0 AND path=?1", page->cache_table);
        ok = ok && sql && sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK &&
             sqlite3_bind_text(statement, 1, entry->cache_key, -1, SQLITE_TRANSIENT) == SQLITE_OK;
        sqlite3_free(sql);
    }
    /* The list may predate the cache. Recovery reads a missing row as a committed
     * deletion, so confirm the row before staging the ROM. BEGIN IMMEDIATE keeps
     * other writers out until the DELETE runs. */
    bool stale = false;
    if (ok && cached) {
        sqlite3_stmt *probe = NULL;
        char *sql = sqlite3_mprintf("SELECT 1 FROM \"%w\" WHERE type=0 AND path=?1 LIMIT 1",
                                    page->cache_table);
        int found = SQLITE_ERROR;
        if (sql && sqlite3_prepare_v2(database, sql, -1, &probe, NULL) == SQLITE_OK &&
            sqlite3_bind_text(probe, 1, entry->cache_key, -1, SQLITE_TRANSIENT) == SQLITE_OK) {
            found = sqlite3_step(probe);
        }
        sqlite3_finalize(probe);
        sqlite3_free(sql);
        stale = found == SQLITE_DONE;
        ok = found == SQLITE_ROW;
    }
    bool prepared = false;
    if (ok && cached) {
        prepared = mainui_delete_prepare(page->cache_file, original, entry->cache_key, temporary);
        ok = prepared;
    }
    bool unlinked = false;
    bool moved = ok && (cached ? mainui_move_file_new_locked(original, temporary)
                               : mainui_remove_file_status(original, &unlinked) == 0);
    if (!cached && unlinked && !moved) {
        snprintf(catalog->error, sizeof catalog->error,
                 "ROM removed, but saving the deletion to the SD card failed. Reopen the console.");
        return false;
    }
#ifdef MAINUI_TEST_FAULTS
    if (moved) {
        mainui_test_fault("delete-moved");
    }
#endif
    ok = moved &&
         (!cached || (sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(database) > 0));
    sqlite3_finalize(statement);
    if (ok && cached) {
        ok = sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    }
    if (!ok && cached) {
        sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(database);
#ifdef MAINUI_TEST_FAULTS
    if (ok) {
        mainui_test_fault("delete-committed");
    }
#endif
    if (!ok) {
        bool recovered = prepared ? mainui_delete_recover(page->cache_file, page->cache_table,
                                                          catalog->pages[1].path, catalog->sd)
                                  : (!moved || mainui_move_file_new_locked(temporary, original));
        snprintf(catalog->error, sizeof catalog->error, "%s",
                 stale       ? "ROM list is out of date; nothing was deleted. Reopen the console."
                 : recovered ? "Deletion failed; ROM and cache were preserved."
                             : "Deletion failed. ROM retained in its recovery file.");
        return false;
    }
    bool removed = !cached || (prepared ? mainui_delete_recover(page->cache_file, page->cache_table,
                                                                catalog->pages[1].path, catalog->sd)
                                        : mainui_remove_file(temporary) == 0);
    MainUIViewport saved = *view;
    bool reopened = mainui_browser_back(catalog, view) && mainui_browser_enter(catalog, view, rows);
    if (reopened) {
        /* One ROM fewer: keep the window, close a gap below the last row. */
        *view = saved;
        mainui_viewport_refit(view, mainui_browser_count(catalog), rows, saved.selected);
    }
    if (!removed || !reopened) {
        snprintf(catalog->error, sizeof catalog->error, "%s",
                 !removed ? "ROM removed from list; pending deletion could not be cleared."
                          : "ROM deleted; reopen the console to refresh its list.");
    }
    return removed && reopened;
}

bool mainui_browser_refresh_all(const char *sd, bool sensitive, char error[256])
{
    return mainui_browser_refresh_control(sd, sensitive, error, (MainUICancel){0});
}

bool mainui_browser_delete(MainUICatalog *catalog, MainUIViewport *view, int rows)
{
    if (!catalog || !catalog->depth) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(catalog->pages[catalog->depth].cache_file);
    if (!lock) {
        snprintf(catalog->error, sizeof catalog->error, "Another catalog writer is active");
        return false;
    }
    bool ok = delete_locked(catalog, view, rows);
    mainui_file_unlock(lock);
    return ok;
}
