/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/session.h"
#include "app/positions.h"
#include "catalog/saved_actions.h"
#include "platform/launch.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *string(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static bool integer(const cJSON *object, const char *key, int *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < INT_MIN ||
        value->valuedouble > INT_MAX || value->valuedouble != value->valueint) {
        return false;
    }
    *out = value->valueint;
    return true;
}

static cJSON *view_json(const MainUIViewport *view)
{
    cJSON *object = cJSON_CreateObject();
    if (!object || !cJSON_AddNumberToObject(object, "currpos", view->selected) ||
        !cJSON_AddNumberToObject(object, "pagestart", view->start) ||
        !cJSON_AddNumberToObject(object, "pageend", view->end)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static bool read_view(const cJSON *object, MainUIViewport *view)
{
    return integer(object, "currpos", &view->selected) &&
           integer(object, "pagestart", &view->start) && integer(object, "pageend", &view->end);
}

static bool attach(cJSON *parent, const char *key, cJSON *value)
{
    if (!value) {
        return false;
    }
    bool ok = key ? cJSON_AddItemToObject(parent, key, value) : cJSON_AddItemToArray(parent, value);
    if (!ok) {
        cJSON_Delete(value);
    }
    return ok;
}

static const char *portable(char out[4096], const char *sd, const char *path)
{
    size_t length = strlen(sd);
    if (!strncmp(path, sd, length) && path[length] == '/') {
        int size = snprintf(out, 4096, "/mnt/SDCARD%s", path + length);
        return size > 0 && size < 4096 ? out : "";
    }
    return path;
}

cJSON *mainui_session_snapshot(MainUIMenuSection section, MainUICatalog *catalog,
                               const MainUILibrary *library, const MainUIViewport *view,
                               const MainUIViewport *home, MainUIStack *legacy)
{
    static const int titles[] = {18, 1, 2, 0, 107, 15}, types[] = {10, 2, 1, 16, 3, 7};
    if (section < 0 || section >= MAINUI_MENU_SECTIONS) {
        return NULL;
    }
    const MainUIViewport *section_view =
        !library && catalog && catalog->depth ? &catalog->pages[0].view : view;
    *legacy = (MainUIStack){.count = 2,
                            .frames = {{157, 0, home->selected, home->start, home->end},
                                       {titles[section], types[section], section_view->selected,
                                        section_view->start, section_view->end}}};
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "section", section) ||
        !attach(root, "home", view_json(home)) || !attach(root, "view", view_json(view))) {
        cJSON_Delete(root);
        return NULL;
    }
    bool ok = true;
    if (library) {
        const char *id = library->current >= 0 ? library->folders[library->current].id : "";
        ok = cJSON_AddStringToObject(root, "folder_id", id) != NULL;
        cJSON *views = cJSON_CreateArray();
        if (!attach(root, "folder_views", views)) {
            ok = false;
        }
        int chain[MAINUI_FOLDER_LIMIT], count = 0, current = library->current;
        while (current >= 0 && count < MAINUI_FOLDER_LIMIT) {
            chain[count++] = current;
            current = library->folders[current].parent;
        }
        char path[256] = "";
        bool path_fits = true;
        for (int i = count - 1; path_fits && i >= 0; i--) {
            const MainUILibraryFolder *folder = &library->folders[chain[i]];
            size_t used = strlen(path);
            int size = snprintf(path + used, sizeof path - used, "/%s", folder->name);
            path_fits = size >= 0 && (size_t)size < sizeof path - used;
        }
        if (ok && path_fits && *path) {
            ok = cJSON_AddStringToObject(root, "favorite_path", path) != NULL;
        }
        for (int i = -1; ok && i < library->folder_count; i++) {
            const MainUIViewport *saved = i == library->current ? view : &library->views[i + 1];
            cJSON *item = view_json(saved);
            ok = item && cJSON_AddStringToObject(item, "id", i < 0 ? "" : library->folders[i].id);
            if (ok) {
                ok = attach(views, NULL, item);
            }
            else {
                cJSON_Delete(item);
            }
        }
    }
    else if (catalog) {
        cJSON *pages = cJSON_CreateArray();
        ok = attach(root, "pages", pages);
        for (int i = 0; ok && i <= catalog->depth; i++) {
            const MainUICatalogPage *page = &catalog->pages[i];
            cJSON *item = view_json(i == catalog->depth ? view : &page->view);
            char path[4096];
            ok = item &&
                 cJSON_AddStringToObject(item, "path", portable(path, catalog->sd, page->path));
            if (ok) {
                ok = attach(pages, NULL, item);
            }
            else {
                cJSON_Delete(item);
            }
        }
        if (ok) {
            MainUIEntry *selected =
                mainui_catalog_entry(catalog, mainui_browser_index(catalog, view->selected));
            if (selected) {
                char path[4096];
                ok = cJSON_AddStringToObject(root, "selected_path",
                                             portable(path, catalog->sd, selected->path)) != NULL;
                if (!catalog->depth && selected->launch) {
                    ok = ok &&
                         cJSON_AddStringToObject(root, "selected_launch",
                                                 portable(path, catalog->sd, selected->launch));
                }
            }
        }
        if (ok && catalog->depth) {
            int selected = catalog->pages[0].view.selected;
            char path[4096];
            ok = selected >= 0 && selected < catalog->pages[0].count &&
                 cJSON_AddStringToObject(
                     root, "system",
                     portable(path, catalog->sd, catalog->pages[0].entries[selected].launch));
        }
    }
    if (!ok) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

void mainui_session_close(MainUISession *session)
{
    if (session->catalog) {
        mainui_catalog_close(session->catalog);
    }
    if (session->library) {
        mainui_library_close(session->library);
    }
    free(session->catalog);
    free(session->library);
    *session = (MainUISession){0};
}

static void restore_view(MainUIViewport *view, int count, int rows, const MainUIViewport *saved)
{
    mainui_viewport_restore(view, count, rows, saved->selected, saved->start, saved->end);
}

bool mainui_session_restore_control(MainUISession *out, const char *sd, bool sensitive, int rows,
                                    const cJSON *resume, const cJSON *record, MainUICancel cancel)
{
    MainUISession pending = {0};
    int section;
    if (!integer(resume, "section", &section) || section < 0 || section >= MAINUI_MENU_SECTIONS ||
        !read_view(cJSON_GetObjectItemCaseSensitive(resume, "home"), &pending.home) ||
        !read_view(cJSON_GetObjectItemCaseSensitive(resume, "view"), &pending.view)) {
        return false;
    }
    pending.section = (MainUIMenuSection)section;
    pending.home_only = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resume, "home_only"));
    if (pending.home_only || section == MAINUI_MENU_SETTINGS) {
        *out = pending;
        return true;
    }
    MainUIViewport selected_view = pending.view;
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(resume, "search_query"))) {
        record = cJSON_GetObjectItemCaseSensitive(resume, "source_record");
    }
    bool ok = true;
    if (section == MAINUI_MENU_FAVORITES || section == MAINUI_MENU_RECENTS) {
        pending.library = calloc(1, sizeof *pending.library);
        ok = pending.library && mainui_library_open_control(pending.library, sd,
                                                            section == MAINUI_MENU_RECENTS, cancel);
        const cJSON *views = cJSON_GetObjectItemCaseSensitive(resume, "folder_views");
        if (!cJSON_IsArray(views) || cJSON_GetArraySize(views) > MAINUI_FOLDER_LIMIT + 1) {
            ok = false;
        }
        const cJSON *item;
        cJSON_ArrayForEach(item, views)
        {
            MainUIViewport saved = {0};
            if (!ok || !read_view(item, &saved)) {
                ok = false;
                break;
            }
            const char *id = string(item, "id");
            if (!*id) {
                pending.library->views[0] = saved;
            }
            for (int i = 0; i < pending.library->folder_count; i++) {
                if (!strcmp(id, pending.library->folders[i].id)) {
                    pending.library->views[i + 1] = saved;
                }
            }
        }
        const char *id = string(resume, "folder_id");
        if (ok && *id) {
            int chain[4], count = 0, folder = -1;
            for (int i = 0; i < pending.library->folder_count; i++) {
                if (!strcmp(id, pending.library->folders[i].id)) {
                    folder = i;
                    break;
                }
            }
            while (folder >= 0 && count < 4) {
                chain[count++] = folder;
                folder = pending.library->folders[folder].parent;
            }
            for (int i = count - 1; i >= 0; i--) {
                for (int row = 0; row < pending.library->visible_count; row++) {
                    if (pending.library->visible[row] == -chain[i] - 1) {
                        mainui_library_enter(pending.library, row);
                        break;
                    }
                }
            }
            /* The saved rows of the folders above may name other folders
             * now: the folders may have been reordered meanwhile. */
            mainui_library_select_path(pending.library);
        }
        if (ok) {
            restore_view(&pending.view, pending.library->visible_count, rows, &selected_view);
            /* Find the selected entry by the list's own identity, so two
             * Recents of one ROM or two Favorites without a ROM stay apart.
             * Without a record (a folder was selected) the position stands. */
            int row = mainui_library_find(pending.library, record);
            if (row >= 0) {
                mainui_viewport_move(&pending.view, rows, row - pending.view.selected, false);
            }
        }
    }
    else if (section == MAINUI_MENU_GAMES || section == MAINUI_MENU_EXPERT ||
             section == MAINUI_MENU_APPS) {
        pending.catalog = calloc(1, sizeof *pending.catalog);
        if (pending.catalog) {
            pending.catalog->cancel = cancel;
        }
        ok = pending.catalog &&
             (section == MAINUI_MENU_EXPERT ? mainui_catalog_expert(pending.catalog, sd, sensitive)
              : section == MAINUI_MENU_APPS ? mainui_catalog_apps(pending.catalog, sd, sensitive)
                                            : mainui_catalog_open(pending.catalog, sd, sensitive));
        const cJSON *pages = cJSON_GetObjectItemCaseSensitive(resume, "pages");
        int count = cJSON_GetArraySize(pages);
        if (!cJSON_IsArray(pages) || count < 1 || count > MAINUI_STACK_MAX) {
            ok = false;
        }
        MainUIViewport saved[MAINUI_STACK_MAX];
        for (int i = 0; ok && i < count; i++) {
            const cJSON *page = cJSON_GetArrayItem(pages, i);
            ok = read_view(page, &saved[i]) && *string(page, "path") &&
                 strlen(string(page, "path")) < 4096;
        }
        if (ok) {
            restore_view(&pending.view, pending.catalog->pages[0].count,
                         section == MAINUI_MENU_APPS     ? 4
                         : section == MAINUI_MENU_EXPERT ? 9
                                                         : 8,
                         &saved[0]);
        }
        for (int level = 1; ok && level < count; level++) {
            if (mainui_cancelled(cancel)) {
                ok = false;
                break;
            }
            const char *target = level == 1 ? string(resume, "system")
                                            : string(cJSON_GetArrayItem(pages, level), "path");
            char resolved[4096];
            if (!*target || !mainui_catalog_path(resolved, sd, sd, target)) {
                ok = false;
                break;
            }
            int found = -1;
            for (int row = 0; row < mainui_browser_count(pending.catalog); row++) {
                MainUIEntry *entry = mainui_catalog_entry(
                    pending.catalog, mainui_browser_index(pending.catalog, row));
                const char *value = entry ? (level == 1 ? entry->launch : entry->path) : NULL;
                if (value && !strcmp(value, resolved)) {
                    found = row;
                    break;
                }
            }
            if (found < 0) {
                break;
            }
            mainui_viewport_move(&pending.view,
                                 level == 1 ? (section == MAINUI_MENU_EXPERT ? 9 : 8) : rows,
                                 found - pending.view.selected, false);
            if (level == 1 && section != MAINUI_MENU_APPS) {
                mainui_browser_grid_restore(pending.catalog, &pending.view, pending.view.selected);
            }
            if (!mainui_browser_enter(pending.catalog, &pending.view, rows)) {
                ok = false;
                break;
            }
            restore_view(&pending.view, mainui_browser_count(pending.catalog), rows, &saved[level]);
        }
        if (ok && pending.catalog->depth == count - 1) {
            int visible_rows = section == MAINUI_MENU_APPS ? 4 : rows;
            restore_view(&pending.view, mainui_browser_count(pending.catalog), visible_rows,
                         &selected_view);
            char resolved[4096];
            const char *identity = *string(record, "rompath") ? string(record, "rompath")
                                   : !pending.catalog->depth && *string(resume, "selected_launch")
                                       ? string(resume, "selected_launch")
                                       : string(resume, "selected_path");
            bool by_launch = !*string(record, "rompath") && !pending.catalog->depth &&
                             *string(resume, "selected_launch");
            if (*identity && mainui_catalog_path(resolved, sd, sd, identity)) {
                for (int row = 0; row < mainui_browser_count(pending.catalog); row++) {
                    MainUIEntry *entry = mainui_catalog_entry(
                        pending.catalog, mainui_browser_index(pending.catalog, row));
                    if (entry && (by_launch ? entry->launch : entry->path) &&
                        !strcmp(by_launch ? entry->launch : entry->path, resolved)) {
                        mainui_viewport_move(&pending.view, visible_rows,
                                             row - pending.view.selected, false);
                        break;
                    }
                }
            }
        }
    }
    else {
        ok = false;
    }
    if (ok && pending.catalog && !pending.catalog->depth && section != MAINUI_MENU_APPS) {
        mainui_browser_grid_restore(pending.catalog, &pending.view, pending.view.selected);
    }
    if (!ok || mainui_cancelled(cancel)) {
        mainui_session_close(&pending);
        return false;
    }
    *out = pending;
    return true;
}

bool mainui_session_launch(const char *directory, const MainUILaunchSource *source,
                           const cJSON *record, char error[256])
{
    cJSON *owned = NULL;
    if (!record) {
        int row = source->view->selected;
        if (source->search && source->search->results) {
            row = source->search->view.selected;
            if (row >= 0 && row < source->search->results->count) {
                owned = cJSON_Duplicate(source->search->results->items[row].json, true);
            }
        }
        else if (source->library && row >= 0 && row < source->library->visible_count &&
                 !mainui_library_is_folder(source->library, row)) {
            owned =
                cJSON_Duplicate(source->library->items[source->library->visible[row]].json, true);
        }
        else if (!source->library && source->catalog) {
            owned =
                mainui_catalog_record(source->catalog, mainui_browser_index(source->catalog, row));
        }
        record = owned;
    }
    MainUIStack legacy = {0};
    cJSON *resume = mainui_session_snapshot(source->section, source->catalog, source->library,
                                            source->view, source->home, &legacy);
    bool ok = record && resume;
    if (ok && source->home_only) {
        ok = cJSON_AddBoolToObject(resume, "home_only", true) != NULL;
    }
    if (ok && source->search) {
        cJSON *original = mainui_catalog_record(
            source->catalog, mainui_browser_index(source->catalog, source->view->selected));
        ok = cJSON_AddStringToObject(resume, "search_query", source->search->query) &&
             attach(resume, "search_view", view_json(&source->search->view));
        if (original) {
            ok = attach(resume, "source_record", original) && ok;
        }
    }
    if (ok && source->catalog && !source->library && !source->home_only) {
        /* Best effort, like Recent below: a full or read-only card must not
         * block the launch, whose handoff files live in /tmp. */
        if (!mainui_positions_save(source->catalog, source->view)) {
            fprintf(stderr, "Could not save ROM-list position; launching anyway.\n");
        }
    }
    ok = ok && mainui_launch_publish(directory, record, &legacy, resume, error);
    if (ok) {
        const char *sd = source->sd ? source->sd : source->catalog ? source->catalog->sd : NULL;
        if (!mainui_recent_add(sd, record)) {
            fprintf(stderr, "Cannot save Recent game; launch handoff remains committed.\n");
        }
    }
    if (!ok && !*error) {
        snprintf(error, 256, "Cannot prepare a launch from this row.");
    }
    cJSON_Delete(owned);
    cJSON_Delete(resume);
    return ok;
}

bool mainui_session_restore(MainUISession *out, const char *sd, bool sensitive, int rows,
                            const cJSON *resume, const cJSON *record)
{
    return mainui_session_restore_control(out, sd, sensitive, rows, resume, record,
                                          (MainUICancel){0});
}
