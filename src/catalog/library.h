/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_LIBRARY_H
#define MAINUI_LIBRARY_H
#include "cJSON.h"
#include "core/core.h"
#include "platform/files.h"
#define MAINUI_LIBRARY_LIMIT 10000
#define MAINUI_FOLDER_LIMIT 256

/* Parsed records own their strings. Visible indexes borrow records and remain
 * valid until close; loading and browsing never modify SD-card files. */
typedef struct {
    cJSON *json;
    const char *label, *rom, *launch;
    char *identity;
    int type, folder, order;
} MainUILibraryItem;

typedef struct {
    cJSON *json;
    const char *id, *parent_id, *name;
    int parent, order;
    int direct_games; /* Refreshed with the logical view, never scanned by the renderer. */
} MainUILibraryFolder;

typedef struct {
    MainUILibraryItem *items;
    MainUILibraryFolder folders[MAINUI_FOLDER_LIMIT];
    int count, folder_count, current;
    /* Negative values denote folders (-index-1); INT_MIN denotes parent. */
    int visible[MAINUI_LIBRARY_LIMIT + MAINUI_FOLDER_LIMIT + 1];
    int visible_count;
    int leading_folders, visible_games;
    bool recent;
    MainUIFileStamp source_stamps[3];
    MainUIViewport views[MAINUI_FOLDER_LIMIT + 1];
} MainUILibrary;

/* Startup compatibility: restore Onion's hidden Recent file only when the
 * normal path is absent. Never merge files or replace an existing destination. */
bool mainui_library_restore_recent(const char *sd);

/* Effective Recent identity: Search writes its source launcher before the
 * ROM ("<...>/launch.sh:<rom>"), which counts as that launcher and ROM.
 * Otherwise launch and rompath as written. Borrowed pointers into record;
 * launch is not NUL-terminated at launch_length. */
typedef struct {
    const char *launch, *rom;
    size_t launch_length;
} MainUIRecentIdentity;

MainUIRecentIdentity mainui_recent_identity(const cJSON *record);
/* Same effective launcher and ROM, as Recents shows and removes them. */
bool mainui_recent_same(const cJSON *a, const cJSON *b);
/* The visible row holding the entry `record` names, for restoring a selection.
 * Favorites first match type, launcher, ROM and label exactly, since one ROM
 * can be listed under several labels. Then the reader's identity: launcher and
 * ROM for Recents (Search's prefix normalized); for Favorites the ROM, or type,
 * launcher and label when the ROM is empty. That is used only if exactly one
 * row has it. -1 when not found or ambiguous. */
int mainui_library_find(const MainUILibrary *library, const cJSON *record);
/* Initialize a zeroed model; close it even when loading reports failure. */
bool mainui_library_changed(const MainUILibrary *, const char *sd);
bool mainui_library_open_control(MainUILibrary *, const char *sd, bool recent, MainUICancel);
bool mainui_library_open(MainUILibrary *library, const char *sd, bool recent);
/* Exact persistent ROM-path membership, independent of folder or display label. */
bool mainui_library_contains(const MainUILibrary *library, const char *rom);
bool mainui_library_reload(MainUILibrary *library, const char *sd);
void mainui_library_close(MainUILibrary *library);
const char *mainui_library_label(const MainUILibrary *library, int index);
bool mainui_library_is_folder(const MainUILibrary *library, int index);
bool mainui_library_enter(MainUILibrary *library, int index);
bool mainui_library_back(MainUILibrary *library);
const char *mainui_library_title(const MainUILibrary *library);
#endif
