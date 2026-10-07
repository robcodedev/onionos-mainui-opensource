/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_SAVED_ACTIONS_H
#define MAINUI_SAVED_ACTIONS_H
#include "cJSON.h"
#include <stdbool.h>

typedef enum {
    SAVED_ADD,
    SAVED_REMOVE,
    SAVED_CLEAR
} MainUISavedAction;

/* Edits one stock NDJSON list while retaining untouched records byte-for-byte.
 * Selected records use stable ROM/launcher identities, never a viewport index. */
bool mainui_saved_action(const char *sd, bool recent, MainUISavedAction action,
                         const cJSON *record);
/* The same with Roms/.mainui-library already held by the caller. */
bool mainui_saved_action_locked(const char *sd, bool recent, MainUISavedAction action,
                                const cJSON *record);
/* Prepend a committed game launch, promote duplicates, retain at most 50. */
bool mainui_recent_add(const char *sd, const cJSON *record);
/* For a Y launch: Onion's Game List Options removes the first line of the
 * Recents list, taken to be this launch, so the record's line goes first and
 * the list's lines stay as they are, duplicates and all; Recents skips a
 * repeated game when it reads the list. Fails, writing nothing, when the
 * list cannot be read or would pass 8 MiB. */
bool mainui_recent_push(const char *sd, const cJSON *record);
#endif
