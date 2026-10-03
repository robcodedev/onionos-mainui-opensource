/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_FAVORITE_EDIT_H
#define MAINUI_FAVORITE_EDIT_H
#include "catalog/library.h"
#include "menus/context.h"

/* Transient cut identity owns key. It survives folder navigation, never process
 * exit or leaving Favorites. Close is safe on a zero-initialized editor. */
typedef struct {
    char *key;
    bool folder;
    int type;
    char error[160];
} MainUIFavoriteEditor;

void mainui_favorite_editor_close(MainUIFavoriteEditor *editor);
bool mainui_favorite_is_cut(const MainUIFavoriteEditor *editor, const MainUILibrary *library,
                            int selected);
/* Apply an action to a freshly loaded model, then publish the sidecar atomically.
 * Folder operations never touch favourite.json or ROM contents. On failure the
 * caller's model/selection remain unchanged until publication succeeds. A reload
 * failure after publication is reported separately: data is saved, but the caller
 * must reopen Favorites. name is required only for naming. On full success the
 * model is reloaded and the affected row selected. */
bool mainui_favorite_edit(MainUIFavoriteEditor *editor, MainUILibrary *library, const char *sd,
                          MainUIContextAction action, const char *name, int *selected);
/* Remove only this saved record's assignment after stock removal succeeds.
 * Failure leaves a harmless orphan assignment for a later retry; never removes
 * another Favorite or alters the stock file. */
bool mainui_favorite_forget_assignment(const char *sd, const cJSON *record);
/* Remove the Favorite row `record` describes (exactly, or by ROM when only one
 * row has it) and, if no other Favorite still uses its folder assignment,
 * that assignment, under one library lock. False if nothing was removed. */
bool mainui_favorite_remove(const char *sd, const cJSON *record);
#endif
