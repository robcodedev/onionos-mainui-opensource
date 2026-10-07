/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_SESSION_H
#define MAINUI_SESSION_H
#include "app/browser.h"
#include "app/search.h"
#include "catalog/library.h"
#include "menus/menu.h"
/* Snapshot borrows models and returns owned JSON plus the legacy two-frame
 * section stack consumed by setState. Stable paths/IDs augment, never replace,
 * that wire format for this implementation's nested return restoration. */
cJSON *mainui_session_snapshot(MainUIMenuSection section, MainUICatalog *catalog,
                               const MainUILibrary *library, const MainUIViewport *view,
                               const MainUIViewport *home, MainUIStack *legacy);

typedef struct {
    MainUIMenuSection section;
    MainUICatalog *catalog;
    MainUILibrary *library;
    MainUIViewport view, home;
    bool home_only;
} MainUISession;

/* Build isolated replacement models. Invalid snapshots fail without touching
 * the active UI. Removed folders fall back to the nearest surviving ancestor;
 * selection is matched by ROM identity, then normalized numeric position. */
bool mainui_session_restore(MainUISession *session, const char *sd, bool sensitive, int rows,
                            const cJSON *resume, const cJSON *record);
bool mainui_session_restore_control(MainUISession *session, const char *sd, bool sensitive,
                                    int rows, const cJSON *resume, const cJSON *record,
                                    MainUICancel cancel);
void mainui_session_close(MainUISession *session);

typedef struct {
    MainUIMenuSection section;
    MainUICatalog *catalog;
    const MainUILibrary *library;
    const MainUIViewport *view, *home;
    const MainUISearch *search;
    const char *sd;
    bool home_only;
    bool alternate; /* Y: Onion's Game List Options follows the launch */
} MainUILaunchSource;

/* Optional record overrides the selected row for registered context launchers.
 * Borrows every argument; obtains/frees its own selected-record copy otherwise. */
bool mainui_session_launch(const char *directory, const MainUILaunchSource *source,
                           const cJSON *record, char error[256]);
#endif
