/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_SCREEN_EVENTS_H
#define MAINUI_SCREEN_EVENTS_H
#include "app/app_context.h"

/* Event handling preserves modal priority. True means the event was consumed. */
bool mainui_dispatch_event(MainUIApp *ui, SDL_Event *event);
bool mainui_screen_language_open(MainUIApp *ui, SDLKey key);
bool mainui_screen_settings_open(MainUIApp *ui, SDLKey key);
bool mainui_screen_apps(MainUIApp *ui, SDLKey key);
/* Screen handlers return false to continue routing. A context-menu section
 * action also sets requested_section for home_key to perform the transition.
 * search_key is called only for an unobstructed game list; list_key is the
 * final fallback after all modal and home handlers. */
bool mainui_screen_name_input_key(MainUIApp *ui, const SDL_keysym *key);
bool mainui_screen_settings_page_key(MainUIApp *ui, SDLKey key);
bool mainui_screen_context_menu_key(MainUIApp *ui, SDLKey key, int *requested_section);
bool mainui_screen_home_key(MainUIApp *ui, SDLKey key, int requested_section);
bool mainui_screen_list_key(MainUIApp *ui, SDLKey key);
bool mainui_screen_search_key(MainUIApp *ui, SDLKey key);
bool mainui_launch_tool(const char *directory, const char *sd, const MainUILaunchSource *source,
                        const char *label, const char *launch, int type, char error[256]);
/* Read the Favorite markers (stars, Add or Remove Favorite) again if their
 * files changed or none are loaded, or always with force. On failure the last
 * markers stay and the next call tries again. */
void mainui_markers_refresh(MainUIApp *ui, bool force);
#endif
