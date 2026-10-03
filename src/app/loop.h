/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_LOOP_H
#define MAINUI_LOOP_H
#include "app/app_context.h"

/* False ends the loop. Poll handles device shutdown; reap handles launch;
 * wait reports SDL event failures through ui->status. */
bool mainui_poll_jobs(MainUIApp *ui);
bool mainui_reap_jobs(MainUIApp *ui);
/* Next status/animation wake, including the delayed Loading panel. */
int mainui_wait_interval(const MainUIApp *ui, Uint32 now);
/* A scrolling title is past (or within one frame of) its scroll delay. */
bool mainui_marquee_moving(const MainUIApp *ui, Uint32 now);
bool mainui_wait_event(MainUIApp *ui, SDL_Event *event);
/* SDL_USEREVENT code of the periodic tick; workers post code 0. */
#define MAINUI_TICK_CODE 1
#endif
