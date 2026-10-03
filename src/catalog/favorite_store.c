/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/favorite_store.h"
#include "catalog/favorite_edit.h"
#include "catalog/saved_actions.h"
#include "platform/files.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SIDECAR_LIMIT (8 * 1024 * 1024)

static const char *string(const cJSON *json, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static char *copy(const char *text)
{
    size_t size = strlen(text) + 1;
    char *result = malloc(size);
    if (result) {
        memcpy(result, text, size);
    }
    return result;
}

static bool put(cJSON *object, const char *key, cJSON *value)
{
    if (!value) {
        return false;
    }
    bool ok = cJSON_HasObjectItem(object, key)
                  ? cJSON_ReplaceItemInObjectCaseSensitive(object, key, value)
                  : cJSON_AddItemToObject(object, key, value);
    if (!ok) {
        cJSON_Delete(value);
    }
    return ok;
}

static int find_row(const MainUILibrary *library, const char *key, bool folder, int type)
{
    int count = folder ? library->folder_count : library->count;
    for (int i = 0; i < count; i++) {
        const char *candidate = folder ? library->folders[i].id : library->items[i].identity;
        if (!strcmp(key, candidate) && (folder || library->items[i].type == type)) {
            return folder ? -i - 1 : i;
        }
    }
    return INT_MIN;
}

/* Retain the original objects, including unknown fields/record kinds. A damaged
 * sidecar is readable via the browser fallback but never silently overwritten. */
static cJSON *read_document(const char *path, char **original)
{
    errno = 0;
    *original = mainui_read_text(path, SIDECAR_LIMIT);
    if (!*original && errno != ENOENT) {
        return NULL;
    }
    cJSON *records = cJSON_CreateArray();
    if (!records) {
        return NULL;
    }
    if (!*original) {
        cJSON *header = cJSON_CreateObject();
        if (!header || !put(header, "schema", cJSON_CreateNumber(1)) ||
            !put(header, "generation", cJSON_CreateNumber(0)) ||
            !cJSON_AddItemToArray(records, header)) {
            cJSON_Delete(header);
            cJSON_Delete(records);
            return NULL;
        }
        return records;
    }
    char *buffer = copy(*original);
    if (!buffer) {
        cJSON_Delete(records);
        return NULL;
    }
    bool valid = true;
    int count = 0;
    for (char *line = buffer; *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = 0;
        }
        cJSON *record = cJSON_ParseWithOpts(line, NULL, true);
        if (!cJSON_IsObject(record) || ++count > MAINUI_LIBRARY_LIMIT + MAINUI_FOLDER_LIMIT + 1 ||
            !cJSON_AddItemToArray(records, record)) {
            cJSON_Delete(record);
            valid = false;
            break;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
    free(buffer);
    const cJSON *schema =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(records, 0), "schema");
    const cJSON *generation =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(records, 0), "generation");
    if (!valid || !cJSON_IsNumber(schema) || schema->valuedouble != 1 ||
        !cJSON_IsNumber(generation) || generation->valuedouble < 0 ||
        generation->valuedouble >= INT_MAX || generation->valuedouble != generation->valueint) {
        cJSON_Delete(records);
        return NULL;
    }
    int folders = 0, assignments = 0;
    cJSON *record;
    cJSON_ArrayForEach(record, records)
    {
        const char *kind = string(record, "kind");
        if ((!strcmp(kind, "folder") && ++folders > MAINUI_FOLDER_LIMIT) ||
            (!strcmp(kind, "item") && ++assignments > MAINUI_LIBRARY_LIMIT)) {
            cJSON_Delete(records);
            return NULL;
        }
    }
    return records;
}

static cJSON *find_assignment(cJSON *records, const MainUILibraryItem *item)
{
    cJSON *record;
    cJSON_ArrayForEach(record, records)
    {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(record, "type");
        if (!strcmp(string(record, "kind"), "item") &&
            !strcmp(string(record, "key"), item->identity) &&
            (cJSON_IsNumber(type) ? type->valueint : 5) == item->type) {
            return cJSON_Duplicate(record, true);
        }
    }
    return cJSON_CreateObject();
}

static bool append(char **text, size_t *length, const cJSON *record)
{
    char *line = cJSON_PrintUnformatted(record);
    if (!line) {
        return false;
    }
    size_t size = strlen(line);
    if (*length + size + 1 > SIDECAR_LIMIT) {
        free(line);
        return false;
    }
    char *grown = realloc(*text, *length + size + 2);
    if (!grown) {
        free(line);
        return false;
    }
    *text = grown;
    memcpy(grown + *length, line, size);
    *length += size;
    grown[(*length)++] = '\n';
    grown[*length] = 0;
    free(line);
    return true;
}

static const char *parent_id(const MainUILibrary *library, int parent)
{
    return parent < 0 ? "" : library->folders[parent].id;
}

static char *serialize(MainUILibrary *library, cJSON *records)
{
    int assignments = library->count;
    char *text = NULL;
    size_t length = 0;
    cJSON *header = cJSON_GetArrayItem(records, 0);
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(header, "generation");
    int next = cJSON_IsNumber(generation) ? generation->valueint : 0;
    if (next < 0 || next == INT_MAX || !put(header, "generation", cJSON_CreateNumber(next + 1)) ||
        !append(&text, &length, header)) {
        goto failed;
    }
    for (int i = 0; i < library->folder_count; i++) {
        MainUILibraryFolder *folder = &library->folders[i];
        if (!put(folder->json, "parent", cJSON_CreateString(parent_id(library, folder->parent))) ||
            !put(folder->json, "order", cJSON_CreateNumber(folder->order)) ||
            !append(&text, &length, folder->json)) {
            goto failed;
        }
    }
    for (int i = 0; i < library->count; i++) {
        MainUILibraryItem *item = &library->items[i];
        cJSON *record = find_assignment(records, item);
        bool ok = record && put(record, "kind", cJSON_CreateString("item")) &&
                  put(record, "key", cJSON_CreateString(item->identity)) &&
                  put(record, "type", cJSON_CreateNumber(item->type)) &&
                  put(record, "folder", cJSON_CreateString(parent_id(library, item->folder))) &&
                  put(record, "order", cJSON_CreateNumber(item->order)) &&
                  append(&text, &length, record);
        cJSON_Delete(record);
        if (!ok) {
            goto failed;
        }
    }
    cJSON *record;
    cJSON_ArrayForEach(record, records)
    {
        const char *kind = string(record, "kind");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(record, "type");
        bool orphan =
            !strcmp(kind, "item") && find_row(library, string(record, "key"), false,
                                              cJSON_IsNumber(type) ? type->valueint : 5) == INT_MIN;
        if (orphan && ++assignments > MAINUI_LIBRARY_LIMIT) {
            goto failed;
        }
        if (record != header && (orphan || (strcmp(kind, "folder") && strcmp(kind, "item"))) &&
            !append(&text, &length, record)) {
            goto failed;
        }
    }
    return text;
failed:
    free(text);
    return NULL;
}

/* An integer `order` as the reader takes it, or absent (the reader's default). */
static bool order_kept(const cJSON *record, int order)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(record, "order");
    return !value || (cJSON_IsNumber(value) && value->valuedouble == (double)order);
}

/* `parent`/`folder` as written, against the reader's result: a string naming
 * the folder it resolved to, or absent/empty for the root. */
static bool reference_kept(const cJSON *record, const char *key, const MainUILibrary *library,
                           int folder)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(record, key);
    if (!value) {
        return folder < 0;
    }
    return cJSON_IsString(value) && !strcmp(value->valuestring, parent_id(library, folder));
}

bool mainui_favorite_store_lossless(const MainUIFavoriteStore *store, const MainUILibrary *library)
{
    if (!store->original) {
        return true;
    }
    bool seen_folder[MAINUI_FOLDER_LIMIT] = {0};
    /* The assignment serialize() keeps for each listed Favorite: the first. */
    const cJSON **kept = calloc(library->count > 0 ? library->count : 1, sizeof *kept);
    if (!kept) {
        return false;
    }
    bool ok = true;
    int folders = 0;
    const cJSON *record;
    cJSON_ArrayForEach(record, store->records)
    {
        if (!ok) {
            break;
        }
        const char *kind = string(record, "kind");
        if (!strcmp(kind, "folder")) {
            /* Each record must be exactly one model folder, unrepaired. */
            int row = find_row(library, string(record, "id"), true, 0);
            int index = row == INT_MIN ? -1 : -row - 1;
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(record, "name");
            ok = index >= 0 && !seen_folder[index] && cJSON_IsString(name) &&
                 !strcmp(name->valuestring, library->folders[index].name) &&
                 reference_kept(record, "parent", library, library->folders[index].parent) &&
                 order_kept(record, library->folders[index].order);
            if (ok) {
                seen_folder[index] = true;
                folders++;
            }
        }
        else if (!strcmp(kind, "item")) {
            /* Assignments of listed Favorites are rewritten from the model;
             * the others are kept byte for byte and need no check. Only the
             * first assignment of a Favorite is written back, so a repeat is
             * redundant only if it is identical, extra fields included. */
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(record, "type");
            int row = find_row(library, string(record, "key"), false,
                               cJSON_IsNumber(type) ? type->valueint : 5);
            if (row != INT_MIN && kept[row]) {
                ok = cJSON_Compare(record, kept[row], true);
            }
            else if (row != INT_MIN) {
                const MainUILibraryItem *item = &library->items[row];
                ok = reference_kept(record, "folder", library, item->folder) &&
                     order_kept(record, item->order);
                kept[row] = record;
            }
        }
    }
    free(kept);
    return ok && folders == library->folder_count;
}

bool mainui_favorite_store_keep_damaged(const MainUIFavoriteStore *store)
{
    if (!store->original) {
        return true;
    }
    const cJSON *generation =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(store->records, 0), "generation");
    for (int attempt = 0; attempt < 2; attempt++) {
        char path[4096];
        int length = attempt ? snprintf(path, sizeof path, "%s.damaged-%d", store->path,
                                        cJSON_IsNumber(generation) ? generation->valueint : 0)
                             : snprintf(path, sizeof path, "%s.damaged", store->path);
        if (length < 0 || length >= (int)sizeof path) {
            return false;
        }
        errno = 0;
        char *existing = mainui_read_text(path, SIDECAR_LIMIT);
        if (existing) {
            bool same = !strcmp(existing, store->original);
            free(existing);
            if (same) {
                return true; /* a retry of the same repair */
            }
            continue;
        }
        if (errno != ENOENT) {
            return false;
        }
        /* FAT has no hard links, so not the link()-based no-replace path:
         * the store holds the .mainui-library lock, which makes the locked
         * check-then-rename publication safe. It is strict about the folder
         * flush; a copy that did land is still a kept copy. */
        if (!mainui_write_bytes_new_locked(path, store->original, strlen(store->original))) {
            char *written = mainui_read_text(path, SIDECAR_LIMIT);
            bool landed = written && !strcmp(written, store->original);
            free(written);
            if (!landed) {
                return false;
            }
        }
        fprintf(stderr, "Repaired damaged Favorite folders; the original is kept as %s\n", path);
        return true;
    }
    return false;
}

static bool publish(const char *path, const char *original, const char *text)
{
    errno = 0;
    char *current = mainui_read_text(path, SIDECAR_LIMIT);
    bool unchanged = original ? current && !strcmp(original, current) : !current && errno == ENOENT;
    free(current);
    if (!unchanged) {
        return false;
    }
    /* Keep the last valid sidecar as the browser's recovery input. Atomic file
     * publication reserves .writing exclusively; foreign temporaries are untouched. */
    char backup[4096];
    int length = snprintf(backup, sizeof backup, "%s.bak", path);
    if (original && (length < 0 || length >= (int)sizeof backup ||
                     !mainui_write_text_atomic(backup, original))) {
        return false;
    }
    return mainui_write_text_atomic(path, text);
}

/* The main sidecar was read but is not a usable schema-1 document, while
 * browsing already shows its .bak. Make editing work the same: keep the
 * damaged bytes as .damaged (never replacing an earlier copy), then publish
 * the backup, which must itself be valid, as the main file. A readable header
 * with another schema is a newer format, not damage, and is left alone. */
static void promote_backup(MainUIFavoriteStore *store)
{
    char *newline = strchr(store->original, '\n');
    cJSON *header = cJSON_ParseWithLength(
        store->original, newline ? (size_t)(newline - store->original) : strlen(store->original));
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(header, "schema");
    bool other_schema = cJSON_IsNumber(schema) && schema->valuedouble != 1;
    cJSON_Delete(header);
    char backup[sizeof store->path + 8];
    snprintf(backup, sizeof backup, "%s.bak", store->path);
    char *saved = NULL;
    cJSON *records = other_schema ? NULL : read_document(backup, &saved);
    if (!records || !saved) {
        cJSON_Delete(records);
        free(saved);
        return;
    }
    /* keep_damaged() names the copy after the generation it was found with. */
    cJSON *damaged_records = records;
    MainUIFavoriteStore damaged = {.original = store->original, .records = damaged_records};
    snprintf(damaged.path, sizeof damaged.path, "%s", store->path);
    if (!mainui_favorite_store_keep_damaged(&damaged) ||
        !mainui_write_text_atomic(store->path, saved)) {
        cJSON_Delete(records);
        free(saved);
        return;
    }
    fprintf(stderr, "Damaged %s replaced by its backup to allow editing\n", store->path);
    free(store->original);
    store->original = saved;
    store->records = records;
}

bool mainui_favorite_store_open(MainUIFavoriteStore *store, const char *sd)
{
    *store = (MainUIFavoriteStore){0};
    int length = snprintf(store->path, sizeof store->path, "%s/Roms/favourite-folders.json", sd);
    if (length < 0 || length >= (int)sizeof store->path) {
        return false;
    }
    char lock_path[4096];
    length = snprintf(lock_path, sizeof lock_path, "%s/Roms/.mainui-library", sd);
    if (length < 0 || length >= (int)sizeof lock_path ||
        !(store->lock = mainui_file_lock(lock_path))) {
        return false;
    }
    store->records = read_document(store->path, &store->original);
    if (!store->records && store->original) {
        promote_backup(store);
    }
    return store->records != NULL;
}

bool mainui_favorite_store_commit(MainUIFavoriteStore *store, MainUILibrary *library)
{
    char *text = serialize(library, store->records);
    bool ok = text && publish(store->path, store->original, text);
    free(text);
    return ok;
}

void mainui_favorite_store_close(MainUIFavoriteStore *store)
{
    mainui_file_unlock(store->lock);
    free(store->original);
    cJSON_Delete(store->records);
    *store = (MainUIFavoriteStore){0};
}

static bool forget_assignment_unlocked(const char *sd, const cJSON *removed)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/Roms/favourite-folders.json", sd);
    if (n < 0 || n >= (int)sizeof path) {
        return false;
    }
    char *original = NULL;
    cJSON *records = read_document(path, &original);
    if (!records) {
        free(original);
        return false;
    }
    if (!original) {
        cJSON_Delete(records);
        return true;
    }
    const cJSON *type_value = cJSON_GetObjectItemCaseSensitive(removed, "type");
    int type = cJSON_IsNumber(type_value) ? type_value->valueint : 5;
    const char *rom = string(removed, "rompath");
    size_t size = strlen(string(removed, "launch")) + strlen(string(removed, "label")) + 32;
    char *fallback = malloc(size);
    if (!fallback) {
        free(original);
        cJSON_Delete(records);
        return false;
    }
    snprintf(fallback, size, "%d|%s|%s", type, string(removed, "launch"), string(removed, "label"));
    const char *key = *rom ? rom : fallback;
    char *output = NULL;
    size_t length = 0;
    bool changed = false, ok = true;
    for (cJSON *record = records->child; record;) {
        cJSON *next = record->next;
        const cJSON *record_type = cJSON_GetObjectItemCaseSensitive(record, "type");
        bool match = *rom ? mainui_same_rom(string(record, "key"), rom)
                          : !strcmp(string(record, "key"), key);
        if (!strcmp(string(record, "kind"), "item") && match &&
            (cJSON_IsNumber(record_type) ? record_type->valueint : 5) == type) {
            changed = true;
            cJSON_Delete(cJSON_DetachItemViaPointer(records, record));
        }
        record = next;
    }
    if (changed) {
        cJSON *header = cJSON_GetArrayItem(records, 0);
        int generation = cJSON_GetObjectItemCaseSensitive(header, "generation")->valueint;
        ok = put(header, "generation", cJSON_CreateNumber(generation + 1));
        cJSON *record;
        cJSON_ArrayForEach(record, records)
        {
            if (ok) {
                ok = append(&output, &length, record);
            }
        }
        if (ok) {
            ok = publish(path, original, output);
        }
    }
    free(fallback);
    free(original);
    free(output);
    cJSON_Delete(records);
    return ok;
}

/* A Favorite with the assignment key and type of `removed` is still listed. */
static bool assignment_still_used(const char *sd, const cJSON *removed, bool *used)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/Roms/favourite.json", sd);
    if (n < 0 || n >= (int)sizeof path) {
        return false;
    }
    char *input = mainui_read_text(path, 8 * 1024 * 1024);
    if (!input) {
        *used = false;
        return errno == ENOENT;
    }
    const cJSON *type_value = cJSON_GetObjectItemCaseSensitive(removed, "type");
    int type = cJSON_IsNumber(type_value) ? type_value->valueint : 5;
    const char *rom = string(removed, "rompath");
    *used = false;
    for (const char *line = input; *line && !*used;) {
        const char *newline = strchr(line, '\n');
        size_t size = newline ? (size_t)(newline - line) : strlen(line);
        cJSON *item = cJSON_ParseWithLength(line, size);
        const cJSON *item_type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (cJSON_IsObject(item) && (cJSON_IsNumber(item_type) ? item_type->valueint : 5) == type) {
            *used = *rom ? mainui_same_rom(string(item, "rompath"), rom)
                         : !*string(item, "rompath") &&
                               !strcmp(string(item, "launch"), string(removed, "launch")) &&
                               !strcmp(string(item, "label"), string(removed, "label"));
        }
        cJSON_Delete(item);
        if (!newline) {
            break;
        }
        line = newline + 1;
    }
    free(input);
    return true;
}

bool mainui_favorite_remove(const char *sd, const cJSON *record)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/Roms/.mainui-library", sd);
    if (n < 0 || n >= (int)sizeof path) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    bool ok = lock && mainui_saved_action_locked(sd, false, SAVED_REMOVE, record);
    /* Under the same lock: forget the folder assignment only if no other
     * Favorite of that ROM (another label or launcher) still uses it. */
    bool used = true;
    if (ok && (!assignment_still_used(sd, record, &used) ||
               (!used && !forget_assignment_unlocked(sd, record)))) {
        fprintf(stderr, "Favorite removed; stale folder assignment cleanup deferred.\n");
    }
    mainui_file_unlock(lock);
    return ok;
}

bool mainui_favorite_forget_assignment(const char *sd, const cJSON *removed)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/Roms/.mainui-library", sd);
    if (n < 0 || n >= (int)sizeof path) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    bool ok = lock && forget_assignment_unlocked(sd, removed);
    mainui_file_unlock(lock);
    return ok;
}
