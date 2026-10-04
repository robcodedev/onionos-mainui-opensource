/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/cache.h"
#include "catalog/catalog.h"
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
    MainUICatalog *c = calloc(1, sizeof *c);
    assert(c && mainui_catalog_open(c, argv[1], false));
    if (!strcmp(argv[2], "rebuild")) {
        bool ok = c->pages[0].count == 1 && mainui_catalog_build_cache(c, 0, true);
        mainui_catalog_close(c);
        free(c);
        return ok ? 0 : 4;
    }
    assert(c->pages[0].count == 1 && mainui_catalog_enter(c, 0));
    MainUICatalogPage *p = &c->pages[1];
    if (!strcmp(argv[2], "fallback")) {
        assert(!p->cache && p->cache_fallback && p->count == 1);
        assert(!strcmp(mainui_catalog_entry(c, 0)->label, "fallback"));
    }
    else if (!strcmp(argv[2], "repaired")) {
        assert(p->cache && !p->cache_fallback && p->count == 1);
        assert(!strcmp(mainui_catalog_entry(c, 0)->label, "fallback"));
        assert(mainui_catalog_back(c) && mainui_catalog_enter(c, 0));
        assert(c->pages[1].cache && !c->pages[1].cache_fallback);
    }
    else if (!strcmp(argv[2], "scan-once")) {
        assert(p->cache && !strcmp(mainui_catalog_entry(c, 0)->label, "Cached"));
        assert(mainui_catalog_back(c) && mainui_catalog_scan_only(c->pages[0].entries[0].path));
        assert(mainui_catalog_enter(c, 0) && !p->cache && p->cache_fallback);
        assert(!strcmp(mainui_catalog_entry(c, 0)->label, "fallback"));
        assert(mainui_catalog_back(c) && mainui_catalog_enter(c, 0));
        assert(p->cache && !p->cache_fallback);
        assert(!strcmp(mainui_catalog_entry(c, 0)->label, "Cached"));
    }
    else if (!strcmp(argv[2], "empty")) {
        assert(p->cache && !p->count && !p->loaded);
        assert(!mainui_catalog_entry(c, 0));
    }
    else if (!strcmp(argv[2], "large")) {
        assert(p->cache && p->count == 13002 && p->loaded == 64);
        assert(p->folder_count == 2);
        int positions[] = {2, 63, 64, 127, 5000, 13001, 700, 0};
        for (unsigned i = 0; i < sizeof positions / sizeof *positions; i++) {
            int index = positions[i];
            MainUIEntry *e = mainui_catalog_entry(c, index);
            assert(e && p->loaded <= 64 && index >= p->offset && index < p->offset + p->loaded);
            char expected[64];
            if (index >= 2) {
                snprintf(expected, sizeof expected, "Game %05d", index - 2);
                assert(!strcmp(e->label, expected));
            }
        }
        p->view.selected = 5000;
        assert(mainui_catalog_enter(c, 0));
        assert(c->pages[2].cache && c->pages[2].count == 1);
        MainUIEntry *e = mainui_catalog_entry(c, 0);
        assert(!strcmp(e->label, "A proper cached title"));
        assert(strstr(e->path, "/Roms/Odd'System/Sets/O'Brien/raw.nes"));
        assert(strstr(e->artwork, "/Roms/Odd'System/Imgs/cover.png"));
        assert(!mainui_catalog_enter(c, 0));
        assert(mainui_catalog_back(c) && p->view.selected == 5000);
        assert(mainui_catalog_enter(c, 1) && !c->pages[2].count);
        assert(mainui_catalog_back(c));
        /* Quoted SQL identifiers and parent strings are data, not SQL syntax. */
        MainUICache *extra = NULL;
        int total = 0, loaded = 0;
        assert(mainui_cache_open(&extra, p->cache_file, "quoted\"table", ".", c->sd, p->path, false,
                                 &total));
        MainUIEntry rows[MAINUI_CACHE_WINDOW];
        assert(total == 1 && mainui_cache_window(extra, 0, rows, &loaded) && loaded == 1);
        assert(!strcmp(rows[0].label, "Quoted identifier"));
        mainui_entry_close(&rows[0]);
        mainui_cache_close(extra);
    }
    else if (!strcmp(argv[2], "bad-later")) {
        assert(p->cache && p->count == 70);
        assert(!mainui_catalog_entry(c, 69));
        assert(*c->error && p->loaded == 64 && p->offset == 0);
        assert(mainui_catalog_entry(c, 0));
    }
    else {
        assert(!strcmp(argv[2], "sort") && p->cache && p->count == 4);
        assert(!strcmp(mainui_catalog_entry(c, 0)->label, "alpha"));
        assert(!strcmp(mainui_catalog_entry(c, 1)->label, "ALPHA"));
        assert(!strcmp(mainui_catalog_entry(c, 2)->label, "beta"));
        assert(!strcmp(mainui_catalog_entry(c, 3)->label, "Zebra"));
        mainui_catalog_close(c);
        memset(c, 0, sizeof *c);
        assert(mainui_catalog_open(c, argv[1], true) && mainui_catalog_enter(c, 0));
        assert(!strcmp(mainui_catalog_entry(c, 0)->label, "ALPHA"));
        assert(!strcmp(mainui_catalog_entry(c, 1)->label, "Zebra"));
    }
    mainui_catalog_close(c);
    free(c);
    puts("Cache scenario passed");
    return 0;
}
