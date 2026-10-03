/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_CACHE_H
#define MAINUI_CACHE_H
#include "catalog/catalog.h"
#define MAINUI_CACHE_WINDOW 64
/* One read-only connection and prepared query per open folder. No user/cache
 * writes, extension loading or schema repair. Close after releasing row data. */
typedef struct MainUICache MainUICache;
/* All arguments must be non-NULL NUL-terminated strings or output pointers.
 * parent is the unmodified cache key (root is "."). Success transfers ownership
 * of *out and sets *total. Failure sets *out=NULL and leaves *total unchanged.
 */
bool mainui_cache_open(MainUICache **out, const char *file, const char *table, const char *parent,
                       const char *sd, const char *romroot, bool sensitive, int *total);
/* Returns owned rows on success; on failure out stays zeroed and *loaded=0.
 * Rows own exact stored ROM/image strings plus resolved host paths; folder
 * query keys are derived relative to the ROM root for stock absolute rows.
 * Offset refers to the full ordered list. At most 64 rows are materialized. */
bool mainui_cache_window(MainUICache *cache, int offset, MainUIEntry out[MAINUI_CACHE_WINDOW],
                         int *loaded);
/* After a failed window: true only for damaged content (a row the reader
 * rejects, or SQLite corruption). Busy, I/O, memory, interrupted or changed-
 * cache failures are false: replacing the cache would not be justified. */
bool mainui_cache_damaged(const MainUICache *cache);
/* Finalize queries and close the connection; NULL is allowed. */
void mainui_cache_close(MainUICache *cache);
/* Temporarily release file handles while retaining query identity and row counts.
 * Caller must prevent paging until resume. Resume refuses changed files so stale
 * counts cannot be combined with a newly published cache; reopen the catalog then.
 * NULL is accepted. Objects remain owned by the caller throughout. */
void mainui_cache_suspend(MainUICache *cache);
bool mainui_cache_resume(MainUICache *cache);
/* Count from the same parent query as pagination; no extra I/O during drawing. */
int mainui_cache_folder_count(const MainUICache *cache);
bool mainui_cache_changed(MainUICache *cache);
#endif
