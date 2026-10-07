/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_MENU_VIEW_H
#define MAINUI_MENU_VIEW_H
#include "catalog/catalog.h"
#include "menus/menu.h"
#include "ui/theme.h"

/* Owns decoded icons/text for one visible console page. The theme and catalog
 * are borrowed and must outlive the view. All access stays on the UI thread.
 */
typedef struct {
    MainUITheme *theme;
    SDL_Surface *home_icons[MAINUI_MENU_SECTIONS][2];
    SDL_Surface *console_icons[9][2];
    SDL_Surface *console_labels[9][2];
    int cached_start;
} MainUIMenuView;

void mainui_menu_view_open(MainUIMenuView *view, MainUITheme *theme);
void mainui_menu_view_close(MainUIMenuView *view);
void mainui_menu_draw_home(MainUIMenuView *view, SDL_Surface *screen, const MainUIMenu *menu,
                           const MainUIViewport *position);
void mainui_menu_draw_systems(MainUIMenuView *view, SDL_Surface *screen, MainUICatalog *catalog,
                              const MainUIViewport *position);
/* Load the icons and labels of the console page at position->start, once per
 * page. Expert icons keep only the part Expert draws, and identical normal and
 * selected icons share one surface. */
void mainui_menu_view_page(MainUIMenuView *view, MainUICatalog *catalog,
                           const MainUIViewport *position);
/* Bytes held by home and console icons; capped at 24 MiB. */
size_t mainui_menu_view_bytes(const MainUIMenuView *view);
/* Decode the icon at path (mainui_theme_console_icon()), cropped to width x
 * height when they are set, or NULL when it would take its holder past the
 * 24 MiB icon budget with `retained` bytes already held. An icon dropped for
 * the budget is remembered with its size for the session, so it is not
 * decoded again while it still cannot fit. UI thread only. */
SDL_Surface *mainui_menu_view_icon(MainUITheme *theme, const char *path, size_t retained, int width,
                                   int height);
#endif
