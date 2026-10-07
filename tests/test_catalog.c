/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/catalog.h"
#include "platform/launch.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int find(MainUICatalogPage *p, const char *name)
{
    for (int i = 0; i < p->count; i++) {
        if (!strcmp(p->entries[i].label, name)) {
            return i;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    const char *sd = argv[1];
    char path[MAINUI_PATH_MAX];
    assert(mainui_catalog_path(path, "X:/SD", "X:/SD/Emu/FC", "../../Roms/FC"));
    assert(!strcmp(path, "X:/SD/Roms/FC"));
    assert(mainui_catalog_path(path, "X:/SD", "ignored", "/mnt/SDCARD/Roms/FC"));
    assert(!strcmp(path, "X:/SD/Roms/FC"));
    assert(mainui_catalog_path(path, ".", "a/b", "../../../c"));
    assert(!strcmp(path, "../c"));
    MainUICatalog *c = calloc(1, sizeof *c);
    assert(c && mainui_catalog_open(c, sd, false));
    if (argc == 3) {
        assert(c->pages[0].count == 1 && mainui_catalog_enter(c, 0));
        cJSON *xml_record = mainui_catalog_record(c, 0);
        assert(xml_record);
        bool xml = !strcmp(argv[2], "xml");
        bool absolute_rom = !strcmp(argv[2], "absolute-rom") || !strcmp(argv[2], "absolute-both");
        bool absolute_image =
            !strcmp(argv[2], "absolute-image") || !strcmp(argv[2], "absolute-both");
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(xml_record, "rompath")->valuestring,
                       xml            ? "/mnt/SDCARD/Emu/FC/../../Roms/FC/./Actual.nes"
                       : absolute_rom ? "/mnt/SDCARD/Roms/FC/Actual.nes"
                                      : "/mnt/SDCARD/Emu/FC/../../Roms/FC/Actual.nes"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(xml_record, "imgpath")->valuestring,
                       xml              ? "/mnt/SDCARD/Emu/FC/../../Roms/FC/./Other/cover.png"
                       : absolute_image ? "/mnt/SDCARD/Roms/FC/Imgs/Actual.png"
                                        : "/mnt/SDCARD/Emu/FC/../../Roms/FC/Imgs/Actual.png"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(xml_record, "launch")->valuestring,
                       "/mnt/SDCARD/Emu/FC/launch.sh"));
        char error[256];
        char *command = mainui_launch_command(xml_record, error);
        assert(
            command &&
            strstr(command, cJSON_GetObjectItemCaseSensitive(xml_record, "rompath")->valuestring));
        free(command);
        cJSON_Delete(xml_record);
        mainui_catalog_close(c);
        free(c);
        return 0;
    }
    assert(c->pages[0].count == 3);
    assert(!strcmp(c->pages[0].entries[0].label, "Game Boy"));
    int nes = find(&c->pages[0], "NES");
    c->pages[0].view.selected = nes;
    assert(nes >= 0 && mainui_catalog_enter(c, nes));
    MainUICatalogPage *p = &c->pages[c->depth];
    assert(p->count == 5); /* Two folders holding ROMs, three supported games; no art,
                              * hidden files, empty or data folders. */
    assert(p->entries[0].directory && !strcmp(p->entries[0].label, "Collections"));
    assert(!strcmp(p->entries[2].label, "Adventure"));
    assert(!strcmp(p->entries[3].label, "alpha"));
    assert(!strcmp(p->entries[4].label, "Zebra"));
    cJSON *record = mainui_catalog_record(c, 2);
    assert(record);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(record, "rompath")->valuestring,
                   "/mnt/SDCARD/Emu/FC/../../Roms/FC/Adventure.nes"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(record, "imgpath")->valuestring,
                   "/mnt/SDCARD/Emu/FC/../../Roms/FC/Imgs/Adventure.png"));
    cJSON_Delete(record);
    /* Search rows carry their launcher and retain the existing record form. */
    MainUIEntry *search = &p->entries[2];
    char *original_path = search->path;
    search->path = "/mnt/SDCARD/Emu/FC/launch.sh:/mnt/SDCARD/Roms/FC/Adventure.nes";
    search->launch = "/mnt/SDCARD/Emu/FC/launch.sh";
    record = mainui_catalog_record(c, 2);
    assert(record &&
           !strcmp(cJSON_GetObjectItemCaseSensitive(record, "rompath")->valuestring, search->path));
    cJSON_Delete(record);
    search->path = original_path;
    search->launch = NULL;
    assert(!mainui_catalog_enter(c, 2) && c->depth == 1);
    p->view.selected = 4;
    assert(mainui_catalog_enter(c, 0));
    assert(c->pages[2].count == 2);
    assert(mainui_catalog_enter(c, 0));
    assert(c->depth == 3 && c->pages[3].count == 1);
    assert(!strcmp(c->pages[3].entries[0].label, "Deep"));
    record = mainui_catalog_record(c, 0);
    assert(record);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(record, "rompath")->valuestring,
                   "/mnt/SDCARD/Emu/FC/../../Roms/FC/Collections/More/Deep.nes"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(record, "imgpath")->valuestring,
                   "/mnt/SDCARD/Emu/FC/../../Roms/FC/Imgs/Deep.png"));
    cJSON_Delete(record);
    assert(mainui_catalog_back(c) && mainui_catalog_back(c));
    assert(c->pages[1].view.selected == 4);
    assert(find(&c->pages[1], "Empty") < 0 && find(&c->pages[1], "Data") < 0);
    assert(mainui_catalog_enter(c, find(&c->pages[1], "Sets")));
    assert(c->pages[2].count == 1 && c->pages[2].entries[0].directory &&
           !strcmp(c->pages[2].entries[0].label, "Inner"));
    assert(mainui_catalog_back(c));
    /* Failed scans retain the parent and don't publish a partial child page. */
    char *saved = c->pages[1].entries[0].path;
    char *saved_key = c->pages[1].entries[0].cache_key;
    c->pages[1].entries[0].cache_key = NULL;
    c->pages[1].entries[0].path = "tests/fixtures/no-such-folder";
    assert(!mainui_catalog_enter(c, 0) && c->depth == 1 && c->pages[2].count == 0);
    c->pages[1].entries[0].path = saved;
    c->pages[1].entries[0].cache_key = saved_key;
    assert(mainui_catalog_back(c) && !mainui_catalog_back(c));
    mainui_catalog_close(c);
    memset(c, 0, sizeof *c);
    assert(mainui_catalog_open(c, sd, true));
    assert(mainui_catalog_enter(c, find(&c->pages[0], "NES")));
    assert(!strcmp(c->pages[1].entries[3].label, "Zebra"));
    mainui_catalog_close(c);
    free(c);
    puts("Catalog tests passed: discovery, filtering, sorting, paths, nested/empty folders, "
         "failure recovery");
    return 0;
}
