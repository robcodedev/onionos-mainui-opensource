/* SPDX-License-Identifier: GPL-3.0-only
 * Shared entry point for the Onion device launcher and host development preview.
 */
#include "app/loop.h"
#include "app/render.h"
#include "app/screen_events.h"
#include "app/startup.h"
#ifdef main
#undef main
#endif
#include "platform/timing.h"
#include <stdlib.h>

static int run(int argc, char **argv, MainUIApp *ui)
{
    mainui_mark(MAINUI_MARK_ENTRY);
    int result = mainui_setup_session(ui, argc, argv);
    mainui_mark(MAINUI_MARK_SESSION);
    if (result >= 0) {
        return result;
    }
    if (ui->real_device && !ui->snapshot) {
        mainui_timing_handoff("/tmp/mainui-exit");
    }
    result = mainui_setup_video(ui);
    mainui_mark(MAINUI_MARK_VIDEO);
    if (result >= 0) {
        return result;
    }
    mainui_restore_session(ui);
    mainui_mark(MAINUI_MARK_RESTORE);
    mainui_setup_render(ui);
    mainui_mark(MAINUI_MARK_READY);
    while (ui->running) {
        if (!mainui_poll_jobs(ui) || !mainui_reap_jobs(ui)) {
            break;
        }
        mainui_timing_interim(60000);
        mainui_prepare_frame(ui);
        if (ui->draw_frame && !mainui_frame_current(ui, SDL_GetTicks())) {
            struct timespec draw_start = mainui_timing_start();
            bool drawn = mainui_draw_frame(ui);
            /* finish accumulates through mainui_count_add, with no clock reads
             * when logging is disabled. Include rotation and SDL_Flip. */
            mainui_timing_finish("draw-ms", draw_start);
            if (!drawn) {
                continue;
            }
        }
        if (mainui_load_deferred_icons(ui)) {
            ui->idle_tick = false; /* draw the selected console's icon */
            continue;
        }
        SDL_Event event;
        if (!mainui_wait_event(ui, &event)) {
            break;
        }
        mainui_dispatch_event(ui, &event);
    }
    return mainui_teardown(ui);
}

int main(int argc, char **argv)
{
    MainUIApp *ui = calloc(1, sizeof *ui);
    if (!ui) {
        return 3;
    }
    int result = run(argc, argv, ui);
    bool informational = ui->informational;
    free(ui);
    /* Also report early initialization failures; completed teardown reports once.
     * --help and --version print only their text. */
    if (!informational) {
        mainui_mark(MAINUI_MARK_EXIT);
        mainui_timing_report();
    }
    return result;
}
