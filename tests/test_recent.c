/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/session.h"
#include "catalog/favorite_edit.h"
#include "catalog/library.h"
#include "catalog/saved_actions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Select row `index` of Recents or Favorites, snapshot it as a launch or a
 * background reload does, restore, and print the restored selection. */
static int restore(const char *sd, bool recent, int index)
{
    MainUILibrary *library = calloc(1, sizeof *library);
    if (!library || !mainui_library_open(library, sd, recent) || index < 0 ||
        index >= library->visible_count) {
        free(library);
        return 2;
    }
    MainUIViewport view, home = {4, 0, 0, 3};
    mainui_viewport_restore(&view, library->visible_count, 6, index, 0, 5);
    MainUIStack legacy;
    MainUIMenuSection section = recent ? MAINUI_MENU_RECENTS : MAINUI_MENU_FAVORITES;
    cJSON *resume = mainui_session_snapshot(section, NULL, library, &view, &home, &legacy);
    int item = library->visible[index];
    const cJSON *record = item >= 0 ? library->items[item].json : NULL;
    MainUISession session = {0};
    bool ok = resume && mainui_session_restore(&session, sd, false, 6, resume, record);
    if (ok) {
        printf("%d\n", session.view.selected);
    }
    mainui_session_close(&session);
    cJSON_Delete(resume);
    mainui_library_close(library);
    free(library);
    return ok ? 0 : 1;
}

/* fixture-recent SD RECORD: add a committed launch.
 * fixture-recent SD remove INDEX: remove the INDEXth row Recents shows.
 * fixture-recent SD remove-favorite LABEL: remove the Favorite with that label.
 * fixture-recent SD restore recents|favorites INDEX: see restore(). */
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[2], "restore")) {
        return restore(argv[1], !strcmp(argv[3], "recents"), atoi(argv[4]));
    }
    if (argc == 4 && !strcmp(argv[2], "remove-favorite")) {
        MainUILibrary *library = calloc(1, sizeof *library);
        if (!library) {
            return 2;
        }
        /* Any loaded Favorite, in a folder or not, chosen by its label. */
        bool ok = mainui_library_open(library, argv[1], false);
        int found = -1;
        for (int i = 0; ok && i < library->count; i++) {
            if (!strcmp(library->items[i].label, argv[3])) {
                found = i;
            }
        }
        ok = ok && found >= 0 && mainui_favorite_remove(argv[1], library->items[found].json);
        mainui_library_close(library);
        free(library);
        return ok ? 0 : 1;
    }
    if (argc == 4 && !strcmp(argv[2], "remove")) {
        MainUILibrary *library = calloc(1, sizeof *library);
        if (!library) {
            return 2;
        }
        int index = atoi(argv[3]);
        bool ok = mainui_library_open(library, argv[1], true) && index >= 0 &&
                  index < library->visible_count &&
                  mainui_saved_action(argv[1], true, SAVED_REMOVE,
                                      library->items[library->visible[index]].json);
        mainui_library_close(library);
        free(library);
        return ok ? 0 : 1;
    }
    if (argc != 3) {
        return 2;
    }
    cJSON *record = cJSON_Parse(argv[2]);
    if (!record) {
        return 2;
    }
    bool ok = mainui_recent_add(argv[1], record);
    cJSON_Delete(record);
    return ok ? 0 : 1;
}
