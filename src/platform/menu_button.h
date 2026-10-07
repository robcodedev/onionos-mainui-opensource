/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_MENU_BUTTON_H
#define MAINUI_MENU_BUTTON_H
#include <stdbool.h>

/* SDL_USEREVENT code posted for each release of the Menu button read from
 * the input device. */
#define MAINUI_MENU_RELEASE_CODE 0x4d454e55

/* Onion's keymon opens MainUI's context menu on a long press of Menu by
 * sending only a release of Menu (KEY_ESC, value 0), with no press before it
 * (src/keymon/menuButtonAction.h). SDL 1.2 drops a release of a key it never
 * saw pressed, so on the device a thread reads the input device itself and
 * posts MAINUI_MENU_RELEASE_CODE for every Menu release. Reading does not
 * consume events: SDL still reads all of them. False when the device cannot
 * be opened (then nothing is posted). */
bool mainui_menu_button_open(const char *device);
/* True when the reader saw a Menu event (press, repeat or release) in the
 * last window_ms. After its long-press release keymon sends an L1 press and
 * an L1 release ("quietMainUI" in src/keymon/menuButtonAction.h), each by a
 * separate `sendkeys`, which syncs the SD card, so they can come long after
 * the release and L1 can stay down long enough to repeat. It sends them again
 * for every repeat of Menu while Menu stays down, and once more when Menu is
 * released. Stock ignores them; see screen_events.c. */
bool mainui_menu_button_recent(unsigned window_ms);
/* Count one Menu event of the given value (0 release, 1 press, 2 repeat) now,
 * as the reader does for each it reads; for the reader and for tests. Any
 * thread. */
void mainui_menu_button_record(int value);
/* Menu events and Menu releases read since the reader opened. Any thread. */
int mainui_menu_button_events(void);
int mainui_menu_button_releases(void);
/* Stops and joins the reader; safe when it never started. */
void mainui_menu_button_close(void);
#endif
