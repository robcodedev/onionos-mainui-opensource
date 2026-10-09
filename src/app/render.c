/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/render.h"
#include "platform/audio.h"
#include "platform/timing.h"
#include "ui/drawing.h"
#include "ui/panels.h"
#include "ui/search_label.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read the line-oriented format used by legacy Favorite/Recent records.
 * This preview only displays labels: it does not claim to implement the full
 * Recent filtering or Favorite-folder model, and never rewrites the input.
 */
bool mainui_load_list(MainUIPreviewList *list, const char *path)
{
    list->data = mainui_read_text(path, 8 * 1024 * 1024);
    if (!list->data) {
        return false;
    }
    char *line = list->data;
    while (*line && list->count < 10000) {
        char *next = strchr(line, '\n');
        if (next) {
            *next = 0;
        }
        cJSON *item = cJSON_ParseWithOpts(line, NULL, true);
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (cJSON_IsObject(item) && cJSON_IsString(label)) {
            list->records[list->count] = item;
            list->labels[list->count++] = label->valuestring;
        }
        else {
            cJSON_Delete(item);
        }
        if (!next) {
            break;
        }
        line = next + 1;
    }
    return true;
}

void mainui_close_list(MainUIPreviewList *list)
{
    for (int i = 0; i < list->count; i++) {
        cJSON_Delete(list->records[i]);
    }
    free(list->data);
}

static void blit(SDL_Surface *surface, SDL_Surface *screen, int x, int y)
{
    SDL_Rect dest = {(Sint16)x, (Sint16)y, 0, 0};
    if (surface) {
        SDL_BlitSurface(surface, NULL, screen, &dest);
    }
}

const char *mainui_list_label_at(void *context, int index)
{
    MainUIListLabels *source = context;
    if (source->library) {
        return mainui_library_label(source->library, index);
    }
    if (source->catalog) {
        return mainui_browser_label(source->catalog, index);
    }
    return source->preview->labels[index];
}

/* Stock list headers retain the section/system name at every folder depth. */
const char *mainui_library_heading(const MainUILibrary *library)
{
    return library->recent ? mainui_translate(18, "Recents") : mainui_translate(1, "Favorites");
}

const char *mainui_catalog_heading(const MainUICatalog *catalog)
{
    return catalog->pages[catalog->depth > 0 ? 1 : 0].title;
}

/* Pagination keeps physical rows; counters exclude folders as in the patcher. */
static void draw_list_counter(SDL_Surface *screen, MainUITheme *theme, const MainUILibrary *library,
                              MainUICatalog *catalog, const MainUIViewport *view)
{
    int current = view->selected + 1;
    int total = view->total;
    if (total <= 0) {
        mainui_draw_footer(screen, theme, 0, -1);
        return;
    }
    if (library) {
        if (mainui_library_is_folder(library, view->selected)) {
            int row = library->visible[view->selected];
            if (row != INT_MIN) {
                mainui_draw_folder_footer(screen, theme, library->folders[-row - 1].direct_games);
                return;
            }
            mainui_draw_footer(screen, theme, 0, -1);
            return;
        }
        current -= library->leading_folders;
        total = library->visible_games;
    }
    else if (catalog) {
        if (mainui_browser_folder(catalog, view->selected)) {
            mainui_draw_footer(screen, theme, 0, -1);
            return;
        }
        int folders = catalog->pages[catalog->depth].folder_count + (catalog->depth > 1);
        current -= folders;
        total -= folders;
    }
    mainui_draw_footer(screen, theme, current, total);
}

void mainui_prepare_frame(MainUIApp *ui)
{
    if (!ui->home && !ui->library && ui->catalog) {
        if (ui->catalog == ui->games) {
            ui->games_view = ui->view;
        }
        else if (ui->catalog == ui->expert) {
            ui->expert_view = ui->view;
        }
    }
    ui->list_labels = (MainUIListLabels){ui->catalog, ui->library, ui->list};
    int destination;
    if (mainui_letter_jump_step(&ui->letter_jump, mainui_list_label_at, &ui->list_labels,
                                &destination)) {
        mainui_viewport_move(&ui->view, ui->config.rows, destination - ui->view.selected, false);
        ui->selected_at = SDL_GetTicks();
    }
    /* Changes to the active selection produce one sample, independent of repaint
     * frequency and marquee timer ticks. Initial presentation is silent. */
    int section = 4;
    int selection = ui->view.selected;
    if (ui->name_input.open) {
        section = 7;
        selection = ui->name_input.selected;
    }
    else if (ui->context_open) {
        section = 5;
        selection = ui->context.selected;
    }
    else if (ui->language_open) {
        section = 6;
        selection = ui->languages.selected;
    }
    else if (ui->settings_open) {
        section = 2;
        selection = ui->settings.selected;
    }
    else if (ui->apps) {
        section = 1;
        selection = ui->apps_view.selected;
    }
    else if (ui->home) {
        section = 0;
        selection = ui->home_view.selected;
    }
    else if (ui->library) {
        section = 3;
    }
    if (ui->sound_section == section && ui->sound_selection != selection) {
        mainui_audio_change();
    }
    /* Own row identity: the index/label can survive deletion or replacement. */
    const char *key = "", *container = "", *label = "";
    if (section == 3 && selection >= 0 && selection < ui->library->visible_count) {
        label = mainui_library_label(ui->library, selection);
        int row = ui->library->visible[selection];
        container = ui->library->current >= 0 ? ui->library->folders[ui->library->current].id : "";
        key = row == INT_MIN ? ".."
              : row < 0      ? ui->library->folders[-row - 1].id
                             : ui->library->items[row].identity;
    }
    else if (section == 4 && ui->catalog) {
        container = ui->catalog->pages[ui->catalog->depth].path;
        MainUIEntry *entry =
            mainui_catalog_entry(ui->catalog, mainui_browser_index(ui->catalog, selection));
        key = entry ? entry->path : "..";
    }
    else if (section == 0 && selection >= 0 && selection < ui->menu.count) {
        key = mainui_menu_label(ui->menu.sections[selection]);
    }
    else if (section == 1) {
        MainUIEntry *entry = ui->apps ? mainui_catalog_entry(ui->apps, selection) : NULL;
        key = entry ? entry->path : "";
    }
    else if (section == 2 && selection >= 0 && selection < ui->settings.count) {
        key = mainui_stock_setting_label(ui->settings.rows[selection]);
    }
    else if (section == 5 && selection >= 0 && selection < ui->context.visible_count) {
        key = mainui_context_label(&ui->context.entries[ui->context.visible[selection]]);
    }
    else if (section == 6 && selection >= 0 && selection < ui->languages.count) {
        key = ui->languages.entries[selection].name;
    }
    else if (section == 4 && ui->list && selection >= 0 && selection < ui->list->count) {
        key = ui->list->labels[selection];
    }
    char identity[sizeof ui->selected_identity];
    snprintf(identity, sizeof identity, "%d:%d:%p:%p:%s:%s:%s", section, ui->confirmation,
             (void *)ui->catalog, (void *)ui->library, container ? container : "", key ? key : "",
             label ? label : "");
    if (strcmp(identity, ui->selected_identity) || ui->confirmation >= 0) {
        memcpy(ui->selected_identity, identity, strlen(identity) + 1);
        ui->selected_at = SDL_GetTicks();
    }
    ui->sound_section = section;
    ui->sound_selection = selection;
    ui->animate = ui->letter_jump.active;
    SDL_Event queued_input;
    ui->draw_frame =
        ui->snapshot || ui->input_script ||
        (!ui->letter_jump.active &&
         SDL_PeepEvents(&queued_input, 1, SDL_PEEKEVENT, SDL_KEYDOWNMASK | SDL_KEYUPMASK) <= 0);
}

/* Idle ticks otherwise repaint the whole frame every 500 ms on the device (#9).
 * Skip only on static browsing screens, with nothing animating or in flight,
 * after a settle period, and while the header status is unchanged. A periodic
 * repaint bounds any staleness, e.g. if another process drew on the display. */
enum {
    IDLE_SETTLE_MS = 1000,
    IDLE_REPAINT_MS = 5000,
    /* A full repaint interrupts scrolling on the device; keep it rare there. */
    MARQUEE_REPAINT_MS = 30000
};

/* Nothing but time has changed since the last presented frame. The periodic
 * status refresh only affects the header, which is compared below. */
static bool idle_unchanged(const MainUIApp *ui, Uint32 now, Uint32 repaint_ms)
{
    bool status_only = !ui->device_job.thread ||
                       (ui->device_job.operation == 0 && !ui->device_job.queued_operation);
    return ui->idle_tick && !ui->snapshot && !ui->input_script && !ui->letter_jump.active &&
           !ui->catalog_job.thread && status_only && !ui->about_job.thread && !ui->preview.thread &&
           !ui->preview.pending && !ui->settings_open && !ui->settings_page.open &&
           !ui->language_open && !ui->details.open && !ui->name_input.open &&
           !ui->search_keyboard && !ui->context_open && ui->confirmation < 0 &&
           !*ui->message_title && !ui->launch_pending && now - ui->presented_at < repaint_ms &&
           ui->presented_battery == ui->theme.battery_percent &&
           ui->presented_wifi_online == ui->theme.wifi_online &&
           ui->presented_wifi_signal == ui->theme.wifi_signal_level &&
           !strcmp(ui->presented_wifi_address, ui->theme.wifi_address);
}

bool mainui_load_deferred_icons(MainUIApp *ui)
{
    bool shown = false;
    while (ui->systems_drawn && !ui->catalog_job.thread && ui->catalog && !ui->catalog->depth &&
           !ui->library) {
        SDL_Event waiting;
        SDL_PumpEvents();
        if (SDL_PeepEvents(&waiting, 1, SDL_PEEKEVENT, SDL_ALLEVENTS) > 0 ||
            !mainui_menu_view_load_pending(&ui->menu_view, &ui->view, &shown) || shown) {
            break;
        }
    }
    return shown;
}

bool mainui_frame_current(const MainUIApp *ui, Uint32 now)
{
    return idle_unchanged(ui, now, IDLE_REPAINT_MS) && !ui->presented_animating &&
           now - ui->active_at >= IDLE_SETTLE_MS;
}

/* Scrolling titles otherwise repaint, rotate and flip the whole frame at 30 fps. */
bool mainui_frame_marquee_only(const MainUIApp *ui, Uint32 now)
{
    int row = ui->view.selected - ui->view.start;
    bool list = !ui->home && !ui->apps && (ui->library || ui->list || ui->catalog) &&
                !(ui->catalog && !ui->library && !ui->catalog->depth);
    /* Lists with the empty panel over their rows always take the full path. */
    bool empty = !ui->view.total ||
                 (ui->catalog && !ui->library && ui->catalog->depth > 1 && ui->view.total == 1) ||
                 (ui->library && ui->library->current >= 0 && ui->library->visible_count == 1 &&
                  ui->library->visible[0] == INT_MIN);
    return idle_unchanged(ui, now, MARQUEE_REPAINT_MS) && ui->presented_animating && list &&
           !empty && row >= 0 && row < ui->config.rows && ui->view.selected < ui->view.total &&
           ui->config.row_height > 0 && 60 + (row + 1) * ui->config.row_height <= 420;
}

/* Set only while the host check re-composes a frame for the same instant. */
static const Uint32 *pinned_elapsed;

static Uint32 list_elapsed(const MainUIApp *ui)
{
    return pinned_elapsed ? *pinned_elapsed
           : ui->snapshot ? ui->snapshot_elapsed
                          : SDL_GetTicks() - ui->selected_at;
}

/* One list row, clipped to its own rectangle. Shared by full frames and
 * marquee-only frames so both compose the row identically. */
static void draw_list_row(MainUIApp *ui, int i, Uint32 elapsed)
{
    int list_origin = 0;
    int list_width = 640;
    int outer = ui->config.rows <= 9 ? 20 : (ui->config.rows >= 14 ? 15 : 29 - ui->config.rows);
    int gap = ui->config.rows <= 7 ? 15 : (ui->config.rows >= 17 ? 5 : 22 - ui->config.rows);
    int y = 60 + i * ui->config.row_height;
    SDL_Rect clip = {(Sint16)list_origin, (Sint16)y, (Uint16)list_width,
                     (Uint16)ui->config.row_height};
    SDL_SetClipRect(ui->screen, &clip);
    bool selected = ui->view.start + i == ui->view.selected;
    if (selected) {
        if (ui->theme.selection) {
            blit(ui->theme.selection, ui->screen, list_origin, y);
        }
        else {
            SDL_FillRect(ui->screen, &clip, SDL_MapRGB(ui->screen->format, 68, 68, 68));
        }
    }
    bool folder_row = ui->library
                          ? mainui_library_is_folder(ui->library, ui->view.start + i)
                          : ui->catalog && mainui_browser_folder(ui->catalog, ui->view.start + i);
    SDL_Surface *row_icon = folder_row ? ui->theme.folder : ui->theme.icon;
    /* Transparent ui->theme spacers still have a meaningful width.
    * Reserving a synthetic 71px slot indented Silky by 70px. */
    int icon_width = row_icon ? row_icon->w : 0;
    int icon_x = list_origin + (ui->theme.icon_margin >= 0 ? ui->theme.icon_margin : 5);
    int text_x = row_icon ? icon_x + icon_width + gap : list_origin + outer;
    int available = list_width - outer - text_x;
    if (ui->favorite_rows[i] && (!ui->library || ui->library->recent || ui->search.results) &&
        ui->theme.favorite && !folder_row) {
        bool spacer = ui->theme.icon && ui->theme.icon->w >= 120 &&
                      ui->theme.icon->w >= 3 * ui->theme.icon->h;
        int origin =
            spacer || (ui->config.dynamic_favorite_position && !ui->preview.image) ? 0 : 250;
        int marker_x = 640 - ui->theme.favorite->w - origin - 20;
        blit(ui->theme.favorite, ui->screen, marker_x,
             y + (ui->config.row_height - ui->theme.favorite->h) / 2);
        if (marker_x - 6 - text_x < available) {
            available = marker_x - 6 - text_x;
        }
    }
    if (available < 1) {
        available = 1;
    }
    /* Preview opacity belongs to the later compositor, not the text
    * clip. Only overflow activation uses the live ui->preview edge. */
    int activation_width = available;
    int pane_width = mainui_preview_edge(&ui->theme) - text_x;
    if (ui->preview.image && pane_width > 0 && pane_width < activation_width) {
        activation_width = pane_width;
    }
    if (row_icon) {
        SDL_Rect source = {0, 0, (Uint16)icon_width, (Uint16)ui->config.row_height};
        if (source.w > row_icon->w) {
            source.w = (Uint16)row_icon->w;
        }
        if (source.h > row_icon->h) {
            source.h = (Uint16)row_icon->h;
        }
        source.y = (Sint16)((row_icon->h - source.h) / 2);
        SDL_Rect dest = {(Sint16)icon_x, (Sint16)(y + (ui->config.row_height - source.h) / 2), 0,
                         0};
        SDL_BlitSurface(row_icon, &source, ui->screen, &dest);
    }
    SDL_Surface *label = !selected && ui->highlighted[i] ? ui->highlighted[i] : ui->labels[i];
    if (!label) {
        return;
    }
    int label_y = y + (ui->config.row_height - label->h) / 2;
    ui->animate =
        ui->animate || (selected && label->w > activation_width && ui->config.scroll_status == 2);
    if (selected && ui->config.scroll_status == 2 && elapsed >= (unsigned)ui->config.scroll_delay &&
        label->w > activation_width) {
        MainUIBlit segments[2];
        /* One whole-pixel step per paced frame, so a late frame never jumps twice.
         * Snapshots render a given instant. 1000 px/s maps pixels to ms exactly. */
        uint64_t offset =
            ui->snapshot || ui->marquee_origin != ui->selected_at
                ? mainui_marquee_pixels(elapsed - (unsigned)ui->config.scroll_delay,
                                        ui->config.scroll_speed)
                : mainui_marquee_step_pixels(ui->marquee_steps, ui->config.scroll_speed);
        int n = mainui_marquee_stream(offset, 1000, label->w, available, segments);
        for (int part = 0; part < n; part++) {
            SDL_Rect source = {(Sint16)segments[part].source_x, 0, (Uint16)segments[part].width,
                               (Uint16)label->h};
            SDL_Rect dest = {(Sint16)(text_x + segments[part].destination_x), (Sint16)label_y, 0,
                             0};
            SDL_BlitSurface(label, &source, ui->screen, &dest);
        }
    }
    else {
        SDL_Rect source = {0, 0, (Uint16)available, (Uint16)label->h};
        SDL_Rect dest = {(Sint16)text_x, (Sint16)label_y, 0, 0};
        SDL_BlitSurface(label, &source, ui->screen, &dest);
    }
}

/* Rotate (on the device) and flip. A rectangle limits the rotated copy; the
 * logical frame stays upright and complete for later partial frames. */
static void present(MainUIApp *ui, const SDL_Rect *area)
{
    /* Spacing of consecutive animation frames, for the timing report. */
    static Uint32 previous;
    Uint32 now = SDL_GetTicks();
    if (ui->presented_animating && ui->animate) {
        Uint32 gap = now - previous;
        mainui_count_add(gap < 35    ? "gap-under35"
                         : gap <= 45 ? "gap-40"
                         : gap <= 65 ? "gap-50-60"
                                     : "gap-over65",
                         1);
    }
    previous = now;
    ui->presented_animating = ui->animate;
    SDL_Rect shown = area ? *area : (SDL_Rect){0, 0, 0, 0};
    if (ui->real_device) {
        /* Copy inverted pixels directly to the display. */
        if (area) {
            shown = mainui_rotated_rect(ui->screen, *area);
            SDL_SetClipRect(ui->display, &shown);
        }
        mainui_blit_rotated(ui->screen, ui->display);
        SDL_SetClipRect(ui->display, NULL);
    }
    /* Without double buffering SDL_Flip is SDL_UpdateRect of the whole screen
     * (Onion's SDL: "use SDL_UpdateRect when flip"). A row-only frame updates
     * just its rectangle: less copying and a far smaller window for tearing. */
    if (area && !(ui->display->flags & SDL_DOUBLEBUF)) {
        SDL_UpdateRects(ui->display, 1, &shown);
        mainui_count_add("frames", 1);
    }
    else if (SDL_Flip(ui->display) == 0) {
        mainui_mark(MAINUI_MARK_FIRST_FRAME);
        mainui_count_add("frames", 1);
    }
}

/* A list page could not be read. Each step runs once, in the background,
 * keeping the selected row and window: reload the list (it may have changed
 * outside MainUI); if the cache content itself is damaged, rebuild it as
 * Refresh roms does; then browse the console by scanning its folder for this
 * visit (the next entry reads the cache again). Search results get only the reload: nothing else reproduces them. A busy, I/O or memory failure never replaces the cache. Only if
 * the scan fails too, leave the list with a message: then the folder itself
 * cannot be read. True when it left. */
static bool recover_page(MainUIApp *ui, const MainUILaunchSource *source)
{
    MainUICatalog *catalog = ui->catalog;
    int step = ui->page_recovery + 1;
    /* Search's results come from its own database: after the reload, only
     * running Search again can repair them. */
    bool search = mainui_catalog_search_results(catalog);
    if (search && step > 1) {
        step = 4;
    }
    if (step == 2 && !mainui_catalog_page_damaged(catalog)) {
        step = 3;
    }
    /* An unfinished ROM deletion is never set aside automatically: the
     * rebuild only recovers it, and no scan bypasses it. Refresh roms, asked
     * for, may abandon it. */
    bool pending = mainui_catalog_deletion_pending(catalog);
    if (step == 3 && (pending || !mainui_catalog_scan_only(catalog->pages[1].path))) {
        step = 4;
    }
    ui->page_recovery = step;
    if (step <= 3) {
        fprintf(stderr, "Recovering an unreadable list page: %s\n",
                step == 1   ? "reloading"
                : step == 2 ? "rebuilding the cache"
                            : "scanning the folder");
        if (mainui_catalog_job_start(&ui->catalog_job, step == 2 ? JOB_REPAIR_SYSTEM : JOB_RELOAD,
                                     source, ui->sd, ui->config.case_sensitive, ui->config.rows,
                                     NULL, ++ui->catalog_generation)) {
            return false;
        }
    }
    ui->page_recovery = 0;
    /* Remembered at the top, so reopening the list starts on a readable page. */
    mainui_viewport_restore(&ui->view, ui->view.total, ui->config.rows, 0, 0, ui->config.rows - 1);
    if (mainui_browser_back(catalog, &ui->view)) {
        ui->preview_sync_once = true;
        ui->menu_view.cached_start = -1;
    }
    ui->cached_start = -1;
    snprintf(ui->message_title, sizeof ui->message_title, "Catalog unavailable");
    snprintf(ui->message_body, sizeof ui->message_body, "%s",
             search    ? "Cannot read Search results. Run Search again."
             : pending ? "This list cannot be read while a ROM deletion is unfinished. Use "
                         "Refresh roms to rebuild it."
                       : "This list cannot be read.");
    return true;
}

static bool compose_full_frame(MainUIApp *ui)
{
    ui->systems_drawn = false;
    if (ui->catalog_job.thread) {
        /* Retain the previous frame until the catalog result is ready. */
    }
    else if (ui->details.open) {
        mainui_details_progress(&ui->details, ui->snapshot ? MAINUI_PREVIEW_WAIT_FOREVER : 0);
        mainui_details_draw(&ui->details, &ui->theme, ui->screen);
    }
    else if (ui->settings_page.open) {
        mainui_settings_page_draw(&ui->settings_page, ui->screen, &ui->theme);
    }
    else if (ui->language_open) {
        mainui_draw_languages(ui->screen, &ui->theme, &ui->languages);
    }
    else if (ui->settings_open) {
        mainui_draw_settings(ui->screen, &ui->theme, &ui->settings);
    }
    else if (ui->apps) {
        mainui_draw_apps(ui->screen, &ui->theme, ui->apps, &ui->apps_view);
    }
    else if (ui->home) {
        mainui_menu_draw_home(&ui->menu_view, ui->screen, &ui->menu, &ui->home_view);
    }
    else if (ui->catalog && !ui->catalog->depth && !ui->library) {
        mainui_menu_draw_systems(&ui->menu_view, ui->screen, ui->catalog, &ui->view);
        ui->systems_drawn = true;
    }
    else {
        /* Keep rendered row labels until the visible window changes. Marquee
 * frames reuse these surfaces rather than rasterizing every 40 ms frame.
 */
        if (ui->cached_start != ui->view.start) {
            for (int i = 0; i < 20; i++) {
                if (ui->labels[i]) {
                    SDL_FreeSurface(ui->labels[i]);
                }
                ui->labels[i] = NULL;
                if (ui->highlighted[i]) {
                    SDL_FreeSurface(ui->highlighted[i]);
                }
                ui->highlighted[i] = NULL;
            }
            bool page_failed = false;
            for (int i = 0; i < ui->config.rows && ui->view.start + i < ui->view.total; i++) {
                const char *label = ui->library
                                        ? mainui_library_label(ui->library, ui->view.start + i)
                                    : ui->catalog ? NULL
                                                  : ui->list->labels[ui->view.start + i];
                if (ui->catalog && !ui->library) {
                    label = mainui_browser_label(ui->catalog, ui->view.start + i);
                    if (!label) {
                        fprintf(stderr, "%s\n", ui->catalog->error);
                        MainUILaunchSource source = {.section = ui->catalog == ui->expert
                                                                    ? MAINUI_MENU_EXPERT
                                                                    : MAINUI_MENU_GAMES,
                                                     .catalog = ui->catalog,
                                                     .view = &ui->view,
                                                     .home = &ui->home_view};
                        page_failed = true;
                        /* A job already running (a search or discovery) keeps its
                         * slot; recovery continues on a later frame. */
                        if (!ui->catalog_job.thread && recover_page(ui, &source)) {
                            return false; /* drawn as the console grid next */
                        }
                        break;
                    }
                }
                ui->favorite_rows[i] = false;
                if (ui->library && !mainui_library_is_folder(ui->library, ui->view.start + i)) {
                    ui->favorite_rows[i] = mainui_library_contains(
                        ui->favorites,
                        ui->library->items[ui->library->visible[ui->view.start + i]].rom);
                }
                else if (ui->catalog && !ui->library) {
                    cJSON *record = mainui_catalog_record(
                        ui->catalog, mainui_browser_index(ui->catalog, ui->view.start + i));
                    const cJSON *rom = cJSON_GetObjectItemCaseSensitive(record, "rompath");
                    ui->favorite_rows[i] = cJSON_IsString(rom) &&
                                           mainui_library_contains(ui->favorites, rom->valuestring);
                    cJSON_Delete(record);
                }
                char *marked = NULL;
                if (mainui_favorite_is_cut(&ui->favorite_editor, ui->library, ui->view.start + i)) {
                    size_t size = strlen(label) + 3;
                    marked = malloc(size);
                    if (marked) {
                        snprintf(marked, size, "> %s", label);
                    }
                }
                ui->labels[i] = TTF_RenderUTF8_Blended(ui->theme.font, marked ? marked : label,
                                                       ui->theme.color);
                if (ui->search.results) {
                    ui->highlighted[i] = mainui_search_label(&ui->theme, label, ui->search.query);
                }
                free(marked);
            }
            if (!page_failed) {
                ui->page_recovery = 0;
            }
            ui->cached_start = ui->view.start;
        }
        if (!ui->running) {
            return false;
        }
        if (ui->catalog_job.thread) {
            return false;
        }
        SDL_FillRect(ui->screen, NULL, SDL_MapRGB(ui->screen->format, 24, 24, 24));
        blit(ui->theme.background, ui->screen, 0, 0);
        mainui_draw_list_header_image(ui->screen, &ui->theme, ui->heading);
        /* The first frame of a list waits briefly for its cover so it does not
         * pop in, but one slow image can never stall opening the list. */
        mainui_preview_request_within(&ui->preview, ui->catalog, ui->library,
                                      ui->library || !ui->catalog
                                          ? ui->view.selected
                                          : mainui_browser_index(ui->catalog, ui->view.selected),
                                      ui->snapshot            ? MAINUI_PREVIEW_WAIT_FOREVER
                                      : ui->preview_sync_once ? 80
                                                              : 0);
        ui->preview_sync_once = false;
        Uint32 elapsed = list_elapsed(ui);
        for (int i = 0; i < ui->config.rows && ui->view.start + i < ui->view.total; i++) {
            draw_list_row(ui, i, elapsed);
        }
        /* Row clips must never leak into the next frame's header/background. */
        SDL_SetClipRect(ui->screen, NULL);
        mainui_preview_draw(&ui->preview, &ui->theme, ui->screen);
        if (!ui->view.total ||
            (ui->catalog && !ui->library && ui->catalog->depth > 1 && ui->view.total == 1) ||
            (ui->library && ui->library->current >= 0 && ui->library->visible_count == 1 &&
             ui->library->visible[0] == INT_MIN)) {
            mainui_draw_empty(ui->screen, &ui->theme);
        }
        draw_list_counter(ui->screen, &ui->theme, ui->library, ui->catalog, &ui->view);
    }
    /* Stock's popup, message and dialog windows darken the screen above the
     * footer before drawing, once however many are open. A frame kept while a
     * catalog job runs is already darkened, so it is not darkened again. */
    if (!ui->catalog_job.thread && ((ui->context_open && ui->context.visible_count) ||
                                    ui->confirmation >= 0 || *ui->message_title)) {
        mainui_dim_popup_background(ui->screen, &ui->theme);
    }
    if (ui->context_open) {
        mainui_draw_context(ui->screen, &ui->theme, &ui->context);
    }
    if (ui->confirmation >= 0) {
        mainui_draw_confirmation(ui->screen, &ui->theme,
                                 ui->confirmation == CONTEXT_CLEAR_RECENT ? 77
                                 : ui->confirmation == CONTEXT_DELETE_ROM ? 116
                                                                          : 86,
                                 ui->confirmation == CONTEXT_CLEAR_RECENT ? 78
                                 : ui->confirmation == CONTEXT_DELETE_ROM ? 117
                                                                          : 108);
    }
    if (ui->name_input.open) {
        mainui_name_input_draw(&ui->name_input, ui->screen, &ui->theme);
    }
    if (*ui->message_title) {
        /* Log each message once as it appears: many are only shown. */
        char shown[sizeof ui->logged_message];
        snprintf(shown, sizeof shown, "%s: %s", ui->message_title, ui->message_body);
        if (strcmp(shown, ui->logged_message)) {
            fprintf(stderr, "[message] %s\n", shown);
            strcpy(ui->logged_message, shown);
        }
        mainui_draw_message(ui->screen, &ui->theme, ui->message_title, ui->message_body);
    }
    else {
        ui->logged_message[0] = 0;
    }
    if (ui->catalog_job.thread && !atomic_load(&ui->catalog_job.done) &&
        (SDL_GetTicks() - ui->catalog_job.started_at >= 500 ||
         atomic_load(&ui->catalog_job.cancel))) {
        mainui_draw_catalog_loading(ui->screen, &ui->theme, atomic_load(&ui->catalog_job.cancel));
    }
    if (ui->snapshot && !ui->catalog_job.thread && !ui->device_job.thread &&
        !(ui->about_visible && ui->about_job.thread) &&
        (ui->launch_pending ||
         (!ui->letter_jump.active && (!ui->input_script || !*ui->input_script)))) {
        if (SDL_SaveBMP(ui->screen, ui->snapshot) < 0) {
            fprintf(stderr, "Snapshot: %s\n", SDL_GetError());
            ui->status = 4;
        }
        ui->running = false;
        return false;
    }
    return true;
}

static bool draw_full_frame(MainUIApp *ui)
{
    if (!compose_full_frame(ui)) {
        return false;
    }
    ui->presented_at = SDL_GetTicks();
    ui->presented_battery = ui->theme.battery_percent;
    ui->presented_wifi_online = ui->theme.wifi_online;
    ui->presented_wifi_signal = ui->theme.wifi_signal_level;
    snprintf(ui->presented_wifi_address, sizeof ui->presented_wifi_address, "%s",
             ui->theme.wifi_address);
    present(ui, NULL);
    return true;
}

/* Recompose only the selected row, bottom-up, as a full frame layers it: fill,
 * background, row, cover. The header (y < 60) and footer (y >= 420) keep their
 * own clips, so nothing else is drawn in the list band on these screens. */
static void draw_marquee_row(MainUIApp *ui)
{
    int row = ui->view.selected - ui->view.start;
    SDL_Rect area = {0, (Sint16)(60 + row * ui->config.row_height), 640,
                     (Uint16)ui->config.row_height};
    SDL_SetClipRect(ui->screen, &area);
    SDL_FillRect(ui->screen, &area, SDL_MapRGB(ui->screen->format, 24, 24, 24));
    blit(ui->theme.background, ui->screen, 0, 0);
    Uint32 elapsed = list_elapsed(ui);
    draw_list_row(ui, row, elapsed);
    SDL_SetClipRect(ui->screen, &area);
    mainui_preview_draw(&ui->preview, &ui->theme, ui->screen);
    SDL_SetClipRect(ui->screen, NULL);
#ifndef MAINUI_ONION
    /* Host check: a partial frame must equal a full frame, pixel for pixel. */
    if (getenv("MAINUI_VERIFY_PARTIAL")) {
        size_t size = (size_t)ui->screen->pitch * (size_t)ui->screen->h;
        void *partial = malloc(size);
        bool animate = ui->animate;
        if (partial) {
            memcpy(partial, ui->screen->pixels, size);
            ui->animate = ui->letter_jump.active;
            pinned_elapsed = &elapsed;
            bool composed = compose_full_frame(ui);
            pinned_elapsed = NULL;
            if (composed && memcmp(partial, ui->screen->pixels, size)) {
                fprintf(stderr, "MAINUI_VERIFY_PARTIAL: partial frame differs from full frame\n");
                abort();
            }
            mainui_count_add("verified-partial", 1);
            free(partial);
        }
        ui->animate = animate;
    }
#endif
    present(ui, &area);
}

bool mainui_draw_frame(MainUIApp *ui)
{
    if (mainui_frame_marquee_only(ui, SDL_GetTicks())) {
        draw_marquee_row(ui);
        return true;
    }
    return draw_full_frame(ui);
}
