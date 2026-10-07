/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_PANELS_H
#define MAINUI_PANELS_H
#include "catalog/catalog.h"
#include "localization/language.h"
#include "menus/context.h"
#include "menus/stock_settings.h"
#include "ui/theme.h"
void mainui_draw_header_image(SDL_Surface *screen, MainUITheme *theme, SDL_Surface *title);
void mainui_draw_list_header_image(SDL_Surface *screen, MainUITheme *theme, SDL_Surface *title);
void mainui_draw_header(SDL_Surface *screen, MainUITheme *theme, const char *title);
void mainui_draw_catalog_loading(SDL_Surface *screen, MainUITheme *theme, bool cancelling);
void mainui_draw_confirmation(SDL_Surface *screen, MainUITheme *theme, int title_id, int body_id);
/* Darken everything but the footer (0,0)-(640,420) with black at 0xAA alpha,
 * as stock does before it draws a popup. */
void mainui_dim_popup_background(SDL_Surface *screen, MainUITheme *theme);
void mainui_draw_context(SDL_Surface *screen, MainUITheme *theme, const MainUIContext *context);
void mainui_draw_languages(SDL_Surface *screen, MainUITheme *theme, const MainUILanguages *list);
void mainui_draw_footer(SDL_Surface *screen, MainUITheme *theme, int page, int total);
void mainui_draw_settings(SDL_Surface *screen, MainUITheme *theme,
                          const MainUIStockSettings *settings);
void mainui_draw_apps(SDL_Surface *screen, MainUITheme *theme, MainUICatalog *apps,
                      const MainUIViewport *view);
void mainui_draw_message(SDL_Surface *screen, MainUITheme *theme, const char *title,
                         const char *body);
/* Draw stock Empty.png at its natural size, centered in the 640x480 surface. */
void mainui_draw_empty(SDL_Surface *screen, MainUITheme *theme);
/* Favorite folders show direct game count instead of the normal current/total. */
void mainui_draw_folder_footer(SDL_Surface *screen, MainUITheme *theme, int count);
/* Popup hints use translated OK/CANCEL with the same theme spacing as lists. */
void mainui_draw_popup_footer(SDL_Surface *screen, MainUITheme *theme);
/* Draw the stock four-digit game ordinal using theme digit-strip artwork. */
void mainui_draw_detail_counter(SDL_Surface *screen, MainUITheme *theme, int ordinal);
/* Footer with explicit screen action labels. */
void mainui_draw_action_footer(SDL_Surface *screen, MainUITheme *theme, const char *action,
                               const char *back);
#endif
