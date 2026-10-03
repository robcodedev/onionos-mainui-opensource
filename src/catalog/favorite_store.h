/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_FAVORITE_STORE_H
#define MAINUI_FAVORITE_STORE_H
#include "catalog/library.h"
#include "platform/files.h"

/* Private editing transaction. Owns the original bytes and parsed records until
 * close; the model editor may adjust record metadata before commit. */
typedef struct {
    char path[4096];
    char *original;
    cJSON *records;
    MainUIFileLock *lock;
} MainUIFavoriteStore;

bool mainui_favorite_store_open(MainUIFavoriteStore *store, const char *sd);
/* True when committing `library`, freshly loaded from the same files, cannot
 * lose or rewrite anything already in the sidecar. Browsing silently skips
 * records it cannot use (a folder without an id or name, a duplicate id) and
 * moves cyclic, too deep or dangling folders and assignments to the root, so
 * an edit saves those repairs. A repeated assignment is dropped too, which
 * loses nothing only when it is identical to the first. Over-long folder
 * paths are kept as they are. A missing sidecar has nothing to lose, also
 * when the model came from the .bak copy. */
bool mainui_favorite_store_lossless(const MainUIFavoriteStore *store, const MainUILibrary *library);
/* Before an edit saves a repair, copy the damaged sidecar once to
 * favourite-folders.json.damaged, which no later edit replaces. A different
 * earlier copy is kept too: this one then goes to .damaged-<generation>.
 * False, with nothing written, when no copy can be kept. */
/* Requires the store's lock (held from open to close); FAT-safe. */
bool mainui_favorite_store_keep_damaged(const MainUIFavoriteStore *store);
/* Refuse an externally changed source or a competing .writing file. Writes a
 * valid backup before atomic sidecar publication; never writes stock Favorites. */
bool mainui_favorite_store_commit(MainUIFavoriteStore *store, MainUILibrary *library);
void mainui_favorite_store_close(MainUIFavoriteStore *store);
#endif
