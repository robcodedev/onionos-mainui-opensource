/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui/menu_view.h"
#include "platform/timing.h"
#include "ui/panels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void blit(SDL_Surface *screen, SDL_Surface *image, int x, int y)
{
    SDL_Rect destination = {(Sint16)x, (Sint16)y, 0, 0};
    if (image) {
        SDL_BlitSurface(image, NULL, screen, &destination);
    }
}

static void text(MainUITheme *theme, SDL_Surface *screen, TTF_Font *font, SDL_Color color,
                 const char *label, int x, int y, int width, int height)
{
    SDL_Surface *image = mainui_theme_text(theme, font, label, color);
    SDL_Rect clip = {(Sint16)x, (Sint16)y, (Uint16)width, (Uint16)height};
    SDL_SetClipRect(screen, &clip);
    if (image) {
        blit(screen, image, x + (width - image->w) / 2, y + (height - image->h) / 2);
    }
    SDL_SetClipRect(screen, NULL);
}

static void frame(MainUITheme *theme, SDL_Surface *screen, const char *title, int page, int pages)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 24, 24, 24));
    blit(screen, theme->background, 0, 0);
    mainui_draw_header(screen, theme, title);
    mainui_draw_footer(screen, theme, page, pages);
}

static void clear_consoles(MainUIMenuView *view)
{
    for (int i = 0; i < 9; i++) {
        /* The selected icon may share the normal one's surface: free it once. */
        SDL_Surface *normal = view->console_icons[i][0], *selected = view->console_icons[i][1];
        if (selected && selected != normal) {
            SDL_FreeSurface(selected);
        }
        if (normal) {
            SDL_FreeSurface(normal);
        }
        free(view->pending_selected[i]);
        view->pending_selected[i] = NULL;
        for (int state = 0; state < 2; state++) {
            if (view->console_labels[i][state]) {
                SDL_FreeSurface(view->console_labels[i][state]);
            }
            view->console_icons[i][state] = NULL;
            view->console_labels[i][state] = NULL;
        }
    }
}

/* Menu icons are decoded at full size, so a large icon pack could hold far
 * more memory than the screen shows. All of them together stay within this
 * budget; Expert icons keep only the part Expert draws. */
#define MENU_ICON_BUDGET (24u * 1024u * 1024u)

static size_t surface_bytes(const SDL_Surface *surface)
{
    return surface ? (size_t)surface->pitch * (size_t)surface->h : 0;
}

size_t mainui_menu_view_bytes(const MainUIMenuView *view)
{
    size_t bytes = 0;
    for (int i = 0; i < MAINUI_MENU_SECTIONS; i++) {
        bytes += surface_bytes(view->home_icons[i][0]) + surface_bytes(view->home_icons[i][1]);
    }
    for (int i = 0; i < 9; i++) {
        bytes += surface_bytes(view->console_icons[i][0]);
        if (view->console_icons[i][1] != view->console_icons[i][0]) {
            bytes += surface_bytes(view->console_icons[i][1]);
        }
    }
    return bytes;
}

/* The centered part of `image` that fits width x height, as a copy with the
 * same pixel format, transparency and palette; the original is freed. On
 * failure the original is returned. */
static SDL_Surface *crop(SDL_Surface *image, int width, int height)
{
    int w = image->w < width ? image->w : width, h = image->h < height ? image->h : height;
    if (w == image->w && h == image->h) {
        return image;
    }
    SDL_PixelFormat *format = image->format;
    SDL_Surface *out =
        SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, format->BitsPerPixel, format->Rmask,
                             format->Gmask, format->Bmask, format->Amask);
    if (!out || SDL_LockSurface(image)) {
        if (out) {
            SDL_FreeSurface(out);
        }
        return image;
    }
    int x = (image->w - w) / 2, y = (image->h - h) / 2, bytes = format->BytesPerPixel;
    for (int row = 0; row < h; row++) {
        memcpy((Uint8 *)out->pixels + row * out->pitch,
               (const Uint8 *)image->pixels + (y + row) * image->pitch + x * bytes,
               (size_t)(w * bytes));
    }
    SDL_UnlockSurface(image);
    if (format->palette) {
        SDL_SetColors(out, format->palette->colors, 0, format->palette->ncolors);
    }
    if (image->flags & SDL_SRCCOLORKEY) {
        SDL_SetColorKey(out, SDL_SRCCOLORKEY, format->colorkey);
    }
    SDL_SetAlpha(out, image->flags & SDL_SRCALPHA, format->alpha);
    SDL_FreeSurface(image);
    return out;
}

/* Icons dropped for the budget, with the bytes they needed, for the session:
 * a page change does not decode one again while it still cannot fit. A full
 * table only means an icon may be decoded once more before it is dropped. */
static struct {
    char *path;
    int width, height; /* the crop it was dropped with: Expert's or none */
    size_t bytes;
} dropped[32];

static bool over_budget(size_t retained, size_t needed)
{
    return retained > MENU_ICON_BUDGET || needed > MENU_ICON_BUDGET - retained;
}

static void report_budget(const char *path)
{
    static bool reported;
    if (!reported) {
        fprintf(stderr, "Menu icon budget exceeded by %s; showing no icon\n", path);
        reported = true;
    }
}

/* Cropped to width x height when they are set (Expert), or dropped (no icon)
 * over the budget. Other icons are kept whole: a large one is drawn clipped
 * by the screen, which no centered crop reproduces. */
static SDL_Surface *keep_icon(size_t retained, SDL_Surface *icon, int width, int height,
                              const char *path)
{
    if (!icon) {
        return NULL;
    }
    if (width > 0) {
        icon = crop(icon, width, height);
    }
    size_t needed = surface_bytes(icon);
    if (over_budget(retained, needed)) {
        report_budget(path);
        for (size_t i = 0; path && i < sizeof dropped / sizeof *dropped; i++) {
            if (!dropped[i].path && (dropped[i].path = strdup(path))) {
                dropped[i].width = width;
                dropped[i].height = height;
                dropped[i].bytes = needed;
                break;
            }
            if (dropped[i].path && !strcmp(dropped[i].path, path) && dropped[i].width == width &&
                dropped[i].height == height) {
                dropped[i].bytes = needed;
                break;
            }
        }
        SDL_FreeSurface(icon);
        return NULL;
    }
    return icon;
}

SDL_Surface *mainui_menu_view_icon(MainUITheme *theme, const char *path, size_t retained, int width,
                                   int height)
{
    if (!path || !*path) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof dropped / sizeof *dropped && dropped[i].path; i++) {
        if (!strcmp(dropped[i].path, path) && dropped[i].width == width &&
            dropped[i].height == height && over_budget(retained, dropped[i].bytes)) {
            report_budget(path);
            return NULL;
        }
    }
    struct timespec start = mainui_timing_start();
    SDL_Surface *icon = mainui_theme_console_icon(theme, path);
    mainui_timing_finish("icon-ms", start);
    return keep_icon(retained, icon, width, height, path);
}

void mainui_menu_view_open(MainUIMenuView *view, MainUITheme *theme)
{
    *view = (MainUIMenuView){.theme = theme, .cached_start = -1};
    for (int i = 0; i < MAINUI_MENU_SECTIONS; i++) {
        for (int selected = 0; selected < 2; selected++) {
            char name[80];
            snprintf(name, sizeof name, "skin/ic-%s-%c.png", mainui_menu_icon(i),
                     selected ? 'f' : 'n');
            view->home_icons[i][selected] = keep_icon(mainui_menu_view_bytes(view),
                                                      mainui_theme_image(theme, name), 0, 0, name);
        }
    }
}

void mainui_menu_view_close(MainUIMenuView *view)
{
    clear_consoles(view);
    for (int i = 0; i < MAINUI_MENU_SECTIONS; i++) {
        for (int selected = 0; selected < 2; selected++) {
            if (view->home_icons[i][selected]) {
                SDL_FreeSurface(view->home_icons[i][selected]);
            }
        }
    }
    *view = (MainUIMenuView){0};
}

/* The patch constrains home labels to a 136px lane, including when fewer
 * cards are visible. Keep two lines centered around the original label slot. */
static void home_label(MainUITheme *theme, SDL_Surface *screen, const char *label, SDL_Color color,
                       int x, int width)
{
    int measured = 0;
    TTF_SizeUTF8(theme->grid_font, label, &measured, NULL);
    int lane = (width < 156 ? width : 156) - 20;
    x += (width - lane) / 2;
    if (measured <= lane) {
        text(theme, screen, theme->grid_font, color, label, x, 260, lane, 50);
        return;
    }
    char first[256], second[256];
    snprintf(first, sizeof first, "%s", label);
    size_t split = strlen(first);
    while (split > 0) {
        split--;
        if (((unsigned char)first[split] & 0xc0) == 0x80) {
            continue;
        }
        char saved = first[split];
        first[split] = 0;
        TTF_SizeUTF8(theme->grid_font, first, &measured, NULL);
        first[split] = saved;
        if (measured <= lane) {
            break;
        }
    }
    for (size_t word = split; word > 0; word--) {
        if (first[word] == ' ') {
            split = word;
            break;
        }
    }
    snprintf(second, sizeof second, "%s", label + split + (label[split] == ' '));
    first[split] = 0;
    int height = TTF_FontHeight(theme->grid_font);
    int spacing = (3 * height + 3) / 4;
    text(theme, screen, theme->grid_font, color, first, x, 285 - spacing / 2 - height / 2, lane,
         height);
    text(theme, screen, theme->grid_font, color, second, x, 285 + spacing / 2 - height / 2, lane,
         height);
}

void mainui_menu_draw_home(MainUIMenuView *view, SDL_Surface *screen, const MainUIMenu *menu,
                           const MainUIViewport *position)
{
    MainUITheme *theme = view->theme;
    frame(theme, screen, "MIYOO", 0, -1);
    int columns = menu->count < 4 ? menu->count : 4;
    if (!columns) {
        return;
    }
    int width = 620 / columns;
    for (int i = position->start; i <= position->end; i++) {
        int selected = i == position->selected;
        int x = 10 + (i - position->start) * width;
        SDL_Surface *icon = view->home_icons[menu->sections[i]][selected];
        if (icon) {
            blit(screen, icon, x + (width - icon->w) / 2, 60 + (360 - icon->h) / 2 - 10);
        }
        if (!theme->hide_icons && !theme->hide_grid_text) {
            home_label(theme, screen, mainui_menu_label(menu->sections[i]),
                       theme->grid_color[selected], x, width);
        }
    }
    if (theme->dots[0]) {
        int stride = theme->dots[0]->w + 5;
        int x = (640 - menu->count * stride) / 2;
        for (int i = 0; i < menu->count; i++) {
            blit(screen, theme->dots[i == position->selected], x + i * stride, 390);
        }
    }
}

void mainui_menu_view_page(MainUIMenuView *view, MainUICatalog *catalog,
                           const MainUIViewport *position)
{
    if (view->cached_start == position->start) {
        return;
    }
    MainUITheme *theme = view->theme;
    bool expert = !strcmp(catalog->pages[0].title, "Expert");
    int capacity = expert ? 9 : 8;
    /* Expert draws the centered 192x72 of an icon. */
    int width = expert ? 192 : 0, height = expert ? 72 : 0;
    clear_consoles(view);
    view->crop_width = width;
    view->crop_height = height;
    for (int i = 0; i < capacity && position->start + i < position->total; i++) {
        MainUIEntry *entry = mainui_catalog_entry(catalog, position->start + i);
        if (!entry) {
            continue;
        }
        view->console_icons[i][0] =
            mainui_menu_view_icon(theme, entry->icon, mainui_menu_view_bytes(view), width, height);
        /* One file for both states is decoded once. */
        bool same =
            !entry->icon_selected || (entry->icon && !strcmp(entry->icon_selected, entry->icon));
        /* Until it is decoded, a selected icon left for later shows as the
         * normal one; the shared surface is freed once, as for one file. */
        if (!same && view->defer_selected && position->start + i != position->selected) {
            view->pending_selected[i] = strdup(entry->icon_selected);
            same = view->pending_selected[i] != NULL;
        }
        view->console_icons[i][1] =
            same ? view->console_icons[i][0]
                 : mainui_menu_view_icon(theme, entry->icon_selected, mainui_menu_view_bytes(view),
                                         width, height);
        if (!view->console_icons[i][1]) {
            view->console_icons[i][1] = view->console_icons[i][0];
        }
        for (int selected = 0; selected < 2; selected++) {
            bool expert_font = expert && theme->expert_font;
            bool hidden = expert_font ? theme->hide_expert_text : theme->hide_grid_text;
            view->console_labels[i][selected] =
                hidden ? NULL
                       : TTF_RenderUTF8_Blended(expert_font ? theme->expert_font : theme->grid_font,
                                                entry->label, theme->grid_color[selected]);
        }
    }
    view->cached_start = position->start;
}

bool mainui_menu_view_load_pending(MainUIMenuView *view, const MainUIViewport *position,
                                   bool *shown)
{
    *shown = false;
    if (view->cached_start < 0 || view->cached_start != position->start) {
        return false;
    }
    int slot = position->selected - position->start;
    if (slot < 0 || slot >= 9 || !view->pending_selected[slot]) {
        slot = -1;
        for (int i = 0; i < 9 && slot < 0; i++) {
            if (view->pending_selected[i]) {
                slot = i;
            }
        }
    }
    if (slot < 0) {
        return false;
    }
    char *path = view->pending_selected[slot];
    view->pending_selected[slot] = NULL;
    SDL_Surface *icon = mainui_menu_view_icon(view->theme, path, mainui_menu_view_bytes(view),
                                              view->crop_width, view->crop_height);
    free(path);
    if (icon) {
        view->console_icons[slot][1] = icon;
        *shown = position->start + slot == position->selected;
    }
    return true;
}

void mainui_menu_draw_systems(MainUIMenuView *view, SDL_Surface *screen, MainUICatalog *catalog,
                              const MainUIViewport *position)
{
    MainUITheme *theme = view->theme;
    bool expert = !strcmp(catalog->pages[0].title, "Expert");
    int columns = expert ? 3 : 4, capacity = expert ? 9 : 8;
    int width = expert ? 213 : 155, height = expert ? 120 : 170;
    int label_offset = expert ? 28 : 45;
    frame(theme, screen,
          !strcmp(catalog->pages[0].title, "Expert") ? mainui_menu_label(MAINUI_MENU_EXPERT)
                                                     : mainui_menu_label(MAINUI_MENU_GAMES),
          position->total ? position->start / capacity + 1 : 0,
          (position->total + capacity - 1) / capacity);
    mainui_menu_view_page(view, catalog, position);
    /* Stock Games: 4x2, margins 10, 155x170 cells. Expert: 3x3, no margins,
     * 213x120 cells, centered 192x72 icon crop, label bottom offset 28. */
    for (int i = 0; i < capacity && position->start + i < position->total; i++) {
        int selected = position->start + i == position->selected;
        int x = (expert ? 0 : 10) + (i % columns) * width;
        int y = (expert ? 60 : 75) + (i / columns) * height;
        SDL_Surface *tile = theme->tiles[selected], *icon = view->console_icons[i][selected];
        if (tile) {
            blit(screen, tile, x + (width - tile->w) / 2 + 2, y + (height - tile->h) / 2 + 2);
        }
        if (icon) {
            if (expert) {
                int w = icon->w < 192 ? icon->w : 192, h = icon->h < 72 ? icon->h : 72;
                SDL_Rect source = {(Sint16)((icon->w - w) / 2), (Sint16)((icon->h - h) / 2),
                                   (Uint16)w, (Uint16)h};
                SDL_Rect dest = {(Sint16)(x + (width - w) / 2), (Sint16)(y + (height - h) / 2 - 10),
                                 0, 0};
                SDL_BlitSurface(icon, &source, screen, &dest);
            }
            else {
                blit(screen, icon, x + (width - icon->w) / 2, y + (height - icon->h) / 2 - 10);
            }
        }
        SDL_Surface *label = view->console_labels[i][selected];
        if (label) {
            SDL_Rect clip = {(Sint16)x, (Sint16)(y + height - label_offset - 15), (Uint16)width,
                             50};
            SDL_SetClipRect(screen, &clip);
            blit(screen, label, x + (width - label->w) / 2,
                 y + height - label_offset + (18 - label->h) / 2);
            SDL_SetClipRect(screen, NULL);
        }
    }
    if (!position->total) {
        mainui_draw_empty(screen, theme);
    }
}
