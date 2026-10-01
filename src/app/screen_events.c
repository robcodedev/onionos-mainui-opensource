/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/screen_events.h"
#include "app/positions.h"
#include "app/render.h"
#include "catalog/saved_actions.h"
#include "menus/favorite_context.h"
#include "platform/audio.h"
#include "platform/device_request.h"
#include "platform/input.h"
#include "platform/launch.h"
#include "platform/timing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool mainui_launch_tool(const char *directory, const char *sd, const MainUILaunchSource *source,
                        const char *label, const char *launch, int type, char error[256])
{
    char resolved[4096];
    FILE *file = mainui_catalog_path(resolved, sd, sd, launch) ? fopen(resolved, "rb") : NULL;
    if (!file) {
        snprintf(error, 256, "The configured launcher is unavailable.");
        return false;
    }
    fclose(file);
    cJSON *record = cJSON_CreateObject();
    bool ok = record && cJSON_AddStringToObject(record, "label", label) &&
              cJSON_AddStringToObject(record, "launch", launch) &&
              cJSON_AddStringToObject(record, "rompath", launch) &&
              cJSON_AddNumberToObject(record, "type", type) &&
              cJSON_AddBoolToObject(record, "app_action", true) &&
              mainui_session_launch(directory, source, record, error);
    cJSON_Delete(record);
    return ok;
}

bool mainui_screen_language_open(MainUIApp *ui, SDLKey key)
{
    if (!ui->language_open) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        ui->language_open = false;
        mainui_languages_close(&ui->languages);
    }
    else if ((key == SDLK_UP || key == SDLK_DOWN) && ui->languages.count) {
        ui->languages.selected =
            (ui->languages.selected + (key == SDLK_UP ? ui->languages.count - 1 : 1)) %
            ui->languages.count;
        if (ui->languages.selected < ui->languages.start) {
            ui->languages.start = ui->languages.selected;
        }
        else if (ui->languages.selected >= ui->languages.start + 6) {
            ui->languages.start = ui->languages.selected - 5;
        }
    }
    else if (key == SDLK_RETURN) {
        const char *chosen =
            ui->languages.selected >= 0 && ui->languages.selected < ui->languages.count
                ? ui->languages.entries[ui->languages.selected].filename
                : "en.lang";
        bool language_font = strncmp(chosen, "en.lang", 7) != 0;
        if (mainui_language_select(&ui->languages, ui->sd)) {
            ui->language_open = false;
            mainui_languages_close(&ui->languages);
            ui->cached_start = -1;
            /* Stock reloads its fonts on a language change. The built-in
             * fallback differs by language, so when the theme relies on it,
             * restart MainUI (Onion starts it again) and resume Settings. */
            if (ui->theme.fallback_font_used && language_font != ui->theme.language_font &&
                ui->handoff_dir) {
                MainUIStack legacy;
                cJSON *resume = mainui_session_snapshot(
                    MAINUI_MENU_SETTINGS, NULL, NULL, &ui->settings_view, &ui->home_view, &legacy);
                char error[256] = "";
                if (resume && mainui_launch_publish_restart(ui->handoff_dir, resume, error)) {
                    ui->running = false;
                }
                else {
                    fprintf(stderr, "Language changed; fonts update at the next start: %s\n",
                            error);
                }
                cJSON_Delete(resume);
            }
        }
        else {
            snprintf(ui->message_title, 256, "Language");
            snprintf(ui->message_body, 256, "Could not load or save language.");
        }
    }
    return true;
}

bool mainui_screen_settings_open(MainUIApp *ui, SDLKey key)
{
    if (!ui->settings_open) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        ui->settings_open = false;
    }
    else if ((key == SDLK_UP || key == SDLK_DOWN) && ui->settings.count) {
        ui->settings.selected =
            (ui->settings.selected + (key == SDLK_UP ? ui->settings.count - 1 : 1)) %
            ui->settings.count;
        if (ui->settings.selected < ui->settings.start) {
            ui->settings.start = ui->settings.selected;
        }
        else if (ui->settings.selected >= ui->settings.start + 6) {
            ui->settings.start = ui->settings.selected - 5;
        }
    }
    else if ((key == SDLK_LEFT || key == SDLK_RIGHT) && ui->settings.count &&
             (ui->settings.rows[ui->settings.selected] == SET_BRIGHTNESS ||
              ui->settings.rows[ui->settings.selected] == SET_SOUND ||
              ui->settings.rows[ui->settings.selected] == SET_SLEEP)) {
        if (!mainui_stock_setting_adjust(&ui->settings, ui->sd, key == SDLK_LEFT ? -1 : 1)) {
            snprintf(ui->message_title, 256, "Settings");
            snprintf(ui->message_body, 256, "Could not save system.json.");
        }
        if (ui->device_enabled) {
            if (ui->settings.rows[ui->settings.selected] == SET_BRIGHTNESS &&
                !mainui_device_brightness(&ui->device_adapter,
                                          ui->settings.values[SET_BRIGHTNESS])) {
                snprintf(ui->message_title, 256, "Brightness");
                snprintf(ui->message_body, 256, "Could not apply display brightness.");
            }
            mainui_device_settings_changed(&ui->device_adapter);
        }
        if (ui->settings.rows[ui->settings.selected] == SET_SOUND) {
            mainui_audio_volume(ui->settings.values[SET_SOUND]);
            mainui_audio_change();
        }
    }
    else if (key == SDLK_RETURN && ui->settings.count &&
             ui->settings.rows[ui->settings.selected] == SET_LANGUAGE) {
        if (mainui_languages_open(&ui->languages, ui->sd, ui->theme.fallback)) {
            ui->language_open = true;
        }
        else {
            snprintf(ui->message_title, 256, "Language");
            snprintf(ui->message_body, 256, "No readable language files.");
        }
    }
    else if (key == SDLK_RETURN && ui->settings.count &&
             (ui->settings.rows[ui->settings.selected] == SET_THEMES ||
              ui->settings.rows[ui->settings.selected] == SET_TWEAKS)) {
        MainUISettingKind kind = ui->settings.rows[ui->settings.selected];
        ui->launch_pending =
            ui->handoff_dir &&
            mainui_launch_tool(ui->handoff_dir, ui->sd, &ui->launch_source,
                               mainui_stock_setting_label(kind),
                               kind == SET_THEMES ? "/mnt/SDCARD/App/ThemeSwitcher/launch.sh"
                                                  : "/mnt/SDCARD/App/Tweaks/launch.sh",
                               3, ui->message_body);
        if (!ui->launch_pending) {
            snprintf(ui->message_title, 256, "Launch unavailable");
            if (!ui->handoff_dir) {
                snprintf(ui->message_body, 256, "No runtime handoff directory configured.");
            }
        }
    }
    else if (key == SDLK_RETURN && ui->settings.count &&
             ui->settings.rows[ui->settings.selected] == SET_SHUTDOWN) {
        ui->confirmation = CONTEXT_SHUTDOWN;
    }
    else if (key == SDLK_RETURN && ui->settings.count &&
             (ui->settings.rows[ui->settings.selected] == SET_DISPLAY ||
              ui->settings.rows[ui->settings.selected] == SET_WIFI ||
              ui->settings.rows[ui->settings.selected] == SET_ABOUT)) {
        MainUISettingKind kind = ui->settings.rows[ui->settings.selected];
        if (kind == SET_ABOUT && ui->device_enabled) {
            /* Cached/pending background readings are applied before the next frame. */
            ui->settings_page = (MainUISettingsPage){.open = true, .kind = SET_ABOUT};
        }
        else {
            mainui_settings_page_open(&ui->settings_page, kind, ui->sd,
                                      ui->device_enabled ? ui->device_adapter.runtime
                                                         : ui->handoff_dir);
        }
        ui->settings_page.managed_device = ui->device_enabled;
    }
    return true;
}

bool mainui_screen_apps(MainUIApp *ui, SDLKey key)
{
    if (!ui->apps) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        mainui_catalog_close(ui->apps);
        free(ui->apps);
        ui->apps = NULL;
        ui->menu_view.cached_start = -1;
    }
    else if (key == SDLK_RETURN && ui->apps_view.selected >= 0) {
        if (ui->handoff_dir) {
            ui->launch_pending =
                mainui_session_launch(ui->handoff_dir, &ui->launch_source, NULL, ui->message_body);
            if (!ui->launch_pending) {
                snprintf(ui->message_title, 256, "Launch unavailable");
            }
            return true;
        }
        MainUIEntry *app = mainui_catalog_entry(ui->apps, ui->apps_view.selected);
        snprintf(ui->message_title, 256, "%s", app->label);
        snprintf(ui->message_body, 256, "App launching requires the Onion runtime.");
        SDL_WM_SetCaption("App launch requires the Onion device runtime", NULL);
    }
    else {
        int delta = mainui_input_list_delta(key, 4);
        if (delta) {
            mainui_viewport_move(&ui->apps_view, 4, delta, delta == 1 || delta == -1);
        }
    }
    return true;
}

bool mainui_screen_name_input_key(MainUIApp *ui, const SDL_keysym *key)
{
    if (!ui->name_input.open) {
        return false;
    }
    MainUINameResult result = mainui_name_input_key(&ui->name_input, key);
    if (ui->settings_keyboard) {
        if (result == NAME_CANCEL) {
            mainui_name_input_close(&ui->name_input);
            ui->settings_page.connect_after_password = false;
            ui->settings_keyboard = 0;
        }
        else if (result == NAME_SUBMIT) {
            size_t length = strlen(ui->name_input.text);
            bool valid = ui->settings_keyboard == 1 ? length > 0 && length <= 32
                                                    : length == 0 || (length >= 8 && length <= 63);
            if (valid) {
                if (ui->settings_keyboard == 1) {
                    strcpy(ui->settings_page.ssid, ui->name_input.text);
                }
                else {
                    strcpy(ui->settings_page.password, ui->name_input.text);
                }
                bool connect =
                    ui->settings_keyboard == 2 && ui->settings_page.connect_after_password;
                ui->settings_page.connect_after_password = false;
                mainui_name_input_close(&ui->name_input);
                ui->settings_keyboard = 0;
                if (connect) {
                    bool started =
                        ui->settings_page.wifi && ui->device_enabled &&
                        mainui_device_job_start(&ui->device_job, &ui->device_adapter, 3,
                                                ui->settings_page.ssid, ui->settings_page.password);
                    ui->settings_page.message[0] = 0;
                    if (started) {
                        memset(ui->settings_page.password, 0, sizeof ui->settings_page.password);
                    }
                }
            }
            else {
                snprintf(ui->name_input.error, sizeof ui->name_input.error, "%s",
                         ui->settings_keyboard == 1
                             ? "SSID must contain 1-32 UTF-8 bytes."
                             : "Use 8-63 bytes, or empty for an open network.");
            }
        }
        return true;
    }
    if (ui->search_keyboard) {
        if (result == NAME_CANCEL) {
            mainui_name_input_close(&ui->name_input);
            ui->search_keyboard = false;
            if (ui->search.results) {
                ui->view = ui->search.source_view;
                ui->library = NULL;
                mainui_search_close(&ui->search);
            }
            ui->cached_start = -1;
            if (ui->heading) {
                SDL_FreeSurface(ui->heading);
            }
            ui->heading = TTF_RenderUTF8_Blended(
                ui->theme.title_font, mainui_catalog_heading(ui->catalog), ui->heading_color);
        }
        else if (result == NAME_SUBMIT) {
            MainUIViewport source_view = ui->search.results ? ui->search.source_view : ui->view;
            MainUILaunchSource source = {.section = ui->catalog == ui->expert ? MAINUI_MENU_EXPERT
                                                                              : MAINUI_MENU_GAMES,
                                         .catalog = ui->catalog,
                                         .view = &source_view,
                                         .home = &ui->home_view};
            ui->reload_search = false;
            ui->search_confirm_held = key->sym == SDLK_RETURN;
            if (!mainui_catalog_job_start(&ui->catalog_job, JOB_SEARCH, &source, ui->sd,
                                          ui->config.case_sensitive, ui->config.rows,
                                          ui->name_input.text, ++ui->catalog_generation)) {
                snprintf(ui->name_input.error, sizeof ui->name_input.error, "Cannot start Search.");
            }
        }
        return true;
    }
    if (result == NAME_CANCEL) {
        mainui_name_input_close(&ui->name_input);
    }
    else if (result == NAME_SUBMIT) {
        if (mainui_favorite_edit(&ui->favorite_editor, ui->library, ui->sd, ui->name_action,
                                 ui->name_input.text, &ui->view.selected)) {
            mainui_name_input_close(&ui->name_input);
            mainui_viewport_restore(&ui->view, ui->library->visible_count, ui->config.rows,
                                    ui->view.selected, ui->view.start, ui->view.end);
            ui->cached_start = -1;
            ui->selected_at = SDL_GetTicks();
        }
        else {
            snprintf(ui->name_input.error, sizeof ui->name_input.error, "%s",
                     ui->favorite_editor.error);
        }
    }
    return true;
}

static bool screen_message_key(MainUIApp *ui, SDLKey key)
{
    if (!*ui->message_title) {
        return false;
    }
    if (key == SDLK_ESCAPE || key == SDLK_RETURN) {
        *ui->message_title = 0;
    }
    return true;
}

static bool screen_confirmation_key(MainUIApp *ui, SDLKey key)
{
    if (ui->confirmation < 0) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        ui->confirmation = -1;
    }
    else if (key == SDLK_RETURN) {
        bool ok = false;
        if (ui->confirmation == CONTEXT_DELETE_ROM) {
            ok = !ui->library && mainui_browser_delete(ui->catalog, &ui->view, ui->config.rows);
            ui->cached_start = -1;
            mainui_preview_close(&ui->preview);
            if (ui->heading) {
                SDL_FreeSurface(ui->heading);
            }
            ui->heading = TTF_RenderUTF8_Blended(
                ui->theme.title_font, mainui_catalog_heading(ui->catalog), ui->heading_color);
        }
        if (ui->confirmation == CONTEXT_SHUTDOWN) {
            ok = ui->device_enabled ? mainui_device_power_off(&ui->device_adapter)
                                    : mainui_device_shutdown(ui->handoff_dir);
            ui->launch_pending = ok;
        }
        if (ui->confirmation == CONTEXT_CLEAR_RECENT) {
            ok = mainui_saved_action(ui->sd, true, SAVED_CLEAR, NULL);
        }
        if (ok && ui->favorites) {
            if (!mainui_library_reload(ui->favorites, ui->sd)) {
                mainui_library_close(ui->favorites);
            }
            ui->cached_start = -1;
        }
        if (ok && ui->library && !ui->search.results) {
            ok = mainui_library_reload(ui->library, ui->sd);
            if (ok) {
                mainui_viewport_restore(&ui->view, ui->library->visible_count, ui->config.rows, 0,
                                        0, ui->config.rows - 1);
                ui->cached_start = -1;
            }
        }
        if (!ok) {
            snprintf(ui->message_title, sizeof ui->message_title, "Action unavailable");
            snprintf(ui->message_body, sizeof ui->message_body, "%s",
                     ui->confirmation == CONTEXT_SHUTDOWN ? "Shutdown requires the Onion runtime."
                     : ui->confirmation == CONTEXT_DELETE_ROM ? ui->catalog->error
                                                              : "Could not clear the saved list.");
        }
        ui->confirmation = -1;
    }
    return true;
}

bool mainui_screen_settings_page_key(MainUIApp *ui, SDLKey key)
{
    if (!ui->settings_page.open) {
        return false;
    }
    int previous_wifi = ui->settings_page.wifi;
    ui->settings_keyboard =
        mainui_settings_page_key(&ui->settings_page, key, ui->sd,
                                 ui->device_enabled ? ui->device_adapter.runtime : ui->handoff_dir);
    if (ui->device_enabled && previous_wifi != ui->settings_page.wifi) {
        mainui_device_settings_changed(&ui->device_adapter);
    }
    if (ui->settings_keyboard >= 3) {
        bool started =
            (ui->settings_page.wifi || ui->settings_keyboard == 6) &&
            mainui_device_job_start(&ui->device_job, &ui->device_adapter, ui->settings_keyboard,
                                    ui->settings_page.ssid, ui->settings_page.password);
        snprintf(ui->settings_page.message, sizeof ui->settings_page.message, "%s",
                 started && (ui->settings_keyboard == 4 || ui->settings_keyboard == 5)
                     ? "Scanning..."
                     : "");
        if (started) {
            memset(ui->settings_page.password, 0, sizeof ui->settings_page.password);
        }
        ui->settings_keyboard = 0;
    }
    if (ui->settings_keyboard) {
        mainui_name_input_open(&ui->name_input, &ui->theme, ui->settings_keyboard == 1 ? 151 : 152,
                               ui->settings_keyboard == 1 ? ui->settings_page.ssid
                                                          : ui->settings_page.password);
        ui->name_input.secret = ui->settings_keyboard == 2;
    }
    return true;
}

bool mainui_screen_context_menu_key(MainUIApp *ui, SDLKey key, int *requested_section)
{
    if (!ui->context_open) {
        return false;
    }
    if (key == SDLK_ESCAPE || key == SDLK_RCTRL) {
        ui->context_open = false;
        return true;
    }
    if (key == SDLK_UP || key == SDLK_DOWN) {
        ui->context.selected =
            (ui->context.selected + (key == SDLK_UP ? ui->context.visible_count - 1 : 1)) %
            ui->context.visible_count;
        return true;
    }
    if (key != SDLK_RETURN) {
        return true;
    }
    const MainUIContextEntry *entry =
        &ui->context.entries[ui->context.visible[ui->context.selected]];
    ui->context_open = false;
    (*requested_section) = mainui_context_section(entry->action);
    if (entry->action == CONTEXT_START) {
        if (ui->handoff_dir) {
            ui->launch_pending = mainui_session_launch(ui->handoff_dir, &ui->launch_source,
                                                       ui->context_record, ui->message_body);
            if (!ui->launch_pending) {
                snprintf(ui->message_title, sizeof ui->message_title, "Launch unavailable");
            }
            return true;
        }
        snprintf(ui->message_title, sizeof ui->message_title, "Launch");
        snprintf(ui->message_body, sizeof ui->message_body,
                 "Launching requires the Onion runtime.");
        return true;
    }
    if (entry->action == CONTEXT_ADD_FAVORITE || entry->action == CONTEXT_REMOVE_RECENT ||
        entry->action == CONTEXT_REMOVE_FAVORITE) {
        bool recent = entry->action == CONTEXT_REMOVE_RECENT;
        MainUISavedAction action = entry->action == CONTEXT_ADD_FAVORITE ? SAVED_ADD : SAVED_REMOVE;
        bool ok = mainui_saved_action(ui->sd, recent, action, ui->context_record);
        if (ok && entry->action == CONTEXT_REMOVE_FAVORITE) {
            mainui_favorite_editor_close(&ui->favorite_editor);
            if (!mainui_favorite_forget_assignment(ui->sd, ui->context_record)) {
                fprintf(stderr, "Favorite removed; stale folder assignment cleanup deferred.\n");
            }
        }
        if (ok && ui->favorites) {
            if (!mainui_library_reload(ui->favorites, ui->sd)) {
                mainui_library_close(ui->favorites);
            }
            ui->cached_start = -1;
        }
        if (ok && ui->library && !ui->search.results) {
            ok = mainui_library_reload(ui->library, ui->sd);
            if (ok) {
                mainui_viewport_restore(&ui->view, ui->library->visible_count, ui->config.rows,
                                        ui->view.selected, ui->view.start, ui->view.end);
                ui->cached_start = -1;
            }
        }
        if (!ok) {
            snprintf(ui->message_title, sizeof ui->message_title, "%s",
                     mainui_context_label(entry));
            snprintf(ui->message_body, sizeof ui->message_body, "Could not update the saved list.");
        }
        return true;
    }
    if (entry->action >= CONTEXT_FAVORITE_MOVE && entry->action <= CONTEXT_FAVORITE_SORT) {
        if (entry->action == CONTEXT_FAVORITE_CREATE || entry->action == CONTEXT_FAVORITE_RENAME) {
            ui->name_action = entry->action;
            const char *initial = "";
            if (ui->name_action == CONTEXT_FAVORITE_RENAME) {
                initial = mainui_library_label(ui->library, ui->view.selected);
            }
            if (ui->input_text) {
                initial = ui->input_text;
            }
            mainui_name_input_open(&ui->name_input, &ui->theme,
                                   ui->name_action == CONTEXT_FAVORITE_CREATE ? 400 : 403, initial);
            ui->input_text = NULL;
        }
        else if (mainui_favorite_edit(&ui->favorite_editor, ui->library, ui->sd, entry->action,
                                      NULL, &ui->view.selected)) {
            int selected = ui->view.selected;
            if (entry->action == CONTEXT_FAVORITE_PASTE) {
                int start = selected >= ui->config.rows ? selected - ui->config.rows + 1 : 0;
                mainui_viewport_restore(&ui->view, ui->library->visible_count, ui->config.rows,
                                        selected, start, start + ui->config.rows - 1);
            }
            else {
                ui->view.total = ui->library->visible_count;
                mainui_viewport_move(&ui->view, ui->config.rows, 0, false);
            }
            ui->library->views[ui->library->current + 1] = ui->view;
            ui->cached_start = -1;
            ui->selected_at = SDL_GetTicks();
        }
        else {
            snprintf(ui->message_title, sizeof ui->message_title, "%s",
                     mainui_context_label(entry));
            snprintf(ui->message_body, sizeof ui->message_body, "%s", ui->favorite_editor.error);
        }
        return true;
    }
    if (entry->action == CONTEXT_REFRESH_SYSTEM) {
        if (!mainui_catalog_job_start(&ui->catalog_job, JOB_REFRESH_SYSTEM, &ui->launch_source,
                                      ui->sd, ui->config.case_sensitive, ui->config.rows, NULL,
                                      ++ui->catalog_generation)) {
            snprintf(ui->message_title, sizeof ui->message_title, "Refresh roms");
            snprintf(ui->message_body, sizeof ui->message_body, "Cannot start refresh.");
        }
        return true;
    }
    if (entry->action == CONTEXT_CLEAR_RECENT || entry->action == CONTEXT_SHUTDOWN) {
        ui->confirmation = entry->action;
        return true;
    }
    if (entry->action == CONTEXT_DELETE_ROM) {
        ui->confirmation = CONTEXT_DELETE_ROM;
        return true;
    }
    if (entry->action == CONTEXT_REFRESH) {
        /* Release inactive console readers too before deleting shared caches. */
        MainUICatalog *opened[] = {ui->games, ui->expert};
        for (int i = 0; i < 2; ++i) {
            if (opened[i]) {
                while (mainui_catalog_back(opened[i])) {
                }
            }
        }
        if (ui->games) {
            ui->games_view = ui->games->pages[0].view;
        }
        if (ui->expert) {
            ui->expert_view = ui->expert->pages[0].view;
        }
        if (ui->catalog && !ui->home && !ui->library) {
            ui->view = ui->catalog->pages[0].view;
            mainui_browser_grid_restore(ui->catalog, &ui->view, ui->view.selected);
        }
        ui->cached_start = ui->menu_view.cached_start = -1;
        if (!mainui_catalog_job_start(&ui->catalog_job, JOB_REFRESH_ALL, &ui->launch_source, ui->sd,
                                      ui->config.case_sensitive, ui->config.rows, NULL,
                                      ++ui->catalog_generation)) {
            snprintf(ui->message_title, sizeof ui->message_title, "Refresh all roms");
            snprintf(ui->message_body, sizeof ui->message_body, "Cannot start refresh.");
        }
        return true;
    }
    if (entry->action == CONTEXT_SEARCH && !ui->home && !ui->apps && !ui->library && ui->catalog &&
        ui->catalog->depth) {
        if (!mainui_positions_save(ui->catalog, &ui->view)) {
            fprintf(stderr, "Could not save source ROM-list position.\n");
        }
        ui->search_keyboard = true;
        mainui_name_input_open(&ui->name_input, &ui->theme, 153,
                               ui->input_text ? ui->input_text : "");
        ui->input_text = NULL;
        return true;
    }
    if (entry->action == CONTEXT_THEMES || entry->action == CONTEXT_TWEAKS ||
        entry->action == CONTEXT_SEARCH ||
        (entry->action >= CONTEXT_CUSTOM1 && entry->action <= CONTEXT_CUSTOM3)) {
        const char *launch = *entry->launch ? entry->launch
                             : entry->action == CONTEXT_THEMES
                                 ? "/mnt/SDCARD/App/ThemeSwitcher/launch.sh"
                             : entry->action == CONTEXT_TWEAKS ? "/mnt/SDCARD/App/Tweaks/launch.sh"
                                                               : "/mnt/SDCARD/App/Search/launch.sh";
        ui->launch_pending =
            ui->handoff_dir &&
            mainui_launch_tool(ui->handoff_dir, ui->sd, &ui->launch_source,
                               mainui_context_label(entry), launch, entry->type, ui->message_body);
        if (!ui->launch_pending) {
            snprintf(ui->message_title, sizeof ui->message_title, "Launch unavailable");
            if (!ui->handoff_dir) {
                snprintf(ui->message_body, sizeof ui->message_body,
                         "No runtime handoff directory configured.");
            }
        }
        return true;
    }
    if ((*requested_section) < 0) {
        snprintf(ui->message_title, sizeof ui->message_title, "%s", mainui_context_label(entry));
        snprintf(ui->message_body, sizeof ui->message_body,
                 "This action requires the Onion runtime.");
        return true;
    }
    return false;
}

static bool screen_context_open_key(MainUIApp *ui, SDLKey key)
{
    if (key != SDLK_RCTRL || ui->settings_open) {
        return false;
    }
    cJSON_Delete(ui->context_record);
    ui->context_record = NULL;
    const MainUIContextAction app_actions[] = {CONTEXT_START, CONTEXT_ADD_FAVORITE};
    const MainUIContextAction game_actions[] = {CONTEXT_START, CONTEXT_ADD_FAVORITE,
                                                CONTEXT_DELETE_ROM};
    const MainUIContextAction rom_actions[] = {CONTEXT_START, CONTEXT_ADD_FAVORITE,
                                               CONTEXT_DELETE_ROM, CONTEXT_SEARCH,
                                               CONTEXT_REFRESH_SYSTEM};
    const MainUIContextAction recent_actions[] = {CONTEXT_START, CONTEXT_REMOVE_RECENT,
                                                  CONTEXT_CLEAR_RECENT};
    const MainUIContextAction system_actions[] = {CONTEXT_SEARCH, CONTEXT_REFRESH_SYSTEM};
    const MainUIContextAction grid_actions[] = {CONTEXT_REFRESH, CONTEXT_REFRESH_SYSTEM};
    int system_index =
        ui->catalog && ui->catalog->depth ? ui->catalog->pages[0].view.selected : ui->view.selected;
    bool refreshable = ui->catalog && system_index >= 0 &&
                       system_index < ui->catalog->pages[0].count &&
                       strcmp(ui->catalog->pages[0].entries[system_index].label, " Search ");
    /* Search is an emulated console, not a ROM collection to refresh. */
    if (ui->apps) {
        ui->context_record = mainui_catalog_record(ui->apps, ui->apps_view.selected);
        mainui_context_rows(&ui->context, app_actions, ui->context_record ? 2 : 0);
    }
    else if (ui->library) {
        if (ui->view.selected >= 0 && !mainui_library_is_folder(ui->library, ui->view.selected)) {
            ui->context_record = cJSON_Duplicate(
                ui->library->items[ui->library->visible[ui->view.selected]].json, true);
        }
        if (ui->search.results) {
            mainui_context_rows(&ui->context, game_actions, ui->context_record ? 2 : 0);
        }
        else if (ui->library->recent) {
            mainui_context_rows(&ui->context, recent_actions, ui->context_record ? 3 : 0);
        }
        else {
            mainui_favorite_context(&ui->context, ui->library, ui->view.selected,
                                    ui->favorite_editor.key != NULL);
        }
    }
    else if (ui->home) {
        ui->context = ui->home_context;
    }
    else if (ui->catalog && !ui->catalog->depth) {
        mainui_context_rows(&ui->context, grid_actions, refreshable ? 2 : 1);
    }
    else {
        if (ui->catalog) {
            ui->context_record = mainui_catalog_record(
                ui->catalog, mainui_browser_index(ui->catalog, ui->view.selected));
        }
        if (ui->context_record) {
            mainui_context_rows(&ui->context, rom_actions, refreshable ? 5 : 4);
        }
        else {
            mainui_context_rows(&ui->context, system_actions, refreshable ? 2 : 1);
        }
    }
    /* The patched ROM popup uses exact persistent ROM membership,
     * including games assigned to a Favorite folder. */
    const cJSON *rom = cJSON_GetObjectItemCaseSensitive(ui->context_record, "rompath");
    if (cJSON_IsString(rom) && mainui_library_contains(ui->favorites, rom->valuestring)) {
        for (int i = 0; i < ui->context.count; i++) {
            if (ui->context.entries[i].action == CONTEXT_ADD_FAVORITE) {
                ui->context.entries[i].action = CONTEXT_REMOVE_FAVORITE;
            }
        }
    }
    ui->context.selected = 0;
    ui->context_open = ui->context.visible_count > 0;
    return true;
}

bool mainui_screen_home_key(MainUIApp *ui, SDLKey key, int requested_section)
{
    if (!ui->home) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        /* B has no action on the main menu. */
        return true;
    }
    else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
        mainui_viewport_move(&ui->home_view, 4, key == SDLK_LEFT ? -1 : 1, true);
    }
    else if (key == SDLK_RETURN && (ui->home_view.selected >= 0 || requested_section >= 0)) {
        MainUIMenuSection section = requested_section >= 0
                                        ? (MainUIMenuSection)requested_section
                                        : ui->menu.sections[ui->home_view.selected];
        if (section == MAINUI_MENU_GAMES || section == MAINUI_MENU_EXPERT) {
            if (section == MAINUI_MENU_EXPERT && (!ui->expert || !ui->expert->pages[0].count)) {
                ui->launch_source.section = section;
                if (!mainui_catalog_job_start(&ui->catalog_job, JOB_DISCOVER, &ui->launch_source,
                                              ui->sd, ui->config.case_sensitive, ui->config.rows,
                                              NULL, ++ui->catalog_generation)) {
                    snprintf(ui->message_title, sizeof ui->message_title, "Expert unavailable");
                    snprintf(ui->message_body, sizeof ui->message_body, "Cannot start discovery.");
                }
                return true;
            }
            ui->catalog = section == MAINUI_MENU_GAMES ? ui->games : ui->expert;
            MainUIViewport saved = ui->catalog->depth             ? ui->catalog->pages[0].view
                                   : section == MAINUI_MENU_GAMES ? ui->games_view
                                                                  : ui->expert_view;
            while (ui->catalog->depth) {
                mainui_catalog_back(ui->catalog);
            }
            mainui_browser_grid_restore(ui->catalog, &ui->view, saved.selected);
            ui->home = false;
            ui->cached_start = -1;
            ui->menu_view.cached_start = -1;
        }
        else if (section == MAINUI_MENU_RECENTS || section == MAINUI_MENU_FAVORITES) {
            ui->library = calloc(1, sizeof *ui->library);
            if (!ui->library ||
                !mainui_library_open(ui->library, ui->sd, section == MAINUI_MENU_RECENTS)) {
                /* Stay on the main menu: an unreadable list must not quit. */
                bool recent = section == MAINUI_MENU_RECENTS;
                fprintf(stderr, "Cannot read saved library (%s)\n",
                        recent ? "recentlist.json" : "favourite.json");
                if (ui->library) {
                    mainui_favorite_editor_close(&ui->favorite_editor);
                    mainui_library_close(ui->library);
                }
                free(ui->library);
                ui->library = NULL;
                snprintf(ui->message_title, sizeof ui->message_title, "%s unavailable",
                         recent ? "Recents" : "Favorites");
                snprintf(ui->message_body, sizeof ui->message_body, "Cannot read Roms/%s.",
                         recent ? "recentlist.json" : "favourite.json");
                return true;
            }
            ui->selected_at = SDL_GetTicks();
            ui->preview_sync_once = true;
            ui->systems_view = ui->view;
            MainUIViewport saved = ui->saved_library_views[ui->library->recent ? 1 : 0];
            mainui_viewport_restore(&ui->view, ui->library->visible_count, ui->config.rows,
                                    saved.selected, saved.start, saved.end);
            ui->home = false;
            ui->cached_start = -1;
            if (ui->heading) {
                SDL_FreeSurface(ui->heading);
            }
            ui->heading = TTF_RenderUTF8_Blended(
                ui->theme.title_font, mainui_library_heading(ui->library), ui->heading_color);
        }
        else if (section == MAINUI_MENU_SETTINGS) {
            mainui_stock_settings_load(&ui->settings, ui->config_dir, ui->sd,
                                       ui->device_status.model.id);
            ui->settings_open = true;
        }
        else if (section == MAINUI_MENU_APPS) {
            ui->launch_source.section = section;
            if (!mainui_catalog_job_start(&ui->catalog_job, JOB_DISCOVER, &ui->launch_source,
                                          ui->sd, ui->config.case_sensitive, ui->config.rows, NULL,
                                          ++ui->catalog_generation)) {
                snprintf(ui->message_title, sizeof ui->message_title, "Apps unavailable");
                snprintf(ui->message_body, sizeof ui->message_body, "Cannot start discovery.");
            }
        }
        /* Every MAINUI_MENU_* section is handled above. */
    }
    return true;
}

bool mainui_screen_list_key(MainUIApp *ui, SDLKey key)
{
    if (ui->library) {
        bool changed = false;
        if (key == SDLK_ESCAPE) {
            ui->library->views[ui->library->current + 1] = ui->view;
            if (mainui_library_back(ui->library)) {
                changed = true;
            }
            else {
                ui->saved_library_views[ui->library->recent ? 1 : 0] = ui->view;
                mainui_favorite_editor_close(&ui->favorite_editor);
                mainui_library_close(ui->library);
                free(ui->library);
                ui->library = NULL;
                ui->home = true;
                ui->view = ui->systems_view;
                ui->cached_start = -1;
                return true;
            }
        }
        else if (key == SDLK_RETURN) {
            ui->library->views[ui->library->current + 1] = ui->view;
            changed = mainui_library_enter(ui->library, ui->view.selected);
            if (!changed && key == SDLK_RETURN && ui->handoff_dir && ui->view.selected >= 0 &&
                !mainui_library_is_folder(ui->library, ui->view.selected)) {
                ui->launch_pending = mainui_session_launch(ui->handoff_dir, &ui->launch_source,
                                                           NULL, ui->message_body);
                if (!ui->launch_pending) {
                    snprintf(ui->message_title, sizeof ui->message_title, "Launch unavailable");
                }
            }
        }
        else {
            int delta = mainui_input_list_delta(key, ui->config.rows);
            if (delta) {
                mainui_viewport_move(&ui->view, ui->config.rows, delta, delta == 1 || delta == -1);
            }
        }
        if (changed) {
            ui->preview_sync_once = true;
            MainUIViewport saved = ui->library->views[ui->library->current + 1];
            mainui_viewport_restore(&ui->view, ui->library->visible_count, ui->config.rows,
                                    saved.selected, saved.start, saved.end);
            ui->cached_start = -1;
            if (ui->heading) {
                SDL_FreeSurface(ui->heading);
            }
            ui->heading = TTF_RenderUTF8_Blended(
                ui->theme.title_font, mainui_library_heading(ui->library), ui->heading_color);
        }
        ui->selected_at = SDL_GetTicks();
        return true;
    }
    if (ui->catalog && !ui->catalog->depth && key != SDLK_RETURN) {
        if (key == SDLK_ESCAPE) {
            ui->home = true;
        }
        else {
            mainui_grid_move(&ui->view, (key == SDLK_RIGHT) - (key == SDLK_LEFT),
                             (key == SDLK_DOWN) - (key == SDLK_UP),
                             (key == SDLK_BACKSPACE) - (key == SDLK_TAB),
                             ui->catalog == ui->expert ? 3 : 4, ui->catalog == ui->expert ? 3 : 2);
        }
        return true;
    }
    int delta = 0;
    switch (key) {
    case SDLK_ESCAPE:
        if (ui->catalog && mainui_browser_back(ui->catalog, &ui->view)) {
            ui->preview_sync_once = true;
            ui->cached_start = ui->menu_view.cached_start = -1;
        }
        else if (key == SDLK_ESCAPE) {
            ui->running = false;
        }
        break;
    case SDLK_RETURN:
        if (ui->catalog && ui->view.selected >= 0) {
            if (mainui_browser_folder(ui->catalog, ui->view.selected) &&
                mainui_browser_index(ui->catalog, ui->view.selected) >= 0) {
                if (!mainui_positions_save(ui->catalog, &ui->view)) {
                    fprintf(stderr, "Could not save ROM-list position.\n");
                }
                if (!mainui_catalog_job_start(&ui->catalog_job, JOB_ENTER, &ui->launch_source,
                                              ui->sd, ui->config.case_sensitive, ui->config.rows,
                                              NULL, ++ui->catalog_generation)) {
                    snprintf(ui->message_title, sizeof ui->message_title, "Catalog unavailable");
                    snprintf(ui->message_body, sizeof ui->message_body,
                             "Cannot start folder read.");
                }
            }
            else if (mainui_browser_index(ui->catalog, ui->view.selected) < 0 &&
                     mainui_browser_back(ui->catalog, &ui->view)) {
                ui->cached_start = ui->menu_view.cached_start = -1;
            }
            else if (key == SDLK_RETURN && ui->handoff_dir &&
                     !mainui_browser_folder(ui->catalog, ui->view.selected) &&
                     !*ui->catalog->error) {
                ui->launch_pending = mainui_session_launch(ui->handoff_dir, &ui->launch_source,
                                                           NULL, ui->message_body);
                if (!ui->launch_pending) {
                    snprintf(ui->message_title, sizeof ui->message_title, "Launch unavailable");
                }
            }
            else if (*ui->catalog->error) {
                fprintf(stderr, "%s\n", ui->catalog->error);
                if (!ui->snapshot) {
                    SDL_WM_SetCaption(ui->catalog->error, NULL);
                }
            }
        }
        break;
    case SDLK_LEFT:
        ui->selected_at = SDL_GetTicks();
        break;
    case SDLK_UP:
        delta = -1;
        break;
    case SDLK_DOWN:
        delta = 1;
        break;
    case SDLK_TAB:
        delta = -ui->config.rows;
        break;
    case SDLK_BACKSPACE:
        delta = ui->config.rows;
        break;
    default:
        break;
    }
    if (ui->catalog && ui->cached_start == -1) {
        if (ui->heading) {
            SDL_FreeSurface(ui->heading);
        }
        ui->heading = TTF_RenderUTF8_Blended(
            ui->theme.title_font, mainui_catalog_heading(ui->catalog), ui->heading_color);
        ui->selected_at = SDL_GetTicks();
        SDL_WM_SetCaption("Open MainUI - game browser", NULL);
    }
    if (delta) {
        mainui_viewport_move(&ui->view, ui->config.rows, delta, delta == 1 || delta == -1);
        ui->selected_at = SDL_GetTicks();
    }
    return true;
}

bool mainui_screen_search_key(MainUIApp *ui, SDLKey key)
{
    if (ui->catalog && ui->catalog->depth && !ui->library && key == SDLK_LSHIFT) {
        if (!mainui_positions_save(ui->catalog, &ui->view)) {
            fprintf(stderr, "Could not save source ROM-list position.\n");
        }
        ui->search_keyboard = true;
        mainui_name_input_open(&ui->name_input, &ui->theme, 153,
                               ui->input_text ? ui->input_text : "");
        ui->input_text = NULL;
        return true;
    }
    if (ui->search.results && key == SDLK_ESCAPE) {
        if (ui->search.postgame) {
            mainui_launch_clear_search(ui->handoff_dir);
            ui->view = ui->search.source_view;
            ui->library = NULL;
            mainui_search_close(&ui->search);
            if (ui->heading) {
                SDL_FreeSurface(ui->heading);
            }
            ui->heading = TTF_RenderUTF8_Blended(
                ui->theme.title_font, mainui_catalog_heading(ui->catalog), ui->heading_color);
            ui->cached_start = -1;
        }
        else {
            ui->search_keyboard = true;
            mainui_name_input_open(&ui->name_input, &ui->theme, 153, ui->search.query);
        }
        return true;
    }
    return false;
}

static void dispatch_key(MainUIApp *ui, SDL_Event *event)
{
    SDLKey key = mainui_input_key(event->key.keysym.sym);
    if (ui->device_enabled && ui->device_status.sleeping == 1) {
        return;
    }
    if (key == SDLK_ESCAPE && !ui->name_input.open && ui->settings_page.open &&
        ui->settings_page.kind == SET_WIFI) {
        /* Leaving the menu cancels visible and silent scans without interrupting
         * radio power transitions. Scan scheduling never depends on message text. */
        if (ui->device_job.queued_operation == 4) {
            ui->device_job.queued_operation = 0;
        }
        if (ui->device_job.thread && ui->device_job.operation == 4) {
            atomic_store(&ui->device_job.cancel, true);
        }
        ui->settings_page.message[0] = 0;
    }
    if (ui->search.release_pending) {
        if (key == SDLK_RETURN) {
            return;
        }
        ui->search.release_pending = false;
    }
    ui->search.view = ui->view;
    bool was_held = key >= 0 && key < SDLK_LAST && ui->held[key];

    if (key >= 0 && key < SDLK_LAST) {
        ui->held[key] = true;
    }
    ui->settings_view = (MainUIViewport){ui->settings.count, ui->settings.selected,
                                         ui->settings.start, ui->settings.start + 5};
    ui->launch_source = (MainUILaunchSource){
        .sd = ui->sd,
        .section = ui->settings_open ? MAINUI_MENU_SETTINGS
                   : ui->apps        ? MAINUI_MENU_APPS
                   : ui->library && !ui->search.results
                       ? (ui->library->recent ? MAINUI_MENU_RECENTS : MAINUI_MENU_FAVORITES)
                   : ui->catalog == ui->expert ? MAINUI_MENU_EXPERT
                                               : MAINUI_MENU_GAMES,
        .catalog = ui->settings_open ? NULL
                   : ui->apps        ? ui->apps
                                     : ui->catalog,
        .library = ui->search.results ? NULL : ui->library,
        .view = ui->settings_open    ? &ui->settings_view
                : ui->search.results ? &ui->search.source_view
                : ui->apps           ? &ui->apps_view
                                     : &ui->view,
        .home = &ui->home_view,
        .search = ui->search.results ? &ui->search : NULL,
        .home_only = ui->home && !ui->settings_open && !ui->apps};
    if (ui->details.open && key == SDLK_RETURN && ui->handoff_dir) {
        ui->launch_pending =
            mainui_session_launch(ui->handoff_dir, &ui->launch_source, NULL, ui->message_body);
        if (!ui->launch_pending) {
            snprintf(ui->message_title, sizeof ui->message_title, "Launch unavailable");
            mainui_details_close(&ui->details);
        }
        return;
    }
    if (ui->details.open) {
        mainui_details_key(&ui->details, ui->catalog, ui->library, ui->favorites, &ui->view,
                           ui->config.rows, key);
        ui->cached_start = -1;
        ui->selected_at = SDL_GetTicks();
        return;
    }
    bool shoulder =
        key == SDLK_PAGEUP || key == SDLK_PAGEDOWN || key == SDLK_HOME || key == SDLK_END;
    if (ui->home && !ui->apps && !ui->settings_open && !ui->context_open && !*ui->message_title &&
        ui->home_context.hotkey && shoulder && !was_held && ui->held[SDLK_PAGEUP] &&
        ui->held[SDLK_PAGEDOWN] && ui->held[SDLK_HOME] && ui->held[SDLK_END]) {
        mainui_context_reveal(&ui->home_context);
        ui->context = ui->home_context;
        ui->context_open = ui->context.visible_count > 0;
        return;
    }
    bool game_list = !ui->home && !ui->apps && !ui->settings_open && !ui->language_open &&
                     !ui->context_open && ui->confirmation < 0 && !*ui->message_title &&
                     (ui->library || !ui->catalog || ui->catalog->depth);
    if (game_list && mainui_screen_search_key(ui, key)) {
        return;
    }
    if (game_list && ui->catalog && key == SDLK_RIGHT &&
        mainui_details_open(&ui->details, ui->catalog, ui->library, ui->favorites,
                            ui->view.selected, &ui->preview)) {
        ui->letter_jump.active = false;
        return;
    }
    if (game_list && (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN)) {
        if (!ui->letter_jump.active) {
            mainui_letter_jump_begin(&ui->letter_jump, ui->view.selected, ui->view.total,
                                     key == SDLK_PAGEUP ? -1 : 1, mainui_list_label_at,
                                     &ui->list_labels);
        }
        return;
    }
    ui->letter_jump.active = false;
    if (game_list &&
        ((key == SDLK_HOME && ui->held[SDLK_UP]) || (key == SDLK_END && ui->held[SDLK_DOWN]))) {
        /* Modifier first, one jump per shoulder press; keyup releases the latch. */
        if (!was_held) {
            int target = key == SDLK_HOME ? 0 : ui->view.total - 1;
            mainui_viewport_move(&ui->view, ui->config.rows, target - ui->view.selected, false);
            ui->selected_at = SDL_GetTicks();
        }
        return;
    }
    /* L2/R2 retain stock paging. The legacy host aliases remain accepted. */
    if (key == SDLK_HOME) {
        key = SDLK_TAB;
    }
    else if (key == SDLK_END) {
        key = SDLK_BACKSPACE;
    }
    event->key.keysym.sym = key;
    if (screen_message_key(ui, key)) {
        return;
    }
    if (screen_confirmation_key(ui, key)) {
        return;
    }
    if (mainui_screen_settings_page_key(ui, key)) {
        return;
    }
    int requested_section = -1;
    if (mainui_screen_context_menu_key(ui, key, &requested_section)) {
        return;
    }
    if (screen_context_open_key(ui, key)) {
        return;
    }
    if (key == SDLK_F2 && !ui->settings_open && !ui->language_open && !ui->home && !ui->library &&
        ui->catalog) {
        ui->reload_search = false;
        mainui_catalog_job_start(&ui->catalog_job, JOB_RELOAD, &ui->launch_source, ui->sd,
                                 ui->config.case_sensitive, ui->config.rows, NULL,
                                 ++ui->catalog_generation);
        return;
    }
    const char *button = mainui_input_button(key);
    if (button && key != SDLK_PAGEUP && key != SDLK_PAGEDOWN) {
        /* Unassigned controller actions are silent. */
        return;
    }
    if (mainui_screen_language_open(ui, key)) {
        return;
    }
    if (mainui_screen_settings_open(ui, key)) {
        return;
    }
    if (mainui_screen_apps(ui, key)) {
        return;
    }
    if (mainui_screen_home_key(ui, key, requested_section)) {
        return;
    }
    mainui_screen_list_key(ui, key);
}

bool mainui_dispatch_event(MainUIApp *ui, SDL_Event *event)
{
    /* Timer, decoder and key-up events must not replace the launch key time. */
    if (event->type == SDL_KEYDOWN) {
        mainui_mark(MAINUI_MARK_EVENT);
    }
    if (event->type == SDL_QUIT) {
        ui->running = false;
    }
    if (event->type == SDL_ACTIVEEVENT && event->active.gain) {
        ui->next_catalog_check = 0;
    }
    if (event->type == SDL_ACTIVEEVENT && !event->active.gain &&
        (event->active.state & SDL_APPINPUTFOCUS)) {
        /* Lost keyup events must not leave a modifier/chord stuck after Alt-Tab. */
        memset(ui->held, 0, sizeof ui->held);
        ui->letter_jump.active = false;
    }
    if (event->type == SDL_KEYUP) {
        SDLKey released = mainui_input_key(event->key.keysym.sym);
        if (released >= 0 && released < SDLK_LAST) {
            ui->held[released] = false;
        }
    }
    if (ui->catalog_job.thread) {
        if (event->type == SDL_KEYUP && mainui_input_key(event->key.keysym.sym) == SDLK_RETURN) {
            ui->search_confirm_held = false;
        }
        if (event->type == SDL_KEYDOWN && mainui_input_key(event->key.keysym.sym) == SDLK_ESCAPE) {
            mainui_catalog_job_cancel(&ui->catalog_job);
            ui->catalog_generation++;
        }

        return true;
    }
    if (event->type == SDL_KEYDOWN && mainui_screen_name_input_key(ui, &event->key.keysym)) {
        return true;
    }
    if (event->type == SDL_KEYUP && ui->search.release_pending &&
        mainui_input_key(event->key.keysym.sym) == SDLK_RETURN) {
        ui->search.release_pending = false;
        return true;
    }
    if (event->type == SDL_KEYDOWN) {
        dispatch_key(ui, event);
        return true;
    }
    return event->type == SDL_QUIT || event->type == SDL_ACTIVEEVENT || event->type == SDL_KEYUP;
}
