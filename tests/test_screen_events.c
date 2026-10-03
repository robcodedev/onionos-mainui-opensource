/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/loop.h"
#include "app/render.h"
#include "app/screen_events.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void reset(MainUIApp *ui)
{
    memset(ui, 0, sizeof *ui);
    ui->running = true;
    ui->home = true;
    ui->confirmation = -1;
    ui->config.rows = 6;
    ui->home_view = (MainUIViewport){5, 1, 0, 3};
    ui->view = (MainUIViewport){3, 1, 0, 2};
}

static bool key_event(MainUIApp *ui, Uint8 type, SDLKey key, Uint16 unicode)
{
    SDL_Event event = {0};
    event.type = type;
    event.key.keysym.sym = key;
    event.key.keysym.unicode = unicode;
    return mainui_dispatch_event(ui, &event);
}

static void home_and_popups(MainUIApp *ui)
{
    reset(ui);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_ESCAPE, 0));
    assert(ui->running && ui->home && ui->home_view.selected == 1);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_LCTRL, 0));
    assert(ui->running && ui->home);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RIGHT, 0));
    assert(ui->home_view.selected == 2);

    /* SELECT closes an existing popup. It must not reopen the home popup. */
    const MainUIContextAction actions[] = {CONTEXT_SHUTDOWN};
    mainui_context_rows(&ui->home_context, actions, 1);
    ui->context = ui->home_context;
    ui->context_open = true;
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RCTRL, 0));
    assert(!ui->context_open && ui->confirmation == -1);
    assert(ui->home_view.selected == 2 && ui->running);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RCTRL, 0));
    assert(ui->context_open);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0));
    assert(!ui->context_open && ui->confirmation == CONTEXT_SHUTDOWN);
    assert(!ui->launch_pending);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_ESCAPE, 0));
    assert(ui->confirmation == -1 && ui->home && ui->running);

    /* Messages consume input before home/list navigation. */
    strcpy(ui->message_title, "Message");
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RIGHT, 0));
    assert(ui->home_view.selected == 2 && *ui->message_title);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_ESCAPE, 0));
    assert(!*ui->message_title && ui->running);
}

static void start_is_consumed(MainUIApp *ui)
{
    /* START is silent when no reload/submission action is available. Check
     * each modal route so it cannot leak into the underlying home/list. */
    for (int screen = 0; screen < 6; ++screen) {
        reset(ui);
        switch (screen) {
        case 1:
            ui->settings_open = true;
            break;
        case 2:
            ui->language_open = true;
            break;
        case 3:
            ui->context_open = true;
            break;
        case 4:
            strcpy(ui->message_title, "Message");
            break;
        case 5:
            ui->home = false;
            break;
        default:
            break;
        }
        assert(key_event(ui, SDL_KEYDOWN, SDLK_F2, 0));
        assert(ui->running && !ui->launch_pending && !ui->catalog_job.thread);
        assert(ui->home_view.selected == 1 && ui->view.selected == 1);
        assert(ui->context_open == (screen == 3));
        assert(ui->settings_open == (screen == 1));
        assert(ui->language_open == (screen == 2));
        assert((bool)*ui->message_title == (screen == 4));
    }
}

static void keyboard_priority(MainUIApp *ui)
{
    reset(ui);
    ui->settings_open = ui->settings_page.open = true;
    ui->settings_keyboard = 1;
    ui->name_input.open = true;
    /* Host SELECT alias remains printable text while the keyboard is open. */
    assert(key_event(ui, SDL_KEYDOWN, SDLK_s, 's'));
    assert(!strcmp(ui->name_input.text, "s") && !ui->context_open);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_F2, 0));
    assert(!ui->name_input.open && !ui->settings_keyboard);
    assert(!strcmp(ui->settings_page.ssid, "s"));
    assert(ui->settings_open && ui->settings_page.open);

    ui->settings_keyboard = 2;
    ui->name_input.open = true;
    ui->settings_page.connect_after_password = true;
    strcpy(ui->name_input.text, "short");
    assert(key_event(ui, SDL_KEYDOWN, SDLK_F2, 0));
    assert(ui->name_input.open && ui->settings_keyboard == 2 && *ui->name_input.error);
    assert(!*ui->settings_page.password && !ui->device_job.thread);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_ESCAPE, 0));
    assert(!ui->name_input.open && !ui->settings_keyboard);
    assert(!ui->settings_page.connect_after_password && ui->settings_page.open);

    /* While another connection runs, a submitted password is not accepted:
     * the keyboard stays open with it and says why (F5). */
    ui->device_enabled = true;
    ui->settings_page.wifi = 1;
    ui->settings_keyboard = 2;
    ui->name_input.open = true;
    ui->settings_page.connect_after_password = true;
    strcpy(ui->name_input.text, "password123");
    ui->device_job.thread = (SDL_Thread *)ui;
    ui->device_job.operation = 3;
    assert(key_event(ui, SDL_KEYDOWN, SDLK_F2, 0));
    assert(ui->name_input.open && ui->settings_keyboard == 2);
    assert(ui->settings_page.connect_after_password && strstr(ui->name_input.error, "busy"));
    assert(!strcmp(ui->name_input.text, "password123") && !ui->device_job.queued_operation);
    ui->device_job.thread = NULL;
    ui->device_job.operation = 0;
    ui->device_enabled = false;
    assert(key_event(ui, SDL_KEYDOWN, SDLK_ESCAPE, 0));
    assert(!ui->name_input.open && !ui->settings_keyboard);
}

/* Holding A after it opened a confirmation never confirms: only a release
 * and a new press do, once. Focus loss cancels it (interaction review F1). */
static void held_confirmation(MainUIApp *ui)
{
    reset(ui);
    const MainUIContextAction actions[] = {CONTEXT_SHUTDOWN};
    mainui_context_rows(&ui->home_context, actions, 1);
    ui->context = ui->home_context;
    ui->context_open = true;
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0));
    assert(ui->confirmation == CONTEXT_SHUTDOWN);
    for (int repeat = 0; repeat < 3; repeat++) {
        assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0));
        assert(ui->confirmation == CONTEXT_SHUTDOWN && !*ui->message_title);
    }
    assert(key_event(ui, SDL_KEYUP, SDLK_RETURN, 0));
    assert(ui->confirmation == CONTEXT_SHUTDOWN && !ui->launch_pending);
    /* The new press confirms: without the runtime, shutdown reports why. */
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0));
    assert(ui->confirmation == -1 && *ui->message_title);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0)); /* its repeat */
    assert(*ui->message_title);
    assert(key_event(ui, SDL_KEYUP, SDLK_RETURN, 0));
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0));
    assert(!*ui->message_title);
    assert(key_event(ui, SDL_KEYUP, SDLK_RETURN, 0));

    ui->context = ui->home_context;
    ui->context_open = true;
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0));
    assert(ui->confirmation == CONTEXT_SHUTDOWN);
    SDL_Event event = {0};
    event.type = SDL_ACTIVEEVENT;
    event.active.state = SDL_APPINPUTFOCUS;
    assert(mainui_dispatch_event(ui, &event));
    assert(ui->confirmation == -1 && !ui->return_latched);
}

/* A message drawn over Details takes the keys first: A or Back dismisses
 * it, nothing reaches Details, and A held on goes no further (F3). */
static void message_over_details(MainUIApp *ui)
{
    reset(ui);
    ui->home = false;
    ui->details.open = true;
    ui->handoff_dir = "/nonexistent/mainui-handoff";
    strcpy(ui->message_title, "Catalog unavailable");
    assert(key_event(ui, SDL_KEYDOWN, SDLK_DOWN, 0));
    assert(*ui->message_title && ui->details.open && ui->view.selected == 1);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0));
    assert(!*ui->message_title && ui->details.open && !ui->launch_pending);
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0)); /* held: no launch */
    assert(!*ui->message_title && ui->details.open && !ui->launch_pending);
    assert(key_event(ui, SDL_KEYUP, SDLK_RETURN, 0));
    strcpy(ui->message_title, "Catalog unavailable");
    assert(key_event(ui, SDL_KEYDOWN, SDLK_ESCAPE, 0));
    assert(!*ui->message_title && ui->details.open);
    ui->details.open = false;
    ui->handoff_dir = NULL;
}

static void releases_and_focus(MainUIApp *ui)
{
    reset(ui);
    ui->search.release_pending = true;
    ui->held[SDLK_RETURN] = true;
    assert(key_event(ui, SDL_KEYDOWN, SDLK_RETURN, 0));
    assert(ui->search.release_pending && ui->home_view.selected == 1);
    assert(key_event(ui, SDL_KEYUP, SDLK_SPACE, 0));
    assert(!ui->search.release_pending && !ui->held[SDLK_RETURN]);

    ui->held[SDLK_HOME] = true;
    ui->letter_jump.active = true;
    SDL_Event event = {0};
    event.type = SDL_ACTIVEEVENT;
    event.active.state = SDL_APPINPUTFOCUS;
    assert(mainui_dispatch_event(ui, &event));
    assert(!ui->held[SDLK_HOME] && !ui->letter_jump.active);
    event.active.gain = 1;
    assert(mainui_dispatch_event(ui, &event));
    event.type = SDL_QUIT;
    assert(mainui_dispatch_event(ui, &event));
    assert(!ui->running);
}

static void wake_intervals(MainUIApp *ui)
{
    reset(ui);
    assert(mainui_wait_interval(ui, 1000) == 5000);
    ui->device_enabled = true;
    assert(mainui_wait_interval(ui, 1000) == 500);
    /* Worker handles are only inspected for presence by these functions. */
    ui->device_job.thread = (SDL_Thread *)ui;
    ui->preview.thread = (SDL_Thread *)ui;
    ui->preview.pending = true;
    mainui_prepare_frame(ui);
    assert(!ui->animate);
    assert(mainui_wait_interval(ui, 1000) == 500);
    ui->catalog_job.thread = (SDL_Thread *)ui;
    ui->catalog_job.started_at = 900;
    mainui_prepare_frame(ui);
    assert(!ui->animate);
    assert(mainui_wait_interval(ui, 1000) == 400);
    assert(mainui_wait_interval(ui, 1399) == 1);
    assert(mainui_wait_interval(ui, 1400) == 500);
    ui->device_enabled = false;
    assert(mainui_wait_interval(ui, 1400) == 5000);
    /* mainui_prepare_frame() stamped the selection with SDL_GetTicks(), which
     * before SDL_Init can be any value; pin it for the checks below. */
    ui->selected_at = 1000;
    ui->animate = true;
    assert(mainui_wait_interval(ui, 1400) == MAINUI_MARQUEE_FRAME_MS);
    /* A long title in its scroll delay keeps maintenance ticks, capped to wake
     * when it starts moving; 30 s is a valid delay (review of v7). */
    ui->device_enabled = true;
    ui->selected_at = 1000;
    ui->config.scroll_delay = 30000;
    assert(!mainui_marquee_moving(ui, 1400) && mainui_wait_interval(ui, 1400) == 500);
    assert(mainui_wait_interval(ui, 30800) == 200);
    assert(!mainui_marquee_moving(ui, 30959) && mainui_marquee_moving(ui, 30960));
    assert(mainui_wait_interval(ui, 30960) == MAINUI_MARQUEE_FRAME_MS);
    assert(mainui_wait_interval(ui, 90000) == MAINUI_MARQUEE_FRAME_MS);
    ui->device_enabled = false;
    ui->config.scroll_delay = 0;
    assert(mainui_wait_interval(ui, 1400) == MAINUI_MARQUEE_FRAME_MS);
    ui->letter_jump.active = true;
    assert(!mainui_marquee_moving(ui, 1400));
    assert(mainui_wait_interval(ui, 1400) == 17);
    ui->letter_jump.active = ui->animate = false;
    ui->catalog_job.started_at = (Uint32)-100;
    assert(mainui_wait_interval(ui, 100) == 300);
    reset(ui);
}

/* SDL_GetTicks() wraps after 49.7 days. Deadlines and intervals that cross
 * the wrap must behave as they do anywhere else. */
static void tick_wraparound(MainUIApp *ui)
{
    const Uint32 before = 0xFFFFF000u; /* 4096 ms before the wrap */
    reset(ui);
    ui->device_enabled = true;
    ui->animate = true;
    ui->selected_at = before;
    ui->config.scroll_delay = 30000;
    assert(!mainui_marquee_moving(ui, before + 1000));
    assert(mainui_wait_interval(ui, before + 1000) == 500);
    assert(mainui_wait_interval(ui, before + 29800) == 200);
    assert(!mainui_marquee_moving(ui, before + 29959));
    assert(mainui_marquee_moving(ui, before + 29960));
    assert(mainui_wait_interval(ui, before + 29960) == MAINUI_MARQUEE_FRAME_MS);
    ui->animate = false;
    ui->catalog_job.thread = (SDL_Thread *)ui;
    ui->catalog_job.started_at = (Uint32)-50;
    assert(mainui_wait_interval(ui, 50) == 400);
    ui->catalog_job.thread = NULL;

    ui->device_enabled = false;
    ui->idle_tick = true;
    ui->active_at = (Uint32)-2000;
    ui->presented_at = (Uint32)-100;
    assert(mainui_frame_current(ui, 100));
    assert(!mainui_frame_current(ui, (Uint32)-100 + 5000));
    ui->active_at = (Uint32)-500;
    assert(!mainui_frame_current(ui, 100));

    reset(ui);
}

/* Idle ticks skip the repaint only when the presented frame is still exact (#9). */
static void idle_frame(MainUIApp *ui)
{
    reset(ui);
    ui->idle_tick = true;
    ui->active_at = 1000;
    ui->presented_at = 2000;
    ui->theme.battery_percent = ui->presented_battery = 80;
    assert(mainui_frame_current(ui, 2500));
    /* The settle period and the periodic repaint bound what is skipped. */
    ui->active_at = 2000;
    assert(!mainui_frame_current(ui, 2500));
    ui->active_at = 1000;
    assert(!mainui_frame_current(ui, 7000));
    /* Every other wake, and anything live on screen, repaints. */
    ui->idle_tick = false;
    assert(!mainui_frame_current(ui, 2500));
    ui->idle_tick = true;
    ui->presented_animating = true;
    assert(!mainui_frame_current(ui, 2500));
    ui->presented_animating = false;
    ui->preview.pending = true;
    assert(!mainui_frame_current(ui, 2500));
    ui->preview.pending = false;
    ui->catalog_job.thread = (SDL_Thread *)ui;
    assert(!mainui_frame_current(ui, 2500));
    ui->catalog_job.thread = NULL;
    /* The periodic status refresh changes only the header; other Wi-Fi work repaints. */
    ui->device_job.thread = (SDL_Thread *)ui;
    ui->device_job.operation = 0;
    assert(mainui_frame_current(ui, 2500));
    ui->device_job.queued_operation = 5;
    assert(!mainui_frame_current(ui, 2500));
    ui->device_job.queued_operation = 0;
    ui->device_job.operation = 4;
    assert(!mainui_frame_current(ui, 2500));
    ui->device_job.thread = NULL;
    ui->device_job.operation = 0;
    ui->settings_page.open = true;
    assert(!mainui_frame_current(ui, 2500));
    ui->settings_page.open = false;
    ui->confirmation = 0;
    assert(!mainui_frame_current(ui, 2500));
    ui->confirmation = -1;
    /* Header status changes repaint. */
    ui->theme.battery_percent = 79;
    assert(!mainui_frame_current(ui, 2500));
    ui->theme.battery_percent = 80;
    ui->theme.wifi_online = true;
    assert(!mainui_frame_current(ui, 2500));
    ui->theme.wifi_online = false;
    snprintf(ui->theme.wifi_address, sizeof ui->theme.wifi_address, "10.0.0.2");
    assert(!mainui_frame_current(ui, 2500));
    ui->theme.wifi_address[0] = 0;
    assert(mainui_frame_current(ui, 2500));
    /* A scrolling title keeps row-only frames for 30 s; idle repaints every 5 s. */
    ui->home = false;
    ui->catalog = (MainUICatalog *)calloc(1, sizeof *ui->catalog);
    assert(ui->catalog);
    ui->catalog->depth = 1;
    ui->config.row_height = 60;
    ui->view = (MainUIViewport){3, 1, 0, 2};
    ui->presented_animating = true;
    assert(mainui_frame_marquee_only(ui, 2500) && mainui_frame_marquee_only(ui, 20000));
    assert(!mainui_frame_marquee_only(ui, 32000));
    ui->idle_tick = false;
    assert(!mainui_frame_marquee_only(ui, 2500));
    free(ui->catalog);
    reset(ui);
}

static void selection_identity(MainUIApp *ui)
{
    reset(ui);
    ui->home = false;
    MainUICatalog *catalog = calloc(1, sizeof *catalog);
    assert(catalog);
    MainUIEntry entry = {.label = "Game", .path = "/Roms/Game.zip"};
    catalog->depth = 1;
    catalog->pages[1].entries = &entry;
    catalog->pages[1].count = catalog->pages[1].loaded = 1;
    ui->catalog = catalog;
    ui->view.selected = 0;
    mainui_prepare_frame(ui);
    ui->selected_at = (Uint32)-10000;
    mainui_prepare_frame(ui);
    assert(ui->selected_at == (Uint32)-10000);
    entry.path = "/Roms/Game.7z";
    mainui_prepare_frame(ui);
    assert(ui->selected_at != (Uint32)-10000);
    ui->selected_at = (Uint32)-10000;
    ui->confirmation = CONTEXT_DELETE_ROM;
    mainui_prepare_frame(ui);
    assert(ui->selected_at != (Uint32)-10000);
    ui->confirmation = -1;
    MainUILibrary *library = calloc(1, sizeof *library);
    assert(library);
    MainUILibraryItem item = {.label = "Game", .identity = "first"};
    library->current = -1;
    library->items = &item;
    library->count = library->visible_count = 1;
    ui->library = library;
    mainui_prepare_frame(ui);
    ui->selected_at = (Uint32)-10000;
    item.identity = "replacement";
    mainui_prepare_frame(ui);
    assert(ui->selected_at != (Uint32)-10000);
    free(library);
    free(catalog);
    reset(ui);
}

static void settings_windows(MainUIApp *ui)
{
    reset(ui);
    ui->settings_open = true;
    ui->settings.count = 10;
    for (int i = 0; i < 7; ++i) {
        mainui_screen_settings_open(ui, SDLK_DOWN);
    }
    mainui_screen_settings_open(ui, SDLK_UP);
    assert(ui->settings.selected == 6 && ui->settings.start == 2);
    ui->settings.selected = ui->settings.start = 0;
    mainui_screen_settings_open(ui, SDLK_UP);
    assert(ui->settings.selected == 9 && ui->settings.start == 4);
    mainui_screen_settings_open(ui, SDLK_DOWN);
    assert(ui->settings.selected == 0 && ui->settings.start == 0);

    ui->language_open = true;
    ui->languages.count = 10;
    for (int i = 0; i < 7; ++i) {
        mainui_screen_language_open(ui, SDLK_DOWN);
    }
    mainui_screen_language_open(ui, SDLK_UP);
    assert(ui->languages.selected == 6 && ui->languages.start == 2);
    ui->languages.selected = ui->languages.start = 0;
    mainui_screen_language_open(ui, SDLK_UP);
    assert(ui->languages.selected == 9 && ui->languages.start == 4);
    mainui_screen_language_open(ui, SDLK_DOWN);
    assert(ui->languages.selected == 0 && ui->languages.start == 0);

    MainUISettingsPage page = {.kind = SET_WIFI, .wifi = 1, .network_count = 10};
    for (int i = 0; i < 6; ++i) {
        mainui_settings_page_key(&page, SDLK_DOWN, NULL, NULL);
    }
    mainui_settings_page_key(&page, SDLK_UP, NULL, NULL);
    assert(page.selected == 5 && page.start == 2);
    page.selected = page.start = 0;
    mainui_settings_page_key(&page, SDLK_UP, NULL, NULL);
    assert(page.selected == 10 && page.start == 6);
    mainui_settings_page_key(&page, SDLK_DOWN, NULL, NULL);
    assert(page.selected == 0 && page.start == 0);
    reset(ui);
}

int mainui_suite_screen_events(void)
{
    MainUIApp *ui = calloc(1, sizeof *ui);
    assert(ui);
    settings_windows(ui);
    selection_identity(ui);
    wake_intervals(ui);
    tick_wraparound(ui);
    idle_frame(ui);
    home_and_popups(ui);
    start_is_consumed(ui);
    keyboard_priority(ui);
    held_confirmation(ui);
    message_over_details(ui);
    releases_and_focus(ui);
    free(ui);
    return 0;
}
