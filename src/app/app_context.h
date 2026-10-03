/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_APP_CONTEXT_H
#define MAINUI_APP_CONTEXT_H
#include "app/catalog_job.h"
#include "app/details.h"
#include "app/device_job.h"
#include "app/settings_page.h"
#include "catalog/favorite_edit.h"
#include "core/letter_jump.h"
#include "ui/menu_view.h"
#include "ui/name_input.h"

typedef struct MainUIPreviewList MainUIPreviewList;

/* Adapts three owned list models to the same incremental navigation code. */
typedef struct {
    MainUICatalog *catalog;
    MainUILibrary *library;
    MainUIPreviewList *preview;
} MainUIListLabels;

/* UI-thread state. Allocate once with calloc: workers and borrowed views need
 * stable addresses. Startup opens the resources; teardown closes them.
 * catalog aliases games or expert; library may alias search.results. Option
 * strings borrow argv or the path buffers below. launch_source borrows the
 * current models and viewports and is rebuilt before each key dispatch. */
typedef struct {
    MainUITheme theme;
    MainUIDetails details;
    MainUICatalogJob catalog_job;
    MainUIDeviceJob device_job;
    MainUIAboutJob about_job;
    MainUILanguages languages;
    MainUIPreview preview;
    bool preview_sync_once; /* Include selected artwork in the first list frame. */

    const char *dir;
    const char *base;
    const char *config_dir;
    const char *list_path;
    const char *snapshot;
    const char *handoff_dir;
    const char *device_directory;
    const char *sd;
    const char *system_name;
    const char *input_script;
    const char *input_text;
    bool real_device;
    bool battery_override;
    bool start_systems;
    bool refresh_caches;
    bool informational;
    unsigned snapshot_elapsed;
    int battery_percent;
    MainUIDeviceAdapter device_adapter;
    MainUIDeviceStatus device_status;
    Uint32 next_device_check, next_wifi_scan;
    bool device_enabled;
    char resolved_theme[MAINUI_PATH_MAX];
    char fallback_path[MAINUI_PATH_MAX];
    char settings_path[MAINUI_PATH_MAX];
    char default_config[MAINUI_PATH_MAX];
    bool handheld_input;
    MainUIConfig config;
    MainUIPreviewList *list;
    MainUICatalog *catalog;
    SDL_Surface *display;
    SDL_Surface *screen;
    MainUIViewport view;
    bool home;
    bool settings_open;
    bool language_open;
    MainUIContext context;
    MainUIContext home_context;
    cJSON *context_record;
    bool context_open;
    int confirmation;
    MainUIStockSettings settings;
    MainUISettingsPage settings_page;
    int settings_keyboard;
    MainUISearch search;
    uint64_t catalog_generation;
    bool search_confirm_held;
    bool reload_search;
    Uint32 next_catalog_check;
    int timer_interval;
    bool timer_failure_logged;
    bool search_keyboard;
    bool wifi_was_visible;
    MainUICatalog *apps;
    MainUICatalog *games;
    MainUICatalog *expert;
    MainUIViewport games_view;
    MainUIViewport expert_view;
    MainUIViewport apps_view;
    MainUIViewport saved_library_views[2];
    char message_title[256];
    char message_body[256];
    MainUILibrary *library;
    MainUILibrary *favorites;
    bool favorite_rows[20];
    MainUIFavoriteEditor favorite_editor;
    MainUINameInput name_input;
    MainUIContextAction name_action;
    MainUIViewport systems_view;
    MainUIMenu menu;
    MainUIViewport home_view;
    MainUIMenuView menu_view;
    SDL_Surface *labels[20];
    SDL_Surface *highlighted[20];
    int cached_start;
    int status;
    SDL_Color heading_color;
    SDL_Surface *heading;
    SDL_TimerID timer;
    /* Idle repaint skipping: what the last presented frame showed, and when. */
    bool idle_tick;
    /* Marquee pacing: next frame deadline, and frames shown since scrolling began
     * for the selection made at marquee_origin. */
    bool marquee_paced;
    Uint32 marquee_due, marquee_origin, marquee_steps;
    bool presented_animating;
    Uint32 active_at, presented_at;
    int presented_battery, presented_wifi_signal;
    bool presented_wifi_online;
    char presented_wifi_address[64];
    Uint32 selected_at;
    char selected_identity[MAINUI_PATH_MAX * 3];
    int sound_selection;
    int sound_section;
    bool running;
    bool launch_pending;
    MainUILetterJump letter_jump;
    bool held[SDLK_LAST];
    MainUIViewport settings_view;
    MainUILaunchSource launch_source;
    bool about_visible, animate, draw_frame;
    MainUIListLabels list_labels;
} MainUIApp;
#endif
