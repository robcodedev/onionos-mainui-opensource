/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/search.h"
#include "platform/system_config.h"
#include "sqlite3.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mainui_search_close(MainUISearch *search)
{
    if (search->results) {
        mainui_library_close(search->results);
        free(search->results);
    }
    *search = (MainUISearch){0};
}

static const char *column(sqlite3_stmt *statement, int index)
{
    const char *text = (const char *)sqlite3_column_text(statement, index);
    int size = sqlite3_column_bytes(statement, index);
    return text && size < 4096 && strlen(text) == (size_t)size ? text : NULL;
}

typedef struct {
    int remaining;
    MainUICancel cancel;
} SearchBudget;

static int stop_query(void *context)
{
    SearchBudget *budget = context;
    return --budget->remaining <= 0 || mainui_cancelled(budget->cancel);
}

static bool field(cJSON *record, const char *key, MainUICatalog *catalog, const char *base,
                  const char *value)
{
    char resolved[4096], portable[4096];
    if (!value || !*value) {
        return cJSON_AddStringToObject(record, key, "") != NULL;
    }
    if (!mainui_catalog_path(resolved, catalog->sd, base, value)) {
        return false;
    }
    size_t size = strlen(catalog->sd);
    if (!strncmp(resolved, catalog->sd, size) && resolved[size] == '/') {
        /* A cache row's stock spelling on the card, such as
         * /mnt/SDCARD/Emu/GBC/../../Roms/..., is kept as the ROM list keeps it:
         * Favorites and Recents store it as is, and Onion's Game List Options
         * recognizes a game by it. */
        if (strcmp(key, "launch") && !strncmp(value, "/mnt/SDCARD/", 12)) {
            return cJSON_AddStringToObject(record, key, value) != NULL;
        }
        int n = snprintf(portable, sizeof portable, "/mnt/SDCARD%s", resolved + size);
        if (n < 0 || n >= (int)sizeof portable) {
            return false;
        }
        return cJSON_AddStringToObject(record, key, portable) != NULL;
    }
    return false;
}

bool mainui_search_open(MainUISearch *search, MainUICatalog *catalog, const MainUIViewport *source,
                        const char *query, int rows)
{
    if (!catalog || catalog->depth < 1 || !query || strlen(query) >= 128) {
        snprintf(search->error, sizeof search->error, "Search requires an open console.");
        return false;
    }
    MainUICatalogPage *root = &catalog->pages[1];
    int system = catalog->pages[0].view.selected;
    if (system < 0 || system >= catalog->pages[0].count) {
        return false;
    }
    MainUILibrary *results = calloc(1, sizeof *results);
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    SearchBudget budget = {20000, catalog->cancel};
    bool ok =
        results && sqlite3_open_v2(root->cache_file, &database,
                                   SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK;
    if (ok) {
        results->current = -1;
        results->items = calloc(MAINUI_LIBRARY_LIMIT, sizeof *results->items);
        ok = results->items != NULL;
        sqlite3_limit(database, SQLITE_LIMIT_LENGTH, 65536);
        sqlite3_busy_timeout(database, 100);
        sqlite3_progress_handler(database, 1000, stop_query, &budget);
        /* Older host headers and firmware may lack these optional settings. */
#ifdef SQLITE_DBCONFIG_DEFENSIVE
        if (sqlite3_libversion_number() >= 3026000) {
            ok = ok && sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) == SQLITE_OK;
        }
#endif
#ifdef SQLITE_DBCONFIG_TRUSTED_SCHEMA
        if (sqlite3_libversion_number() >= 3031000) {
            ok = ok &&
                 sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL) == SQLITE_OK;
        }
#endif
    }
    cJSON *settings = mainui_system_read(catalog->sd);
    const cJSON *language = cJSON_GetObjectItemCaseSensitive(settings, "language");
    bool chinese = catalog->pages[0].entries[system].shortname && cJSON_IsString(language) &&
                   !strcmp(language->valuestring, "ch.lang");
    cJSON_Delete(settings);
    char *sql =
        sqlite3_mprintf("SELECT disp,path,imgpath FROM \"%w\" WHERE type=0 AND %s LIKE ?1 "
                        "ORDER BY disp COLLATE %s,path COLLATE %s,path LIMIT %d",
                        root->cache_table, chinese ? "cpinyin" : "pinyin",
                        catalog->case_sensitive ? "BINARY" : "NOCASE",
                        catalog->case_sensitive ? "BINARY" : "NOCASE", MAINUI_LIBRARY_LIMIT + 1);
    if (ok) {
        ok = sql && sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
    }
    sqlite3_free(sql);
    char pattern[132];
    snprintf(pattern, sizeof pattern, "%%%s%%", query);
    if (ok) {
        ok = sqlite3_bind_text(statement, 1, pattern, -1, SQLITE_TRANSIENT) == SQLITE_OK;
    }
    int status = SQLITE_DONE;
    while (ok && (status = sqlite3_step(statement)) == SQLITE_ROW) {
        if (results->count >= MAINUI_LIBRARY_LIMIT) {
            ok = false;
            break;
        }
        const char *label = column(statement, 0), *path = column(statement, 1),
                   *artwork = column(statement, 2);
        cJSON *record = cJSON_CreateObject();
        ok = record && label && path && *path && artwork &&
             cJSON_AddStringToObject(record, "label", label) &&
             field(record, "rompath", catalog, root->path, path) &&
             field(record, "imgpath", catalog, root->path, artwork) &&
             field(record, "launch", catalog, catalog->sd,
                   catalog->pages[0].entries[system].launch) &&
             cJSON_AddNumberToObject(record, "type", 5);
        if (!ok) {
            cJSON_Delete(record);
            break;
        }
        int i = results->count++;
        results->items[i] = (MainUILibraryItem){
            .json = record,
            .label = cJSON_GetObjectItemCaseSensitive(record, "label")->valuestring,
            .rom = cJSON_GetObjectItemCaseSensitive(record, "rompath")->valuestring,
            .launch = cJSON_GetObjectItemCaseSensitive(record, "launch")->valuestring,
            .type = 5,
            .folder = -1,
            .order = i};
        results->visible[i] = i;
    }
    ok = ok && status == SQLITE_DONE && !mainui_cancelled(catalog->cancel);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (!ok) {
        if (results) {
            mainui_library_close(results);
            free(results);
        }
        snprintf(search->error, sizeof search->error,
                 "Cannot read Search results (cache unavailable or result/work limit exceeded).");
        return false;
    }
    MainUIViewport saved = *source;
    mainui_search_close(search);
    search->results = results;
    search->source_view = saved;
    results->visible_count = results->visible_games = results->count;
    snprintf(search->query, sizeof search->query, "%s", query);
    snprintf(search->title, sizeof search->title, "%s: %s", root->title, query);
    mainui_viewport_restore(&search->view, results->count, rows, 0, 0, rows - 1);
    return true;
}

bool mainui_search_restore_view(MainUISearch *search, const cJSON *saved, const cJSON *record,
                                int rows)
{
    const char *keys[] = {"currpos", "pagestart", "pageend"};
    int values[3];
    for (int i = 0; i < 3; i++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(saved, keys[i]);
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
            value->valuedouble < INT_MIN || value->valuedouble > INT_MAX ||
            value->valuedouble != value->valueint) {
            return false;
        }
        values[i] = value->valueint;
    }
    mainui_viewport_restore(&search->view, search->results->count, rows, values[0], values[1],
                            values[2]);
    const cJSON *rom = cJSON_GetObjectItemCaseSensitive(record, "rompath");
    if (cJSON_IsString(rom)) {
        for (int i = 0; i < search->results->count; i++) {
            if (!strcmp(rom->valuestring, search->results->items[i].rom)) {
                mainui_viewport_move(&search->view, rows, i - search->view.selected, false);
                break;
            }
        }
    }
    search->postgame = true;
    return true;
}
