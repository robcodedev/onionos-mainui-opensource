/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_AUDIO_H
#define MAINUI_AUDIO_H
#include <stdbool.h>

/* SDL_mixer is loaded at runtime so host previews do not require its optional
 * decoder DLLs. All functions run on the SDL/UI thread; close before SDL_Quit.
 * Open returns false on unavailable audio, with no partially owned resources. */
bool mainui_audio_open(const char *theme, const char *fallback, int volume);
void mainui_audio_volume(int volume); /* Onion menu volume: 0..20. */
void mainui_audio_change(void);       /* One navigation sample; music keeps looping. */
void mainui_audio_close(void);
bool mainui_audio_available(void);
void mainui_audio_pause(bool paused);
/* Diagnostics: change samples requested since start, counted even without
 * audio, and the menu volume (0..20, or -1 before any) requested before the
 * latest one. Lets tests check sound order without a mixer. */
unsigned mainui_audio_change_requests(int *volume);
#endif
