/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/cache.h"
#include "platform/files.h"
#include "sqlite3.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A query may execute at most 20 million SQLite VM instructions. This bounds
 * CPU work for malformed/unindexed inputs, not wall time on slow storage.
 * The initial budget is a host guardrail, not a measured device performance gate.
 */
#define CACHE_QUERY_BUDGET 20000
#define CACHE_PROGRESS_INTERVAL 1000
#define CACHE_MAX_VALUE_BYTES 65536

struct MainUICache {
    sqlite3 *database;
    sqlite3_stmt *rows;
    char sd_root[MAINUI_PATH_MAX];
    char rom_root[MAINUI_PATH_MAX];
    int total, folders;
    char table[512], parent[MAINUI_PATH_MAX];
    bool sensitive, search_results;
    char search_launcher[MAINUI_PATH_MAX], search_rom_root[MAINUI_PATH_MAX];
    char search_images[MAINUI_PATH_MAX]; /* Source console's artwork directory. */
    char file[4096];
    MainUIFileStamp stamp;
    int data_version;
    int remaining_query_budget;
    /* Why the last window failed: damaged content (a row the reader rejects,
     * or SQLite corruption), not a busy, I/O, memory or changed-cache error. */
    bool row_rejected, damaged;
};

static char *duplicate_text(const char *text)
{
    size_t size = strlen(text) + 1;
    char *result = malloc(size);
    if (result) {
        memcpy(result, text, size);
    }
    return result;
}

static int query_budget_exhausted(void *context)
{
    MainUICache *cache = context;
    return --cache->remaining_query_budget <= 0;
}

void mainui_cache_close(MainUICache *cache)
{
    if (!cache) {
        return;
    }
    sqlite3_finalize(cache->rows);
    sqlite3_close(cache->database);
    free(cache);
}

static bool configure_reader(MainUICache *cache)
{
    sqlite3 *database = cache->database;
    sqlite3_limit(database, SQLITE_LIMIT_LENGTH, CACHE_MAX_VALUE_BYTES);
    sqlite3_limit(database, SQLITE_LIMIT_SQL_LENGTH, 16384);
    /* Firmware can resolve an older libsqlite3.so.0 than the build-time SDK.
     * Unsupported optional db_config operations must not reject every cache.
     * The connection remains read-only on every supported runtime. */
#ifdef SQLITE_DBCONFIG_DEFENSIVE
    if (sqlite3_libversion_number() >= 3026000 &&
        sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK) {
        return false;
    }
#endif
#ifdef SQLITE_DBCONFIG_TRUSTED_SCHEMA
    if (sqlite3_libversion_number() >= 3031000 &&
        sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL) != SQLITE_OK) {
        return false;
    }
#endif
    if (sqlite3_busy_timeout(database, 100) != SQLITE_OK) {
        return false;
    }

    cache->remaining_query_budget = CACHE_QUERY_BUDGET;
    sqlite3_progress_handler(database, CACHE_PROGRESS_INTERVAL, query_budget_exhausted, cache);
    return sqlite3_exec(database,
                        "PRAGMA query_only=ON; PRAGMA cache_size=-256; PRAGMA temp_store=MEMORY",
                        NULL, NULL, NULL) == SQLITE_OK;
}

static bool count_rows(MainUICache *cache, const char *table, const char *parent)
{
    sqlite3_stmt *statement = NULL;
    /* %w escapes a double-quoted identifier. Folder keys are bound values, so
     * quotes in system names and folder paths never become executable SQL.
     */
    char *sql = sqlite3_mprintf(
        "SELECT count(*),coalesce(sum(type>0),0) FROM \"%w\" WHERE ppath=?1", table);
    int result =
        sql ? sqlite3_prepare_v2(cache->database, sql, -1, &statement, NULL) : SQLITE_NOMEM;
    sqlite3_free(sql);
    if (result == SQLITE_OK) {
        result = sqlite3_bind_text(statement, 1, parent, -1, SQLITE_TRANSIENT);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
    }

    sqlite3_int64 count = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    int folders = result == SQLITE_ROW ? sqlite3_column_int(statement, 1) : 0;
    sqlite3_finalize(statement);
    /* Leave one logical slot for a controller-owned parent row. */
    if (count < 0 || count >= INT_MAX) {
        return false;
    }
    cache->total = (int)count;
    cache->folders = folders;
    return true;
}

static bool prepare_rows(MainUICache *cache, const char *table, const char *parent,
                         bool case_sensitive)
{
    /* disp is authoritative for the displayed name, so it is used unchanged.
     * Do not derive names from filenames on this path. Ordering is by type then
     * name; id stabilizes exact name ties so paging is deterministic. */
    char *sql = sqlite3_mprintf("SELECT disp,path,imgpath,type FROM \"%w\" WHERE ppath=?1 "
                                "ORDER BY type DESC,disp COLLATE %s,id LIMIT %d OFFSET ?2",
                                table, case_sensitive ? "BINARY" : "NOCASE", MAINUI_CACHE_WINDOW);
    int result =
        sql ? sqlite3_prepare_v2(cache->database, sql, -1, &cache->rows, NULL) : SQLITE_NOMEM;
    sqlite3_free(sql);
    return result == SQLITE_OK &&
           sqlite3_bind_text(cache->rows, 1, parent, -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

/* Refuse WAL databases before SQLite sees them. A WAL reader creates the -shm
 * companion file even on a read-only open, which would write to the user's card;
 * the caller falls back to a directory scan instead. Reading journal mode from
 * the header keeps this behaviour identical whether the linked SQLite was built
 * with WAL support or with SQLITE_OMIT_WAL. Bytes 18 and 19 hold the
 * read/write version; 2 means WAL. A file too short or unreadable here is left
 * for sqlite3_open_v2 to reject. */
static bool journal_is_wal(const char *file)
{
    FILE *input = fopen(file, "rb");
    if (!input) {
        return false;
    }
    unsigned char header[20];
    size_t size = fread(header, 1, sizeof header, input);
    bool failed = ferror(input) != 0;
    fclose(input);
    return !failed && size == sizeof header && !memcmp(header, "SQLite format 3", 16) &&
           (header[18] == 2 || header[19] == 2);
}

bool mainui_cache_open(MainUICache **out, const char *file, const char *table, const char *parent,
                       const char *sd_root, const char *rom_root, bool case_sensitive, int *total)
{
    *out = NULL;
    MainUICache *cache = calloc(1, sizeof *cache);
    if (!cache) {
        return false;
    }
    if (strlen(sd_root) >= sizeof cache->sd_root || strlen(rom_root) >= sizeof cache->rom_root) {
        goto fail;
    }
    if (strlen(table) >= sizeof cache->table || strlen(parent) >= sizeof cache->parent) {
        goto fail;
    }
    strcpy(cache->table, table);
    strcpy(cache->parent, parent);
    cache->sensitive = case_sensitive;
    snprintf(cache->file, sizeof cache->file, "%s", file);
    cache->stamp = mainui_file_stamp(file);
    strcpy(cache->sd_root, sd_root);
    strcpy(cache->rom_root, rom_root);
    cache->search_results = mainui_search_root(sd_root, rom_root);

    /* Read-only mode also prevents creation of a missing database. Connections,
     * statements and progress callbacks have one owner; worker handoff follows a join.
     */
    if (journal_is_wal(file)) {
        goto fail;
    }
    if (sqlite3_open_v2(file, &cache->database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) !=
        SQLITE_OK) {
        goto fail;
    }
    if (!configure_reader(cache) || !count_rows(cache, table, parent) ||
        !prepare_rows(cache, table, parent, case_sensitive)) {
        goto fail;
    }

    if (mainui_cache_changed(cache)) {
        goto fail;
    }
    *total = cache->total;
    *out = cache;
    return true;

fail:
    fprintf(stderr, "Cache open failed: %s table=%s parent=%s SQLite=%s: %s\n", file, table, parent,
            sqlite3_libversion(),
            cache->database ? sqlite3_errmsg(cache->database) : "allocation/open failure");
    mainui_cache_close(cache);
    return false;
}

void mainui_cache_suspend(MainUICache *cache)
{
    if (cache) {
        sqlite3_finalize(cache->rows);
        cache->rows = NULL;
        sqlite3_close(cache->database);
        cache->database = NULL;
    }
}

bool mainui_cache_resume(MainUICache *cache)
{
    if (!cache || cache->database) {
        return true;
    }
    if (!mainui_file_stamp_equal(cache->stamp, mainui_file_stamp(cache->file))) {
        return false;
    }
    bool ok = sqlite3_open_v2(cache->file, &cache->database,
                              SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK &&
              configure_reader(cache) &&
              prepare_rows(cache, cache->table, cache->parent, cache->sensitive);
    cache->data_version = 0;
    if (!ok) {
        mainui_cache_suspend(cache);
    }
    return ok;
}

static const char *text_column(sqlite3_stmt *row, int column)
{
    if (sqlite3_column_type(row, column) != SQLITE_TEXT) {
        return NULL;
    }
    const char *text = (const char *)sqlite3_column_text(row, column);
    int size = sqlite3_column_bytes(row, column);
    /* Reject embedded NULs and overlong values instead of truncating identity. */
    return text && size < MAINUI_PATH_MAX && strlen(text) == (size_t)size ? text : NULL;
}

static bool decode_row(MainUICache *cache, MainUIEntry *entry)
{
    const char *label = text_column(cache->rows, 0);
    const char *path = text_column(cache->rows, 1);
    const char *artwork = text_column(cache->rows, 2);
    int type = sqlite3_column_int(cache->rows, 3);
    char host_path[MAINUI_PATH_MAX];
    char image_path[MAINUI_PATH_MAX] = "";

    /* Stock cache rows use 0 for games and 1 for folders. The patcher's parent
     * row uses 2 but is synthesized by its query, never stored in the cache.
     */
    if (!label || !path || (!*path && !(cache->search_results && type == 1)) || !artwork ||
        sqlite3_column_type(cache->rows, 3) != SQLITE_INTEGER || (type != 0 && type != 1) ||
        !mainui_catalog_path(host_path, cache->sd_root, cache->rom_root, path) ||
        (*artwork && !mainui_catalog_path(image_path, cache->sd_root, cache->rom_root, artwork))) {
        cache->row_rejected = true;
        return false;
    }

    const char *search_images = "";
    if (cache->search_results && type == 0) {
        const char *separator = strstr(path, "launch.sh:");
        if (separator) {
            char launch[MAINUI_PATH_MAX], directory[MAINUI_PATH_MAX], config[MAINUI_PATH_MAX];
            size_t length = (size_t)(separator - path) + strlen("launch.sh");
            if (length >= sizeof launch) {
                return false;
            }
            memcpy(launch, path, length);
            launch[length] = 0;
            if (!mainui_catalog_path(directory, cache->sd_root, cache->sd_root, launch)) {
                return false;
            }
            entry->launch = duplicate_text(directory);
            char *slash = strrchr(directory, '/');
            if (!entry->launch || !slash) {
                return false;
            }
            *slash = 0;
            const char *rom = path + length + 1;
            /* Results carry no artwork, so resolve the source console's ROM and
             * image directories from its config once per consecutive launcher. */
            if (strcmp(cache->search_launcher, directory)) {
                if (!mainui_catalog_path(config, cache->sd_root, directory, "config.json")) {
                    return false;
                }
                char *text = mainui_read_text(config, 1024 * 1024);
                cJSON *json = text ? cJSON_Parse(text) : NULL;
                const cJSON *root = cJSON_GetObjectItemCaseSensitive(json, "rompath");
                const cJSON *images = cJSON_GetObjectItemCaseSensitive(json, "imgpath");
                bool ok =
                    mainui_catalog_path(cache->search_images, cache->sd_root, directory,
                                        cJSON_IsString(images) ? images->valuestring : "Imgs");
                if (!cJSON_IsString(root) ||
                    !mainui_catalog_path(cache->search_rom_root, cache->sd_root, directory,
                                         root->valuestring)) {
                    /* Absolute ROM paths do not need rompath. */
                    cache->search_rom_root[0] = 0;
                }
                cJSON_Delete(json);
                free(text);
                if (!ok) {
                    return false;
                }
                strcpy(cache->search_launcher, directory);
            }
            if (*rom == '/') {
                if (!mainui_catalog_path(host_path, cache->sd_root, directory, rom)) {
                    return false;
                }
            }
            else if (!*cache->search_rom_root ||
                     !mainui_catalog_path(host_path, cache->sd_root, cache->search_rom_root, rom)) {
                return false;
            }
            search_images = cache->search_images;
        }
    }
    /* Search owns virtual folders (empty filesystem path, display-label key)
     * and action arguments. Keep action tokens intact for its launch script. */
    if (cache->search_results && type == 0 &&
        (!strcmp(path, "search") || !strcmp(path, "clear") || !strncmp(path, "setstate:", 9))) {
        snprintf(host_path, sizeof host_path, "%s", path);
    }
    entry->label = duplicate_text(label);
    entry->path = duplicate_text(host_path);
    const char *key = cache->search_results && type == 1 ? label : path;
    if (!cache->search_results && type == 1 && path[0] == '/') {
        size_t root = strlen(cache->rom_root);
        if (strncmp(host_path, cache->rom_root, root) || host_path[root] != '/') {
            return false;
        }
        key = host_path + root + 1;
    }
    entry->cache_key = duplicate_text(key);
    entry->stored_path = duplicate_text(path);
    entry->stored_image = duplicate_text(artwork);
    entry->artwork = duplicate_text(image_path);
    entry->extensions = duplicate_text("");
    entry->images = duplicate_text(search_images);
    entry->directory = type == 1;
    return entry->label && entry->path && entry->cache_key && entry->artwork && entry->extensions &&
           entry->images && entry->stored_path && entry->stored_image;
}

bool mainui_cache_window(MainUICache *cache, int offset, MainUIEntry out[MAINUI_CACHE_WINDOW],
                         int *loaded)
{
    memset(out, 0, MAINUI_CACHE_WINDOW * sizeof *out);
    *loaded = 0;
    cache->row_rejected = cache->damaged = false;
    if (mainui_cache_changed(cache) || offset < 0 || offset > cache->total) {
        return false;
    }
    cache->remaining_query_budget = CACHE_QUERY_BUDGET;
    sqlite3_reset(cache->rows);
    if (sqlite3_bind_int(cache->rows, 2, offset) != SQLITE_OK) {
        return false;
    }

    int count = 0;
    int result;
    while ((result = sqlite3_step(cache->rows)) == SQLITE_ROW) {
        if (count >= MAINUI_CACHE_WINDOW) {
            goto fail;
        }
        /* Count the row before decoding so partial allocations are owned by
         * this cleanup path even if a later field allocation fails.
         */
        if (!decode_row(cache, &out[count++])) {
            fprintf(
                stderr, "Cache row rejected: %s table=%s row=%d type=%d column-types=%d,%d,%d,%d\n",
                cache->file, cache->table, offset + count - 1, sqlite3_column_int(cache->rows, 3),
                sqlite3_column_type(cache->rows, 0), sqlite3_column_type(cache->rows, 1),
                sqlite3_column_type(cache->rows, 2), sqlite3_column_type(cache->rows, 3));
            cache->damaged = cache->row_rejected;
            goto fail;
        }
    }
    sqlite3_reset(cache->rows);
    int expected = cache->total - offset;
    if (expected > MAINUI_CACHE_WINDOW) {
        expected = MAINUI_CACHE_WINDOW;
    }
    /* Count/query calls don't hold a long read transaction. Detect a shortened
     * result rather than publishing a partial window after an external edit.
     * Same-size external edits require reopening the system for a fresh view.
     */
    if (result != SQLITE_DONE || count != expected) {
        fprintf(stderr, "Cache window failed: %s table=%s result=%d count=%d expected=%d: %s\n",
                cache->file, cache->table, result, count, expected,
                sqlite3_errmsg(cache->database));
        cache->damaged = (result & 0xff) == SQLITE_CORRUPT || (result & 0xff) == SQLITE_NOTADB;
        goto fail;
    }
    *loaded = count;
    return true;

fail:
    sqlite3_reset(cache->rows);
    for (int i = 0; i < count; i++) {
        mainui_entry_close(&out[i]);
    }
    return false;
}

bool mainui_cache_damaged(const MainUICache *cache)
{
    return cache && cache->damaged;
}

int mainui_cache_folder_count(const MainUICache *cache)
{
    return cache->folders;
}

bool mainui_cache_changed(MainUICache *cache)
{
    if (!cache || !cache->database ||
        !mainui_file_stamp_equal(cache->stamp, mainui_file_stamp(cache->file))) {
        return cache != NULL;
    }
    sqlite3_stmt *statement = NULL;
    int version = 0;
    bool ok = sqlite3_prepare_v2(cache->database, "PRAGMA data_version", -1, &statement, NULL) ==
                  SQLITE_OK &&
              sqlite3_step(statement) == SQLITE_ROW;
    if (ok) {
        version = sqlite3_column_int(statement, 0);
    }
    sqlite3_finalize(statement);
    if (!ok) {
        return true;
    }
    if (!cache->data_version) {
        cache->data_version = version;
    }
    return cache->data_version != version;
}
