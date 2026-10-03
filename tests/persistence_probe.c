/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/browser.h"
#include "app/loop.h"
#include "catalog/catalog.h"
#include "catalog/delete.h"
#include "catalog/favorite_edit.h"
#include "catalog/saved_actions.h"
#include "platform/files.h"
#include "platform/launch.h"
#include "platform/system_config.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void pause_writer(void)
{
    usleep(1000);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        return 2;
    }
    const char *mode = argv[1], *root = argv[2], *key = argc > 3 ? argv[3] : "value";
    char path[4096];
    if (!strcmp(mode, "system-once")) {
        cJSON *value = cJSON_CreateNumber(1);
        bool ok = value && mainui_system_write(root, key, value);
        cJSON_Delete(value);
        return ok ? 0 : 3;
    }
    if (!strcmp(mode, "catalog-open") || !strcmp(mode, "apps-open")) {
        MainUICatalog *catalog = calloc(1, sizeof *catalog);
        bool ok =
            catalog && (!strcmp(mode, "apps-open") ? mainui_catalog_apps(catalog, root, false)
                                                   : mainui_catalog_open(catalog, root, false));
        if (catalog) {
            if (!ok) {
                fprintf(stderr, "%s\n", catalog->error);
            }
            printf("%d\n", catalog->pages[0].count);
            mainui_catalog_close(catalog);
        }
        free(catalog);
        return ok ? 0 : 3;
    }
    if (!strcmp(mode, "ticks")) {
        /* Three maintenance wakes with nothing else happening. */
        MainUIApp *ui = calloc(1, sizeof *ui);
        if (!ui || SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
            free(ui);
            return 4;
        }
        ui->running = ui->device_enabled = true;
        ui->confirmation = -1;
        int ticks = 0;
        Uint32 started = SDL_GetTicks();
        for (int i = 0; i < 3; ++i) {
            SDL_Event event;
            if (!mainui_wait_event(ui, &event)) {
                break;
            }
            ticks += event.type == SDL_USEREVENT && event.user.code == MAINUI_TICK_CODE;
        }
        printf("ticks=%d elapsed=%u timer=%d\n", ticks, (unsigned)(SDL_GetTicks() - started),
               ui->timer != NULL);
        if (ui->timer) {
            SDL_RemoveTimer(ui->timer);
        }
        SDL_Quit();
        free(ui);
        return ticks == 3 ? 0 : 3;
    }
    if (!strcmp(mode, "hold")) {
        snprintf(path, sizeof path, "%s/system.json", root);
        MainUIFileLock *lock = mainui_file_lock(path);
        if (!lock) {
            return 3;
        }
        snprintf(path, sizeof path, "%s/holder-ready", root);
        if (!mainui_write_text_atomic(path, "ready")) {
            return 4;
        }
        for (;;) {
            pause_writer();
        }
    }
    if (!strcmp(mode, "locked-write")) {
        snprintf(path, sizeof path, "%s/locked.bin", root);
        MainUIFileLock *lock = mainui_file_lock(path);
        bool ok = lock && mainui_write_bytes_new_locked(path, key, strlen(key));
        mainui_file_unlock(lock);
        return ok ? 0 : 3;
    }
    if (!strcmp(mode, "new")) {
        snprintf(path, sizeof path, "%s/new.txt", root);
        return mainui_write_bytes_new(path, key, strlen(key)) ? 0 : 3;
    }
    if (!strcmp(mode, "atomic")) {
        snprintf(path, sizeof path, "%s/atomic.json", root);
        return mainui_write_text_atomic(path, key) ? 0 : 3;
    }
    if (!strcmp(mode, "take")) {
        cJSON *returned = mainui_launch_take_return(root);
        puts(returned ? "returned" : "empty");
        cJSON_Delete(returned);
        return 0;
    }
    if (!strcmp(mode, "recover-only")) {
        char cache[4096], roms[4096];
        snprintf(cache, sizeof cache, "%s/Roms/Test/Test_cache6.db", root);
        snprintf(roms, sizeof roms, "%s/Roms/Test", root);
        MainUIFileLock *lock = mainui_file_lock(cache);
        bool ok = lock && mainui_delete_recover(cache, "Test_roms", roms, root);
        mainui_file_unlock(lock);
        return ok ? 0 : 3;
    }
    if (!strcmp(mode, "recover-busy")) {
        snprintf(path, sizeof path, "%s/Roms/Test/Test_cache6.db", root);
        MainUIFileLock *lock = mainui_file_lock(path);
        MainUICatalog *catalog = calloc(1, sizeof *catalog);
        bool ok = lock && catalog && mainui_catalog_open(catalog, root, false) &&
                  mainui_catalog_enter(catalog, 0);
        snprintf(path, sizeof path, "%s/Roms/Test/Test_cache6.db.delete.json", root);
        ok = ok && mainui_file_stamp(path).exists;
        mainui_file_unlock(lock);
        if (ok) {
            ok = mainui_catalog_back(catalog) && mainui_catalog_enter(catalog, 0) &&
                 !mainui_file_stamp(path).exists;
        }
        if (catalog) {
            mainui_catalog_close(catalog);
        }
        free(catalog);
        return ok ? 0 : 3;
    }
    if (!strcmp(mode, "delete") || !strcmp(mode, "delete-stale") || !strcmp(mode, "recover")) {
        MainUICatalog *catalog = calloc(1, sizeof *catalog);
        MainUIViewport view = {0};
        bool ok = catalog && mainui_catalog_open(catalog, root, false) &&
                  mainui_catalog_enter(catalog, 0);
        if (ok && !strcmp(mode, "delete-stale")) {
            /* Another writer changes the cache after the list was read. */
            sqlite3 *database = NULL;
            snprintf(path, sizeof path, "%s/Roms/Test/Test_cache6.db", root);
            bool changed = sqlite3_open(path, &database) == SQLITE_OK &&
                           sqlite3_exec(database, key, NULL, NULL, NULL) == SQLITE_OK &&
                           sqlite3_changes(database) > 0;
            sqlite3_close(database);
            if (!changed) {
                mainui_catalog_close(catalog);
                free(catalog);
                return 4;
            }
        }
        if (ok && strcmp(mode, "recover")) {
            mainui_viewport_restore(&view, mainui_browser_count(catalog), 6, 0, 0, 5);
            ok = mainui_browser_delete(catalog, &view, 6);
            if (!ok) {
                puts(catalog->error);
            }
        }
        if (catalog) {
            mainui_catalog_close(catalog);
        }
        free(catalog);
        return ok ? 0 : 3;
    }
    if (!strcmp(mode, "cache") || !strcmp(mode, "remove-cache")) {
        MainUICatalog *catalog = calloc(1, sizeof *catalog);
        bool ok = catalog && mainui_catalog_open(catalog, root, false) &&
                  (!strcmp(mode, "remove-cache") ? mainui_catalog_remove_cache(catalog, 0)
                                                 : mainui_catalog_build_cache(catalog, 0, true));
        if (!ok && catalog) {
            fprintf(stderr, "%s\n", catalog->error);
        }
        if (catalog) {
            mainui_catalog_close(catalog);
        }
        free(catalog);
        return ok ? 0 : 3;
    }
    cJSON *record = cJSON_CreateObject();
    if (!record) {
        return 3;
    }
    cJSON_AddStringToObject(record, "label", key);
    snprintf(path, sizeof path, "/mnt/SDCARD/Roms/Test/%s.nes", key);
    cJSON_AddStringToObject(record, "rompath", path);
    cJSON_AddStringToObject(record, "launch", "/mnt/SDCARD/Emu/Test/launch.sh");
    cJSON_AddNumberToObject(record, "type", 5);
    if (!strcmp(mode, "favorite-remove-once")) {
        bool removed = mainui_saved_action(root, false, SAVED_REMOVE, record);
        cJSON_Delete(record);
        return removed ? 0 : 3;
    }
    if (!strcmp(mode, "launch")) {
        MainUIStack state = {0};
        char error[256] = "";
        cJSON *resume = cJSON_CreateObject();
        bool launched = resume && cJSON_AddStringToObject(resume, "favorite_path", "/Games") &&
                        cJSON_AddStringToObject(resume, "search_query", "game") &&
                        mainui_launch_publish(root, record, &state, resume, error);
        if (!launched) {
            fprintf(stderr, "%s\n", error);
        }
        cJSON_Delete(resume);
        cJSON_Delete(record);
        return launched ? 0 : 3;
    }
    bool ok = false;
    for (int attempt = 0; attempt < 10000 && !ok; attempt++) {
        if (!strcmp(mode, "system")) {
            cJSON *value = cJSON_CreateNumber(1);
            ok = value && mainui_system_write(root, key, value);
            cJSON_Delete(value);
        }
        else if (!strcmp(mode, "favorite")) {
            ok = mainui_saved_action(root, false, SAVED_ADD, record);
        }
        else if (!strcmp(mode, "folder")) {
            MainUILibrary *library = calloc(1, sizeof *library);
            MainUIFavoriteEditor editor = {0};
            int selected = 0;
            ok = library && mainui_library_open(library, root, false) &&
                 mainui_favorite_edit(&editor, library, root, CONTEXT_FAVORITE_CREATE, key,
                                      &selected);
            mainui_favorite_editor_close(&editor);
            if (library) {
                mainui_library_close(library);
            }
            free(library);
        }
        else {
            break;
        }
        if (!ok) {
            pause_writer();
        }
    }
    cJSON_Delete(record);
    return ok ? 0 : 3;
}
