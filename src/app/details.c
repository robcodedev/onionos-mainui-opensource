/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/details.h"
#include "app/browser.h"
#include "ui/drawing.h"
#include "ui/panels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mainui_details_close(MainUIDetails *details)
{
    *details = (MainUIDetails){0};
}

void mainui_details_progress(MainUIDetails *details, Uint32 wait_ms)
{
    if (details->open && details->preview) {
        mainui_preview_request_within(details->preview, details->catalog, details->library,
                                      details->index, wait_ms);
    }
}

static bool folder(MainUICatalog *catalog, const MainUILibrary *library, int row)
{
    return library ? mainui_library_is_folder(library, row) : mainui_browser_folder(catalog, row);
}

bool mainui_details_open(MainUIDetails *details, MainUICatalog *catalog,
                         const MainUILibrary *library, const MainUILibrary *favorites, int row,
                         MainUIPreview *preview)
{
    if (!preview || !catalog || row < 0 || folder(catalog, library, row)) {
        return false;
    }
    const char *label = NULL, *rom = NULL;
    int index = library ? row : mainui_browser_index(catalog, row);
    if (library) {
        if (row >= library->visible_count) {
            return false;
        }
        const MainUILibraryItem *item = &library->items[library->visible[row]];
        if (item->type == 3) {
            return false;
        }
        label = item->label;
        rom = item->rom;
    }
    else {
        if (!catalog->depth) {
            return false;
        }
        MainUIEntry *entry = mainui_catalog_entry(catalog, index);
        if (!entry) {
            return false;
        }
        label = entry->label;
        rom = entry->path;
    }
    char host[MAINUI_PATH_MAX];
    if (!mainui_catalog_path(host, catalog->sd, catalog->sd, rom) ||
        strlen(label) >= sizeof details->title) {
        return false;
    }
    mainui_details_close(details);
    details->preview = preview;
    strcpy(details->title, label);
    const char *root = catalog->depth ? catalog->pages[1].path : NULL;
    for (int i = 0; i < catalog->pages[0].count; i++) {
        const MainUIEntry *system = &catalog->pages[0].entries[i];
        size_t length = strlen(system->path);
        if (!strncmp(host, system->path, length) && host[length] == '/') {
            snprintf(details->system, sizeof details->system, "%s", system->label);
            root = system->path;
            break;
        }
    }
    /* Saved games can belong to a system outside the currently active catalog. */
    if (library && !*details->system) {
        const MainUILibraryItem *item = &library->items[library->visible[row]];
        char launcher[MAINUI_PATH_MAX], config[MAINUI_PATH_MAX];
        if (item->launch && mainui_catalog_path(launcher, catalog->sd, catalog->sd, item->launch)) {
            char *slash = strrchr(launcher, '/');
            if (slash) {
                *slash = 0;
                if (mainui_catalog_path(config, catalog->sd, launcher, "config.json")) {
                    char *text = mainui_read_text(config, 1024 * 1024);
                    cJSON *json = text ? cJSON_Parse(text) : NULL;
                    const cJSON *system_label = cJSON_GetObjectItemCaseSensitive(json, "label");
                    if (cJSON_IsString(system_label)) {
                        snprintf(details->system, sizeof details->system, "%s",
                                 system_label->valuestring);
                    }
                    cJSON_Delete(json);
                    free(text);
                }
            }
        }
    }
    cJSON *record = library ? NULL : mainui_catalog_record(catalog, index);
    const cJSON *saved_rom = cJSON_GetObjectItemCaseSensitive(record, "rompath");
    details->favorite =
        mainui_library_contains(favorites, library                     ? rom
                                           : cJSON_IsString(saved_rom) ? saved_rom->valuestring
                                                                       : "");
    cJSON_Delete(record);
    mainui_gamelist_metadata(host, root, &details->metadata);
    /* Match the reference metadata parser's collapsed XML whitespace. */
    char *write = details->metadata.description;
    bool space = false;
    for (const char *read = details->metadata.description; *read; ++read) {
        if (*read == ' ' || *read == '\n' || *read == '\r' || *read == '\t') {
            space = write != details->metadata.description;
        }
        else {
            if (space) {
                *write++ = ' ';
            }
            *write++ = *read;
            space = false;
        }
    }
    *write = 0;
    /* Select the cover without waiting: a cached one shows at once, any
     * other when its decode is done (mainui_details_progress). */
    details->catalog = catalog;
    details->library = library;
    details->index = index;
    mainui_preview_request_within(details->preview, catalog, library, index, 0);
    int leading = library ? library->leading_folders
                          : catalog->pages[catalog->depth].folder_count + (catalog->depth > 1);
    details->ordinal = row + 1 - leading;
    details->total = library ? library->visible_games : mainui_browser_count(catalog) - leading;
    details->open = true;
    return true;
}

void mainui_details_key(MainUIDetails *details, MainUICatalog *catalog,
                        const MainUILibrary *library, const MainUILibrary *favorites,
                        MainUIViewport *view, int rows, SDLKey key)
{
    if (key == SDLK_ESCAPE || key == SDLK_LEFT) {
        mainui_details_close(details);
        return;
    }
    if (key == SDLK_HOME || key == SDLK_END) {
        int last = details->line_count - details->lines_per_page;
        if (last < 0) {
            last = 0;
        }
        details->scroll_line +=
            key == SDLK_HOME ? -details->lines_per_page : details->lines_per_page;
        if (details->scroll_line < 0) {
            details->scroll_line = 0;
        }
        if (details->scroll_line > last) {
            details->scroll_line = last;
        }
    }
    if (key != SDLK_UP && key != SDLK_DOWN) {
        return;
    }
    int total = library ? library->visible_count : mainui_browser_count(catalog);
    int target = view->selected, direction = key == SDLK_UP ? -1 : 1;
    for (int checked = 0; checked < total; checked++) {
        target = (target + total + direction) % total;
        if (!folder(catalog, library, target) &&
            mainui_details_open(details, catalog, library, favorites, target, details->preview)) {
            mainui_viewport_move(view, rows, target - view->selected, false);
            return;
        }
    }
}

/* Font-measured wrapping never splits a UTF-8 continuation byte. Return the
 * next input byte, preserving explicit newlines and making progress on wide glyphs. */
static const char *line(TTF_Font *font, const char *text, char out[4096], int width)
{
    const char *p = text, *fit = text, *space = NULL;
    while (*p && *p != '\n') {
        const char *next = p + 1;
        while (((unsigned char)*next & 0xc0) == 0x80) {
            next++;
        }
        size_t size = (size_t)(next - text);
        if (size >= 4096) {
            break;
        }
        memcpy(out, text, size);
        out[size] = 0;
        int measured = 0;
        if (TTF_SizeUTF8(font, out, &measured, NULL) < 0 || measured > width) {
            break;
        }
        fit = next;
        if (*p == ' ') {
            space = p;
        }
        p = next;
    }
    if (p == text && *p && *p != '\n') {
        fit = p + 1;
        while (((unsigned char)*fit & 0xc0) == 0x80) {
            fit++;
        }
    }
    else if (*p && *p != '\n' && space) {
        fit = space;
    }
    size_t size = (size_t)(fit - text);
    memcpy(out, text, size);
    out[size] = 0;
    if (*fit == ' ' || *fit == '\n') {
        fit++;
    }
    return fit;
}

void mainui_details_draw(MainUIDetails *details, MainUITheme *theme, SDL_Surface *screen)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 24, 24, 24));
    mainui_blit(screen, theme->background, 0, 0);
    mainui_draw_header(screen, theme, mainui_translate(84, "GAME INFO"));
    mainui_draw_detail_counter(screen, theme, details->ordinal);
    if (details->favorite && theme->favorite) {
        int x = 634 - theme->favorite->w;
        mainui_blit(screen, theme->favorite, x < 274 ? 274 : x, 60);
    }
    SDL_Surface *image = details->preview && details->preview->image
                             ? details->preview->image
                             : mainui_theme_detail_default(theme);
    if (image) {
        mainui_blit(screen, image, (250 - image->w) / 2, 67);
    }
    SDL_Rect clip = {274, 98, 360, 322};
    SDL_SetClipRect(screen, &clip);
    char system[4096];
    const char *begin = details->system;
    while (*begin == ' ') {
        begin++;
    }
    snprintf(system, sizeof system, "%s", begin);
    size_t end = strlen(system);
    while (end && system[end - 1] == ' ') {
        system[--end] = 0;
    }
    mainui_label(screen, theme->menu_font, theme->color, system, 274, 98);
    char wrapped[4096];
    const char *text = details->title;
    int y = 130, height = TTF_FontHeight(theme->title_font) * 7 / 8, count = 0;
    if (height < 20) {
        height = 20;
    }
    if (height > 40) {
        height = 40;
    }
    while (*text) {
        text = line(theme->title_font, text, wrapped, 360);
        mainui_label(screen, theme->title_font, theme->color, wrapped, 274, y);
        y += height;
    }
    char summary[4120];
    snprintf(summary, sizeof summary, "%s%s%s", details->metadata.genre,
             *details->metadata.genre && *details->metadata.rating ? " | " : "",
             details->metadata.rating);
    /* meta_draw: 14px after title, font height + 2px between lines,
     * then 5px after the summary before the description. */
    y += 14;
    int metadata_height = TTF_FontHeight(theme->detail_font);
    if (metadata_height < 10) {
        metadata_height = 10;
    }
    if (metadata_height > 40) {
        metadata_height = 40;
    }
    bool summary_drawn = false;
    text = summary;
    while (*text && y + metadata_height <= 420) {
        text = line(theme->detail_font, text, wrapped, 360);
        mainui_label(screen, theme->detail_font, theme->color, wrapped, 274, y);
        y += metadata_height + 2;
        summary_drawn = true;
    }
    if (summary_drawn) {
        y += 5;
    }
    height = metadata_height + 2;
    int visible = (420 - y + 2) / height;
    if (visible < 1) {
        details->lines_per_page = 0;
        SDL_SetClipRect(screen, NULL);
        mainui_draw_footer(screen, theme, 0, -1);
        return;
    }
    text = details->metadata.description;
    count = 0;
    while (*text) {
        text = line(theme->detail_font, text, wrapped, 360);
        count++;
    }
    details->line_count = count;
    details->lines_per_page = visible;
    int start = details->scroll_line;
    /* Patcher meta_input clamps to line_count - lines_per_page, so the final
     * page remains full even when its last advance is less than one page. */
    int last = count > visible ? count - visible : 0;
    if (start > last) {
        start = last;
    }
    details->scroll_line = start;
    text = details->metadata.description;
    for (int i = 0; *text && i < start + visible; i++) {
        text = line(theme->detail_font, text, wrapped, 360);
        if (i >= start) {
            mainui_label(screen, theme->detail_font, theme->color, wrapped, 274,
                         y + (i - start) * height);
        }
    }
    SDL_SetClipRect(screen, NULL);
    mainui_draw_footer(screen, theme, 0, -1);
}
