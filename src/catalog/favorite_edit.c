/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/favorite_edit.h"
#include "catalog/favorite_store.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOLDER_NAME_LIMIT 127

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

static bool fail(MainUIFavoriteEditor *editor, const char *message)
{
    snprintf(editor->error, sizeof editor->error, "%s", message);
    return false;
}

void mainui_favorite_editor_close(MainUIFavoriteEditor *editor)
{
    free(editor->key);
    *editor = (MainUIFavoriteEditor){0};
}

static int row_at(const MainUILibrary *library, int selected)
{
    if (selected < 0 || selected >= library->visible_count) {
        return INT_MIN;
    }
    return library->visible[selected];
}

static const char *row_key(const MainUILibrary *library, int row)
{
    if (row == INT_MIN) {
        return "";
    }
    return row < 0 ? library->folders[-row - 1].id : library->items[row].identity;
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

bool mainui_favorite_is_cut(const MainUIFavoriteEditor *editor, const MainUILibrary *library,
                            int selected)
{
    if (!editor->key || !library || library->recent) {
        return false;
    }
    int row = row_at(library, selected);
    return row != INT_MIN && (row < 0) == editor->folder &&
           (row < 0 || library->items[row].type == editor->type) &&
           !strcmp(row_key(library, row), editor->key);
}

static int depth(const MainUILibrary *library, int folder)
{
    int count = 0;
    while (folder >= 0) {
        if (folder >= library->folder_count || ++count > 3) {
            return 4;
        }
        folder = library->folders[folder].parent;
    }
    return count;
}

/* Checks the 255-byte path limit only for `changed` and its descendants,
 * the only paths an edit can lengthen. A sidecar that already holds an
 * over-long path elsewhere must not block unrelated edits. */
static bool folder_paths_fit(const MainUILibrary *library, int changed)
{
    if (changed < 0 || changed >= library->folder_count) {
        return true;
    }
    for (int i = 0; i < library->folder_count; ++i) {
        size_t bytes = 0;
        int levels = 0;
        bool affected = false;
        for (int folder = i; folder >= 0; folder = library->folders[folder].parent) {
            if (folder >= library->folder_count || ++levels > 3) {
                return false;
            }
            affected = affected || folder == changed;
            bytes += strlen(library->folders[folder].name) + 1;
        }
        if (affected && bytes > 255) {
            return false;
        }
    }
    return true;
}

static bool whitespace(unsigned char ch)
{
    return ch == ' ' || (ch >= '\t' && ch <= '\r');
}

static bool normalize_name(const char *input, char output[FOLDER_NAME_LIMIT + 1])
{
    if (!input) {
        return false;
    }
    while (whitespace((unsigned char)*input)) {
        input++;
    }
    while (*input == '>' || *input == '.') {
        input++;
        while (whitespace((unsigned char)*input)) {
            input++;
        }
    }
    size_t length = strlen(input);
    while (length && whitespace((unsigned char)input[length - 1])) {
        length--;
    }
    if (!length || length > FOLDER_NAME_LIMIT) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)input[i];
        if (ch < 32 || ch == 127 || ch == '/') {
            return false;
        }
    }
    memcpy(output, input, length);
    output[length] = 0;
    return true;
}

static const char *parent_id(const MainUILibrary *library, int parent)
{
    return parent < 0 ? "" : library->folders[parent].id;
}

static int compare_names(const char *a, const char *b)
{
    const unsigned char *left = (const unsigned char *)a, *right = (const unsigned char *)b;
    while (*left && *right) {
        unsigned x = *left++, y = *right++;
        if (x >= 'A' && x <= 'Z') {
            x += 32;
        }
        if (y >= 'A' && y <= 'Z') {
            y += 32;
        }
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (*left != *right) {
        return *left ? 1 : -1;
    }
    return strcmp(a, b);
}

typedef struct {
    int row, order;
    const char *label;
} OrderedRow;

static int positional(const void *a, const void *b);

static int alphabetical(const void *a, const void *b)
{
    const OrderedRow *left = a, *right = b;
    int result = compare_names(left->label, right->label);
    return result ? result : positional(a, b);
}

static int positional(const void *a, const void *b)
{
    const OrderedRow *left = a, *right = b;
    if (left->order != right->order) {
        return left->order < right->order ? -1 : 1;
    }
    if (left->row < 0 && right->row < 0) {
        return (left->row < right->row) - (left->row > right->row);
    }
    return (left->row > right->row) - (left->row < right->row);
}

static int collect(const MainUILibrary *library, OrderedRow *rows, bool folders, int exclude)
{
    int count = 0, limit = folders ? library->folder_count : library->count;
    for (int i = 0; i < limit; i++) {
        int row = folders ? -i - 1 : i;
        int parent = folders ? library->folders[i].parent : library->items[i].folder;
        if (row != exclude && parent == library->current) {
            rows[count++] =
                (OrderedRow){row, folders ? library->folders[i].order : library->items[i].order,
                             folders ? library->folders[i].name : library->items[i].label};
        }
    }
    qsort(rows, (size_t)count, sizeof *rows, positional);
    return count;
}

static void set_order(MainUILibrary *library, int row, int order)
{
    if (row < 0) {
        library->folders[-row - 1].order = order;
    }
    else {
        library->items[row].order = order;
    }
}

static bool reorder(MainUILibrary *library, int moving, int anchor, bool sort)
{
    OrderedRow *rows = calloc(MAINUI_LIBRARY_LIMIT + MAINUI_FOLDER_LIMIT, sizeof *rows);
    if (!rows) {
        return false;
    }
    if (sort) {
        for (int folders = 0; folders < 2; folders++) {
            int count = collect(library, rows, folders != 0, INT_MIN);
            qsort(rows, (size_t)count, sizeof *rows, alphabetical);
            for (int i = 0; i < count; i++) {
                set_order(library, rows[i].row, i);
            }
        }
    }
    else {
        bool folder = moving < 0;
        int count = collect(library, rows, folder, INT_MIN), position = -1;
        for (int i = 0; i < count; i++) {
            if (rows[i].row == anchor) {
                position = i;
            }
        }
        if (position < 0) {
            position = anchor == INT_MIN || folder ? count : 0;
        }
        count = collect(library, rows, folder, moving);
        if (position > count) {
            position = count;
        }
        for (int i = 0; i < count; i++) {
            set_order(library, rows[i].row, i < position ? i : i + 1);
        }
        set_order(library, moving, position);
        if (folder) {
            MainUILibraryFolder *source = &library->folders[-moving - 1];
            for (int i = 0; i < library->folder_count; i++) {
                if (i != -moving - 1 && library->folders[i].parent == library->current &&
                    !strcmp(library->folders[i].name, source->name)) {
                    free(rows);
                    return false;
                }
            }
            source->parent = library->current;
        }
        else {
            library->items[moving].folder = library->current;
        }
        for (int i = 0; i < library->folder_count; i++) {
            if (depth(library, i) > 3) {
                free(rows);
                return false;
            }
        }
    }
    free(rows);
    return sort || moving >= 0 || folder_paths_fit(library, -moving - 1);
}

static bool name_folder(MainUILibrary *library, int row, const char *input, bool create,
                        char **affected, unsigned serial)
{
    char name[FOLDER_NAME_LIMIT + 1];
    if (!normalize_name(input, name) || (!create && row >= 0)) {
        return false;
    }
    int index = create ? library->folder_count : -row - 1;
    if (create && (index >= MAINUI_FOLDER_LIMIT || depth(library, library->current) >= 3)) {
        return false;
    }
    int parent = create ? library->current : library->folders[index].parent;
    for (int i = 0; i < library->folder_count; i++) {
        if (i != index && library->folders[i].parent == parent &&
            !strcmp(library->folders[i].name, name)) {
            return false;
        }
    }
    if (create) {
        char id[32];
        do {
            snprintf(id, sizeof id, "f_%08x", serial++);
        } while (find_row(library, id, true, 0) != INT_MIN);
        cJSON *json = cJSON_CreateObject();
        if (!json || !put(json, "kind", cJSON_CreateString("folder")) ||
            !put(json, "id", cJSON_CreateString(id)) ||
            !put(json, "name", cJSON_CreateString(name))) {
            cJSON_Delete(json);
            return false;
        }
        int order = 0;
        for (int i = 0; i < library->folder_count; i++) {
            if (library->folders[i].parent == parent && library->folders[i].order >= order) {
                if (library->folders[i].order == INT_MAX) {
                    cJSON_Delete(json);
                    return false;
                }
                order = library->folders[i].order + 1;
            }
        }
        library->folders[index] = (MainUILibraryFolder){.json = json,
                                                        .id = string(json, "id"),
                                                        .name = string(json, "name"),
                                                        .parent = parent,
                                                        .order = order};
        library->folder_count++;
    }
    else {
        if (!put(library->folders[index].json, "name", cJSON_CreateString(name))) {
            return false;
        }
        library->folders[index].name = string(library->folders[index].json, "name");
    }
    *affected = copy(library->folders[index].id);
    return *affected != NULL && folder_paths_fit(library, index);
}

/* Removing a folder moves its subfolders up a level. Like create, rename and
 * move, that must not leave two folders of the same name side by side. */
static bool promotion_fits(const MainUILibrary *library, int index)
{
    int parent = library->folders[index].parent;
    for (int child = 0; child < library->folder_count; child++) {
        if (library->folders[child].parent != index) {
            continue;
        }
        for (int i = 0; i < library->folder_count; i++) {
            if (i != index && library->folders[i].parent == parent &&
                !strcmp(library->folders[i].name, library->folders[child].name)) {
                return false;
            }
        }
    }
    return true;
}

static bool delete_folder(MainUILibrary *library, int row)
{
    if (row == INT_MIN || row >= 0) {
        return false;
    }
    int index = -row - 1, parent = library->folders[index].parent;
    for (int i = 0; i < library->folder_count; i++) {
        int *value = &library->folders[i].parent;
        if (*value == index) {
            *value = parent;
        }
        if (*value > index) {
            (*value)--;
        }
    }
    for (int i = 0; i < library->count; i++) {
        int *value = &library->items[i].folder;
        if (*value == index) {
            *value = parent;
        }
        if (*value > index) {
            (*value)--;
        }
    }
    if (library->current > index) {
        library->current--;
    }
    cJSON_Delete(library->folders[index].json);
    memmove(&library->folders[index], &library->folders[index + 1],
            (size_t)(library->folder_count - index - 1) * sizeof library->folders[0]);
    library->folder_count--;
    return true;
}

bool mainui_favorite_edit(MainUIFavoriteEditor *editor, MainUILibrary *library, const char *sd,
                          MainUIContextAction action, const char *name, int *selected)
{
    editor->error[0] = 0;
    if (!library || library->recent || !sd) {
        return fail(editor, "No Favorite folder is open.");
    }
    int row = row_at(library, *selected);
    if (action == CONTEXT_FAVORITE_MOVE) {
        if (row == INT_MIN) {
            return fail(editor, "Select a Favorite or folder first.");
        }
        char *key = copy(row_key(library, row));
        if (!key) {
            return fail(editor, "Not enough memory.");
        }
        mainui_favorite_editor_close(editor);
        editor->key = key;
        editor->folder = row < 0;
        editor->type = row < 0 ? 0 : library->items[row].type;
        return true;
    }
    MainUIFavoriteStore store = {0};
    bool opened = mainui_favorite_store_open(&store, sd);
    cJSON *records = store.records;
    char *affected = NULL;
    MainUILibrary *fresh = calloc(1, sizeof *fresh);
    bool ok = opened && fresh && mainui_library_open(fresh, sd, false);
    if (!ok) {
        fail(editor, "Cannot read Favorites or sidecar; existing files were preserved.");
        goto cleanup;
    }
    /* Browsing already shows damaged records repaired; an edit saves that
     * view, as before, but first keeps the damaged original. */
    bool repair = !mainui_favorite_store_lossless(&store, fresh);
    if (library->current >= 0) {
        int parent = find_row(fresh, library->folders[library->current].id, true, 0);
        if (parent == INT_MIN) {
            ok = fail(editor, "The open folder changed. Reopen Favorites.");
            goto cleanup;
        }
        fresh->current = -parent - 1;
    }
    bool selected_folder = row != INT_MIN && row < 0;
    int type = row >= 0 ? library->items[row].type : 0;
    if (row != INT_MIN) {
        row = find_row(fresh, row_key(library, row), selected_folder, type);
    }
    if (action == CONTEXT_FAVORITE_CREATE || action == CONTEXT_FAVORITE_RENAME) {
        ok = (action == CONTEXT_FAVORITE_CREATE || row != INT_MIN) &&
             name_folder(fresh, row, name, action == CONTEXT_FAVORITE_CREATE, &affected,
                         (unsigned)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                             cJSON_GetArrayItem(records, 0), "generation")) +
                             1);
        selected_folder = true;
    }
    else if (action == CONTEXT_FAVORITE_DELETE) {
        if (row != INT_MIN && row < 0 && !promotion_fits(fresh, -row - 1)) {
            ok = fail(editor, "A folder inside has the same name as one beside it. "
                              "Rename it first.");
            goto cleanup;
        }
        if (row != INT_MIN && row < 0) {
            MainUILibraryFolder *deleted = &fresh->folders[-row - 1];
            cJSON *record;
            cJSON_ArrayForEach(record, records)
            {
                if (!strcmp(string(record, "kind"), "item") &&
                    !strcmp(string(record, "folder"), deleted->id) &&
                    !put(record, "folder", cJSON_CreateString(parent_id(fresh, deleted->parent)))) {
                    ok = fail(editor, "Not enough memory.");
                    goto cleanup;
                }
            }
        }
        ok = delete_folder(fresh, row);
    }
    else if (action == CONTEXT_FAVORITE_SORT) {
        ok = row >= 0 && reorder(fresh, INT_MIN, INT_MIN, true);
        /* Sorting retains the numeric selection, rather than following the ROM. */
    }
    else if (action == CONTEXT_FAVORITE_PASTE) {
        int moving =
            editor->key ? find_row(fresh, editor->key, editor->folder, editor->type) : INT_MIN;
        if (moving != INT_MIN && moving == row) {
            /* The reference treats pasting onto the cut row as cancellation,
             * without changing order, generation or any file. */
            mainui_favorite_editor_close(editor);
            ok = true;
            goto cleanup;
        }
        ok = moving != INT_MIN && reorder(fresh, moving, row, false);
        if (ok) {
            affected = copy(editor->key);
            ok = affected != NULL;
            selected_folder = editor->folder;
            type = editor->type;
        }
    }
    else {
        ok = false;
    }
    if (!ok) {
        fail(editor, "Invalid name, duplicate, missing item or folder path/depth/cycle limit.");
        goto cleanup;
    }
    if (repair && !mainui_favorite_store_keep_damaged(&store)) {
        ok = fail(editor, "Could not keep a copy of the damaged Favorite folders; nothing was "
                          "changed.");
        goto cleanup;
    }
    ok = mainui_favorite_store_commit(&store, fresh);
    if (!ok) {
        fail(editor, "Could not save Favorite folders; check file access or another writer.");
        goto cleanup;
    }
    ok = mainui_library_reload(library, sd);
    if (!ok) {
        fail(editor, "Saved successfully, but could not reload. Reopen Favorites.");
        goto cleanup;
    }
    if (affected) {
        int target = find_row(library, affected, selected_folder, type);
        for (int i = 0; i < library->visible_count; i++) {
            if (library->visible[i] == target) {
                *selected = i;
                break;
            }
        }
    }
    if (action == CONTEXT_FAVORITE_PASTE || action == CONTEXT_FAVORITE_SORT ||
        action == CONTEXT_FAVORITE_DELETE) {
        mainui_favorite_editor_close(editor);
    }
cleanup:
    if (fresh) {
        mainui_library_close(fresh);
    }
    free(fresh);
    mainui_favorite_store_close(&store);
    free(affected);
    return ok;
}
