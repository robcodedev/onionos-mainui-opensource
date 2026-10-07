/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_RENDER_H
#define MAINUI_RENDER_H
#include "app/app_context.h"

/* JSON objects own the label strings; labels[] merely borrows them until
 * close_list. The source buffer and all parsed records belong to this list.
 */
struct MainUIPreviewList {
    char *data;
    cJSON *records[10000];
    const char *labels[10000];
    int count;
};

bool mainui_load_list(MainUIPreviewList *list, const char *path);
void mainui_close_list(MainUIPreviewList *list);
const char *mainui_list_label_at(void *context, int index);
const char *mainui_library_heading(const MainUILibrary *library);
const char *mainui_catalog_heading(const MainUICatalog *catalog);
void mainui_prepare_frame(MainUIApp *ui);
/* False skips the wait/dispatch phase. Snapshot completion or a fatal render
 * error also clears running; starting a catalog reload leaves it set. */
bool mainui_draw_frame(MainUIApp *ui);
/* True when an idle tick would repaint exactly the presented frame, so the
 * draw, rotation and flip can be skipped. Conservative: see render.c. */
bool mainui_frame_current(const MainUIApp *ui, Uint32 now);
/* After a frame of the console grid, while no input waits, decode the
 * selected icons the page left for later, one at a time. True when the
 * selected console's icon changed and the frame must be drawn again. */
bool mainui_load_deferred_icons(MainUIApp *ui);
/* True when only the selected row's marquee moved since the presented frame,
 * so a frame can recompose just that row. */
bool mainui_frame_marquee_only(const MainUIApp *ui, Uint32 now);
#endif
