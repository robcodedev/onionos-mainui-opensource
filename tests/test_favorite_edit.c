/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/favorite_edit.h"
#include "catalog/favorite_store.h"
#include "platform/files.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int selected(MainUILibrary *library, const char *name)
{
    for (int i = 0; i < library->visible_count; i++) {
        if (!strcmp(mainui_library_label(library, i), name)) {
            return i;
        }
    }
    assert(false);
    return -1;
}

/* Reopen from published bytes, discarding every in-memory folder/order value. */
static void reopen(MainUILibrary *library, const char *sd)
{
    mainui_library_close(library);
    assert(mainui_library_open(library, sd, false));
}

/* fixture-favorite_edit SD create NAME: one Create at the root; 0 if saved. */
static int create_once(const char *sd, const char *name)
{
    MainUILibrary *library = calloc(1, sizeof *library);
    MainUIFavoriteEditor editor = {0};
    assert(library && mainui_library_open(library, sd, false));
    int row = 0;
    bool ok = mainui_favorite_edit(&editor, library, sd, CONTEXT_FAVORITE_CREATE, name, &row);
    if (!ok) {
        puts(editor.error);
    }
    mainui_favorite_editor_close(&editor);
    mainui_library_close(library);
    free(library);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[2], "create")) {
        return create_once(argv[1], argv[3]);
    }
    assert(argc == 2);
    MainUILibrary *library = calloc(1, sizeof *library);
    MainUIFavoriteEditor editor = {0};
    assert(library && mainui_library_open(library, argv[1], false));
    int row = selected(library, "Zebra");
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_CREATE, " > .. New ",
                                &row));
    assert(!strcmp(mainui_library_label(library, row), "New"));
    char id[128];
    strcpy(id, library->folders[-library->visible[row] - 1].id);
    assert(
        mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_RENAME, "Renamed", &row));
    assert(!strcmp(id, library->folders[-library->visible[row] - 1].id));
    int old_row = row;
    char path[4096];
    snprintf(path, sizeof path, "%s/Roms/favourite-folders.json", argv[1]);
    char *before = mainui_read_text(path, 8 * 1024 * 1024);
    assert(!mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_RENAME, "Container",
                                 &row));
    assert(row == old_row);
    char *after = mainui_read_text(path, 8 * 1024 * 1024);
    assert(before && after && !strcmp(before, after));
    free(before);
    free(after);
    row = selected(library, "Zebra");
    old_row = row;
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_SORT, NULL, &row));
    assert(row == old_row);
    assert(strcmp(mainui_library_label(library, row), "Zebra"));
    reopen(library, argv[1]);
    assert(!strcmp(mainui_library_label(library, library->leading_folders), "Apple"));
    row = selected(library, "Zebra");
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_MOVE, NULL, &row));
    assert(mainui_favorite_is_cut(&editor, library, row));
    assert(mainui_library_enter(library, selected(library, "Container")));
    row = selected(library, "Nested");
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_PASTE, NULL, &row));
    assert(!editor.key && !strcmp(mainui_library_label(library, row), "Zebra"));
    assert(library->items[library->visible[row]].folder == library->current);
    reopen(library, argv[1]);
    assert(mainui_library_enter(library, selected(library, "Container")));
    row = selected(library, "Zebra");
    assert(library->items[library->visible[row]].folder == library->current);
    assert(mainui_library_back(library));
    row = selected(library, "Container");
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_MOVE, NULL, &row));
    assert(mainui_library_enter(library, row));
    assert(mainui_library_enter(library, selected(library, "Nested")));
    row = 0;
    before = mainui_read_text(path, 8 * 1024 * 1024);
    assert(!mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_PASTE, NULL, &row));
    after = mainui_read_text(path, 8 * 1024 * 1024);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    assert(mainui_library_back(library) && mainui_library_back(library));
    row = selected(library, "Container");
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_DELETE, NULL, &row));
    reopen(library, argv[1]);
    for (int i = 0; i < library->visible_count; i++) {
        assert(strcmp(mainui_library_label(library, i), "Container"));
    }
    selected(library, "Nested");
    selected(library, "Zebra");
    selected(library, "Inside");
    assert(library->count == 4);
    /* Same-row paste cancels without even advancing the sidecar generation. */
    row = selected(library, "Apple");
    before = mainui_read_text(path, 8 * 1024 * 1024);
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_MOVE, NULL, &row));
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_PASTE, NULL, &row));
    after = mainui_read_text(path, 8 * 1024 * 1024);
    assert(!editor.key && !strcmp(before, after));
    free(before);
    free(after);

    /* Moving down must use the original target ordinal, after removing source. */
    row = selected(library, "Apple");
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_MOVE, NULL, &row));
    row = library->visible_count - 1;
    int destination = row;
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_PASTE, NULL, &row));
    assert(row == destination && !strcmp(mainui_library_label(library, row), "Apple"));
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_MOVE, NULL, &row));
    row = library->leading_folders;
    destination = row;
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_PASTE, NULL, &row));
    assert(row == destination && !strcmp(mainui_library_label(library, row), "Apple"));

    assert(mainui_library_enter(library, selected(library, "Nested")));
    row = 0;
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_CREATE, "Level two",
                                &row));
    assert(mainui_library_enter(library, row));
    row = 0;
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_CREATE, "Level three",
                                &row));
    assert(mainui_library_enter(library, row));
    row = 0;
    before = mainui_read_text(path, 8 * 1024 * 1024);
    assert(!mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_CREATE, "Too deep",
                                 &row));
    after = mainui_read_text(path, 8 * 1024 * 1024);
    assert(!strcmp(before, after));
    free(before);
    free(after);

    /* Each component is legal, but separators count toward the full path cap. */
    while (mainui_library_back(library)) {
    }
    char long_name[128];
    memset(long_name, 'L', 127);
    long_name[127] = 0;
    row = 0;
    assert(
        mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_CREATE, long_name, &row));
    assert(mainui_library_enter(library, row));
    row = 0;
    before = mainui_read_text(path, 8 * 1024 * 1024);
    assert(
        !mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_CREATE, long_name, &row));
    after = mainui_read_text(path, 8 * 1024 * 1024);
    assert(before && after && !strcmp(before, after));
    free(before);
    free(after);

    /* An over-long path already in the sidecar (older build or hand edit)
     * must not block unrelated edits, only edits to that subtree. */
    char long_id[128] = "";
    for (int i = 0; i < library->folder_count; i++) {
        if (!strcmp(library->folders[i].name, long_name)) {
            strcpy(long_id, library->folders[i].id);
        }
    }
    assert(*long_id);
    FILE *legacy = fopen(path, "ab");
    assert(legacy);
    fprintf(legacy, "{\"kind\":\"folder\",\"id\":\"legacy\",\"parent\":\"%s\",\"name\":\"",
            long_id);
    for (int i = 0; i < 127; i++) {
        fputc('M', legacy);
    }
    fputs("\",\"order\":0}\n", legacy);
    assert(fclose(legacy) == 0);
    reopen(library, argv[1]);
    row = 0;
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_CREATE, "Unrelated",
                                &row));
    row = -1;
    for (int i = 0; i < library->visible_count; i++) {
        if (!strcmp(mainui_library_label(library, i), long_name)) {
            row = i;
        }
    }
    assert(row >= 0);
    int long_row = row;
    char other_long[128];
    memset(other_long, 'K', 127);
    other_long[127] = 0;
    assert(!mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_RENAME, other_long,
                                 &row));
    row = long_row;
    assert(mainui_favorite_edit(&editor, library, argv[1], CONTEXT_FAVORITE_RENAME, "Short", &row));

    /* A transaction cannot overwrite a sidecar changed since it was opened. */
    MainUIFavoriteStore transaction = {0};
    assert(mainui_favorite_store_open(&transaction, argv[1]));
    FILE *external = fopen(path, "ab");
    assert(external && fputs("\n", external) >= 0 && fclose(external) == 0);
    before = mainui_read_text(path, 8 * 1024 * 1024);
    assert(!mainui_favorite_store_commit(&transaction, library));
    after = mainui_read_text(path, 8 * 1024 * 1024);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    mainui_favorite_store_close(&transaction);
    mainui_favorite_editor_close(&editor);
    mainui_library_close(library);
    free(library);
    puts("Favorite create/rename/sort/move/delete, normalization, stable IDs and cycle "
         "preservation passed");
    return 0;
}
