/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/browser.h"
#include "app/search.h"
#include "catalog/gamelist.h"
#include "catalog/library.h"
#include "catalog/pinyin.h"
#include "menus/menu.h"
#include "platform/system_config.h"
#include "support.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int find(MainUICatalog *catalog, const char *label)
{
    for (int i = 0; i < mainui_browser_count(catalog); i++) {
        const char *value = mainui_browser_label(catalog, i);
        if (value && !strcmp(value, label)) {
            return i;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *sd = argv[1];
    MainUICatalog *catalog = calloc(1, sizeof *catalog);
    assert(catalog && mainui_catalog_open(catalog, sd, false));
    MainUIViewport view;
    mainui_grid_restore(&view, mainui_browser_count(catalog), 0, 4, 2);
    assert(mainui_browser_enter(catalog, &view, 6));
    assert(catalog->depth == 1 && find(catalog, "..") == -1);
    MainUISearch search = {0};
    MainUIViewport original = view;
    assert(mainui_search_open(&search, catalog, &view, "nested", 6));
    assert(search.results->count == 70 && search.view.selected == 0 && search.view.start == 0);
    assert(!strcmp(search.results->items[0].label, "nested00"));
    assert(!strcmp(search.results->items[69].rom, "/mnt/SDCARD/Roms/Normal/Folder/nested69.nes"));
    assert(!memcmp(&view, &original, sizeof view) && catalog->depth == 1);
    cJSON *search_view = cJSON_Parse("{\"currpos\":44,\"pagestart\":42,\"pageend\":47}");
    assert(mainui_search_restore_view(&search, search_view, search.results->items[44].json, 6));
    assert(search.view.selected == 44 && search.view.start == 42 && search.view.end == 47);
    cJSON_Delete(search_view);
    search_view = cJSON_Parse("{\"currpos\":1.5,\"pagestart\":0,\"pageend\":5}");
    assert(!mainui_search_restore_view(&search, search_view, NULL, 6));
    assert(search.view.selected == 44);
    cJSON_Delete(search_view);
    assert(mainui_search_open(&search, catalog, &view, "Alpha", 6));
    assert(search.results->count == 1 && !strcmp(search.results->items[0].label, "Alpha final"));
    assert(mainui_search_open(&search, catalog, &view, "' OR 1=1 --", 6));
    assert(search.results->count == 0 && search.view.selected == -1);
    /* Cached pinyin/cpinyin may differ on existing SD cards. */
    sqlite3 *db = NULL;
    assert(sqlite3_open(catalog->pages[1].cache_file, &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
                        "UPDATE Normal_roms SET pinyin='latin-only', cpinyin='chinese-only' "
                        "WHERE path='/mnt/SDCARD/Emu/Normal/../../Roms/Normal/alpha.nes'",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_changes(db) == 1);
    const char *locales[] = {"en.lang", "ch.lang", "cht.lang", "CH.lang", "custom.lang"};
    for (int shortname = 0; shortname < 2; shortname++) {
        catalog->pages[0].entries[0].shortname = shortname != 0;
        for (size_t i = 0; i < sizeof locales / sizeof *locales; i++) {
            cJSON *language = cJSON_CreateString(locales[i]);
            assert(mainui_system_write(sd, "language", language));
            cJSON_Delete(language);
            bool chinese = shortname && i == 1;
            assert(mainui_search_open(&search, catalog, &view, "chinese-only", 6));
            assert(search.results->count == (chinese ? 1 : 0));
            assert(mainui_search_open(&search, catalog, &view, "latin-only", 6));
            assert(search.results->count == (chinese ? 0 : 1));
        }
    }
    cJSON *language = cJSON_CreateString("en.lang");
    assert(mainui_system_write(sd, "language", language));
    cJSON_Delete(language);
    assert(sqlite3_exec(db,
                        "UPDATE Normal_roms SET pinyin=disp,cpinyin=disp "
                        "WHERE path='/mnt/SDCARD/Emu/Normal/../../Roms/Normal/alpha.nes'",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_changes(db) == 1);
    sqlite3_close(db);
    mainui_search_close(&search);
    view.selected = find(catalog, "Folder");
    assert(view.selected >= 0);
    MainUIViewport saved = view;
    assert(mainui_browser_enter(catalog, &view, 6));
    assert(view.total == 71 && catalog->pages[2].count == 70);
    assert(!strcmp(mainui_browser_label(catalog, 0), ".."));
    assert(mainui_browser_folder(catalog, 0));
    assert(mainui_browser_index(catalog, 0) == -1);
    assert(!mainui_catalog_record(catalog, mainui_browser_index(catalog, 0)));
    assert(!strcmp(mainui_browser_label(catalog, 65), "nested64"));
    assert(catalog->pages[2].loaded <= 64);
    assert(mainui_browser_enter(catalog, &view, 6));
    assert(!memcmp(&saved, &view, sizeof view));
    assert(mainui_browser_enter(catalog, &view, 6));
    assert(mainui_browser_back(catalog, &view));
    assert(!memcmp(&saved, &view, sizeof view));
    view.selected = find(catalog, "Disc");
    assert(mainui_browser_enter(catalog, &view, 6));
    assert(view.total == 2 && !strcmp(mainui_browser_label(catalog, 0), "..") &&
           !strcmp(mainui_browser_label(catalog, 1), "Sub"));
    assert(mainui_browser_enter(catalog, &view, 6));
    mainui_catalog_close(catalog);
    memset(catalog, 0, sizeof *catalog);
    assert(mainui_catalog_expert(catalog, sd, false));
    assert(catalog->pages[0].count == 10);
    assert(!strcmp(catalog->pages[0].title, "Expert"));
    mainui_grid_restore(&view, 10, 9, 4, 2);
    assert(mainui_browser_enter(catalog, &view, 6));
    assert(view.total == 1);
    cJSON *record = mainui_catalog_record(catalog, 0);
    const cJSON *launch = cJSON_GetObjectItemCaseSensitive(record, "launch");
    assert(cJSON_IsString(launch) && strstr(launch->valuestring, "/RApp/09/launch.sh"));
    cJSON_Delete(record);
    assert(mainui_browser_back(catalog, &view) && view.selected == 9);
    mainui_catalog_close(catalog);
    free(catalog);
    MainUINameLookup names = {0};
    assert(!strcmp(mainui_name_lookup(&names, sd, "alpha", "miss"), "Alpha final"));
    assert(!strcmp(mainui_name_lookup(&names, sd, "ALPHA", "miss"), "Upper"));
    assert(!strcmp(mainui_name_lookup(&names, sd, "afterblank", "miss"), "After blank"));
    assert(!strcmp(mainui_name_lookup(&names, sd, "absent", "miss"), "miss"));
    assert(!strcmp(mainui_name_lookup(&names, sd, "empty", "miss"), ""));
    mainui_names_close(&names);
    char map_path[4096];
    snprintf(map_path, sizeof map_path, "%s/BIOS/arcade_lists/arcade-rom-names.txt", sd);
    char *saved_map = mainui_read_text(map_path, 1024 * 1024);
    assert(saved_map && mainui_write_text_atomic(map_path, "alpha \"Changed\"\n"));
    /* Catalog close/reopen and even separate handles retain the process snapshot. */
    MainUINameLookup reopened = {0};
    assert(!strcmp(mainui_name_lookup(&reopened, sd, "alpha", "miss"), "Alpha final"));
    mainui_names_close(&reopened);
    assert(mainui_write_text_atomic(map_path, saved_map));
    free(saved_map);
    char other_sd[4096];
    snprintf(other_sd, sizeof other_sd, "%s/MapMissing", sd);
    assert(!strcmp(mainui_name_lookup(&reopened, other_sd, "alpha", "filename"), "filename"));
    TEST_PATH(map_path, "%s/BIOS/arcade_lists/arcade-rom-names.txt", other_sd);
    assert(mainui_write_text_atomic(map_path, "alpha \"Appeared\"\n"));
    mainui_names_close(&reopened);
    assert(!strcmp(mainui_name_lookup(&reopened, other_sd, "alpha", "filename"), "filename"));
    snprintf(other_sd, sizeof other_sd, "%s/MapLarge", sd);
    assert(!strcmp(mainui_name_lookup(&reopened, other_sd, "tail", "miss"), "Large map tail"));
    assert(!strcmp(mainui_name_lookup(&reopened, other_sd, "leading", "miss"), "Whitespace"));
    mainui_names_close(&reopened);
    char converted[128];
    const char *inputs[] = {"AbC 97!?", "超级马里奥", "重行乐", "拳皇97", "𩜇", "éあ😀", ""};
    const char *expected[] = {"AbC 97!?", "cjmla", "zxl", "qh97", "j", "   ", ""};
    for (size_t i = 0; i < sizeof inputs / sizeof *inputs; i++) {
        mainui_pinyin(sd, inputs[i], converted, sizeof converted);
        assert(!strcmp(converted, expected[i]));
    }
    mainui_pinyin(sd, "超级马里奥", converted, 3);
    assert(!strcmp(converted, "cj"));
    mainui_pinyin(sd, "\xf0\x9f", converted, sizeof converted);
    assert(!strcmp(converted, "  "));
    MainUIMetadata metadata;
    char path[4096];
    snprintf(path, sizeof path, "%s/Roms/Normal/alpha.nes", sd);
    assert(mainui_gamelist_metadata(path, NULL, &metadata) && metadata.found);
    assert(!strcmp(metadata.genre, "Action & Arcade"));
    size_t builds, reads, bytes, builds_after, reads_after, bytes_after;
    mainui_gamelist_metadata_stats(&builds, &reads, &bytes);
    for (int i = 0; i < 20; i++) {
        assert(mainui_gamelist_metadata(path, NULL, &metadata));
    }
    mainui_gamelist_metadata_stats(&builds_after, &reads_after, &bytes_after);
    assert(builds_after == builds && bytes_after == bytes && bytes > 0);
    assert(reads_after > reads && reads_after - reads <= 20 * 32768u);
    assert(!strcmp(metadata.rating, "5/10"));
    assert(strstr(metadata.description, "Caf") && strstr(metadata.description, "<test>"));
    snprintf(path, sizeof path, "%s/Roms/Normal/Folder/nested00.nes", sd);
    assert(!mainui_gamelist_metadata(path, NULL, &metadata) && !metadata.found && !*metadata.genre);
    snprintf(path, sizeof path, "%s/Roms/Bad/bad.nes", sd);
    assert(!mainui_gamelist_metadata(path, NULL, &metadata) && !metadata.found && !*metadata.genre);
    snprintf(path, sizeof path, "%s/Roms/Cut/cut.nes", sd);
    assert(mainui_gamelist_metadata(path, NULL, &metadata) && metadata.found);
    assert(!strcmp(metadata.genre, "Kept"));
    snprintf(path, sizeof path, "%s/Roms/Loose/loose.nes", sd);
    assert(mainui_gamelist_metadata(path, NULL, &metadata) && metadata.found);
    assert(!strcmp(metadata.genre, "Tom & Jerry &nbsp;"));
    assert(!strcmp(metadata.description, "Caf\xc3\xa9 \xe2\x80\x9c"
                                         "A < B\xe2\x80\x9d"));
    char root[4096], xml[4096];
    snprintf(root, sizeof root, "%s/Roms/Normal", sd);
    TEST_PATH(path, "%s/Folder/nested00.nes", root);
    TEST_PATH(xml, "%s/gamelist.xml", root);
    char *original_xml = mainui_read_text(xml, 8 * 1024 * 1024);
    assert(original_xml);
    assert(mainui_write_text_atomic(
        xml, "<gameList><game><path>Folder/nested00.nes</path><genre>Root</genre></game>"
             "<game><path>nested01.nes</path><genre>Wrong basename</genre></game></gameList>"));
    assert(mainui_gamelist_metadata(path, root, &metadata) && metadata.found);
    assert(!strcmp(metadata.genre, "Root"));
    char local[4096];
    TEST_PATH(local, "%s/Folder/gamelist.xml", root);
    assert(mainui_write_text_atomic(local,
                                    "<gameList><game><path>other.nes</path></game></gameList>"));
    assert(mainui_gamelist_metadata(path, root, &metadata) && metadata.found);
    assert(!strcmp(metadata.genre, "Root"));
    assert(mainui_write_text_atomic(
        local,
        "<gameList><game><path>./NESTED00.nes</path><genre>Local</genre></game></gameList>"));
    assert(mainui_gamelist_metadata(path, root, &metadata) && metadata.found);
    assert(!strcmp(metadata.genre, "Local"));
    TEST_PATH(path, "%s/Folder/nested01.nes", root);
    assert(mainui_gamelist_metadata(path, root, &metadata) && !metadata.found);
    assert(!*metadata.genre);
    /* A changed malformed XML replaces the old index, never its old metadata. */
    assert(mainui_write_text_atomic(local, "<gameList><game><path>nested01.nes</path>"));
    assert(!mainui_gamelist_metadata(path, root, &metadata) && !metadata.found);
    mainui_gamelist_metadata_stats(&builds, &reads, &bytes);
    assert(!mainui_gamelist_metadata(path, root, &metadata));
    mainui_gamelist_metadata_stats(&builds_after, &reads_after, &bytes_after);
    assert(builds_after == builds && bytes_after == bytes);
    assert(mainui_write_text_atomic(local, "<gameList><game><path>nested01.nes</path>"
                                           "<genre>Repaired</genre></game></gameList>"));
    assert(mainui_gamelist_metadata(path, root, &metadata) && metadata.found);
    assert(!strcmp(metadata.genre, "Repaired"));
    assert(remove(local) == 0);
    assert(mainui_write_text_atomic(xml, original_xml));
    free(original_xml);
    /* Seventeen paths exercise eviction; six full indexes exceed the byte budget. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 17; i++) {
            snprintf(path, sizeof path, "%s/Metadata/%d/target.nes", sd, i);
            assert(mainui_gamelist_metadata(path, NULL, &metadata) && metadata.found);
            assert(!strcmp(metadata.genre, "Indexed"));
            mainui_gamelist_metadata_stats(&builds, &reads, &bytes);
            assert(bytes <= 2u * 1024u * 1024u);
        }
    }
    snprintf(path, sizeof path, "%s/Metadata/TooMany/target.nes", sd);
    assert(!mainui_gamelist_metadata(path, NULL, &metadata) && !metadata.found);
    snprintf(path, sizeof path, "%s/Metadata/LargeRecord/target.nes", sd);
    assert(!mainui_gamelist_metadata(path, NULL, &metadata) && !metadata.found);
    mainui_gamelist_metadata_close();
    MainUILibraryItem item = {.rom = "/mnt/SDCARD/Roms/Normal/alpha.nes"};
    MainUILibrary library = {.items = &item, .count = 1};
    assert(mainui_library_contains(&library, item.rom));
    assert(!mainui_library_contains(&library, "alpha"));
    assert(!mainui_library_contains(&library, ""));
    puts("Boundary invariants passed: parent projection, paging, return, Expert, lookup, metadata, "
         "membership");
    return 0;
}
