/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/library.h"
#include "catalog/saved_actions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fixture-recent SD RECORD: add a committed launch.
 * fixture-recent SD remove INDEX: remove the INDEXth row Recents shows. */
int main(int argc, char **argv)
{
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
