/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_THEME_H
#define MAINUI_THEME_H
#include "core/core.h"
#include "menus/stock_settings.h"
#include "platform/files.h"
#include <SDL.h>
#include <SDL_ttf.h>

typedef struct {
    bool loaded;
    SDL_Surface *icons[SET_COUNT], *selection, *left, *right;
} MainUISettingsArtwork;

/* Owns all loaded surfaces and fonts. Initialize with mainui_theme_open and
 * release exactly once with mainui_theme_close; both operate on the UI thread.
 * This initial preview loader does not yet implement the full theme contract.
 */
typedef struct {
    SDL_Surface *background, *titlebar, *footer, *selection, *icon, *folder, *preview_background,
        *logo, *empty, *favorite, *number_background, *number_digits, *divider;
    MainUISettingsArtwork settings_artwork;
    SDL_Surface *popup_backgrounds[6], *popup_selection;
    bool popup_loaded[6], popup_selection_loaded;
    SDL_Surface *loading_background;
    bool loading_background_loaded;
    SDL_Surface *apps_selection;
    bool apps_selection_loaded;

    struct {
        TTF_Font *font;
        SDL_Color color;
        char text[4096];
        SDL_Surface *surface;
        unsigned used;
    } text_cache[32];

    unsigned text_clock;
    SDL_Surface *detail_default;
    bool detail_default_loaded;
    TTF_Font *expert_font;
    TTF_Font *battery_font;
    SDL_Surface *battery_icons[6];
    SDL_Surface *wifi_connected, *wifi_locked, *wifi_signal[4];
    bool wifi_online;
    int wifi_signal_level;
    char wifi_address[64];
    int battery_percent, battery_align, battery_offset_x, battery_offset_y;
    bool battery_visible, battery_fixed;
    SDL_Color battery_color;
    TTF_Font *font, *title_font, *grid_font, *hint_font, *menu_font, *description_font,
        *detail_font;
    SDL_Surface *tiles[2], *dots[2], *buttons[2];
    SDL_Color grid_color[2], hint_color, title_color, page_color, total_color;
    bool hide_icons, hide_hints;
    /* Font size 0 in config.json: stock draws no text in that role. The font
     * itself still opens at the default size for dialogs and other screens. */
    bool hide_title_text, hide_hint_text, hide_grid_text, hide_expert_text;
    char directory[4096], fallback[4096], sd[4096], profile[4096];
    SDL_Color color;
    int icon_margin;
    /* A non-English language is saved: prefer wqy-microhei.ttc as fallback font.
     * Read when the theme is opened; a language change that needs another
     * fallback restarts MainUI (docs/THEMES.md). */
    bool language_font;
    /* At least one theme font could not be opened and a built-in one is used. */
    bool fallback_font_used;
    /* First reason mainui_theme_open_sd() failed, when it is not an SDL error. */
    char error[512];
} MainUITheme;

/* Load the selected theme, with the given built-in asset directory as fallback.
 * On failure the context is cleared and any partially loaded resources freed.
 */
bool mainui_theme_open(MainUITheme *theme, const char *directory, const char *fallback,
                       const MainUIConfig *config);
/* Explicit SD root enables profile artwork and device-absolute font mapping.
 * Profile overrides affect images only. Closing/reopening invalidates caches. */
bool mainui_theme_open_sd(MainUITheme *theme, const char *directory, const char *fallback,
                          const char *sd, const MainUIConfig *config);
/* Resolve a configured font path; loading failure uses the built-in default. */
bool mainui_theme_font_path(const MainUITheme *theme, const char *name, char out[4096]);
/* Return caller-owned profile/active/built-in artwork, or NULL. */
SDL_Surface *mainui_theme_image(const MainUITheme *theme, const char *name);
/* Return a caller-owned icon from its resolved path or built-in icon pack. */
SDL_Surface *mainui_theme_console_icon(const MainUITheme *theme, const char *path);
/* Lazy, theme-owned immutable artwork, UI thread only. Missing/undecodable
 * assets are cached too. Borrow until theme_close; do not free or modify.
 * Popup rows must be 1..6; invalid rows return NULL. Theme reopening invalidates
 * every cache and retries assets, including previous misses. */
const MainUISettingsArtwork *mainui_theme_settings_artwork(MainUITheme *theme);
SDL_Surface *mainui_theme_text(MainUITheme *, TTF_Font *, const char *, SDL_Color);
SDL_Surface *mainui_theme_popup_background(MainUITheme *theme, int rows);
SDL_Surface *mainui_theme_popup_selection(MainUITheme *theme);
/* Lazy default detail thumbnail, borrowed until theme_close. */
SDL_Surface *mainui_theme_detail_default(MainUITheme *theme);
void mainui_theme_close(MainUITheme *theme);
#endif
