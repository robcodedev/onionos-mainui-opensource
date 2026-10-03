/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/loop.h"
#include "app/render.h"
#include "platform/audio.h"
#include "platform/input.h"
#include "platform/launch.h"
#include "platform/system_config.h"
#include "platform/timing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Uint32 timer_tick(Uint32 interval, void *unused)
{
    (void)unused;
    /* SDL invokes this on its timer thread. Only enqueue an event here;
     * font access, rendering and application-state mutation stay on the UI thread.
     */
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = SDL_USEREVENT;
    event.user.code = MAINUI_TICK_CODE;
    /* Slow frames must not fill SDL's bounded queue with repaint requests:
     * queued timer events can otherwise crowd out physical button presses. */
    if (SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_EVENTMASK(SDL_USEREVENT)) == 0) {
        event.type = SDL_USEREVENT;
        SDL_PushEvent(&event);
    }
    return interval;
}

bool mainui_poll_jobs(MainUIApp *ui)
{
    mainui_about_update(&ui->about_job, &ui->settings_page);
    ui->about_visible = ui->settings_page.open && ui->settings_page.kind == SET_ABOUT;
    if (ui->device_enabled && ui->about_visible && !ui->about_job.visible &&
        !ui->about_job.thread) {
        mainui_about_start(&ui->about_job, &ui->device_adapter, ui->sd);
    }
    ui->about_job.visible = ui->about_visible;
    bool wifi_visible = ui->settings_page.open && ui->settings_page.kind == SET_WIFI;
    if (wifi_visible && !ui->wifi_was_visible) {
        ui->next_wifi_scan = SDL_GetTicks();
    }
    ui->wifi_was_visible = wifi_visible;
    if (ui->device_enabled) {
        bool resumed = false, quit = false, status_ready = false;
        if (ui->real_device) {
            mainui_device_signals_take(&resumed, &quit);
        }
        if (quit) {
            ui->running = false;
            return false;
        }
        if (ui->device_job.thread) {
            int completed = mainui_device_job_take(&ui->device_job);
            int operation = ui->device_job.operation;
            if (completed >= 0) {
                status_ready = true;
                if (operation == 3 || operation == 4) {
                    ui->next_wifi_scan = SDL_GetTicks() + 3000;
                }
            }
            if (completed >= 0 && operation && ui->settings_page.open &&
                ui->settings_page.kind == SET_WIFI) {
                ui->settings_page.message[0] = 0;
                if (!completed && *ui->device_job.adapter.error) {
                    fprintf(stderr, "Wi-Fi: %s\n", ui->device_job.adapter.error);
                }
                if (completed && operation == 4) {
                    mainui_settings_page_scan_results(&ui->settings_page,
                                                      ui->device_adapter.runtime);
                }
                if (completed && operation == 5 && ui->settings_page.wifi) {
                    ui->next_wifi_scan = SDL_GetTicks();
                }
            }
        }
        if (wifi_visible && ui->settings_page.wifi && !ui->device_job.thread &&
            (Sint32)(SDL_GetTicks() - ui->next_wifi_scan) >= 0) {
            if (mainui_device_job_start(&ui->device_job, &ui->device_adapter, 4, NULL, NULL)) {
                ui->next_wifi_scan = SDL_GetTicks() + 5500;
                if (!ui->settings_page.network_count) {
                    snprintf(ui->settings_page.message, sizeof ui->settings_page.message,
                             "Scanning...");
                }
            }
        }
        Uint32 now = SDL_GetTicks();
        Uint32 status_interval =
            ui->settings_open || ui->settings_page.open || ui->language_open ? 1000 : 5000;
        /* Entering Settings must shorten an already scheduled background poll. */
        if ((Sint32)(ui->next_device_check - now) > (Sint32)status_interval) {
            ui->next_device_check = now + status_interval;
        }
        bool refresh_due = resumed || (Sint32)(now - ui->next_device_check) >= 0;
        if (status_ready || refresh_due) {
            if (refresh_due) {
                ui->next_device_check = now + status_interval;
            }
            int previous_sleep = ui->device_status.sleeping;
            ui->device_status = mainui_device_status(&ui->device_adapter);
            if (!ui->battery_override) {
                ui->theme.battery_percent = ui->device_status.battery;
            }
            if (resumed || previous_sleep != ui->device_status.sleeping) {
                ui->active_at = now;
                mainui_audio_pause(!resumed && ui->device_status.sleeping == 1);
                memset(ui->held, 0, sizeof ui->held);
                SDL_EnableKeyRepeat(0, 0);
                SDL_Event stale[16];
                for (int drained = 0; drained < 256; drained += 16) {
                    if (SDL_PeepEvents(stale, 16, SDL_GETEVENT, SDL_KEYDOWNMASK | SDL_KEYUPMASK) <=
                        0) {
                        break;
                    }
                }
                SDL_EnableKeyRepeat(ui->config.repeat_delay, ui->config.repeat_interval);
                ui->letter_jump.active = false;
                ui->search_confirm_held = ui->search.release_pending = false;
                ui->next_catalog_check = 0;
            }
            if (refresh_due && ui->real_device && !ui->device_job.thread && !ui->snapshot) {
                cJSON *system = mainui_system_read(ui->sd);
                const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(system, "wifi");
                bool wifi_disabled = cJSON_IsNumber(wifi) && wifi->valuedouble == 0;
                cJSON_Delete(system);
                if (!wifi_disabled) {
                    mainui_device_job_start(&ui->device_job, &ui->device_adapter, 0, NULL, NULL);
                }
                else {
                    ui->device_status.wifi = 0;
                    ui->device_status.wifi_signal = 0;
                    ui->device_status.address[0] = ui->device_status.ssid[0] = 0;
                }
            }
        }
        ui->theme.wifi_online = ui->device_status.wifi == 1;
        ui->theme.wifi_signal_level = ui->device_status.wifi_signal;
        snprintf(ui->theme.wifi_address, sizeof ui->theme.wifi_address, "%s",
                 ui->device_status.wifi == 1 ? ui->device_status.address : "");
        if (ui->settings_page.open) {
            snprintf(ui->settings_page.device_status, sizeof ui->settings_page.device_status,
                     "%s%s",
                     ui->device_status.wifi == 1   ? "Connected: "
                     : ui->device_status.wifi == 0 ? "Wi-Fi disconnected"
                                                   : "Wi-Fi status unavailable",
                     ui->device_status.wifi == 1 ? ui->device_status.ssid : "");
            ui->settings_page.wifi_connection = ui->device_status.wifi;
            snprintf(ui->settings_page.connected_ssid, sizeof ui->settings_page.connected_ssid,
                     "%s", ui->device_status.wifi == 1 ? ui->device_status.ssid : "");
            ui->settings_page.audio_available = mainui_audio_available();
            ui->settings_page.sleeping = ui->device_status.sleeping;
        }
    }
    return true;
}

bool mainui_reap_jobs(MainUIApp *ui)
{
    if (ui->launch_pending && !ui->snapshot) {
        mainui_mark(MAINUI_MARK_HANDOFF);
        return false;
    }
    if (ui->catalog_job.thread && ui->snapshot && ui->input_script && *ui->input_script == 'C') {
        ui->input_script++;
        mainui_catalog_job_cancel(&ui->catalog_job);
        ui->catalog_generation++;
    }
    if (ui->catalog_job.thread) {
        MainUIJobKind kind = ui->catalog_job.kind;
        MainUISession completed = {0};
        MainUISearch found = {0};
        MainUIJobResult result = mainui_catalog_job_take(&ui->catalog_job, ui->catalog_generation,
                                                         &completed, &found, ui->message_body);
        if (result == JOB_READY && kind == JOB_SEARCH) {
            if (ui->reload_search && ui->search.results) {
                cJSON *saved = cJSON_CreateObject();
                cJSON_AddNumberToObject(saved, "currpos", ui->view.selected);
                cJSON_AddNumberToObject(saved, "pagestart", ui->view.start);
                cJSON_AddNumberToObject(saved, "pageend", ui->view.end);
                const cJSON *selected =
                    ui->view.selected >= 0 && ui->view.selected < ui->search.results->count
                        ? ui->search.results->items[ui->view.selected].json
                        : NULL;
                mainui_search_restore_view(&found, saved, selected, ui->config.rows);
                found.postgame = ui->search.postgame;
                cJSON_Delete(saved);
            }
            if (completed.catalog) {
                MainUICatalog **target =
                    completed.section == MAINUI_MENU_EXPERT ? &ui->expert : &ui->games;
                if (*target) {
                    mainui_catalog_close(*target);
                    free(*target);
                }
                *target = ui->catalog = completed.catalog;
                completed.catalog = NULL;
            }
            mainui_search_close(&ui->search);
            ui->search = found;
            found = (MainUISearch){0};
            ui->library = ui->search.results;
            ui->view = ui->search.view;
            ui->search.release_pending = ui->search_confirm_held;
            if (!ui->reload_search) {
                mainui_launch_clear_search(ui->handoff_dir);
            }
            ui->search_keyboard = false;
            mainui_name_input_close(&ui->name_input);
        }
        else if (result == JOB_READY && completed.catalog) {
            if (completed.section == MAINUI_MENU_APPS) {
                if (ui->apps) {
                    mainui_catalog_close(ui->apps);
                    free(ui->apps);
                }
                ui->apps = completed.catalog;
                ui->apps_view = completed.view;
                ui->home = true;
            }
            else {
                MainUICatalog **target =
                    completed.section == MAINUI_MENU_EXPERT ? &ui->expert : &ui->games;
                if (*target) {
                    mainui_catalog_close(*target);
                    free(*target);
                }
                *target = completed.catalog;
                ui->catalog = completed.catalog;
                ui->view = completed.view;
                if (completed.section == MAINUI_MENU_EXPERT) {
                    ui->expert_view = ui->view;
                }
                else {
                    ui->games_view = ui->view;
                }
                ui->home = false;
            }
            completed.catalog = NULL;
        }
        else if (result == JOB_READY && completed.library && kind == JOB_MARKERS) {
            if (ui->favorites) {
                mainui_library_close(ui->favorites);
                free(ui->favorites);
            }
            ui->favorites = completed.library;
            completed.library = NULL;
        }
        else if (result == JOB_READY && completed.library) {
            if (ui->library) {
                mainui_library_close(ui->library);
                free(ui->library);
            }
            ui->library = completed.library;
            completed.library = NULL;
            ui->view = completed.view;
        }
        bool recovering = ui->page_recovery && (kind == JOB_RELOAD || kind == JOB_REFRESH_SYSTEM);
        if (recovering && result == JOB_CANCELLED) {
            /* Back during recovery leaves the list instead of starting the
             * next step; it reopens at its top. */
            ui->page_recovery = 0;
            if (ui->catalog && !ui->library) {
                mainui_viewport_restore(&ui->view, ui->view.total, ui->config.rows, 0, 0,
                                        ui->config.rows - 1);
                mainui_browser_back(ui->catalog, &ui->view);
            }
        }
        if (result == JOB_FAILED && recovering) {
            /* The next recovery step follows when the page is drawn again. */
            fprintf(stderr, "%s\n", ui->message_body);
            ui->message_body[0] = 0;
        }
        else if (result == JOB_FAILED) {
            if (kind == JOB_SEARCH && ui->name_input.open) {
                snprintf(ui->name_input.error, sizeof ui->name_input.error, "%.159s",
                         ui->message_body);
            }
            else {
                snprintf(ui->message_title, sizeof ui->message_title, "Catalog unavailable");
            }
        }
        if (result != JOB_WAITING) {
            ui->preview_sync_once = true;
            ui->selected_at = SDL_GetTicks();
            ui->next_catalog_check = SDL_GetTicks() + 5000;
            ui->cached_start = -1;
            ui->menu_view.cached_start = -1;
            /* Keep the thumbnail LRU across catalog replacements and list entry. */
            if (ui->heading) {
                SDL_FreeSurface(ui->heading);
            }
            ui->heading = TTF_RenderUTF8_Blended(ui->theme.title_font,
                                                 ui->search.results ? ui->search.title
                                                 : ui->library ? mainui_library_heading(ui->library)
                                                 : ui->catalog ? mainui_catalog_heading(ui->catalog)
                                                               : "Games",
                                                 ui->heading_color);
        }
        mainui_session_close(&completed);
        mainui_search_close(&found);
    }
    if (!ui->catalog_job.thread && !ui->snapshot && !ui->name_input.open && !ui->context_open &&
        ui->confirmation < 0 && !ui->settings_open &&
        (Sint32)(SDL_GetTicks() - ui->next_catalog_check) >= 0) {
        ui->next_catalog_check = SDL_GetTicks() + 5000;
        if (ui->library && !ui->search.results && mainui_library_changed(ui->library, ui->sd)) {
            MainUILaunchSource source = {.section = ui->library->recent ? MAINUI_MENU_RECENTS
                                                                        : MAINUI_MENU_FAVORITES,
                                         .library = ui->library,
                                         .view = &ui->view,
                                         .home = &ui->home_view};
            mainui_catalog_job_start(&ui->catalog_job, JOB_RELOAD, &source, ui->sd,
                                     ui->config.case_sensitive, ui->config.rows, NULL,
                                     ++ui->catalog_generation);
        }
        else if (ui->favorites && mainui_library_changed(ui->favorites, ui->sd)) {
            MainUILaunchSource source = {.section = MAINUI_MENU_FAVORITES};
            mainui_catalog_job_start(&ui->catalog_job, JOB_MARKERS, &source, ui->sd,
                                     ui->config.case_sensitive, ui->config.rows, NULL,
                                     ++ui->catalog_generation);
        }
        MainUICatalog *active = ui->apps                                            ? ui->apps
                                : !ui->home && (!ui->library || ui->search.results) ? ui->catalog
                                                                                    : NULL;
        if (active && !active->depth) {
            active->pages[0].view = ui->apps ? ui->apps_view : ui->view;
        }
        if (!ui->catalog_job.thread && active && mainui_catalog_changed(active)) {
            MainUILaunchSource source = {.section = ui->apps                    ? MAINUI_MENU_APPS
                                                    : ui->catalog == ui->expert ? MAINUI_MENU_EXPERT
                                                                                : MAINUI_MENU_GAMES,
                                         .catalog = active,
                                         .view = ui->search.results ? &ui->search.source_view
                                                 : ui->apps         ? &ui->apps_view
                                                                    : &ui->view,
                                         .home = &ui->home_view};
            ui->reload_search = ui->search.results != NULL;
            mainui_details_close(&ui->details);
            ui->letter_jump.active = false;
            mainui_catalog_job_start(&ui->catalog_job, ui->reload_search ? JOB_SEARCH : JOB_RELOAD,
                                     &source, ui->sd, ui->config.case_sensitive, ui->config.rows,
                                     ui->reload_search ? ui->search.query : NULL,
                                     ++ui->catalog_generation);
        }
    }
    return true;
}

bool mainui_marquee_moving(const MainUIApp *ui, Uint32 now)
{
    Uint32 start = ui->selected_at + (Uint32)ui->config.scroll_delay;
    return ui->animate && !ui->letter_jump.active &&
           (Sint32)(now - start) >= -MAINUI_MARQUEE_FRAME_MS;
}

int mainui_wait_interval(const MainUIApp *ui, Uint32 now)
{
    /* Workers post completion events; only visible animation needs fast ticks.
     * Keep letter-jump responsive independently of the marquee frame rate. */
    int interval = ui->letter_jump.active           ? 17
                   : mainui_marquee_moving(ui, now) ? MAINUI_MARQUEE_FRAME_MS
                   : ui->device_enabled             ? 500
                                                    : 5000;
    /* A long title still in its scroll delay keeps normal maintenance ticks,
     * but wakes exactly when it starts to move. */
    if (ui->animate && !ui->letter_jump.active && interval > MAINUI_MARQUEE_FRAME_MS) {
        Sint32 until = (Sint32)(ui->selected_at + (Uint32)ui->config.scroll_delay - now);
        if (until > 0 && until < interval) {
            interval = (int)until;
        }
    }
    if (ui->catalog_job.thread) {
        Uint32 elapsed = now - ui->catalog_job.started_at;
        if (elapsed < 500 && (Uint32)interval > 500 - elapsed) {
            interval = (int)(500 - elapsed);
        }
    }
    return interval;
}

/* SDL 1.2's timer and SDL_WaitEvent both poll on the 10 ms kernel tick, so
 * marquee frames arrived 20-60 ms apart and the title moved in uneven jumps.
 * While a title scrolls, sleep to fixed deadlines 40 ms (four ticks) apart.
 * On the device every wake lands just after a tick, so the first deadline,
 * 35 ms after a wake, and all later ones sit mid-interval, clear of tick
 * boundaries: each frame then wakes exactly 40 ms after the previous one. */
static void paced_wait(MainUIApp *ui, SDL_Event *event)
{
    Uint32 now = SDL_GetTicks();
    if (ui->marquee_origin != ui->selected_at) {
        ui->marquee_origin = ui->selected_at;
        ui->marquee_steps = 0;
        ui->marquee_paced = false;
    }
    Uint32 start = ui->selected_at + (Uint32)ui->config.scroll_delay;
    if (!ui->marquee_paced || (Sint32)(now - ui->marquee_due) > MAINUI_MARQUEE_FRAME_MS) {
        ui->marquee_due = now + MAINUI_MARQUEE_FRAME_MS - 5;
        ui->marquee_paced = true;
    }
    for (;;) {
        if (SDL_PollEvent(event)) {
            return;
        }
        Sint32 remaining = (Sint32)(ui->marquee_due - SDL_GetTicks());
        if (remaining <= 0) {
            ui->marquee_due += MAINUI_MARQUEE_FRAME_MS;
            if ((Sint32)(SDL_GetTicks() - start) >= 0) {
                ui->marquee_steps++;
            }
            memset(event, 0, sizeof *event);
            event->type = SDL_USEREVENT;
            event->user.code = MAINUI_TICK_CODE;
            return;
        }
        /* Input slices end before the deadline even on a coarse kernel tick;
         * one final sleep then targets the deadline itself. Input latency
         * stays under 30 ms. */
        SDL_Delay(remaining > 20 ? 10 : (Uint32)remaining);
    }
}

static SDL_TimerID add_timer(Uint32 interval)
{
#ifdef MAINUI_TEST_FAULTS
    if (getenv("MAINUI_TEST_TIMER_FAILURE")) {
        return NULL;
    }
#endif
    return SDL_AddTimer(interval, timer_tick, NULL);
}

/* Without the SDL timer, SDL_WaitEvent would block until input arrives and
 * battery, Wi-Fi and catalog checks would stop. Wait on a deadline instead
 * and post the tick ourselves; the timer is retried on the next wait. */
static void deadline_wait(MainUIApp *ui, SDL_Event *event, int interval)
{
    if (!ui->timer_failure_logged) {
        fprintf(stderr, "SDL_AddTimer failed (%s); waking on a %d ms deadline\n", SDL_GetError(),
                interval);
        ui->timer_failure_logged = true;
    }
    Uint32 deadline = SDL_GetTicks() + (Uint32)interval;
    for (;;) {
        if (SDL_PollEvent(event)) {
            return;
        }
        Sint32 remaining = (Sint32)(deadline - SDL_GetTicks());
        if (remaining <= 0) {
            memset(event, 0, sizeof *event);
            event->type = SDL_USEREVENT;
            event->user.code = MAINUI_TICK_CODE;
            return;
        }
        SDL_Delay(remaining > 10 ? 10 : (Uint32)remaining);
    }
}

bool mainui_wait_event(MainUIApp *ui, SDL_Event *event)
{
    int interval = mainui_wait_interval(ui, SDL_GetTicks());
    /* Paced frames only while the title moves; the scroll delay uses the normal
     * wait, so maintenance keeps running however long the delay is. */
    bool paced = !ui->input_script && mainui_marquee_moving(ui, SDL_GetTicks());
    if (ui->timer && (paced || ui->timer_interval != interval)) {
        SDL_RemoveTimer(ui->timer);
        ui->timer = NULL;
    }
    if (!paced) {
        ui->marquee_paced = false;
    }
    if (!ui->timer && !paced) {
        ui->timer_interval = interval;
        ui->timer = add_timer((Uint32)interval);
    }
    bool scripted_event = false;
    memset(event, 0, sizeof *event);
    if (ui->input_script &&
        ((ui->catalog_job.thread && *ui->input_script != 'C') || ui->device_job.thread)) {
        SDL_Delay(1);
        event->type = SDL_USEREVENT;
    }
    else if (ui->input_script && ui->letter_jump.active) {
        event->type = SDL_USEREVENT;
    }
    else if (ui->input_script && *ui->input_script) {
        scripted_event = true;
        event->type = SDL_KEYDOWN;
        if (*ui->input_script == '-') {
            event->type = SDL_KEYUP;
            ui->input_script++;
        }
        event->key.keysym.sym =
            *ui->input_script == 'C' ? SDLK_ESCAPE : mainui_input_script(*ui->input_script);
        if (*ui->input_script) {
            ui->input_script++;
        }
        if (event->key.keysym.sym == SDLK_UNKNOWN) {
            fprintf(stderr, "Invalid --input action\n");
            ui->status = 2;
            ui->running = false;
        }
    }
    else if (paced) {
        paced_wait(ui, event);
    }
    else if (!ui->timer) {
        deadline_wait(ui, event, interval);
    }
    else if (!SDL_WaitEvent(event)) {
        ui->status = 4;
        return false;
    }
    if (!scripted_event && (ui->handheld_input || ui->real_device)) {
        mainui_input_device_event(event);
    }
    ui->idle_tick = event->type == SDL_USEREVENT && (event->user.code == MAINUI_TICK_CODE ||
                                                     event->user.code == MAINUI_STATUS_CODE);
    if (!ui->idle_tick) {
        ui->active_at = SDL_GetTicks();
    }
    return true;
}
