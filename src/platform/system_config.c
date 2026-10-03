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

static bool patch_unlocked(const char *sd, const cJSON *values)
{
    cJSON *root = mainui_system_read(sd);
    if (!root || !cJSON_IsObject(values)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *item;
#ifdef MAINUI_ONION
    MainUIDeviceAdapter adapter = {.backend = DEVICE_ONION};
    if (!strcmp(sd, "/mnt/SDCARD")) {
        cJSON_ArrayForEach(item, values)
        {
            if (cJSON_IsNumber(item) &&
                !mainui_device_setting_sync(&adapter, item->string, item->valueint)) {
                cJSON_Delete(root);
                return false;
            }
        }
    }
#endif
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
            return false;
        }
    }
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    char path[4096];
    bool ok = text && path_for(path, sd, "") && mainui_write_text_atomic(path, text);
    free(text);
    return ok;
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

bool mainui_system_write(const char *sd, const char *key, const cJSON *value)
{
    cJSON *values = cJSON_CreateObject();
    cJSON *copy = cJSON_Duplicate(value, true);
    bool ok = values && copy && cJSON_AddItemToObject(values, key, copy);
    if (!ok) {
        cJSON_Delete(copy);
    }
    ok = ok && mainui_system_patch(sd, values);
    cJSON_Delete(values);
    return ok;
}

bool mainui_system_patch(const char *sd, const cJSON *values)
{
    char path[4096];
    if (!path_for(path, sd, "")) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    bool ok = lock && patch_unlocked(sd, values);
    mainui_file_unlock(lock);
    return ok;
}
