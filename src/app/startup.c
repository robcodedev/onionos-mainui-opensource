/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/startup.h"
#include "app/options.h"
#include "app/positions.h"
#include "app/render.h"
#include "cJSON.h"
#include "localization/language.h"
#include "platform/audio.h"
#include "platform/launch.h"
#include "platform/system_config.h"
#include "platform/timing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef MAINUI_ONION
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Onion's Search app hands its results back through state.json in the handoff
 * directory: stock MainUI's navigation stack, whose last type 5 frame names the
 * console in "emuname" (" Search " for the results). Open that console once,
 * then move the file aside so later restarts start normally. Open MainUI's own
 * state.json has no "emuname" and is ignored. Any problem keeps the normal start.
 */
static void open_state_console(MainUIApp *ui)
{
    char path[1024], used[1040];
    const char *dir = ui->handoff_dir ? ui->handoff_dir : "/tmp";
    int n = snprintf(path, sizeof path, "%s/state.json", dir);
    if (n < 0 || n >= (int)sizeof path) {
        return;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return;
    }
    char *text = malloc(65536);
    if (!text) {
        fclose(file);
        return;
    }
    size_t bytes = fread(text, 1, 65535, file);
    fclose(file);
    text[bytes] = 0;
    cJSON *root = cJSON_Parse(text);
    free(text);
    const cJSON *list = root ? cJSON_GetObjectItemCaseSensitive(root, "list") : NULL;
    int count = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
    const cJSON *last = count > 0 ? cJSON_GetArrayItem(list, count - 1) : NULL;
    const cJSON *type = last ? cJSON_GetObjectItemCaseSensitive(last, "type") : NULL;
    const cJSON *name = last ? cJSON_GetObjectItemCaseSensitive(last, "emuname") : NULL;
    if (cJSON_IsNumber(type) && type->valueint == 5 && cJSON_IsString(name) && name->valuestring) {
        bool opened = false;
        for (int i = 0; i < ui->catalog->pages[0].count; i++) {
            if (!strcmp(ui->catalog->pages[0].entries[i].label, name->valuestring)) {
                opened = mainui_catalog_enter(ui->catalog, i);
                if (opened) {
                    mainui_grid_restore(&ui->catalog->pages[0].view, ui->catalog->pages[0].count, i,
                                        4, 2);
                    /* Start in that console, as with --system. */
                    static char console_name[256];
                    snprintf(console_name, sizeof console_name, "%s", name->valuestring);
                    ui->system_name = console_name;
                }
                break;
            }
        }
        fprintf(stderr, "state.json console \"%s\": %s\n", name->valuestring,
                opened ? "opened" : "not found");
        if (snprintf(used, sizeof used, "%s.used", path) < (int)sizeof used) {
            rename(path, used);
        }
    }
    cJSON_Delete(root);
}
#endif

/* Temporary startup diagnostics. Use 64-bit milliseconds on the 32-bit device. */
static long long startup_ms(void)
{
    struct timespec t = {0};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

#ifdef MAINUI_ONION
/* RetroArch pans the framebuffer for double/triple buffering and resets the
 * pan on a normal exit. An App that exits through pressMenu2Kill SIGKILLs it,
 * leaving the pan non-zero. Our single-buffered SDL surface is drawn at offset
 * 0, so every frame would be invisible. Best effort: never fails startup. */
static void reset_framebuffer_pan(const char *when)
{
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Cannot open /dev/fb0 to check pan (%s): %s\n", when, strerror(errno));
        return;
    }
    struct fb_var_screeninfo var;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        fprintf(stderr, "Cannot read framebuffer pan (%s): %s\n", when, strerror(errno));
    }
    else if (var.xoffset != 0 || var.yoffset != 0) {
        unsigned x = var.xoffset, y = var.yoffset;
        var.xoffset = 0;
        var.yoffset = 0;
        if (ioctl(fd, FBIOPAN_DISPLAY, &var) < 0 && ioctl(fd, FBIOPUT_VSCREENINFO, &var) < 0) {
            fprintf(stderr, "Cannot reset framebuffer pan from %u,%u (%s): %s\n", x, y, when,
                    strerror(errno));
        }
        else {
            fprintf(stderr, "Reset framebuffer pan from %u,%u to 0,0 (%s)\n", x, y, when);
        }
    }
    close(fd);
}
#endif

/* Miyoo's SDL saves the framebuffer settings when video starts and writes them
 * back in SDL_Quit (FB_VideoQuit: FBIOPUT_VSCREENINFO with saved_vinfo). After
 * a killed RetroArch that restores its non-zero pan, and the next program's
 * single-buffered SDL draws off screen. Stock MainUI never calls SDL_Quit on a
 * normal exit. Keep the clean shutdown and reset the pan afterwards instead.
 * The reset before SDL_Init normally makes the saved pan 0 already. */
static void quit_sdl(const MainUIApp *ui)
{
#ifdef MAINUI_ONION
    bool device = ui->real_device && !ui->snapshot;
#else
    (void)ui;
#endif
    SDL_Quit();
#ifdef MAINUI_ONION
    if (device) {
        reset_framebuffer_pan("exit");
    }
#endif
}

int mainui_setup_session(MainUIApp *ui, int argc, char **argv)
{
    MainUIOptions options;
    int parsed = mainui_options_parse(&options, argc, argv);
    if (parsed >= 0) {
        ui->informational = options.informational;
        return parsed;
    }
    ui->dir = options.dir;
    ui->base = options.base;
    ui->config_dir = options.config_dir;
    ui->list_path = options.list_path;
    ui->snapshot = options.snapshot;
    ui->handoff_dir = options.handoff_dir;
    ui->device_directory = options.device_directory;
    ui->sd = options.sd;
    ui->system_name = options.system_name;
    ui->input_script = options.input_script;
    ui->input_text = options.input_text;
    ui->real_device = options.real_device;
    ui->battery_override = options.battery_override;
    ui->start_systems = options.start_systems;
    ui->refresh_caches = options.refresh_caches;
    ui->snapshot_elapsed = options.snapshot_elapsed;
    ui->battery_percent = options.battery_percent;
    ui->device_adapter = (MainUIDeviceAdapter){0};
    ui->device_status = (MainUIDeviceStatus){.battery = -1, .sleeping = -1, .wifi = -1, .lid = -1};
    ui->next_device_check = 0;
    ui->device_enabled = false;
#ifdef MAINUI_ONION
    ui->handheld_input = true;
#else
    ui->handheld_input = false;
#endif
    if (ui->real_device || ui->device_directory) {
        if ((ui->real_device && ui->device_directory) || !ui->sd ||
            !mainui_device_adapter_open(&ui->device_adapter,
                                        ui->real_device ? DEVICE_ONION : DEVICE_SIMULATED,
                                        ui->real_device ? "/tmp" : ui->device_directory) ||
            (ui->real_device && !mainui_device_signals_install())) {
            fprintf(stderr, "Device adapter unavailable or conflicting options\n");
            return 2;
        }
        ui->device_enabled = true;
        if (!ui->handoff_dir) {
            ui->handoff_dir = ui->device_adapter.runtime;
        }
        ui->device_status = mainui_device_status(&ui->device_adapter);
        if (!ui->battery_override) {
            ui->battery_percent = ui->device_status.battery;
        }
    }
    if (ui->sd) {
        /* Patcher startup contract: restore hidden Recent data before readers,
         * regardless of whether the Recent menu is visible. */
        if (!mainui_library_restore_recent(ui->sd)) {
            fprintf(stderr, "Could not restore hidden Recent list.\n");
        }
        if (!mainui_catalog_path(ui->fallback_path, ui->sd, ui->sd, "miyoo/app") ||
            !mainui_catalog_path(ui->settings_path, ui->sd, ui->sd, "system.json") ||
            !mainui_catalog_path(ui->default_config, ui->sd, ui->sd, ".tmp_update/config")) {
            return 2;
        }
        if (!ui->base) {
            ui->base = ui->dir ? ui->dir : ui->fallback_path;
        }
        if (!ui->config_dir) {
            ui->config_dir = ui->default_config;
        }
        if (!ui->dir) {
            char *text = mainui_read_text(ui->settings_path, 1024 * 1024);
            cJSON *system_settings = text ? cJSON_Parse(text) : NULL;
            const cJSON *active = cJSON_GetObjectItemCaseSensitive(system_settings, "theme");
            const char *value = cJSON_IsString(active) ? active->valuestring : "";
            ui->dir = ui->base;
            if (*value && strcmp(value, "./")) {
                if (mainui_catalog_path(ui->resolved_theme, ui->sd, ui->sd, value)) {
                    ui->dir = ui->resolved_theme;
                }
                else {
                    /* Like a missing theme folder: the stock theme still starts. */
                    fprintf(stderr, "Ignoring unusable theme path in system.json: %.80s\n", value);
                }
            }
            cJSON_Delete(system_settings);
            free(text);
        }
    }
    if (!ui->base) {
        ui->base = ui->dir;
    }
    if (ui->config_dir) {
        mainui_config_load(&ui->config, ui->config_dir);
    }
    else {
        mainui_config_parse(&ui->config, NULL, NULL, NULL, NULL);
    }
    ui->list = calloc(1, sizeof *ui->list);
    ui->catalog = ui->sd ? calloc(1, sizeof *ui->catalog) : NULL;
    bool catalog_ok =
        ui->catalog && mainui_catalog_open(ui->catalog, ui->sd, ui->config.case_sensitive);
    if (ui->catalog && !catalog_ok && ui->catalog->unreadable) {
        /* An unusable Emu leaves an empty Games list; Apps, settings and
         * shutdown still work, which beats a launcher that cannot start. */
        fprintf(stderr, "%s; starting with an empty Games list\n", ui->catalog->error);
        catalog_ok = true;
    }
    if (!ui->list || (ui->sd && !catalog_ok) ||
        (!ui->sd && !mainui_load_list(ui->list, ui->list_path))) {
        fprintf(stderr, "%s\n",
                ui->catalog ? ui->catalog->error : "Cannot read list or allocate catalog");
        if (ui->catalog) {
            mainui_catalog_close(ui->catalog);
        }
        free(ui->catalog);
        if (ui->list) {
            mainui_close_list(ui->list);
        }
        free(ui->list);
        return 2;
    }
    if (ui->refresh_caches && ui->catalog &&
        !mainui_browser_refresh_all(ui->sd, ui->config.case_sensitive, ui->catalog->error)) {
        fprintf(stderr, "%s\n", ui->catalog->error);
        mainui_catalog_close(ui->catalog);
        free(ui->catalog);
        mainui_close_list(ui->list);
        free(ui->list);
        return 4;
    }
    if (ui->system_name) {
        int found = -1;
        for (int i = 0; i < ui->catalog->pages[0].count; i++) {
            if (!strcmp(ui->catalog->pages[0].entries[i].label, ui->system_name)) {
                found = i;
                break;
            }
        }
        if (found < 0 || !mainui_catalog_enter(ui->catalog, found)) {
            fprintf(stderr, "Cannot open requested system: %s\n", ui->system_name);
            mainui_catalog_close(ui->catalog);
            free(ui->catalog);
            mainui_close_list(ui->list);
            free(ui->list);
            return 2;
        }
        mainui_grid_restore(&ui->catalog->pages[0].view, ui->catalog->pages[0].count, found, 4, 2);
    }
    else if (ui->catalog && !ui->snapshot) {
        open_state_console(ui);
    }
    return -1;
}

int mainui_setup_video(MainUIApp *ui)
{
    if (ui->snapshot) {
        SDL_putenv("SDL_VIDEODRIVER=dummy");
    }
#ifdef MAINUI_ONION
    /* Reset before SDL saves the framebuffer settings, so SDL_Quit restores
     * pan 0 rather than a killed RetroArch's buffer (a brief flash of its last
     * frame on exit). The resets after SDL_SetVideoMode and SDL_Quit stay. */
    if (ui->real_device && !ui->snapshot) {
        reset_framebuffer_pan("before SDL");
    }
#endif
    long long t0 = startup_ms();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        if (ui->catalog) {
            mainui_catalog_close(ui->catalog);
        }
        free(ui->catalog);
        mainui_close_list(ui->list);
        free(ui->list);
        return 3;
    }
    long long t1 = startup_ms();
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF: %s\n", TTF_GetError());
        quit_sdl(ui);
        if (ui->catalog) {
            mainui_catalog_close(ui->catalog);
        }
        free(ui->catalog);
        mainui_close_list(ui->list);
        free(ui->list);
        return 3;
    }
    long long t2 = startup_ms();
    /* Request the real device surface; our software screen already buffers
     * drawing. Host and deterministic snapshots retain software video. */
    Uint32 video_flags = ui->real_device && !ui->snapshot ? SDL_HWSURFACE : SDL_SWSURFACE;
    ui->display = SDL_SetVideoMode(640, 480, 32, video_flags);
    long long t3 = startup_ms();
#ifdef MAINUI_ONION
    if (ui->display && ui->real_device) {
        reset_framebuffer_pan("startup");
    }
#endif
    ui->screen = ui->display;
    if (ui->display && ui->real_device) {
        ui->screen = SDL_CreateRGBSurface(
            SDL_SWSURFACE, 640, 480, ui->display->format->BitsPerPixel, ui->display->format->Rmask,
            ui->display->format->Gmask, ui->display->format->Bmask, 0);
    }
    if (!ui->screen || !mainui_theme_open_sd(&ui->theme, ui->dir, ui->base, ui->sd, &ui->config)) {
        fprintf(stderr, "Cannot initialize theme/display: %s\n",
                ui->screen && *ui->theme.error ? ui->theme.error : SDL_GetError());
        if (ui->screen && ui->screen != ui->display) {
            SDL_FreeSurface(ui->screen);
        }
        TTF_Quit();
        quit_sdl(ui);
        if (ui->catalog) {
            mainui_catalog_close(ui->catalog);
        }
        free(ui->catalog);
        mainui_close_list(ui->list);
        free(ui->list);
        return 3;
    }
    if (ui->sd) {
        mainui_language_load(ui->sd, ui->theme.fallback);
    }
    /* Kept deliberately for startup timing diagnostics. */
    fprintf(stderr, "[startup] sdl-init %lld ttf-init %lld set-mode %lld theme %lld\n", t1 - t0,
            t2 - t1, t3 - t2, startup_ms() - t3);
    SDL_WM_SetCaption("Open MainUI", NULL);
    SDL_EnableKeyRepeat(ui->config.repeat_delay, ui->config.repeat_interval);
    SDL_EnableUNICODE(1);
    return -1;
}

void mainui_restore_session(MainUIApp *ui)
{
    ui->preview_sync_once = true;
    mainui_viewport_restore(
        &ui->view, ui->catalog ? ui->catalog->pages[ui->catalog->depth].count : ui->list->count,
        ui->config.rows, 0, 0, ui->config.rows - 1);
    ui->home = ui->catalog && !ui->system_name && !ui->start_systems;
    ui->settings_open = false;
    ui->language_open = false;
    ui->context = (MainUIContext){0};
    ui->home_context = (MainUIContext){0};
    mainui_context_load(&ui->home_context, ui->config_dir, ui->sd);
    ui->context_record = NULL;
    ui->context_open = false;
    ui->confirmation = -1;
    ui->settings = (MainUIStockSettings){0};
    ui->settings_page = (MainUISettingsPage){0};
    ui->settings_keyboard = 0;
    ui->search = (MainUISearch){0};
    ui->catalog_generation = 0;
    ui->search_confirm_held = false;
    ui->reload_search = false;
    ui->next_catalog_check = SDL_GetTicks() + 5000;
    ui->timer_interval = 0;
    ui->search_keyboard = false;
    ui->wifi_was_visible = false;
    ui->apps = NULL;
    ui->games = ui->catalog;
    ui->expert = NULL;
    ui->games_view = ui->view;
    ui->expert_view = (MainUIViewport){0};
    ui->apps_view = (MainUIViewport){0};
    memset(ui->saved_library_views, 0, sizeof ui->saved_library_views);
    memset(ui->message_title, 0, sizeof ui->message_title);
    memset(ui->message_body, 0, sizeof ui->message_body);
    ui->library = NULL;
    ui->favorites = ui->sd ? calloc(1, sizeof *ui->favorites) : NULL;
    if (ui->favorites && !mainui_library_open(ui->favorites, ui->sd, false)) {
        mainui_library_close(ui->favorites);
        free(ui->favorites);
        ui->favorites = NULL;
    }
    memset(ui->favorite_rows, 0, sizeof ui->favorite_rows);
    ui->favorite_editor = (MainUIFavoriteEditor){0};
    ui->name_input = (MainUINameInput){0};
    ui->name_action = CONTEXT_FAVORITE_CREATE;
    ui->systems_view = ui->view;
    mainui_menu_load(&ui->menu, ui->config_dir, &ui->config);
    mainui_viewport_restore(&ui->home_view, ui->menu.count, 4, 0, 0, 3);
    if (ui->catalog && !ui->catalog->depth) {
        mainui_grid_restore(&ui->view, ui->view.total, 0, 4, 2);
    }
    ui->theme.battery_percent = ui->battery_percent;
    mainui_menu_view_open(&ui->menu_view, &ui->theme);
    if (ui->handoff_dir && !ui->system_name && !ui->start_systems) {
        cJSON *returned = mainui_launch_take_return(ui->handoff_dir);
        MainUISession restored = {0};
        if (returned &&
            mainui_session_restore(&restored, ui->sd, ui->config.case_sensitive, ui->config.rows,
                                   cJSON_GetObjectItemCaseSensitive(returned, "resume"),
                                   cJSON_GetObjectItemCaseSensitive(returned, "record"))) {
            mainui_viewport_restore(&ui->home_view, ui->menu.count, 4, restored.home.selected,
                                    restored.home.start, restored.home.end);
            if (restored.home_only) {
                ui->home = true;
            }
            else if (restored.section == MAINUI_MENU_SETTINGS) {
                mainui_stock_settings_load(&ui->settings, ui->config_dir, ui->sd,
                                           ui->device_status.model.id);
                ui->settings.selected = restored.view.selected;
                if (ui->settings.selected < 0 || ui->settings.selected >= ui->settings.count) {
                    ui->settings.selected = 0;
                }
                ui->settings.start = restored.view.start;
                if (ui->settings.start < 0 || ui->settings.selected < ui->settings.start ||
                    ui->settings.selected >= ui->settings.start + 6) {
                    ui->settings.start = ui->settings.selected >= 6 ? ui->settings.selected - 5 : 0;
                }
                ui->home = true;
                ui->settings_open = true;
            }
            else if (restored.library) {
                ui->library = restored.library;
                restored.library = NULL;
                ui->systems_view = ui->view;
                ui->view = restored.view;
                ui->home = false;
            }
            else if (restored.section == MAINUI_MENU_APPS) {
                ui->apps = restored.catalog;
                restored.catalog = NULL;
                ui->apps_view = restored.view;
                ui->home = true;
            }
            else {
                if (restored.section == MAINUI_MENU_EXPERT) {
                    ui->expert = restored.catalog;
                    ui->expert_view = restored.view;
                }
                else {
                    mainui_catalog_close(ui->games);
                    free(ui->games);
                    ui->games = restored.catalog;
                    ui->games_view = restored.view;
                }
                ui->catalog = restored.catalog;
                restored.catalog = NULL;
                ui->view = restored.view;
                ui->home = false;
            }
        }
        const cJSON *resume = cJSON_GetObjectItemCaseSensitive(returned, "resume");
        const cJSON *query = cJSON_GetObjectItemCaseSensitive(resume, "search_query");
        if (!ui->home && !ui->library && cJSON_IsString(query) &&
            mainui_search_open(&ui->search, ui->catalog, &ui->view, query->valuestring,
                               ui->config.rows)) {
            if (mainui_search_restore_view(
                    &ui->search, cJSON_GetObjectItemCaseSensitive(resume, "search_view"),
                    cJSON_GetObjectItemCaseSensitive(returned, "record"), ui->config.rows)) {
                ui->library = ui->search.results;
                ui->view = ui->search.view;
            }
            else {
                mainui_search_close(&ui->search);
            }
        }
        const cJSON *record = cJSON_GetObjectItemCaseSensitive(returned, "record");
        const cJSON *tool = cJSON_GetObjectItemCaseSensitive(record, "launch");
        const cJSON *rom = cJSON_GetObjectItemCaseSensitive(record, "rompath");
        if (cJSON_IsString(tool) && cJSON_IsString(rom) &&
            !strcmp(tool->valuestring, "/mnt/SDCARD/App/Search/launch.sh") &&
            (!strcmp(rom->valuestring, tool->valuestring) || !strcmp(rom->valuestring, "search") ||
             !strcmp(rom->valuestring, "clear") || !strncmp(rom->valuestring, "setstate:", 9) ||
             strstr(rom->valuestring, "/App/Search/data/") ||
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "app_action")))) {
            while (ui->games->depth) {
                mainui_catalog_back(ui->games);
            }
            for (int i = 0; i < ui->games->pages[0].count; ++i) {
                const MainUIEntry *entry = &ui->games->pages[0].entries[i];
                char launcher[4096];
                if (entry->launch &&
                    mainui_catalog_path(launcher, ui->sd, ui->sd, tool->valuestring) &&
                    !strcmp(entry->launch, launcher)) {
                    MainUIViewport target;
                    mainui_grid_restore(&target, ui->games->pages[0].count, i, 4, 2);
                    if (mainui_browser_enter(ui->games, &target, ui->config.rows)) {
                        if (ui->library == ui->search.results) {
                            mainui_search_close(&ui->search);
                            ui->library = NULL;
                        }
                        else if (ui->library) {
                            mainui_library_close(ui->library);
                            free(ui->library);
                            ui->library = NULL;
                        }
                        if (ui->apps) {
                            mainui_catalog_close(ui->apps);
                            free(ui->apps);
                            ui->apps = NULL;
                        }
                        ui->catalog = ui->games;
                        ui->view = ui->games_view = target;
                        ui->home = ui->settings_open = false;
                    }
                    break;
                }
            }
        }
        mainui_session_close(&restored);
        cJSON_Delete(returned);
    }
}

void mainui_setup_render(MainUIApp *ui)
{
    memset(ui->labels, 0, sizeof ui->labels);
    memset(ui->highlighted, 0, sizeof ui->highlighted);
    ui->cached_start = -1;
    ui->status = 0;
    ui->heading_color = ui->theme.title_color;
    ui->heading = TTF_RenderUTF8_Blended(ui->theme.title_font,
                                         ui->search.results ? ui->search.title
                                         : ui->library      ? mainui_library_heading(ui->library)
                                         : ui->catalog      ? mainui_catalog_heading(ui->catalog)
                                                            : "Games",
                                         ui->heading_color);
    ui->timer = NULL;
    ui->selected_at = SDL_GetTicks();
    /* Snapshot tests stay silent. Playback is optional on the host. */
    if (!ui->snapshot) {
        cJSON *system_config = mainui_system_read(ui->sd ? ui->sd : ".");
        const cJSON *volume = cJSON_GetObjectItemCaseSensitive(system_config, "bgmvol");
        int level = cJSON_IsNumber(volume) ? volume->valueint : 20;
        /* mainui_audio_open() logs the specific reason. */
        mainui_audio_open(ui->theme.directory, ui->theme.fallback, level);
        cJSON_Delete(system_config);
    }
    mainui_audio_pause(ui->device_enabled && ui->device_status.sleeping == 1);
    ui->sound_selection = -1;
    ui->sound_section = -1;
    ui->running = true;
    ui->launch_pending = false;
    ui->letter_jump = (MainUILetterJump){0};
    memset(ui->held, 0, sizeof ui->held);
    if (ui->device_enabled) {
        mainui_about_start(&ui->about_job, &ui->device_adapter, ui->sd);
    }
}

int mainui_teardown(MainUIApp *ui)
{
    if (ui->catalog && !ui->library && !ui->home && !ui->snapshot &&
        !mainui_positions_save(ui->catalog, &ui->view)) {
        fprintf(stderr, "Could not save ROM-list position on exit.\n");
    }
    mainui_about_close(&ui->about_job);
    mainui_device_job_close(&ui->device_job);
    mainui_catalog_job_close(&ui->catalog_job);
    if (ui->timer) {
        SDL_RemoveTimer(ui->timer);
        ui->timer = 0;
    }
#ifdef MAINUI_ONION
    /* The loop has already submitted the launch blank. Stop every SDL caller before
     * releasing the device video/audio drivers, then skip unrelated memory
     * cleanup. Host and snapshot runs retain full cleanup for leak checking. */
    if (ui->launch_pending && !ui->snapshot && ui->status == 0) {
        if (ui->preview.thread) {
            SDL_WaitThread(ui->preview.thread, NULL);
            ui->preview.thread = NULL;
        }
        mainui_audio_close();
        quit_sdl(ui);
        mainui_mark(MAINUI_MARK_EXIT);
        mainui_timing_report();
        fflush(stdout);
        fflush(stderr);
        _Exit(0);
    }
#endif
    for (int i = 0; i < 20; i++) {
        if (ui->highlighted[i]) {
            SDL_FreeSurface(ui->highlighted[i]);
        }
        if (ui->labels[i]) {
            SDL_FreeSurface(ui->labels[i]);
        }
    }
    if (ui->heading) {
        SDL_FreeSurface(ui->heading);
    }
    if (ui->library) {
        mainui_favorite_editor_close(&ui->favorite_editor);
        mainui_library_close(ui->library);
    }
    free(ui->library);
    if (ui->favorites) {
        mainui_library_close(ui->favorites);
    }
    free(ui->favorites);
    if (ui->apps) {
        mainui_catalog_close(ui->apps);
    }
    free(ui->apps);
    cJSON_Delete(ui->context_record);
    mainui_name_input_close(&ui->name_input);
    mainui_favorite_editor_close(&ui->favorite_editor);
    mainui_languages_close(&ui->languages);
    mainui_language_close();
    mainui_details_close(&ui->details);
    mainui_gamelist_metadata_close();
    mainui_preview_close(&ui->preview);
    mainui_menu_view_close(&ui->menu_view);
    mainui_audio_close();
    mainui_theme_close(&ui->theme);
    if (ui->screen != ui->display) {
        SDL_FreeSurface(ui->screen);
    }
    TTF_Quit();
    quit_sdl(ui);
    if (ui->catalog) {
        mainui_catalog_close(ui->catalog);
    }
    if (ui->expert && ui->expert != ui->catalog) {
        mainui_catalog_close(ui->expert);
    }
    if (ui->games && ui->games != ui->catalog) {
        mainui_catalog_close(ui->games);
    }
    free(ui->expert);
    free(ui->games);
    mainui_close_list(ui->list);
    free(ui->list);
    mainui_mark(MAINUI_MARK_EXIT);
    mainui_timing_report();
    return ui->status;
}
