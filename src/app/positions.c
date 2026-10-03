/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/positions.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool location(const MainUICatalog *catalog, char out[4096], bool create)
{
#ifdef MAINUI_ONION
    if (!strcmp(catalog->sd, "/mnt/SDCARD")) {
        strcpy(out, "/appconfigs/romwinidx.json");
        return true;
    }
#endif
    char directory[4096];
    int n = snprintf(directory, sizeof directory, "%s/appconfigs", catalog->sd);
    if (n <= 0 || n >= (int)sizeof directory) {
        return false;
    }
    if (create) {
        int result = mkdir(directory, 0755);
        if (result && errno != EEXIST) {
            return false;
        }
    }
    n = snprintf(out, 4096, "%s/romwinidx.json", directory);
    return n > 0 && n < 4096;
}

/* repair (save path, under the file lock): a file that was read completely but
 * is not a valid position list (also one with NUL bytes, or too large) is
 * moved aside to <file>.bad and replaced, so one damaged file does not stop
 * positions being saved forever. A file that cannot be read at all (I/O
 * error) is still never replaced. */
static cJSON *read_positions(const char *file, bool repair)
{
    char *text = mainui_read_text(file, 4 * 1024 * 1024);
    int read_error = text ? 0 : errno;
    cJSON *root = text ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    /* Read in full but not usable text (NUL bytes, EINVAL) or too large to be
     * a position list (EFBIG): damaged content, repaired like invalid JSON.
     * The rename keeps every byte without reading them. */
    bool readable = text != NULL || read_error == EINVAL || read_error == EFBIG;
    free(text);
    if (repair && readable &&
        (!cJSON_IsObject(root) || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "list")))) {
        char aside[4096];
        int n = snprintf(aside, sizeof aside, "%s.bad", file);
        if (n > 0 && n < (int)sizeof aside && rename(file, aside) == 0) {
            fprintf(stderr, "Invalid %s moved to %s\n", file, aside);
            mainui_sync_parent(file);
            cJSON_Delete(root);
            root = NULL;
        }
    }
    if (!root && !mainui_file_stamp(file).exists) {
        root = cJSON_CreateObject();
        if (root && !cJSON_AddArrayToObject(root, "list")) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    if (!cJSON_IsObject(root) || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "list"))) {
        cJSON_Delete(root);
        return NULL; /* Never replace an unreadable existing state file. */
    }
    return root;
}

static cJSON *find_position(cJSON *root, const MainUICatalog *catalog, const char *directory)
{
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "list"))
    {
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "rompath");
        char normalized[4096];
        if (cJSON_IsString(path) &&
            mainui_catalog_path(normalized, catalog->sd, catalog->sd, path->valuestring) &&
            !strcmp(normalized, directory)) {
            return item;
        }
    }
    return NULL;
}

static bool number(const cJSON *item, const char *key, int *value)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(item, key);
    if (!cJSON_IsNumber(n) || n->valuedouble < INT_MIN || n->valuedouble > INT_MAX ||
        n->valuedouble != n->valueint) {
        return false;
    }
    *value = n->valueint;
    return true;
}

/* Search folders are transient query groups, not filesystem lists. Their paths
 * can equal real console roots (GB), or be empty for virtual groups. Never share
 * persistent positions with those consoles or carry them into a new search.
 * Browser page views still preserve parents while backing out of a result. */
static bool search_results(const MainUICatalog *catalog)
{
    char root[4096];
    return catalog && catalog->depth &&
           mainui_catalog_path(root, catalog->sd, catalog->sd, "App/Search/data") &&
           !strcmp(root, catalog->pages[1].path);
}

void mainui_positions_restore(const MainUICatalog *catalog, MainUIViewport *view, int rows)
{
    char file[4096];
    if (!catalog->depth || search_results(catalog) || !location(catalog, file, false)) {
        return;
    }
    cJSON *root = read_positions(file, false);
    cJSON *item = find_position(root, catalog, catalog->pages[catalog->depth].path);
    int pos, start, end;
    if (number(item, "pos", &pos) && number(item, "start", &start) && number(item, "end", &end)) {
        mainui_viewport_restore(view, view->total, rows, pos, start, end);
    }
    cJSON_Delete(root);
}

static bool stock_key(const MainUICatalog *catalog, int level, char out[4096])
{
    const MainUICatalogPage *page = &catalog->pages[level];
    int system = catalog->pages[0].view.selected;
    const char *config = system >= 0 && system < catalog->pages[0].count
                             ? catalog->pages[0].entries[system].config
                             : NULL;
    char *text = config ? mainui_read_text(config, 1024 * 1024) : NULL;
    cJSON *root = text ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    free(text);
    const cJSON *rom = cJSON_GetObjectItemCaseSensitive(root, "rompath");
    const char *slash = config ? strrchr(config, '/') : NULL;
    char raw[4096];
    int n = -1;
    if (cJSON_IsString(rom) && *rom->valuestring && slash) {
        n = rom->valuestring[0] == '/' ? snprintf(raw, sizeof raw, "%s", rom->valuestring)
                                       : snprintf(raw, sizeof raw, "%.*s/%s", (int)(slash - config),
                                                  config, rom->valuestring);
    }
    cJSON_Delete(root);
    if (n <= 0 || n >= (int)sizeof raw) {
        n = snprintf(out, 4096, "/mnt/SDCARD%s", page->path + strlen(catalog->sd));
        return n > 0 && n < 4096;
    }
    /* Stock compares the configured ROM spelling, including Emu/../ segments.
     * Preserve that spelling for newly added records, normalize only on lookup. */
    size_t sd_length = strlen(catalog->sd);
    const char *suffix = page->path + strlen(catalog->pages[1].path);
    if (!strncmp(raw, catalog->sd, sd_length) && raw[sd_length] == '/') {
        n = snprintf(out, 4096, "/mnt/SDCARD%s%s", raw + sd_length, suffix);
    }
    else {
        n = snprintf(out, 4096, "%s%s", raw, suffix);
    }
    return n > 0 && n < 4096;
}

bool mainui_positions_save(const MainUICatalog *catalog, const MainUIViewport *view)
{
    if (!catalog || !catalog->depth || search_results(catalog)) {
        return true;
    }
    char file[4096];
    if (!location(catalog, file, true)) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(file);
    cJSON *root = lock ? read_positions(file, true) : NULL;
    bool ok = root != NULL;
    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    for (int i = 1; ok && i <= catalog->depth; ++i) {
        const MainUICatalogPage *page = &catalog->pages[i];
        const MainUIViewport *saved = i == catalog->depth ? view : &page->view;
        cJSON *item = find_position(root, catalog, page->path);
        if (!item) {
            char path[4096];
            item = stock_key(catalog, i, path) ? cJSON_CreateObject() : NULL;
            ok = item && cJSON_AddStringToObject(item, "rompath", path);
            if (!ok || !cJSON_AddItemToArray(list, item)) {
                cJSON_Delete(item);
                ok = false;
                break;
            }
        }
        const char *keys[] = {"pos", "start", "end"};
        int values[] = {saved->selected, saved->start, saved->end};
        for (int k = 0; ok && k < 3; ++k) {
            cJSON_DeleteItemFromObjectCaseSensitive(item, keys[k]);
            ok = cJSON_AddNumberToObject(item, keys[k], values[k]) != NULL;
        }
    }
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
    ok = text && mainui_write_text_atomic(file, text);
    free(text);
    cJSON_Delete(root);
    mainui_file_unlock(lock);
    return ok;
}
