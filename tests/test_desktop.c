/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/catalog.h"
#include "platform/files.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    assert(argc == 3);
    MainUICatalog *apps = calloc(1, sizeof *apps);
    assert(apps && mainui_catalog_apps(apps, argv[1], false));
    assert(apps->pages[0].count == 2);
    /* Apps keep the card's folder order, which a host disk does not fix:
     * find the app by its label. */
    MainUIEntry *first = mainui_catalog_entry(apps, 0);
    if (strcmp(first->label, "Alpha app")) {
        first = mainui_catalog_entry(apps, 1);
    }
    assert(!strcmp(first->label, "Alpha app") && !first->directory);
    assert(strstr(first->path, "/App/alpha/launch.sh"));
    assert(strstr(first->icon, "/Icons/Default/app/pacman.png"));
    mainui_catalog_close(apps);
    /* .appsort in the config folder sorts the Apps by name. */
    char flag[4096];
    snprintf(flag, sizeof flag, "%s/.appsort", argv[2]);
    FILE *file = fopen(flag, "w");
    assert(file && fclose(file) == 0);
    memset(apps, 0, sizeof *apps);
    assert(mainui_catalog_apps(apps, argv[1], false));
    assert(!strcmp(mainui_catalog_entry(apps, 0)->label, "Alpha app"));
    assert(!strcmp(mainui_catalog_entry(apps, 1)->label, "Zulu app"));
    mainui_catalog_close(apps);
    assert(remove(flag) == 0);
    free(apps);
    puts("App discovery checks passed");
    return 0;
}
