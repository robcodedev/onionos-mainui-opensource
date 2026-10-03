/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/system_config.h"
#include "platform/device_adapter.h"
#include "platform/files.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool path_for(char path[4096], const char *sd, const char *suffix)
{
    int n = snprintf(path, 4096, "%s/system.json%s", sd, suffix);
    return n > 0 && n < 4096;
}

cJSON *mainui_system_read(const char *sd)
{
    char path[4096];
    if (!path_for(path, sd, "")) {
        return NULL;
    }
    /* Distinguish a missing file from unreadable or malformed existing data. */
    FILE *file = fopen(path, "rb");
    if (!file) {
        return errno == ENOENT ? cJSON_CreateObject() : NULL;
    }
    fclose(file);
    char *text = mainui_read_text(path, 1024 * 1024);
    /* Blank files have no settings to preserve; nonempty malformed data does. */
    bool blank = text && text[strspn(text, " \t\r\n")] == 0;
    cJSON *root =
        blank ? cJSON_CreateObject() : (text ? cJSON_ParseWithOpts(text, NULL, true) : NULL);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static const MainUISettingsMonitor *installed_monitor;

void mainui_system_set_monitor(const MainUISettingsMonitor *monitor)
{
    installed_monitor = monitor;
}

#ifdef MAINUI_ONION
static bool keymon_get(void *context, const char *key, int *value)
{
    return mainui_device_setting_value(context, key, value);
}

static bool keymon_set(void *context, const char *key, int value)
{
    return mainui_device_setting_sync(context, key, value);
}
#endif

/* The live copy to keep in step: a test's, or keymon's for the real card. */
static const MainUISettingsMonitor *monitor_for(const char *sd)
{
    if (installed_monitor) {
        return installed_monitor;
    }
#ifdef MAINUI_ONION
    static MainUIDeviceAdapter adapter = {.backend = DEVICE_ONION};
    static const MainUISettingsMonitor keymon = {keymon_get, keymon_set, &adapter};
    if (!strcmp(sd, "/mnt/SDCARD")) {
        return &keymon;
    }
#else
    (void)sd;
#endif
    return NULL;
}

typedef struct {
    const char *key;
    int before, after;
} MonitorChange;

/* The file was not replaced: put back the live values this save changed.
 * Only a value still holding what was written here is restored, so a newer
 * change by keymon is kept. Reading and writing are separate steps of the
 * shared memory, not one atomic exchange; keymon changing a value between
 * them is not detected. False when a value could not be put back, or was
 * changed meanwhile: the live value then differs from the file. */
static bool restore_monitor(const MainUISettingsMonitor *monitor, MonitorChange *changes, int count)
{
    bool restored = true;
    for (int i = count - 1; i >= 0; i--) {
        int now = 0;
        bool read = monitor->get(monitor->context, changes[i].key, &now);
        /* Still the old value (the write did not take) needs nothing. */
        bool ok = read && (now == changes[i].before ||
                           (now == changes[i].after &&
                            monitor->set(monitor->context, changes[i].key, changes[i].before) &&
                            monitor->get(monitor->context, changes[i].key, &now) &&
                            now == changes[i].before));
        if (!ok) {
            fprintf(stderr, "Settings: the live value of %s could not be put back\n",
                    changes[i].key);
            restored = false;
        }
    }
    return restored;
}

static MainUISettingsResult patch_unlocked(const char *sd, const cJSON *values)
{
    cJSON *root = mainui_system_read(sd);
    if (!root || !cJSON_IsObject(values)) {
        cJSON_Delete(root);
        return MAINUI_SETTINGS_NOT_SAVED;
    }
    /* Everything that can fail without side effects comes first: the new
     * document, its text and its path. */
    const cJSON *item;
    cJSON_ArrayForEach(item, values)
    {
        cJSON *copy = cJSON_Duplicate(item, true);
        /* Every occurrence: a duplicate left behind would be read instead. */
        while (cJSON_GetObjectItemCaseSensitive(root, item->string)) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, item->string);
        }
        if (!copy || !cJSON_AddItemToObject(root, item->string, copy)) {
            cJSON_Delete(copy);
            cJSON_Delete(root);
            return MAINUI_SETTINGS_NOT_SAVED;
        }
    }
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    char path[4096];
    if (!text || !path_for(path, sd, "")) {
        free(text);
        return MAINUI_SETTINGS_NOT_SAVED;
    }
    /* keymon writes its live values back to system.json, so they change
     * first; if the file then cannot be replaced, they are put back. */
    const MainUISettingsMonitor *monitor = monitor_for(sd);
    MonitorChange changes[32];
    int count = 0;
    bool ok = true;
    cJSON_ArrayForEach(item, values)
    {
        if (!monitor || !cJSON_IsNumber(item) || count == (int)(sizeof changes / sizeof *changes)) {
            continue;
        }
        MonitorChange change = {.key = item->string, .after = item->valueint};
        /* A value the monitor does not keep, or cannot read (then it cannot
         * be written either), needs nothing put back. */
        bool kept = monitor->get(monitor->context, item->string, &change.before);
        if (kept) {
            changes[count++] = change; /* even unconfirmed, it may have changed */
        }
        if (!monitor->set(monitor->context, item->string, item->valueint)) {
            ok = false;
            break;
        }
    }
    ok = ok && mainui_write_text_atomic_result(path, text) != MAINUI_WRITE_UNCHANGED;
    free(text);
    if (ok) {
        return MAINUI_SETTINGS_SAVED;
    }
    return !monitor || restore_monitor(monitor, changes, count) ? MAINUI_SETTINGS_NOT_SAVED
                                                                : MAINUI_SETTINGS_PARTLY;
}

bool mainui_system_damaged(const char *sd)
{
    char path[4096];
    if (!path_for(path, sd, "")) {
        return false;
    }
    char *text = mainui_read_text(path, 1024 * 1024);
    if (!text) {
        return errno == EINVAL || errno == EFBIG; /* NUL bytes, or too large */
    }
    bool blank = text[strspn(text, " \t\r\n")] == 0;
    cJSON *root = blank ? NULL : cJSON_ParseWithOpts(text, NULL, true);
    bool damaged = !blank && !cJSON_IsObject(root);
    cJSON_Delete(root);
    free(text);
    return damaged;
}

MainUISettingsResult mainui_system_write_result(const char *sd, const char *key, const cJSON *value)
{
    cJSON *values = cJSON_CreateObject();
    cJSON *copy = cJSON_Duplicate(value, true);
    bool ok = values && copy && cJSON_AddItemToObject(values, key, copy);
    if (!ok) {
        cJSON_Delete(copy);
    }
    MainUISettingsResult result =
        ok ? mainui_system_patch_result(sd, values) : MAINUI_SETTINGS_NOT_SAVED;
    cJSON_Delete(values);
    return result;
}

bool mainui_system_write(const char *sd, const char *key, const cJSON *value)
{
    return mainui_system_write_result(sd, key, value) == MAINUI_SETTINGS_SAVED;
}

MainUISettingsResult mainui_system_patch_result(const char *sd, const cJSON *values)
{
    char path[4096];
    if (!path_for(path, sd, "")) {
        return MAINUI_SETTINGS_NOT_SAVED;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    MainUISettingsResult result = lock ? patch_unlocked(sd, values) : MAINUI_SETTINGS_NOT_SAVED;
    mainui_file_unlock(lock);
    return result;
}

bool mainui_system_patch(const char *sd, const cJSON *values)
{
    return mainui_system_patch_result(sd, values) == MAINUI_SETTINGS_SAVED;
}

bool mainui_system_live_value(const char *sd, const char *key, int *value)
{
    const MainUISettingsMonitor *monitor = monitor_for(sd);
    return monitor && monitor->get(monitor->context, key, value);
}
