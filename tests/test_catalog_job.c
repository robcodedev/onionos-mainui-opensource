/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/catalog_job.h"
#include "app/details.h"
#include "catalog/cache.h"
#include "sqlite3.h"
#include "ui/preview.h"
#ifdef main
#undef main
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static MainUIJobResult wait_job(MainUICatalogJob *job, uint64_t generation, MainUISession *out)
{
    MainUISearch search = {0};
    char error[256] = "";
    Uint32 started = SDL_GetTicks();
    MainUIJobResult result;
    while ((result = mainui_catalog_job_take(job, generation, out, &search, error)) ==
           JOB_WAITING) {
        assert(SDL_GetTicks() - started < 10000);
        SDL_Delay(1);
    }
    mainui_search_close(&search);
    if (result == JOB_FAILED) {
        fprintf(stderr, "%s\n", error);
    }
    return result;
}

static unsigned hash_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    unsigned hash = 2166136261u;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        hash = (hash ^ (unsigned char)ch) * 16777619u;
    }
    fclose(file);
    return hash;
}

static bool cancel_after(void *context)
{
    return ++*(int *)context > 40;
}

/* Large covers are scaled by the decode worker, not when the UI thread reaps them (#8). */
static void cover_scaled_in_worker(const char *sd)
{
    MainUIPreview *preview = calloc(1, sizeof *preview);
    MainUICatalog *catalog = calloc(1, sizeof *catalog);
    MainUIEntry entry = {0};
    char path[4096];
    assert(preview && catalog);
    snprintf(catalog->sd, sizeof catalog->sd, "%s", sd);
    snprintf(path, sizeof path, "%s/large-cover.png", sd);
    SDL_Surface *picture =
        SDL_CreateRGBSurface(SDL_SWSURFACE, 500, 720, 32, 0xff0000, 0xff00, 0xff, 0);
    assert(picture && SDL_SaveBMP(picture, path) == 0);
    SDL_FreeSurface(picture);
    catalog->depth = 1;
    catalog->pages[1].entries = &entry;
    catalog->pages[1].count = 1;
    entry.artwork = entry.path = entry.label = path;
    mainui_preview_request(preview, catalog, NULL, 0, false);
    assert(preview->thread);
    Uint32 started = SDL_GetTicks();
    while (!atomic_load_explicit(&preview->done, memory_order_acquire)) {
        assert(SDL_GetTicks() - started < 5000);
        SDL_Delay(1);
    }
    assert(preview->decoded && preview->decoded->w == 250 && preview->decoded->h == 360);
    mainui_preview_request(preview, catalog, NULL, 0, false);
    assert(!preview->thread && preview->image && preview->image->w == 250);
    mainui_preview_close(preview);
    free(preview);
    free(catalog);
    remove(path);
}

static void thumbnail_cache(const char *sd)
{
    enum {
        CACHE_ITEMS = MAINUI_THUMBNAIL_CACHE_SIZE,
        EXTRA_SECOND = CACHE_ITEMS + 1,
        ITEM_COUNT = CACHE_ITEMS + 2
    };

    MainUIPreview *preview = calloc(1, sizeof *preview);
    MainUICatalog *catalog = calloc(1, sizeof *catalog);
    MainUIEntry entries[ITEM_COUNT] = {0};
    char paths[ITEM_COUNT][4096];
    assert(preview && catalog);
    snprintf(catalog->sd, sizeof catalog->sd, "%s", sd);
    catalog->depth = 1;
    catalog->pages[1].entries = entries;
    catalog->pages[1].count = ITEM_COUNT;
    SDL_Surface *picture = SDL_CreateRGBSurface(SDL_SWSURFACE, 4, 4, 32, 0xff0000, 0xff00, 0xff, 0);
    assert(picture);
    for (int i = 0; i < ITEM_COUNT; ++i) {
        snprintf(paths[i], sizeof paths[i], "%s/thumb%d.png", sd, i);
        SDL_FillRect(picture, NULL, SDL_MapRGB(picture->format, (Uint8)i, 0, 0));
        assert(SDL_SaveBMP(picture, paths[i]) == 0);
        entries[i].artwork = paths[i];
        entries[i].path = paths[i];
        entries[i].label = paths[i];
    }
    SDL_FreeSurface(picture);
    Uint32 deadline = SDL_GetTicks() + 3000;
    do {
        mainui_preview_request(preview, catalog, NULL, 10, false);
        SDL_Delay(1);
    } while ((preview->thread || preview->prefetch_next < 4) &&
             (Sint32)(deadline - SDL_GetTicks()) > 0);
    assert(!preview->thread && preview->prefetch_next == 4 && preview->image);
    for (int neighbor = 8; neighbor <= 12; ++neighbor) {
        bool found = false;
        char normalized[4096];
        assert(mainui_catalog_path(normalized, sd, sd, paths[neighbor]));
        for (int slot = 0; slot < MAINUI_THUMBNAIL_CACHE_SIZE; ++slot) {
            found |=
                !strcmp(preview->cache[slot].key, normalized) && preview->cache[slot].image != NULL;
        }
        assert(found);
    }
    Uint8 selected_red, selected_green, selected_blue;
    SDL_GetRGB(*(Uint32 *)preview->image->pixels, preview->image->format, &selected_red,
               &selected_green, &selected_blue);
    assert(selected_red == 10); /* Prefetch never replaces selected artwork. */
    mainui_preview_close(preview);
    /* A ROM subfolder's parent row maps to -1, but its first games should
     * already be cached before the cursor moves down from "..". */
    catalog->depth = 2;
    catalog->pages[2].entries = entries;
    catalog->pages[2].count = 2;
    int parent = mainui_browser_index(catalog, 0);
    assert(parent == -1);
    deadline = SDL_GetTicks() + 3000;
    do {
        mainui_preview_request(preview, catalog, NULL, parent, false);
        SDL_Delay(1);
    } while ((preview->thread || preview->prefetch_next < 4) &&
             (Sint32)(deadline - SDL_GetTicks()) > 0);
    assert(!preview->thread && preview->prefetch_next == 4 && !preview->image);
    SDL_Surface *first = NULL;
    for (int neighbor = 0; neighbor < 2; ++neighbor) {
        SDL_Surface *cached = NULL;
        char normalized[4096];
        assert(mainui_catalog_path(normalized, sd, sd, paths[neighbor]));
        for (int slot = 0; slot < MAINUI_THUMBNAIL_CACHE_SIZE; ++slot) {
            if (!strcmp(preview->cache[slot].key, normalized)) {
                cached = preview->cache[slot].image;
            }
        }
        assert(cached);
        if (!neighbor) {
            first = cached;
        }
    }
    mainui_preview_request(preview, catalog, NULL, mainui_browser_index(catalog, 1), false);
    assert(preview->image == first && !preview->pending && !preview->thread);
    mainui_preview_close(preview);
    catalog->pages[2].count = 0;
    mainui_preview_request(preview, catalog, NULL, parent, false);
    assert(!preview->image && !preview->pending && !preview->thread);
    mainui_preview_close(preview);
    catalog->pages[2] = (MainUICatalogPage){0};
    catalog->depth = 1;
    MainUIDetails details = {0};
    for (int i = 0; i < CACHE_ITEMS; ++i) {
        /* Opening never waits for a cover; the frame's progress step does,
         * in deterministic mode as snapshots use it. */
        assert(mainui_details_open(&details, catalog, NULL, NULL, i, preview));
        if (!i) { /* nothing cached after close: the decode runs on its own */
            assert(!preview->image && preview->thread && preview->pending);
        }
        mainui_details_progress(&details, MAINUI_PREVIEW_WAIT_FOREVER);
        assert(details.preview == preview);
        mainui_details_close(&details);
        assert(preview->image);
        assert(remove(paths[i]) == 0);
    }
    /* Enter from the list cache, navigate in details, then press B. All source
     * images are gone, so a separate cache or a reload would lose the cover. */
    assert(mainui_details_open(&details, catalog, NULL, NULL, 0, preview));
    assert(preview->image); /* cached: shown at once, without waiting */
    MainUIViewport detail_view = {.total = ITEM_COUNT, .selected = 0};
    mainui_details_key(&details, catalog, NULL, NULL, &detail_view, 6, SDLK_DOWN);
    mainui_details_progress(&details, MAINUI_PREVIEW_WAIT_FOREVER);
    assert(detail_view.selected == 1 && details.preview == preview && preview->image);
    SDL_Surface *detail_image = preview->image;
    mainui_details_key(&details, catalog, NULL, NULL, &detail_view, 6, SDLK_ESCAPE);
    assert(!details.open && preview->image == detail_image);
    mainui_preview_request(preview, catalog, NULL, detail_view.selected, false);
    assert(preview->image == detail_image && !preview->pending && !preview->thread);
    /* All cache-capacity decoded images must survive after their source files vanish. */
    for (int i = 0; i < CACHE_ITEMS; ++i) {
        mainui_preview_update(preview, catalog, NULL, i);
        assert(preview->image);
        Uint8 red, green, blue;
        SDL_GetRGB(*(Uint32 *)preview->image->pixels, preview->image->format, &red, &green, &blue);
        assert(red == i && green == 0 && blue == 0);
    }
    mainui_preview_update(preview, catalog, NULL, CACHE_ITEMS);
    mainui_preview_update(preview, catalog, NULL, EXTRA_SECOND);
    mainui_preview_update(preview, catalog, NULL, 0);
    assert(!preview->image); /* Least-recently-used image was evicted. */
    mainui_preview_close(preview);
    mainui_preview_request(preview, catalog, NULL, CACHE_ITEMS, false);
    assert(preview->pending && preview->thread); /* No deferred decode delay. */
    /* A finished old decode cannot resurrect a cover after selecting a folder. */
    entries[EXTRA_SECOND].directory = true;
    mainui_preview_request(preview, catalog, NULL, EXTRA_SECOND, true);
    assert(!preview->image && !preview->pending);
    mainui_preview_update(preview, catalog, NULL, CACHE_ITEMS);
    assert(preview->image);
    mainui_preview_close(preview);
    remove(paths[CACHE_ITEMS]);
    remove(paths[EXTRA_SECOND]);
    /* A fast cover is part of the first frame within the budget. */
    entries[EXTRA_SECOND].directory = false;
    SDL_Surface *quick = SDL_CreateRGBSurface(SDL_SWSURFACE, 4, 4, 32, 0xff0000, 0xff00, 0xff, 0);
    assert(quick && SDL_SaveBMP(quick, paths[EXTRA_SECOND]) == 0);
    SDL_FreeSurface(quick);
    mainui_preview_request_within(preview, catalog, NULL, EXTRA_SECOND, 80);
    assert(preview->image && !preview->pending);
    mainui_preview_close(preview);
    remove(paths[EXTRA_SECOND]);
    /* A cover that never arrives cannot stall the first frame: a FIFO blocks
     * the decoder's open until a writer appears. */
    char fifo[4096];
    snprintf(fifo, sizeof fifo, "%s/slow.png", sd);
    remove(fifo);
    assert(mkfifo(fifo, 0600) == 0);
    entries[0].artwork = entries[0].path = entries[0].label = fifo;
    Uint32 started = SDL_GetTicks();
    mainui_preview_request_within(preview, catalog, NULL, 0, 80);
    Uint32 waited = SDL_GetTicks() - started;
    assert(waited >= 80 && waited < 1000);
    assert(preview->thread && preview->pending && !preview->image);
    int writer = open(fifo, O_WRONLY);
    assert(writer >= 0);
    close(writer);
    Uint32 until = SDL_GetTicks() + 3000;
    while (preview->thread && (Sint32)(until - SDL_GetTicks()) > 0) {
        mainui_preview_request_within(preview, catalog, NULL, 0, 0);
        SDL_Delay(1);
    }
    assert(!preview->thread || preview->prefetch_next <= 4);
    assert(!preview->pending && !preview->image); /* Unreadable: no cover. */
    mainui_preview_close(preview);
    remove(fifo);
    free(preview);
    free(catalog);
}

/* Refresh and repair touch only the console they were started for: when it
 * cannot be found again (config gone, or one read error), restoration falls
 * back to the grid, where console B is selected now, and B's cache, its
 * unfinished deletion and its ROMs stay as they are (rebuild review R1). */
static int target(const char *sd)
{
    char config[4096], moved[4096], cache[4096], journal[4096], staged[4096], control[4096];
    snprintf(config, sizeof config, "%s/Emu/A/config.json", sd);
    snprintf(moved, sizeof moved, "%s/Emu/A/config.json.away", sd);
    snprintf(cache, sizeof cache, "%s/Roms/B/B_cache6.db", sd);
    snprintf(journal, sizeof journal, "%s/Roms/B/B_cache6.db.delete.json", sd);
    snprintf(staged, sizeof staged, "%s/Roms/B/b0.nes.mainui-delete", sd);
    snprintf(control, sizeof control, "%s/fault", sd);
    MainUICatalog *catalog = calloc(1, sizeof *catalog);
    assert(catalog && mainui_catalog_open(catalog, sd, false) && catalog->pages[0].count == 2);
    MainUIViewport view, home = {1, 0, 0, 0};
    MainUICatalogJob job = {0};
    MainUISession b = {0}, a = {0}, result = {0};
    uint64_t generation = 1;
    /* Build B's cache by opening it, then leave a deletion it cannot recover. */
    mainui_grid_restore(&view, 2, 1, 4, 2);
    MainUILaunchSource source = {
        .section = MAINUI_MENU_GAMES, .catalog = catalog, .view = &view, .home = &home};
    assert(mainui_catalog_job_start(&job, JOB_ENTER, &source, sd, false, 6, NULL, generation));
    assert(wait_job(&job, generation++, &b) == JOB_READY && b.catalog->depth == 1);
    mainui_session_close(&b);
    assert(mainui_write_text_atomic(journal, "{"));
    assert(mainui_write_text_atomic(staged, "rom"));
    unsigned before = hash_file(cache);
    mainui_grid_restore(&view, 2, 0, 4, 2);
    assert(mainui_catalog_job_start(&job, JOB_ENTER, &source, sd, false, 6, NULL, generation));
    assert(wait_job(&job, generation++, &a) == JOB_READY && a.catalog->depth == 1);
    for (int fault = 0; fault < 4; fault++) {
        /* 0-1: A's config gone; 2: one read error on it; 3: gone, refresh
         * asked for on the grid with A selected. */
        if (fault < 2 || fault == 3) {
            assert(rename(config, moved) == 0);
        }
        else {
            FILE *file = fopen(control, "w");
            assert(file && fputs("Emu/A/config.json eio", file) >= 0 && !fclose(file));
        }
        MainUILaunchSource from = {.section = MAINUI_MENU_GAMES,
                                   .catalog = fault == 3 ? catalog : a.catalog,
                                   .view = fault == 3 ? &view : &a.view,
                                   .home = &home};
        MainUIJobKind kind = fault == 1 || fault == 3 ? JOB_REFRESH_SYSTEM : JOB_REPAIR_SYSTEM;
        assert(mainui_catalog_job_start(&job, kind, &from, sd, false, 6, NULL, generation));
        assert(wait_job(&job, generation++, &result) == JOB_FAILED && !result.catalog);
        assert(hash_file(cache) == before && mainui_file_stamp(journal).exists &&
               mainui_file_stamp(staged).exists);
        if (fault == 2) {
            assert(!mainui_file_stamp(control).exists); /* the read error happened */
        }
        else {
            assert(rename(moved, config) == 0);
        }
    }
    /* With A found again, its repair works and B is still untouched. */
    MainUILaunchSource from = {
        .section = MAINUI_MENU_GAMES, .catalog = a.catalog, .view = &a.view, .home = &home};
    assert(
        mainui_catalog_job_start(&job, JOB_REPAIR_SYSTEM, &from, sd, false, 6, NULL, generation));
    assert(wait_job(&job, generation, &result) == JOB_READY && result.catalog->depth == 1);
    assert(hash_file(cache) == before && mainui_file_stamp(journal).exists);
    mainui_session_close(&result);
    mainui_session_close(&a);
    mainui_catalog_close(catalog);
    free(catalog);
    SDL_Quit();
    puts("Refresh and repair leave other consoles alone when theirs is gone");
    return 0;
}

int main(int argc, char **argv)
{
    assert((argc == 2 || argc == 3) && SDL_Init(SDL_INIT_TIMER) == 0);
    const char *sd = argv[1];
    if (argc == 3 && !strcmp(argv[2], "target")) {
        return target(sd);
    }
    cover_scaled_in_worker(sd);
    thumbnail_cache(sd);
    MainUICatalog *catalog = calloc(1, sizeof *catalog);
    assert(catalog && mainui_catalog_open(catalog, sd, false));
    MainUIViewport view, home = {1, 0, 0, 0};
    mainui_grid_restore(&view, catalog->pages[0].count, 0, 4, 2);
    MainUILaunchSource source = {
        .section = MAINUI_MENU_GAMES, .catalog = catalog, .view = &view, .home = &home};
    MainUICatalogJob job = {0};
    MainUISession opened = {0};
    assert(mainui_catalog_job_start(&job, JOB_ENTER, &source, sd, false, 6, NULL, 1));
    assert(wait_job(&job, 1, &opened) == JOB_READY);
    assert(catalog->depth == 0 && opened.catalog != catalog && opened.catalog->depth == 1);
    assert(opened.view.total == 200 && !opened.catalog->cancel.requested);
    source.catalog = opened.catalog;
    source.view = &opened.view;
    char file[4096];
    snprintf(file, sizeof file, "%s/Roms/Host/Host_cache6.db", sd);
    sqlite3 *database = NULL;
    assert(sqlite3_open(file, &database) == SQLITE_OK);
    assert(sqlite3_exec(database, "UPDATE Host_roms SET disp='Zulu moved' WHERE disp='game000'",
                        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(database);
    /* A changed generation cannot be paged into the old count/window. */
    MainUIEntry window[MAINUI_CACHE_WINDOW];
    int loaded = 0;
    assert(!mainui_cache_window(opened.catalog->pages[1].cache, 64, window, &loaded));
    MainUISession refreshed = {0};
    assert(mainui_catalog_job_start(&job, JOB_RELOAD, &source, sd, false, 6, NULL, 2));
    assert(wait_job(&job, 2, &refreshed) == JOB_READY);
    assert(refreshed.view.selected == 199);
    assert(!strcmp(mainui_browser_label(refreshed.catalog, 199), "Zulu moved"));
    mainui_session_close(&refreshed);
    assert(mainui_catalog_job_start(&job, JOB_RELOAD, &source, sd, false, 6, NULL, 3));
    assert(wait_job(&job, 4, &refreshed) == JOB_CANCELLED && !refreshed.catalog);
    assert(mainui_catalog_job_start(&job, JOB_RELOAD, &source, sd, false, 6, NULL, 4));
    mainui_catalog_job_cancel(&job);
    assert(wait_job(&job, 4, &refreshed) == JOB_CANCELLED && !refreshed.catalog);
    /* Refresh failures must release file handles without stranding the old
     * list: paging outside its current window works after the readers resume. */
    assert(mainui_catalog_job_start(&job, JOB_RELOAD, &source, sd, false, 6, NULL, 6));
    assert(wait_job(&job, 6, &refreshed) == JOB_READY);
    mainui_session_close(&opened);
    opened = refreshed;
    refreshed = (MainUISession){0};
    source.catalog = opened.catalog;
    source.view = &opened.view;
    /* An interrupted build that cannot be cleaned up (a folder in its place)
     * makes the refresh fail. */
    char stale[4200], xml[4096];
    snprintf(stale, sizeof stale, "%s.building.stale", file);
    assert(mkdir(stale, 0755) == 0);
    unsigned unchanged = hash_file(file);
    assert(mainui_catalog_job_start(&job, JOB_REFRESH_SYSTEM, &source, sd, false, 6, NULL, 7));
    assert(wait_job(&job, 7, &refreshed) == JOB_FAILED);
    assert(rmdir(stale) == 0);
    assert(hash_file(file) == unchanged);
    assert(mainui_browser_label(opened.catalog, 80));
    /* A gamelist with no usable <gameList> is not imported: the files are listed. */
    snprintf(xml, sizeof xml, "%s/Roms/Host/miyoogamelist.xml", sd);
    assert(mainui_write_text_atomic(xml, "<gameList><broken>"));
    assert(mainui_catalog_job_start(&job, JOB_REFRESH_SYSTEM, &source, sd, false, 6, NULL, 8));
    assert(wait_job(&job, 8, &refreshed) == JOB_READY);
    assert(refreshed.catalog->depth == 1 && refreshed.view.total == 200);
    mainui_session_close(&refreshed);
    assert(remove(xml) == 0);
    assert(mainui_catalog_job_start(&job, JOB_REFRESH_SYSTEM, &source, sd, false, 6, NULL, 8));
    assert(wait_job(&job, 8, &refreshed) == JOB_READY);
    assert(refreshed.catalog->depth == 1 && refreshed.view.total == 200);
    assert(!strcmp(mainui_browser_label(refreshed.catalog, refreshed.view.selected), "game000"));
    mainui_session_close(&refreshed);
    mainui_session_close(&opened);
    /* Selector refresh only removes the selected cache and stays on the grid.
     * The next ordinary entry performs the missing-cache rebuild. */
    source.catalog = catalog;
    source.view = &view;
    assert(catalog->depth == 0 && mainui_file_stamp(file).exists);
    assert(mainui_catalog_job_start(&job, JOB_REFRESH_SYSTEM, &source, sd, false, 6, NULL, 9));
    assert(wait_job(&job, 9, &refreshed) == JOB_READY);
    assert(refreshed.catalog && refreshed.catalog->depth == 0);
    assert(refreshed.view.selected == view.selected);
    assert(refreshed.view.start == view.start && refreshed.view.end == view.end);
    assert(!mainui_file_stamp(file).exists);
    source.catalog = refreshed.catalog;
    source.view = &refreshed.view;
    assert(mainui_catalog_job_start(&job, JOB_ENTER, &source, sd, false, 6, NULL, 10));
    assert(wait_job(&job, 10, &opened) == JOB_READY);
    assert(mainui_file_stamp(file).exists);
    assert(opened.catalog && opened.catalog->depth == 1 && opened.view.total == 200);
    assert(!strcmp(mainui_browser_label(opened.catalog, 0), "game000"));
    mainui_session_close(&opened);
    mainui_session_close(&refreshed);
    unsigned before = hash_file(file);
    int calls = 0;
    catalog->cancel = (MainUICancel){cancel_after, &calls};
    assert(!mainui_catalog_build_cache(catalog, 0, true));
    assert(calls > 40 && hash_file(file) == before);
    char temporary[4096];
    snprintf(temporary, sizeof temporary, "%s/Roms/Host/Host_cache6.db.building", sd);
    assert(!mainui_file_stamp(temporary).exists);
    catalog->cancel = (MainUICancel){0};
    mainui_catalog_close(catalog);
    free(catalog);
    MainUILibrary *saved = calloc(1, sizeof *saved);
    assert(saved);
    char favorites_path[4096];
    snprintf(favorites_path, sizeof favorites_path, "%s/Roms/favourite.json", sd);
    assert(mainui_write_text_atomic(
        favorites_path,
        "{\"label\":\"Alpha\",\"rompath\":\"/mnt/SDCARD/Roms/Host/game000.nes\",\"type\":5}\n"));
    assert(mainui_library_open(saved, sd, false));
    assert(!mainui_library_changed(saved, sd));
    assert(mainui_write_text_atomic(
        favorites_path,
        "{\"label\":\"Bravo\",\"rompath\":\"/mnt/SDCARD/Roms/Host/game000.nes\",\"type\":5}\n"));
    assert(mainui_library_changed(saved, sd));
    assert(!strcmp(mainui_library_label(saved, 0), "Alpha"));
    mainui_library_close(saved);
    free(saved);
    SDL_Quit();
    puts("Catalog worker cancellation, generation and external-change tests passed");
    return 0;
}
