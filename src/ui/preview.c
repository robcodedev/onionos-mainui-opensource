/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui/preview.h"
#include "ui/artwork.h"
#include <stdio.h>
#include <string.h>

static const char *json_string(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

void mainui_preview_close(MainUIPreview *preview)
{
    if (preview->thread) {
        SDL_WaitThread(preview->thread, NULL);
    }
    if (preview->decoded) {
        SDL_FreeSurface(preview->decoded);
    }
    for (int i = 0; i < MAINUI_THUMBNAIL_CACHE_SIZE; ++i) {
        if (preview->cache[i].image) {
            SDL_FreeSurface(preview->cache[i].image);
        }
    }
    *preview = (MainUIPreview){0};
}

static bool image_path(char out[MAINUI_PATH_MAX], MainUICatalog *catalog,
                       const MainUILibrary *library, int selected)
{
    if (!catalog || selected < 0) {
        return false;
    }
    const char *rom = NULL, *images = NULL, *saved = NULL;
    const char *root = catalog->sd;
    if (library) {
        if (selected >= library->visible_count || mainui_library_is_folder(library, selected)) {
            return false;
        }
        const MainUILibraryItem *item = &library->items[library->visible[selected]];
        rom = item->rom;
        saved = json_string(item->json, "imgpath");
        /* Find the source system by ROM-root containment. Search identities
         * were normalized while reading Recent records. */
        for (int i = 0; i < catalog->pages[0].count; i++) {
            MainUIEntry *system = &catalog->pages[0].entries[i];
            char host_rom[MAINUI_PATH_MAX];
            if (mainui_catalog_path(host_rom, catalog->sd, catalog->sd, rom) &&
                !strncmp(host_rom, system->path, strlen(system->path)) &&
                host_rom[strlen(system->path)] == '/') {
                images = system->images;
                root = system->path;
                break;
            }
        }
    }
    else {
        if (!catalog->depth) {
            return false;
        }
        MainUIEntry *entry = mainui_catalog_entry(catalog, selected);
        if (!entry || entry->directory) {
            return false;
        }
        rom = entry->path;
        saved = entry->artwork;
        /* Search results name their source console's artwork directory. */
        images =
            entry->images && *entry->images ? entry->images : catalog->pages[catalog->depth].images;
        root = catalog->pages[1].path;
    }
    if (saved && *saved && (strstr(saved, ".png") || strstr(saved, ".PNG"))) {
        return mainui_catalog_path(out, catalog->sd, root, saved);
    }
    if (!rom || !images || !*images) {
        return false;
    }
    /* Paths may arrive with either separator. Use the same normalized
     * path for the basename as for source-system matching above. */
    char normalized[MAINUI_PATH_MAX];
    if (!mainui_catalog_path(normalized, catalog->sd, catalog->sd, rom)) {
        return false;
    }
    const char *name = strrchr(normalized, '/');
    name = name ? name + 1 : normalized;
    char png[MAINUI_PATH_MAX];
    int size = snprintf(png, sizeof png, "%s", name);
    if (size < 0 || size >= (int)sizeof png - 4) {
        return false;
    }
    char *dot = strrchr(png, '.');
    if (dot) {
        *dot = 0;
    }
    strcat(png, ".png");
    return mainui_catalog_path(out, catalog->sd, images, png);
}

static SDL_Surface *scale_image(SDL_Surface *image)
{
    if (!image) {
        return NULL;
    }
    if (image->w <= 250 && image->h <= 360) {
        return image;
    }
    /* Normalize indexed artwork before stretching: a new paletted surface would
     * otherwise have an unrelated palette. Keep alpha for theme compositing. */
    SDL_Surface *normalized = mainui_artwork_software(image);
    SDL_FreeSurface(image);
    if (!normalized) {
        return NULL;
    }
    image = normalized;
    /* Downscale only, preserving aspect ratio. */
    int width = 250, height = (int)((int64_t)image->h * 250 / image->w);
    if (height > 360) {
        height = 360;
        width = (int)((int64_t)image->w * 360 / image->h);
    }
    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }
    SDL_Surface *scaled = SDL_CreateRGBSurface(
        SDL_SWSURFACE, width, height, image->format->BitsPerPixel, image->format->Rmask,
        image->format->Gmask, image->format->Bmask, image->format->Amask);
    if (scaled && SDL_SoftStretch(image, NULL, scaled, NULL) == 0) {
        SDL_FreeSurface(image);
        return scaled;
    }
    else if (scaled) {
        SDL_FreeSurface(scaled);
    }
    SDL_FreeSurface(image);
    return NULL;
}

static int decode_image(void *context)
{
    MainUIPreview *preview = context;
    /* The worker owns only this copied path and decoded surface. It never reads
     * catalog entries, which paging can invalidate, or touches the display.
     * Scaling uses software surfaces only, so it stays off the UI thread too. */
    preview->decoded = scale_image(mainui_artwork_load(preview->loading));
    atomic_store_explicit(&preview->done, true, memory_order_release);
    SDL_Event event = {.type = SDL_USEREVENT};
    SDL_PushEvent(&event);
    return 0;
}

static bool cached_image(MainUIPreview *preview, const char *path, bool select)
{
    for (int i = 0; i < MAINUI_THUMBNAIL_CACHE_SIZE; ++i) {
        if (*path && !strcmp(preview->cache[i].key, path)) {
            preview->cache[i].used = ++preview->clock;
            if (select) {
                preview->image = preview->cache[i].image;
            }
            return true;
        }
    }
    return false;
}

/* Takes ownership of an already scaled surface, or NULL for a failed read. */
static void cache_image(MainUIPreview *preview, const char *path, SDL_Surface *scaled)
{
    int slot = 0;
    for (int i = 1; i < MAINUI_THUMBNAIL_CACHE_SIZE; ++i) {
        if (preview->cache[i].used < preview->cache[slot].used) {
            slot = i;
        }
    }
    if (preview->cache[slot].image) {
        if (preview->image == preview->cache[slot].image) {
            preview->image = NULL;
        }
        SDL_FreeSurface(preview->cache[slot].image);
    }
    snprintf(preview->cache[slot].key, sizeof preview->cache[slot].key, "%s", path);
    preview->cache[slot].image = scaled;
    preview->cache[slot].used = ++preview->clock;
}

/* Wait for the current decode until the deadline; forever ignores it. */
static bool await_decode(MainUIPreview *preview, Uint32 deadline, bool forever)
{
    while (!atomic_load_explicit(&preview->done, memory_order_acquire)) {
        if (!forever && (Sint32)(SDL_GetTicks() - deadline) >= 0) {
            return false;
        }
        SDL_Delay(1);
    }
    return true;
}

/* Join a finished decode and select it if it is the current key. */
static void reap_decode(MainUIPreview *preview, const char *path)
{
    SDL_WaitThread(preview->thread, NULL);
    preview->thread = NULL;
    cache_image(preview, preview->loading, preview->decoded);
    preview->decoded = NULL;
    if (*path && cached_image(preview, path, true)) {
        preview->pending = false;
    }
}

void mainui_preview_request(MainUIPreview *preview, MainUICatalog *catalog,
                            const MainUILibrary *library, int selected, bool synchronous)
{
    mainui_preview_request_within(preview, catalog, library, selected,
                                  synchronous ? MAINUI_PREVIEW_WAIT_FOREVER : 0);
}

void mainui_preview_request_within(MainUIPreview *preview, MainUICatalog *catalog,
                                   const MainUILibrary *library, int selected, Uint32 wait_ms)
{
    bool synchronous = wait_ms == MAINUI_PREVIEW_WAIT_FOREVER;
    Uint32 deadline = SDL_GetTicks() + wait_ms;
    char path[MAINUI_PATH_MAX] = "";
    image_path(path, catalog, library, selected);
    const char *directory = catalog ? catalog->pages[catalog->depth].path : "";
    int folder = library ? library->current : -1;
    if (strcmp(path, preview->key) || selected != preview->prefetch_selected ||
        catalog != preview->prefetch_catalog || library != preview->prefetch_library ||
        folder != preview->prefetch_folder || strcmp(directory, preview->prefetch_directory)) {
        preview->prefetch_next = 0;
        preview->prefetch_selected = selected;
        preview->prefetch_catalog = catalog;
        preview->prefetch_library = library;
        preview->prefetch_folder = folder;
        snprintf(preview->prefetch_directory, sizeof preview->prefetch_directory, "%s", directory);
    }
    if (strcmp(path, preview->key)) {
        preview->image = NULL;
        snprintf(preview->key, sizeof preview->key, "%s", path);
        preview->changed_at = SDL_GetTicks();
        preview->pending = *path && !cached_image(preview, path, true);
    }
    /* A bounded wait covers both an in-flight decode (often a neighbor) and the
     * selected decode it starts; past the deadline both finish asynchronously. */
    if (preview->thread && (atomic_load_explicit(&preview->done, memory_order_acquire) ||
                            (wait_ms && await_decode(preview, deadline, synchronous)))) {
        reap_decode(preview, path);
    }
    if (!preview->thread && preview->pending) {
        if (synchronous) {
            cache_image(preview, path, scale_image(mainui_artwork_load(path)));
            cached_image(preview, path, true);
            preview->pending = false;
        }
        else {
            snprintf(preview->loading, sizeof preview->loading, "%s", path);
            /* The flag is reused across decodes, so store, not initialize:
             * atomic_init on a live object is not an atomic operation. */
            atomic_store(&preview->done, false);
            preview->thread = SDL_CreateThread(decode_image, preview);
            if (!preview->thread) {
                preview->pending = false;
            }
            else if (wait_ms && await_decode(preview, deadline, false)) {
                reap_decode(preview, path);
            }
        }
    }
    /* One decoder at a time: a changed selection takes priority over all queued
     * neighbors. Resolve paths on the UI thread; the worker never touches the
     * catalog or library. Do not prefetch in deterministic snapshot mode. */
    /* -1 is the ".." row of a ROM subfolder: prefetch the games below it too. */
    if (!synchronous && !preview->thread && !preview->pending && selected >= -1) {
        static const int offsets[] = {1, 2, -1, -2};
        int total = library   ? library->visible_count
                    : catalog ? catalog->pages[catalog->depth].count
                              : 0;
        while (preview->prefetch_next < 4) {
            int64_t index = (int64_t)selected + offsets[preview->prefetch_next++];
            char neighbor[MAINUI_PATH_MAX] = "";
            if (index < 0 || index >= total ||
                !image_path(neighbor, catalog, library, (int)index) ||
                cached_image(preview, neighbor, false)) {
                continue;
            }
            snprintf(preview->loading, sizeof preview->loading, "%s", neighbor);
            atomic_store(&preview->done, false);
            preview->thread = SDL_CreateThread(decode_image, preview);
            break;
        }
    }
}

void mainui_preview_update(MainUIPreview *preview, MainUICatalog *catalog,
                           const MainUILibrary *library, int selected)
{
    mainui_preview_request(preview, catalog, library, selected, true);
}

int mainui_preview_edge(const MainUITheme *theme)
{
    return 640 - (theme->preview_background ? theme->preview_background->w : 250);
}

void mainui_preview_draw(MainUIPreview *preview, const MainUITheme *theme, SDL_Surface *screen)
{
    if (!preview->image) {
        return;
    }
    /* Stock 0x31698 overlays the theme background after the list. At 0x31794
     * the cover is centered in a 250x360 area starting at that background's edge. */
    int edge = mainui_preview_edge(theme);
    if (theme->preview_background) {
        SDL_Rect background = {(Sint16)edge, 60, 0, 0};
        SDL_BlitSurface(theme->preview_background, NULL, screen, &background);
    }
    SDL_Rect destination = {(Sint16)(edge + (250 - preview->image->w) / 2),
                            (Sint16)(60 + (360 - preview->image->h) / 2), 0, 0};
    SDL_BlitSurface(preview->image, NULL, screen, &destination);
}
