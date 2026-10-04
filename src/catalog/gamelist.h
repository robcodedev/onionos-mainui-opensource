/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_GAMELIST_H
#define MAINUI_GAMELIST_H
#include "core/core.h"
#include "sqlite3.h"
#include <stdbool.h>
/* Import a present ROM-root miyoogamelist.xml into the caller's transaction.
 * root is the normalized host ROM directory; saved_root is its verbatim stock
 * prefix. Borrows all arguments. Never commits or publishes the database.
 * The XML is read leniently, as stock reads it. A missing file, or one whose
 * content is unusable (over 16 MiB, or no usable <gameList>; logged), returns
 * true with *present=false and inserts nothing: the caller lists the ROM
 * files instead. A file that cannot be opened or read (I/O, permission) is
 * logged and returns false, so the previous cache is kept. Also returns false
 * for allocation, SQL, bounds or cancellation errors; the caller must roll
 * back. This does not read the game-detail gamelist.xml.
 */
bool mainui_gamelist_import(sqlite3 *database, sqlite3_stmt *insert, const char *sd,
                            const char *root, const char *saved_root, bool *present);

bool mainui_gamelist_import_control(sqlite3 *database, sqlite3_stmt *insert, const char *sd,
                                    const char *root, const char *saved_root, bool *present,
                                    MainUICancel cancel);

/* Independent game-detail data; never stored in cache6.db.
 * Strings are owned inline. Missing or unusable XML clears the complete result.
 * The bounded XML subset is shared with import, not the database transaction. */
typedef struct {
    char genre[4096], rating[8], description[4096];
    bool found;
} MainUIMetadata;

/* Borrows normalized paths. Try the ROM directory first, then the given console
 * root on missing XML or no match. NULL root disables fallback.
 * Returns false on missing/invalid XML; a valid XML without a match returns true. */
/* Release the UI-thread metadata index cache at shutdown. */
void mainui_gamelist_metadata_close(void);
#ifdef MAINUI_METADATA_TEST
void mainui_gamelist_metadata_stats(size_t *builds, size_t *reads, size_t *bytes);
#endif
bool mainui_gamelist_metadata(const char *rom, const char *root, MainUIMetadata *result);
#endif
