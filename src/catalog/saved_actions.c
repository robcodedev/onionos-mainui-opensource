/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/saved_actions.h"
#include "catalog/catalog.h"
#include "catalog/library.h"
#include "platform/files.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *string(const cJSON *record, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(record, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static const char *real_rom(const cJSON *record)
{
    const char *rom = string(record, "rompath");
    const char *separator = strstr(rom, "launch.sh:");
    return separator ? separator + strlen("launch.sh:") : rom;
}

/* Favorites keep matching by ROM path alone. Recents use mainui_recent_same():
 * the reader lists one ROM under two launchers as two rows, so the launcher is
 * part of a Recent's identity. */
static bool same(const cJSON *a, const cJSON *b)
{
    const char *rom = real_rom(a), *other = real_rom(b);
    if (*rom || *other) {
        return mainui_same_rom(rom, other);
    }
    return !strcmp(string(a, "launch"), string(b, "launch")) &&
           !strcmp(string(a, "label"), string(b, "label"));
}

/* The Favorite row a record describes, as listed: one ROM can be a Favorite
 * under several labels and launchers. */
static bool exact(const cJSON *a, const cJSON *b)
{
    const cJSON *ta = cJSON_GetObjectItemCaseSensitive(a, "type");
    const cJSON *tb = cJSON_GetObjectItemCaseSensitive(b, "type");
    const char *rom = real_rom(a), *other = real_rom(b);
    return (*rom || *other ? mainui_same_rom(rom, other) : true) &&
           !strcmp(string(a, "launch"), string(b, "launch")) &&
           !strcmp(string(a, "label"), string(b, "label")) &&
           (cJSON_IsNumber(ta) ? ta->valueint : 5) == (cJSON_IsNumber(tb) ? tb->valueint : 5);
}

/* How many lines of a Favorites file match `record` exactly and by ROM. */
static void count_matches(const char *input, const cJSON *record, int *exact_count, int *same_count)
{
    *exact_count = *same_count = 0;
    for (const char *line = input; *line;) {
        const char *newline = strchr(line, '\n');
        size_t size = newline ? (size_t)(newline - line) : strlen(line);
        cJSON *item = cJSON_ParseWithLength(line, size);
        if (cJSON_IsObject(item)) {
            *exact_count += exact(item, record);
            *same_count += same(item, record);
        }
        cJSON_Delete(item);
        if (!newline) {
            break;
        }
        line = newline + 1;
    }
}

bool mainui_saved_action_locked(const char *sd, bool recent, MainUISavedAction action,
                                const cJSON *record)
{
    if (action != SAVED_CLEAR && !cJSON_IsObject(record)) {
        return false;
    }
    if (action == SAVED_ADD && !*string(record, "label")) {
        return false;
    }
    if (recent && action == SAVED_ADD) {
        return false;
    }
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/Roms/%s", sd,
                     recent ? "recentlist.json" : "favourite.json");
    if (n < 0 || n >= (int)sizeof path) {
        return false;
    }
    /* A confirmed clear needs nothing from the old list, so damaged content
     * cannot block it. */
    if (action == SAVED_CLEAR) {
        return mainui_write_text_atomic(path, "");
    }
    FILE *file = fopen(path, "rb");
    bool missing = !file && errno == ENOENT;
    if (!file && !missing) {
        return false;
    }
    if (file) {
        fclose(file);
    }
    char *input = missing ? calloc(1, 1) : mainui_read_text(path, 8 * 1024 * 1024);
    if (!input) {
        return false;
    }
    char *added = action == SAVED_ADD ? cJSON_PrintUnformatted(record) : NULL;
    if (action == SAVED_ADD && !added) {
        free(input);
        return false;
    }
    size_t length = strlen(input), extra = added ? strlen(added) + 2 : 0;
    char *output = malloc(length + extra + 1);
    if (!output) {
        free(input);
        free(added);
        return false;
    }
    size_t written = 0;
    bool ok = true, found = false;
    int count = 0;
    /* Removing a Favorite takes the selected row exactly. A record that matches
     * no row exactly (the list changed) falls back to the ROM only if exactly
     * one row has it; otherwise nothing is removed. */
    int exact_count = 0, same_count = 0;
    if (!recent && action == SAVED_REMOVE) {
        count_matches(input, record, &exact_count, &same_count);
    }
    for (char *line = input; *line;) {
        char *newline = strchr(line, '\n');
        size_t size = newline ? (size_t)(newline - line) : strlen(line);
        char saved = line[size];
        line[size] = 0;
        const char *start = line;
        while (*start == ' ' || *start == '\t' || *start == '\r') {
            start++;
        }
        cJSON *item = *start ? cJSON_ParseWithOpts(start, NULL, true) : NULL;
        bool keep = action != SAVED_CLEAR;
        if (*start && (!cJSON_IsObject(item) || (++count > 10000 && action == SAVED_ADD))) {
            ok = false;
        }
        if (item && action == SAVED_ADD &&
            !strcmp(string(item, "label"), string(record, "label"))) {
            found = true; /* Patched duplicate-label guard covers Apps as well as ROMs. */
        }
        if (item && action == SAVED_REMOVE && !found &&
            (recent        ? mainui_recent_same(item, record)
             : exact_count ? exact(item, record)
                           : same_count == 1 && same(item, record))) {
            keep = false;
            found = true;
        }
        cJSON_Delete(item);
        line[size] = saved;
        size_t bytes = size + (newline ? 1 : 0);
        if (keep) {
            memcpy(output + written, line, bytes);
            written += bytes;
        }
        if (!ok || !newline) {
            break;
        }
        line = newline + 1;
    }
    if (action == SAVED_ADD && !found && count >= 10000) {
        ok = false;
    }
    if (!recent && action == SAVED_REMOVE && !found) {
        ok = false; /* not listed any more, or ambiguous: report, change nothing */
    }
    if (ok && action == SAVED_ADD && !found) {
        if (written && output[written - 1] != '\n') {
            output[written++] = '\n';
        }
        memcpy(output + written, added, strlen(added));
        written += strlen(added);
        output[written++] = '\n';
    }
    output[written] = 0;
    if (written > 8 * 1024 * 1024) {
        ok = false;
    }
    if (ok && (action != SAVED_ADD || !found)) {
        ok = mainui_write_text_atomic(path, output);
    }
    free(input);
    free(output);
    free(added);
    return ok;
}

bool mainui_saved_action(const char *sd, bool recent, MainUISavedAction action, const cJSON *record)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/Roms/.mainui-library", sd);
    if (n < 0 || n >= (int)sizeof path) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    bool ok = lock && mainui_saved_action_locked(sd, recent, action, record);
    mainui_file_unlock(lock);
    return ok;
}

/* Called only after a launch handoff commits. Keep ROM spelling intact: it is
 * also GameSwitcher's screenshot key. Search's launcher prefix is transport. */
static bool recent_game(const cJSON *record)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(record, "type");
    return cJSON_IsNumber(type) && (type->valuedouble == 5 || type->valuedouble == 17) &&
           !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "app_action")) &&
           !strncmp(real_rom(record), "/mnt/SDCARD/", 12) && *string(record, "label") &&
           strcmp(string(record, "launch"), "setstate");
}

/* Keep a damaged list's bytes as <list>.damaged, once: a different earlier
 * copy is not replaced. Needs the library lock (FAT has no hard links). */
static bool keep_damaged(const char *path, const char *original)
{
    char copy[4096 + 16];
    snprintf(copy, sizeof copy, "%s.damaged", path);
    struct stat info;
    if (!lstat(copy, &info)) {
        /* An earlier original is kept, readable or not. Anything but a regular
         * file there (a directory, say) cannot be shown to hold it. */
        return S_ISREG(info.st_mode);
    }
    if (errno != ENOENT) {
        return false; /* cannot tell: change nothing */
    }
    if (!mainui_write_bytes_new_locked(copy, original, strlen(original))) {
        return false;
    }
    fprintf(stderr, "Recents had unreadable lines; the original is kept as %s\n", copy);
    return true;
}

static bool recent_unlocked(const char *sd, const cJSON *record)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/Roms/recentlist.json", sd);
    if (n < 0 || n >= (int)sizeof path) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    bool missing = !file && errno == ENOENT;
    if (!file && !missing) {
        return false;
    }
    if (file) {
        fclose(file);
    }
    char *input = missing ? calloc(1, 1) : mainui_read_text(path, 8u * 1024u * 1024u);
    if (!input) {
        return false;
    }
    cJSON *items[50] = {0};
    /* Preserve stock spelling, missing artwork and Search launch records. */
    items[0] = cJSON_Duplicate(record, true);
    bool ok = items[0] != NULL;
    int count = 1, parsed = 0;
    bool kept_original = false;
    /* The loop splits `input` in place; keep the bytes for keep_damaged(). */
    char *original = ok ? strdup(input) : NULL;
    ok = original != NULL;
    for (char *line = input; ok && *line && parsed < 200 && count < 50;) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = 0;
        }
        const char *start = line;
        while (*start == ' ' || *start == '\t' || *start == '\r') {
            start++;
        }
        if (*start) {
            cJSON *item = cJSON_ParseWithOpts(start, NULL, true);
            parsed++;
            if (!cJSON_IsObject(item)) {
                /* Recents is history MainUI keeps, and the list already skips
                 * this line. Keep the original once, then record the launch
                 * without it rather than stop recording games. */
                cJSON_Delete(item);
                if (!kept_original) {
                    ok = keep_damaged(path, original);
                    kept_original = true;
                }
                if (!ok) {
                    break;
                }
            }
            else {
                bool keep = recent_game(item);
                for (int i = 0; keep && i < count; i++) {
                    if (mainui_recent_same(items[i], item)) {
                        keep = false;
                    }
                }
                if (keep) {
                    items[count++] = item;
                }
                else {
                    cJSON_Delete(item);
                }
            }
        }
        if (!next) {
            break;
        }
        line = next;
    }
    char *rows[50] = {0};
    size_t size = 0;
    for (int i = 0; ok && i < count; i++) {
        rows[i] = cJSON_PrintUnformatted(items[i]);
        ok = rows[i] != NULL;
        if (ok) {
            size += strlen(rows[i]) + 1;
        }
    }
    char *output = ok && size <= 8u * 1024u * 1024u ? malloc(size + 1) : NULL;
    ok = output != NULL;
    if (ok) {
        size_t offset = 0;
        for (int i = 0; i < count; i++) {
            size_t length = strlen(rows[i]);
            memcpy(output + offset, rows[i], length);
            offset += length;
            output[offset++] = '\n';
        }
        output[offset] = 0;
        ok = mainui_write_text_atomic(path, output);
    }
    free(output);
    free(input);
    free(original);
    for (int i = 0; i < count; i++) {
        free(rows[i]);
        cJSON_Delete(items[i]);
    }
    return ok;
}

bool mainui_recent_add(const char *sd, const cJSON *record)
{
    if (!recent_game(record)) {
        return true;
    }
    if (!sd || !*sd) {
        return false;
    }
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/Roms/.mainui-library", sd);
    if (n < 0 || n >= (int)sizeof path) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    bool ok = lock && recent_unlocked(sd, record);
    mainui_file_unlock(lock);
    return ok;
}
