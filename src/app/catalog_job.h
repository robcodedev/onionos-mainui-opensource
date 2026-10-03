/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_CATALOG_JOB_H
#define MAINUI_CATALOG_JOB_H
#include "app/session.h"
#include <SDL.h>
#include <stdatomic.h>

typedef enum {
    JOB_DISCOVER,
    JOB_ENTER,
    JOB_RELOAD,
    JOB_SEARCH,
    JOB_REFRESH_SYSTEM,
    JOB_REPAIR_SYSTEM, /* JOB_REFRESH_SYSTEM in a list, never abandoning a deletion */
    JOB_REFRESH_ALL
} MainUIJobKind;

typedef enum {
    JOB_WAITING,
    JOB_READY,
    JOB_CANCELLED,
    JOB_FAILED
} MainUIJobResult;

typedef struct {
    SDL_Thread *thread;
    Uint32 started_at;
    atomic_bool cancel, done;
    uint64_t generation;
    MainUIJobKind kind;
    MainUIMenuSection section;
    bool sensitive, success;
    int rows;
    char sd[4096], query[128], error[256];
    /* Refresh and repair: the console they were started for (its config
     * file), and whether from inside its ROM list. */
    char target[4096];
    bool target_list;
    cJSON *resume, *record;
    MainUISession session;
    MainUISearch search;
    MainUICatalog *suspended_catalog; /* Borrowed until the job is joined/closed. */
} MainUICatalogJob;

/* Input snapshots are copied before dispatch. For console refresh, the source
 * catalog is borrowed until close/take: its readers are suspended on the UI thread
 * and resumed after joining. The caller must keep it alive and prevent navigation.
 * The worker owns its SQLite connections
 * exclusively. A completed model is transferred only after joining the worker. */
bool mainui_catalog_job_start(MainUICatalogJob *job, MainUIJobKind kind,
                              const MainUILaunchSource *source, const char *sd, bool sensitive,
                              int rows, const char *query, uint64_t generation);
void mainui_catalog_job_cancel(MainUICatalogJob *job);
MainUIJobResult mainui_catalog_job_take(MainUICatalogJob *job, uint64_t generation,
                                        MainUISession *session, MainUISearch *search,
                                        char error[256]);
void mainui_catalog_job_close(MainUICatalogJob *job);
#endif
