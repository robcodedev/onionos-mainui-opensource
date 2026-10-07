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
    /* The selected icon of a console not yet decoded: its path, while the
     * slot shows the normal icon in its place. NULL when decoded or shared. */
    char *pending_selected[9];
    int cached_start, crop_width, crop_height;
    /* Decode only the selected console's selected icon with the page; the
     * others follow one at a time while MainUI is idle. Off for snapshots and
     * scripted input, which draw every icon as soon as the page is shown. */
    bool defer_selected;
} MainUIMenuView;

void mainui_menu_view_open(MainUIMenuView *view, MainUITheme *theme);
void mainui_menu_view_close(MainUIMenuView *view);
void mainui_menu_draw_home(MainUIMenuView *view, SDL_Surface *screen, const MainUIMenu *menu,
                           const MainUIViewport *position);
void mainui_menu_draw_systems(MainUIMenuView *view, SDL_Surface *screen, MainUICatalog *catalog,
                              const MainUIViewport *position);
/* Load the icons and labels of the console page at position->start, once per
 * page. Expert icons keep only the part Expert draws, and identical normal and
 * selected icons share one surface. With defer_selected, every normal icon and
 * the selected console's selected icon are decoded now; the other selected
 * icons are left for mainui_menu_view_load_pending(). */
void mainui_menu_view_page(MainUIMenuView *view, MainUICatalog *catalog,
                           const MainUIViewport *position);
/* Decode one selected icon left for later by mainui_menu_view_page(): the
 * selected console's first, then the others in page order. Nothing when the
 * page at position->start is not the one loaded. *shown is set when the icon
 * is the selected console's, so the frame must be drawn again. Returns false
 * when none was left. UI thread only. */
bool mainui_menu_view_load_pending(MainUIMenuView *view, const MainUIViewport *position,
                                   bool *shown);
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
