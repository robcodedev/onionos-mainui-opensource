/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/catalog_job.h"
#include "catalog/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool cancelled(void *context)
{
    return atomic_load_explicit(&((MainUICatalogJob *)context)->cancel, memory_order_relaxed);
}

static int work(void *context)
{
    MainUICatalogJob *job = context;
    MainUICancel cancel = {cancelled, job};
    bool ok = false;
    if (job->kind == JOB_REFRESH_ALL) {
        ok = mainui_browser_refresh_control(job->sd, job->sensitive, job->error, cancel);
    }
    else if (job->kind == JOB_DISCOVER) {
        MainUICatalog *catalog = calloc(1, sizeof *catalog);
        job->session.catalog = catalog;
        job->session.section = job->section;
        if (catalog) {
            catalog->cancel = cancel;
        }
        ok = catalog && (job->section == MAINUI_MENU_EXPERT
                             ? mainui_catalog_expert(catalog, job->sd, job->sensitive)
                         : job->section == MAINUI_MENU_APPS
                             ? mainui_catalog_apps(catalog, job->sd, job->sensitive)
                             : mainui_catalog_open(catalog, job->sd, job->sensitive));
        if (ok) {
            if (job->section == MAINUI_MENU_APPS) {
                mainui_viewport_restore(&job->session.view, catalog->pages[0].count, 4, 0, 0, 3);
            }
            else {
                mainui_browser_grid_restore(catalog, &job->session.view, 0);
            }
        }
    }
    else {
        ok = mainui_session_restore_control(&job->session, job->sd, job->sensitive, job->rows,
                                            job->resume, job->record, cancel);
        if (ok && job->kind == JOB_ENTER) {
            ok = job->session.catalog &&
                 mainui_browser_enter(job->session.catalog, &job->session.view, job->rows);
        }
        else if (ok && (job->kind == JOB_REFRESH_SYSTEM || job->kind == JOB_REPAIR_SYSTEM)) {
            MainUICatalog *catalog = job->session.catalog;
            bool in_list = catalog && catalog->depth > 0;
            int system = in_list ? catalog->pages[0].view.selected : job->session.view.selected;
            /* Only the console the job was started for. If it could not be
             * found again (gone, or its config unreadable), restoration falls
             * back to the grid, where another console is selected now: that
             * one must not be touched. */
            const MainUIEntry *entry = catalog && system >= 0 && system < catalog->pages[0].count
                                           ? &catalog->pages[0].entries[system]
                                           : NULL;
            if (!entry || !entry->config || strcmp(entry->config, job->target) ||
                in_list != job->target_list) {
                snprintf(job->error, sizeof job->error, "%s",
                         "This console cannot be found now. Open Games again.");
                catalog = NULL;
            }
            /* From the selector, invalidate only; normal entry rebuilds on demand.
             * In a ROM list, rebuild the console root even from a nested folder,
             * then restore against the published cache. */
            while (catalog && catalog->depth) {
                mainui_catalog_back(catalog);
            }
            ok = catalog && (!in_list ? mainui_catalog_remove_cache(catalog, system)
                             : job->kind == JOB_REPAIR_SYSTEM
                                 ? mainui_catalog_repair_cache(catalog, system)
                                 : mainui_catalog_build_cache(catalog, system, true));
            if (ok && in_list) {
                mainui_session_close(&job->session);
                ok = mainui_session_restore_control(&job->session, job->sd, job->sensitive,
                                                    job->rows, job->resume, job->record, cancel);
            }
        }
        else if (ok && job->kind == JOB_SEARCH) {
            ok = mainui_search_open(&job->search, job->session.catalog, &job->session.view,
                                    job->query, job->rows);
        }
    }
    if (!ok && !*job->error) {
        snprintf(job->error, sizeof job->error, "%s",
                 *job->search.error ? job->search.error
                 : job->session.catalog && *job->session.catalog->error
                     ? job->session.catalog->error
                     : "Cannot complete catalog operation.");
    }
    job->success = ok && !cancelled(job);
    atomic_store_explicit(&job->done, true, memory_order_release);
    SDL_Event event = {.type = SDL_USEREVENT};
    SDL_PushEvent(&event);
    return 0;
}

void mainui_catalog_job_cancel(MainUICatalogJob *job)
{
    if (job->thread) {
        atomic_store_explicit(&job->cancel, true, memory_order_relaxed);
    }
}

void mainui_catalog_job_close(MainUICatalogJob *job)
{
    if (job->thread) {
        mainui_catalog_job_cancel(job);
        SDL_WaitThread(job->thread, NULL);
    }
    if (job->suspended_catalog) {
        for (int i = 1; i <= job->suspended_catalog->depth; i++) {
            if (!mainui_cache_resume(job->suspended_catalog->pages[i].cache)) {
                snprintf(job->suspended_catalog->error, sizeof job->suspended_catalog->error,
                         "Cache changed; reopen the console to reload the list");
            }
        }
    }
    mainui_session_close(&job->session);
    mainui_search_close(&job->search);
    cJSON_Delete(job->resume);
    cJSON_Delete(job->record);
    memset(job, 0, sizeof *job);
}

bool mainui_catalog_job_start(MainUICatalogJob *job, MainUIJobKind kind,
                              const MainUILaunchSource *source, const char *sd, bool sensitive,
                              int rows, const char *query, uint64_t generation)
{
    if (job->thread || !sd || strlen(sd) >= sizeof job->sd ||
        (query && strlen(query) >= sizeof job->query)) {
        return false;
    }
    memset(job, 0, sizeof *job);
    atomic_init(&job->cancel, false);
    atomic_init(&job->done, false);
    job->started_at = SDL_GetTicks();
    job->kind = kind;
    job->section = source->section;
    job->generation = generation;
    job->sensitive = sensitive;
    job->rows = rows;
    strcpy(job->sd, sd);
    if (query) {
        strcpy(job->query, query);
    }
    if (kind != JOB_DISCOVER && kind != JOB_REFRESH_ALL) {
        MainUIStack ignored;
        job->resume = mainui_session_snapshot(source->section, source->catalog, source->library,
                                              source->view, source->home, &ignored);
        if (source->catalog && !source->library) {
            job->record = mainui_catalog_record(
                source->catalog, mainui_browser_index(source->catalog, source->view->selected));
        }
        else if (source->library && source->view->selected >= 0 &&
                 source->view->selected < source->library->visible_count &&
                 !mainui_library_is_folder(source->library, source->view->selected)) {
            job->record = cJSON_Duplicate(
                source->library->items[source->library->visible[source->view->selected]].json,
                true);
        }
        if (!job->resume) {
            mainui_catalog_job_close(job);
            return false;
        }
    }
    if ((kind == JOB_REFRESH_SYSTEM || kind == JOB_REPAIR_SYSTEM) && source->catalog) {
        const MainUICatalog *catalog = source->catalog;
        int index = catalog->depth ? catalog->pages[0].view.selected : source->view->selected;
        const MainUIEntry *entry = index >= 0 && index < catalog->pages[0].count
                                       ? &catalog->pages[0].entries[index]
                                       : NULL;
        if (!entry || !entry->config) {
            mainui_catalog_job_close(job);
            return false;
        }
        snprintf(job->target, sizeof job->target, "%s", entry->config);
        job->target_list = catalog->depth > 0;
        job->suspended_catalog = source->catalog;
        for (int i = 1; i <= source->catalog->depth; i++) {
            mainui_cache_suspend(source->catalog->pages[i].cache);
        }
    }
    job->thread = SDL_CreateThread(work, job);
    if (!job->thread) {
        mainui_catalog_job_close(job);
        return false;
    }
    return true;
}

MainUIJobResult mainui_catalog_job_take(MainUICatalogJob *job, uint64_t generation,
                                        MainUISession *session, MainUISearch *search,
                                        char error[256])
{
    if (!job->thread || !atomic_load_explicit(&job->done, memory_order_acquire)) {
        return JOB_WAITING;
    }
    SDL_WaitThread(job->thread, NULL);
    job->thread = NULL;
    MainUIJobResult result = cancelled(job) || generation != job->generation ? JOB_CANCELLED
                             : job->success                                  ? JOB_READY
                                                                             : JOB_FAILED;
    if (result == JOB_READY) {
        if (job->session.catalog) {
            job->session.catalog->cancel = (MainUICancel){0};
        }
        *session = job->session;
        *search = job->search;
        job->session = (MainUISession){0};
        job->search = (MainUISearch){0};
    }
    if (result == JOB_FAILED) {
        snprintf(error, 256, "%s", job->error);
    }
    mainui_catalog_job_close(job);
    return result;
}
