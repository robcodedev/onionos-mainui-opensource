/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/library.h"
#include "catalog/catalog.h"
#include "platform/files.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *string(const cJSON *json, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static int number(const cJSON *json, const char *key, int fallback)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

static int folder_index(const MainUILibrary *library, const char *id)
{
    if (!*id) {
        return -1;
    }
    for (int i = 0; i < library->folder_count; i++) {
        if (!strcmp(library->folders[i].id, id)) {
            return i;
        }
    }
    return -1;
}

static cJSON *next_record(char **cursor)
{
    char *line = *cursor;
    char *end = strchr(line, '\n');
    if (end) {
        *end = 0;
    }
    *cursor = end ? end + 1 : line + strlen(line);
    return cJSON_ParseWithOpts(line, NULL, true);
}

bool mainui_rom_key(char *out, const char *rompath)
{
    const char *transport = strstr(rompath, "launch.sh:");
    const char *in = transport ? transport + strlen("launch.sh:") : rompath;
    size_t used = 0;
    bool absolute = *in == '/';
    while (*in) {
        while (*in == '/') {
            in++;
        }
        const char *end = strchr(in, '/');
        size_t length = end ? (size_t)(end - in) : strlen(in);
        if (!length) {
            break;
        }
        bool parent = length == 2 && in[0] == '.' && in[1] == '.';
        if ((length == 1 && in[0] == '.') || (parent && !used && absolute)) {
            /* "." and ".." at the root change nothing */
        }
        else if (parent && used &&
                 !(used >= 2 && !strncmp(out + used - 2, "..", 2) &&
                   (used == 2 || out[used - 3] == '/'))) {
            while (used && out[used - 1] != '/') {
                used--;
            }
            if (used) {
                used--; /* the separator before the dropped segment */
            }
        }
        else {
            if (used + (used || absolute) + length >= MAINUI_PATH_MAX) {
                return false;
            }
            if (used || absolute) {
                out[used++] = '/';
            }
            memcpy(out + used, in, length);
            used += length;
        }
        in += length;
    }
    if (!used && absolute) {
        out[used++] = '/';
    }
    out[used] = 0;
    return true;
}

bool mainui_same_rom(const char *a, const char *b)
{
    char left[MAINUI_PATH_MAX], right[MAINUI_PATH_MAX];
    return *a && *b && mainui_rom_key(left, a) && mainui_rom_key(right, b) && !strcmp(left, right);
}

MainUIRecentIdentity mainui_recent_identity(const cJSON *record)
{
    const char *rom = string(record, "rompath"), *launch = string(record, "launch");
    /* Normalize only Search's known encoding; a drive-letter colon is not a
     * Search separator. */
    const char *separator = strstr(rom, "launch.sh:");
    if (separator) {
        size_t length = (size_t)(separator - rom) + strlen("launch.sh");
        return (MainUIRecentIdentity){rom, rom + length + 1, length};
    }
    return (MainUIRecentIdentity){launch, rom, strlen(launch)};
}

/* A Recent's key: its effective launcher and ROM, both through mainui_rom_key,
 * so a console list's stock spelling and Search's spelling of one launch are
 * one Recent. The launcher stays part of it: one ROM under two emulators is
 * two Recents. Owned by the caller; NULL if out of memory or too long. */
static char *recent_key(const cJSON *record)
{
    MainUIRecentIdentity effective = mainui_recent_identity(record);
    char launch[MAINUI_PATH_MAX], launch_key[MAINUI_PATH_MAX], rom_key[MAINUI_PATH_MAX];
    if (effective.launch_length >= sizeof launch) {
        return NULL;
    }
    memcpy(launch, effective.launch, effective.launch_length);
    launch[effective.launch_length] = 0;
    if (!mainui_rom_key(launch_key, launch) || !mainui_rom_key(rom_key, effective.rom)) {
        return NULL;
    }
    size_t size = strlen(launch_key) + strlen(rom_key) + 2;
    char *key = malloc(size);
    if (key) {
        snprintf(key, size, "%s\n%s", launch_key, rom_key);
    }
    return key;
}

bool mainui_recent_same(const cJSON *a, const cJSON *b)
{
    char *left = recent_key(a), *right = recent_key(b);
    bool same = left && right && !strcmp(left, right);
    free(left);
    free(right);
    return same;
}

/* An entry's identity; see mainui_library_find(). Owned by the caller. */
static char *record_key(const MainUILibrary *library, const cJSON *json)
{
    if (library->recent) {
        return recent_key(json); /* the reader dedupes as the writer does */
    }
    const char *rom = string(json, "rompath"), *launch = string(json, "launch");
    const char *label = string(json, "label");
    size_t size = *rom ? strlen(rom) + 1 : strlen(launch) + strlen(label) + 32;
    char *key = malloc(size);
    if (key) {
        if (*rom) {
            snprintf(key, size, "%s", rom);
        }
        else {
            snprintf(key, size, "%d|%s|%s", number(json, "type", 5), launch, label);
        }
    }
    return key;
}

int mainui_library_find(const MainUILibrary *library, const cJSON *record)
{
    if (!record) {
        return -1;
    }
    /* Favorites list one ROM more than once when the labels differ, and those
     * rows share an identity. Prefer the row the record describes exactly. */
    if (!library->recent) {
        const char *label = string(record, "label"), *rom = string(record, "rompath");
        const char *launch = string(record, "launch");
        int type = number(record, "type", 5);
        for (int row = 0; row < library->visible_count; row++) {
            int index = library->visible[row];
            const MainUILibraryItem *item = index >= 0 ? &library->items[index] : NULL;
            if (item && item->type == type && !strcmp(item->label, label) &&
                !strcmp(item->rom, rom) && !strcmp(item->launch, launch)) {
                return row;
            }
        }
    }
    /* Otherwise by identity, but only if it names exactly one row: with several,
     * keeping the remembered position beats guessing between them. */
    char *key = record_key(library, record);
    int found = -1, matches = 0;
    for (int row = 0; key && row < library->visible_count; row++) {
        int index = library->visible[row];
        if (index >= 0 && !strcmp(key, library->items[index].identity) && !matches++) {
            found = row;
        }
    }
    free(key);
    return matches == 1 ? found : -1;
}

static bool add_record(MainUILibrary *library, cJSON *json)
{
    const char *label = string(json, "label");
    const char *rom = string(json, "rompath"), *launch = string(json, "launch");
    int type = number(json, "type", 5);
    if (!*label || (library->recent && (!*rom || type == 3 || !strcmp(launch, "setstate")))) {
        cJSON_Delete(json);
        return true;
    }
    /* Search encodes the source launcher before the real ROM; store the
     * effective launcher and ROM, the identity removal also uses. */
    MainUIRecentIdentity effective = mainui_recent_identity(json);
    if (library->recent && effective.rom != rom) {
        size_t length = effective.launch_length;
        char *source = malloc(length + 1);
        if (!source) {
            cJSON_Delete(json);
            return false;
        }
        memcpy(source, effective.launch, length);
        source[length] = 0;
        cJSON *new_launch = cJSON_CreateString(source);
        cJSON *new_rom = cJSON_CreateString(effective.rom);
        free(source);
        if (!new_launch || !new_rom) {
            cJSON_Delete(new_launch);
            cJSON_Delete(new_rom);
            cJSON_Delete(json);
            return false;
        }
        if (cJSON_HasObjectItem(json, "launch")) {
            cJSON_ReplaceItemInObjectCaseSensitive(json, "launch", new_launch);
        }
        else {
            cJSON_AddItemToObject(json, "launch", new_launch);
        }
        cJSON_ReplaceItemInObjectCaseSensitive(json, "rompath", new_rom);
        launch = string(json, "launch");
        rom = string(json, "rompath");
    }
    char *key = record_key(library, json);
    if (!key) {
        cJSON_Delete(json);
        return false;
    }
    for (int i = 0; i < library->count; i++) {
        bool duplicate = library->recent ? !strcmp(library->items[i].identity, key)
                                         : !strcmp(library->items[i].label, label);
        if (duplicate) {
            free(key);
            cJSON_Delete(json);
            return true;
        }
    }
    MainUILibraryItem *item = &library->items[library->count];
    *item = (MainUILibraryItem){.json = json,
                                .label = label,
                                .rom = rom,
                                .launch = launch,
                                .identity = key,
                                .type = type,
                                .folder = -1,
                                .order = library->count};
    library->count++;
    return true;
}

static void clear_folders(MainUILibrary *library)
{
    for (int i = 0; i < library->folder_count; i++) {
        cJSON_Delete(library->folders[i].json);
    }
    library->folder_count = 0;
}

static bool load_folders(MainUILibrary *library, const char *path)
{
    char *data = mainui_read_text(path, 8 * 1024 * 1024);
    if (!data) {
        return false;
    }
    size_t data_size = strlen(data);
    char *cursor = data;
    cJSON *header = next_record(&cursor);
    bool valid = number(header, "schema", 0) == 1;
    cJSON_Delete(header);
    while (valid && *cursor) {
        cJSON *json = next_record(&cursor);
        if (!json) {
            valid = false;
            break;
        }
        if (!strcmp(string(json, "kind"), "folder")) {
            const char *id = string(json, "id"), *name = string(json, "name");
            if (*id && *name && folder_index(library, id) < 0 &&
                library->folder_count < MAINUI_FOLDER_LIMIT) {
                MainUILibraryFolder *folder = &library->folders[library->folder_count++];
                *folder = (MainUILibraryFolder){.json = json,
                                                .id = id,
                                                .name = name,
                                                .parent_id = string(json, "parent"),
                                                .parent = -1,
                                                .order = number(json, "order", 0)};
                json = NULL;
            }
        }
        cJSON_Delete(json);
    }
    if (!valid) {
        clear_folders(library);
        free(data);
        return false;
    }
    for (int i = 0; i < library->folder_count; i++) {
        library->folders[i].parent = folder_index(library, library->folders[i].parent_id);
    }
    /* Repair cycles and excessive depth to root. No repaired data is published. */
    for (int i = 0; i < library->folder_count; i++) {
        int parent = library->folders[i].parent, depth = 1;
        while (parent >= 0 && parent != i && depth <= 3) {
            parent = library->folders[parent].parent;
            depth++;
        }
        if (parent == i || depth > 3) {
            library->folders[i].parent = -1;
        }
    }
    /* Restore the line separators for a second in-memory pass; assignments
     * must be resolved against the same bytes as the folder definitions. */
    for (size_t i = 0; i < data_size; i++) {
        if (!data[i]) {
            data[i] = '\n';
        }
    }
    cursor = data;
    header = next_record(&cursor);
    cJSON_Delete(header);
    bool assigned[MAINUI_LIBRARY_LIMIT] = {0};
    int assignments = 0;
    while (*cursor && assignments < MAINUI_LIBRARY_LIMIT) {
        cJSON *json = next_record(&cursor);
        if (!strcmp(string(json, "kind"), "item")) {
            assignments++;
            for (int i = 0; i < library->count; i++) {
                MainUILibraryItem *item = &library->items[i];
                if (!assigned[i] && item->type == number(json, "type", 5) &&
                    !strcmp(item->identity, string(json, "key"))) {
                    item->folder = folder_index(library, string(json, "folder"));
                    item->order = number(json, "order", item->order);
                    assigned[i] = true;
                }
            }
        }
        cJSON_Delete(json);
    }
    free(data);
    return true;
}

static int row_order(const MainUILibrary *library, int row)
{
    return row < 0 ? library->folders[-row - 1].order : library->items[row].order;
}

static void show_folder(MainUILibrary *library)
{
    int count = 0;
    library->visible_games = 0;
    for (int i = 0; i < library->folder_count; i++) {
        library->folders[i].direct_games = 0;
    }
    for (int i = 0; i < library->count; i++) {
        int folder = library->items[i].folder;
        if (folder >= 0 && folder < library->folder_count) {
            library->folders[folder].direct_games++;
        }
        if (folder == library->current) {
            library->visible_games++;
        }
    }
    if (library->current >= 0) {
        library->visible[count++] = INT_MIN;
    }
    for (int i = 0; i < library->folder_count; i++) {
        if (library->folders[i].parent == library->current) {
            library->visible[count++] = -i - 1;
        }
    }
    for (int i = 0; i < library->count; i++) {
        if (library->items[i].folder == library->current) {
            library->visible[count++] = i;
        }
    }
    /* Stable insertion preserves source order for equal order values. Folders
     * stay ahead of ordinary entries, and the synthetic parent stays first. */
    int start = library->current >= 0 ? 1 : 0;
    for (int i = start + 1; i < count; i++) {
        int row = library->visible[i], j = i;
        while (j > start) {
            int before = library->visible[j - 1];
            if ((before < 0) != (row < 0) ||
                row_order(library, before) <= row_order(library, row)) {
                break;
            }
            library->visible[j] = before;
            j--;
        }
        library->visible[j] = row;
    }
    library->visible_count = count;
    library->leading_folders = count - library->visible_games;
}

bool mainui_library_restore_recent(const char *sd)
{
    char normal[4096], hidden[4096];
    int a = snprintf(normal, sizeof normal, "%s/Roms/recentlist.json", sd);
    int b = snprintf(hidden, sizeof hidden, "%s/Roms/recentlist-hidden.json", sd);
    if (a <= 0 || a >= (int)sizeof normal || b <= 0 || b >= (int)sizeof hidden) {
        return false;
    }
    /* patch-main-menu-layout startup contract: no merge and no overwrite. */
    if (mainui_file_stamp(normal).exists || !mainui_file_stamp(hidden).exists) {
        return true;
    }
    /* Onion SD cards use FAT, which cannot implement move via hard links.
     * Serialize launcher recovery and recheck immediately before stock rename. */
    MainUIFileLock *lock = mainui_file_lock(normal);
    if (!lock) {
        return false;
    }
    bool present = mainui_file_stamp(normal).exists;
    bool ok = present || rename(hidden, normal) == 0;
    if (ok && !present && !mainui_sync_parent(normal)) {
        /* Renamed either way; only the flush is in doubt. */
        fprintf(stderr, "Restored %s, but flushing its folder failed: %s\n", normal,
                strerror(errno));
    }
    mainui_file_unlock(lock);
    return ok;
}

static bool stamps(MainUIFileStamp out[3], const char *sd, bool recent)
{
    const char *names[] = {recent ? "recentlist.json" : "favourite.json", "favourite-folders.json",
                           "favourite-folders.json.bak"};
    for (int i = 0; i < 3; ++i) {
        char path[4096];
        int n = snprintf(path, sizeof path, "%s/Roms/%s", sd, names[i]);
        if (n <= 0 || n >= (int)sizeof path) {
            return false;
        }
        out[i] = recent && i ? (MainUIFileStamp){0} : mainui_file_stamp(path);
    }
    return true;
}

bool mainui_library_changed(const MainUILibrary *library, const char *sd)
{
    MainUIFileStamp current[3];
    if (!library || !sd || !stamps(current, sd, library->recent)) {
        return true;
    }
    for (int i = 0; i < 3; ++i) {
        if (!mainui_file_stamp_equal(current[i], library->source_stamps[i])) {
            return true;
        }
    }
    return false;
}

bool mainui_library_open(MainUILibrary *library, const char *sd, bool recent)
{
    return mainui_library_open_control(library, sd, recent, (MainUICancel){0});
}

bool mainui_library_open_control(MainUILibrary *library, const char *sd, bool recent,
                                 MainUICancel cancel)
{
    *library = (MainUILibrary){.recent = recent, .current = -1};
    if (mainui_cancelled(cancel) || !stamps(library->source_stamps, sd, recent)) {
        return false;
    }
    library->items = calloc(recent ? 50 : MAINUI_LIBRARY_LIMIT, sizeof *library->items);
    if (!library->items) {
        return false;
    }
    char path[4096];
    int length = snprintf(path, sizeof path, "%s/Roms/%s", sd,
                          recent ? "recentlist.json" : "favourite.json");
    if (length < 0 || length >= (int)sizeof path) {
        return false;
    }
    errno = 0;
    char *data = mainui_read_text(path, 8 * 1024 * 1024);
    if (!data && errno != ENOENT) {
        return false;
    }
    if (data) {
        char *cursor = data;
        int parsed = 0;
        while (*cursor && library->count < (recent ? 50 : MAINUI_LIBRARY_LIMIT) &&
               (!recent || parsed < 200)) {
            if (mainui_cancelled(cancel)) {
                free(data);
                return false;
            }
            cJSON *json = next_record(&cursor);
            if (!json) {
                continue;
            }
            parsed++;
            if (!add_record(library, json)) {
                free(data);
                return false;
            }
        }
        free(data);
    }
    if (!recent) {
        length = snprintf(path, sizeof path, "%s/Roms/favourite-folders.json", sd);
        if (length > 0 && length < (int)sizeof path && !load_folders(library, path)) {
            length = snprintf(path, sizeof path, "%s/Roms/favourite-folders.json.bak", sd);
            if (length > 0 && length < (int)sizeof path) {
                load_folders(library, path);
            }
        }
    }
    show_folder(library);
    return !mainui_cancelled(cancel) && !mainui_library_changed(library, sd);
}

void mainui_library_close(MainUILibrary *library)
{
    for (int i = 0; i < library->count; i++) {
        cJSON_Delete(library->items[i].json);
        free(library->items[i].identity);
    }
    clear_folders(library);
    free(library->items);
    *library = (MainUILibrary){0};
}

const char *mainui_library_label(const MainUILibrary *library, int index)
{
    if (index < 0 || index >= library->visible_count) {
        return "";
    }
    int row = library->visible[index];
    return row == INT_MIN ? ".."
           : row < 0      ? library->folders[-row - 1].name
                          : library->items[row].label;
}

bool mainui_library_is_folder(const MainUILibrary *library, int index)
{
    return index >= 0 && index < library->visible_count && library->visible[index] < 0;
}

bool mainui_library_enter(MainUILibrary *library, int index)
{
    if (!mainui_library_is_folder(library, index)) {
        return false;
    }
    int row = library->visible[index];
    if (row == INT_MIN) {
        return mainui_library_back(library);
    }
    library->current = -row - 1;
    show_folder(library);
    return true;
}

bool mainui_library_back(MainUILibrary *library)
{
    if (library->current < 0) {
        return false;
    }
    library->current = library->folders[library->current].parent;
    show_folder(library);
    return true;
}

const char *mainui_library_title(const MainUILibrary *library)
{
    return library->current >= 0 ? library->folders[library->current].name
           : library->recent     ? "Recents"
                                 : "Favorites";
}

/* The row of folder `child` in its parent's list, and that list's length,
 * from show_folder() itself so the order always matches what is displayed:
 * sibling folders sort by their order value, not by their position in the
 * sidecar. Leaves the parent's list shown; the caller shows the right one. */
static int row_in_parent(MainUILibrary *library, int child, int *total)
{
    library->current = library->folders[child].parent;
    show_folder(library);
    *total = library->visible_count;
    for (int row = 0; row < library->visible_count; row++) {
        if (library->visible[row] == -child - 1) {
            return row;
        }
    }
    /* Unreachable: show_folder() lists every folder whose parent is the
     * current one. Row 0 (".." or the first entry) keeps the window valid
     * if a later change ever breaks that. */
    return 0;
}

void mainui_library_select_path(MainUILibrary *library)
{
    int current = library->current;
    for (int child = current, depth = 0; child >= 0 && depth <= MAINUI_FOLDER_LIMIT; depth++) {
        int parent = library->folders[child].parent, total = 0;
        MainUIViewport *view = &library->views[parent + 1];
        view->selected = row_in_parent(library, child, &total);
        view->total = total;
        child = parent;
    }
    if (current >= 0) {
        library->current = current;
        show_folder(library);
    }
}

/* An edit reloads the whole library; keep the windows remembered for other
 * folders, matched by folder id (a removed folder's goes), then point the way
 * down to the current folder at its rows. */
static void keep_views(MainUILibrary *fresh, const MainUILibrary *old)
{
    fresh->views[0] = old->views[0];
    for (int i = 0; i < old->folder_count; i++) {
        for (int j = 0; j < fresh->folder_count; j++) {
            if (!strcmp(old->folders[i].id, fresh->folders[j].id)) {
                fresh->views[j + 1] = old->views[i + 1];
                break;
            }
        }
    }
    mainui_library_select_path(fresh);
}

bool mainui_library_reload(MainUILibrary *library, const char *sd)
{
    MainUILibrary *fresh = calloc(1, sizeof *fresh);
    if (!fresh) {
        return false;
    }
    if (!mainui_library_open(fresh, sd, library->recent)) {
        mainui_library_close(fresh);
        free(fresh);
        return false;
    }
    if (library->current >= 0) {
        const char *id = library->folders[library->current].id;
        for (int i = 0; i < fresh->folder_count; i++) {
            if (!strcmp(id, fresh->folders[i].id)) {
                fresh->current = i;
                break;
            }
        }
        show_folder(fresh);
    }
    keep_views(fresh, library);
    mainui_library_close(library);
    *library = *fresh;
    free(fresh);
    return true;
}

bool mainui_library_contains(const MainUILibrary *library, const char *rom)
{
    if (!library || !rom || !*rom) {
        return false;
    }
    for (int i = 0; i < library->count; i++) {
        if (mainui_same_rom(library->items[i].rom, rom)) {
            return true;
        }
    }
    return false;
}
