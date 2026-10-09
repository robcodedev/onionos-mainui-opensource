/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui/panels.h"
#include "ui/drawing.h"
#include "ui/menu_view.h"
#include <SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* current/total is the footer counter; total -1 shows none. */
static void list_frame(SDL_Surface *screen, MainUITheme *theme, const char *title, int current,
                       int total)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 24, 24, 24));
    mainui_blit(screen, theme->background, 0, 0);
    mainui_draw_header(screen, theme, title);
    mainui_draw_footer(screen, theme, current, total);
}

void mainui_draw_settings(SDL_Surface *screen, MainUITheme *theme,
                          const MainUIStockSettings *settings)
{
    list_frame(screen, theme, mainui_translate(15, "Settings"), 0, -1);
    const MainUISettingsArtwork *art = mainui_theme_settings_artwork(theme);
    SDL_Surface *selection = art->selection;
    int start = settings->start;
    for (int i = start; i < settings->count && i < start + 6; i++) {
        int y = 60 + (i - start) * 60;
        SDL_Rect clip = {0, (Sint16)y, 640, 60};
        SDL_SetClipRect(screen, &clip);
        if (i == settings->selected) {
            mainui_blit(screen, selection, 0, y);
        }
        SDL_Surface *icon = art->icons[settings->rows[i]];
        if (icon) {
            mainui_blit(screen, icon, 20, y + (60 - icon->h) / 2);
        }
        mainui_label(screen, theme->menu_font, theme->color,
                     mainui_stock_setting_label(settings->rows[i]), 80,
                     y + (60 - TTF_FontHeight(theme->menu_font)) / 2);
        MainUISettingKind kind = settings->rows[i];
        if (kind == SET_WIFI && theme->wifi_online && *theme->wifi_address) {
            int width = 0;
            TTF_SizeUTF8(theme->menu_font, theme->wifi_address, &width, NULL);
            mainui_label(screen, theme->menu_font, theme->color, theme->wifi_address, 600 - width,
                         y + (60 - TTF_FontHeight(theme->menu_font)) / 2);
        }
        if (kind == SET_BRIGHTNESS || kind == SET_SOUND || kind == SET_SLEEP) {
            /* Stock Settings renderer 0x28f34 reserves a 200px value lane
             * between the independently themed 24px arrow surfaces. */
            SDL_Surface *left = art->left;
            SDL_Surface *right = art->right;
            int right_width = right ? right->w : 24;
            if (left) {
                mainui_blit(screen, left, 640 - right_width - left->w - 240,
                            y + (60 - left->h) / 2);
            }
            if (right) {
                mainui_blit(screen, right, 640 - right_width - 40, y + (60 - right->h) / 2);
            }
            char value[48];
            if (kind == SET_SLEEP) {
                if (settings->values[kind]) {
                    snprintf(value, sizeof value, "%d min", settings->values[kind]);
                }
                else {
                    snprintf(value, sizeof value, "%s", mainui_translate(114, "Off"));
                }
            }
            else {
                snprintf(value, sizeof value, "%02d/%02d", settings->values[kind],
                         kind == SET_BRIGHTNESS ? 10 : 20);
            }
            SDL_Surface *number = TTF_RenderUTF8_Blended(theme->menu_font, value, theme->color);
            if (number) {
                mainui_blit(screen, number, 640 - right_width - 240 + (200 - number->w) / 2,
                            y + (60 - number->h) / 2);
                SDL_FreeSurface(number);
            }
        }
    }
    SDL_SetClipRect(screen, NULL);
}

/* The decoded icon for path, kept in the theme's Apps slots so a redraw
 * reuses it; *borrowed is false when the caller must free the surface (no
 * slot was free). Slots not marked `used` by this draw are freed after it. */
static SDL_Surface *app_icon(MainUITheme *theme, const char *path, bool used[4], bool *borrowed)
{
    *borrowed = true;
    if (!path || !*path) {
        return NULL;
    }
    size_t retained = 0;
    for (int i = 0; i < 4; i++) {
        if (theme->app_icon_paths[i] && !strcmp(theme->app_icon_paths[i], path)) {
            used[i] = true;
            return theme->app_icons[i];
        }
        if (theme->app_icons[i]) {
            retained += (size_t)theme->app_icons[i]->pitch * (size_t)theme->app_icons[i]->h;
        }
    }
    /* The icon this one replaces, off screen, is freed before the decode, so
     * it does not count against the budget. */
    int slot = -1;
    for (int i = 0; i < 4 && slot < 0; i++) {
        if (!used[i]) {
            slot = i;
        }
    }
    if (slot >= 0) {
        if (theme->app_icons[slot]) {
            retained -= (size_t)theme->app_icons[slot]->pitch * (size_t)theme->app_icons[slot]->h;
            SDL_FreeSurface(theme->app_icons[slot]);
        }
        free(theme->app_icon_paths[slot]);
        theme->app_icons[slot] = NULL;
        theme->app_icon_paths[slot] = NULL;
    }
    SDL_Surface *icon = mainui_menu_view_icon(theme, path, retained, 0, 0);
    if (slot >= 0 && (theme->app_icon_paths[slot] = strdup(path))) {
        theme->app_icons[slot] = icon;
        used[slot] = true;
        return icon;
    }
    *borrowed = false;
    return icon;
}

void mainui_draw_apps(SDL_Surface *screen, MainUITheme *theme, MainUICatalog *apps,
                      const MainUIViewport *view)
{
    bool used[4] = {false};
    /* Stock counts Apps rows as it counts games: 10/15 on row 10 of 15. */
    list_frame(screen, theme, mainui_translate(107, "Apps"), view->total ? view->selected + 1 : 0,
               view->total ? view->total : -1);
    if (!view->total) {
        mainui_draw_empty(screen, theme);
        return;
    }
    /* Apps constructor 0x1bc80 requests four rows, then sets row height90
     * at 0x1bc94; it passes bg-list-l.png, not the console-grid tiles. */
    if (!theme->apps_selection_loaded) {
        theme->apps_selection = mainui_theme_image(theme, "skin/bg-list-l.png");
        theme->apps_selection_loaded = true;
    }
    SDL_Surface *selection = theme->apps_selection;
    /* TextMenu 0x1f8b0 draws row separators before selection/text. */
    SDL_Rect content = {0, 60, 640, 360};
    SDL_SetClipRect(screen, &content);
    for (int i = 1; i < 4; i++) {
        mainui_blit(screen, theme->divider, 0, 60 + i * 90);
    }
    /* Mark the icons this draw shows first, so a new one never takes the
     * slot of another visible row. */
    for (int i = view->start; i <= view->end; i++) {
        MainUIEntry *app = mainui_catalog_entry(apps, i);
        for (int slot = 0; app && app->icon && slot < 4; slot++) {
            if (theme->app_icon_paths[slot] && !strcmp(theme->app_icon_paths[slot], app->icon)) {
                used[slot] = true;
            }
        }
    }
    for (int i = view->start; i <= view->end; i++) {
        MainUIEntry *app = mainui_catalog_entry(apps, i);
        if (!app) {
            continue;
        }
        /* Four 90px rows from y=62, as stock draws them. Each is drawn in
         * full: the fourth's highlight reaches y=421, over the footer's top
         * 2 px, as in stock; clipping it at 420 cut off a framed highlight. */
        int y = 62 + (i - view->start) * 90;
        SDL_Rect clip = {0, (Sint16)y, 640, 90};
        SDL_SetClipRect(screen, &clip);
        if (i == view->selected) {
            mainui_blit(screen, selection, 0,
                        y + (selection && selection->h < 90 ? (90 - selection->h) / 2 : 0));
        }
        bool borrowed;
        SDL_Surface *icon = app_icon(theme, app->icon, used, &borrowed);
        int x = 20;
        if (icon) {
            /* Stock 0x20090..0x201e8 reserves a fixed 71px icon lane.
             * Only icons exceeding the row in both dimensions are center-cropped. */
            if (icon->w > 90 && icon->h > 90) {
                SDL_Rect source = {(Sint16)((icon->w - 71) / 2), (Sint16)((icon->h - 71) / 2), 71,
                                   71};
                SDL_Rect destination = {20, (Sint16)y, 0, 0};
                SDL_BlitSurface(icon, &source, screen, &destination);
            }
            else {
                mainui_blit(screen, icon, 20, y + (90 - icon->h) / 2);
            }
            x = 111;
            if (!borrowed) {
                SDL_FreeSurface(icon);
            }
        }
        bool description = app->description && *app->description;
        mainui_label(screen, theme->menu_font, theme->color, app->label, x,
                     description ? y + 20 : y + (90 - TTF_FontHeight(theme->menu_font)) / 2);
        if (description) {
            mainui_label(screen, theme->menu_font, theme->grid_color[0], app->description, x,
                         y + 45);
        }
    }

    SDL_SetClipRect(screen, NULL);
    /* Icons of rows no longer shown are freed; the shown ones stay. */
    for (int slot = 0; slot < 4; slot++) {
        if (!used[slot]) {
            SDL_FreeSurface(theme->app_icons[slot]);
            free(theme->app_icon_paths[slot]);
            theme->app_icons[slot] = NULL;
            theme->app_icon_paths[slot] = NULL;
        }
    }
}

/* Length of the longest prefix of text, ending at a character boundary, that
 * fits in width; at least one character so a line always advances. */
static size_t fitting_prefix(TTF_Font *font, const char *text, size_t length, int width)
{
    char line[256];
    size_t best = 0;
    for (size_t end = 1; end <= length && end < sizeof line; end++) {
        if (end < length && ((unsigned char)text[end] & 0xc0) == 0x80) {
            continue; /* Inside a UTF-8 character. */
        }
        memcpy(line, text, end);
        line[end] = 0;
        int w = 0;
        if (TTF_SizeUTF8(font, line, &w, NULL) || w > width) {
            break;
        }
        best = end;
    }
    if (!best) {
        while (best < length && (best == 0 || ((unsigned char)text[best] & 0xc0) == 0x80)) {
            best++;
        }
    }
    return best;
}

/* Split text into lines no wider than width, breaking at spaces and inside a
 * word only when it is wider than a line. Returns the number of lines. */
static int wrap_text(TTF_Font *font, const char *text, int width, char lines[][256], int most)
{
    int count = 0;
    while (*text == ' ') {
        text++;
    }
    while (*text && count < most) {
        size_t length = strlen(text);
        size_t fit = fitting_prefix(font, text, length, width);
        size_t cut = fit;
        if (fit < length && text[fit] != ' ') {
            /* Break at the last space that fits, if there is one. */
            for (size_t i = fit; i > 0; i--) {
                if (text[i - 1] == ' ') {
                    cut = i - 1;
                    break;
                }
            }
            if (cut == fit || !cut) {
                cut = fit;
            }
        }
        if (cut >= sizeof lines[0]) {
            cut = sizeof lines[0] - 1;
        }
        memcpy(lines[count], text, cut);
        lines[count][cut] = 0;
        count++;
        text += cut;
        while (*text == ' ') {
            text++;
        }
    }
    return count;
}

void mainui_draw_message(SDL_Surface *screen, MainUITheme *theme, const char *title,
                         const char *body)
{
    /* The body wraps to as many lines as it needs; the panel grows down for
     * more than one line, up to the bottom of the screen. Messages are
     * deliberately bounded; no path is ever interpreted as a format string or
     * a host command. */
    enum {
        TOP = 145,
        LEFT = 35,
        WIDTH = 570,
        BODY = 215,
        BOTTOM = 470
    };

    int skip = TTF_FontLineSkip(theme->title_font);
    if (skip < 1) {
        skip = 1;
    }
    char lines[8][256];
    int most = (BOTTOM - 10 - skip - 10 - BODY) / skip;
    if (most < 1) {
        most = 1;
    }
    if (most > 8) {
        most = 8;
    }
    int count = wrap_text(theme->title_font, body ? body : "", WIDTH, lines, most);
    int hint = count <= 1 ? 280 : BODY + count * skip + 10;
    int bottom = hint + skip + 10 > TOP + 190 ? hint + skip + 10 : TOP + 190;
    if (bottom > BOTTOM) {
        bottom = BOTTOM;
    }
    SDL_Rect panel = {20, TOP, 600, (Uint16)(bottom - TOP)};
    SDL_FillRect(screen, &panel, SDL_MapRGB(screen->format, 24, 24, 24));
    SDL_Rect clip = {LEFT, TOP + 5, WIDTH, (Uint16)(bottom - TOP - 10)};
    SDL_SetClipRect(screen, &clip);
    mainui_label(screen, theme->title_font, theme->title_color, title, LEFT, 160);
    for (int i = 0; i < count; i++) {
        mainui_label(screen, theme->title_font, theme->color, lines[i], LEFT, BODY + i * skip);
    }
    mainui_label(screen, theme->title_font, theme->hint_color, "A / B: close", LEFT, hint);
    SDL_SetClipRect(screen, NULL);
}

void mainui_draw_languages(SDL_Surface *screen, MainUITheme *theme, const MainUILanguages *list)
{
    list_frame(screen, theme, mainui_translate(23, "Change language"), 0, -1);
    SDL_Surface *selection = mainui_theme_image(theme, "skin/bg-list-s.png");
    int start = list->start;
    for (int i = start; i < list->count && i < start + 6; i++) {
        int y = 60 + (i - start) * 60;
        SDL_Rect clip = {0, (Sint16)y, 640, 60};
        SDL_SetClipRect(screen, &clip);
        if (i == list->selected) {
            mainui_blit(screen, selection, 0, y);
        }
        mainui_label(screen, theme->menu_font, theme->color, list->entries[i].name, 20,
                     y + (60 - TTF_FontHeight(theme->menu_font)) / 2);
    }
    if (selection) {
        SDL_FreeSurface(selection);
    }
    SDL_SetClipRect(screen, NULL);
}
