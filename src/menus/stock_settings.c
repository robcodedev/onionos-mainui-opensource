/* SPDX-License-Identifier: GPL-3.0-only */
#include "menus/stock_settings.h"
#include "cJSON.h"
#include "localization/language.h"
#include "menus/menu.h"
#include "platform/files.h"
#include "platform/system_config.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *keys[] = {"shutdown", "brightness", "wifi",  "display", "themes",
                             "tweaks",   "language",   "sound", "sleep",   "about"};

const char *mainui_stock_setting_label(MainUISettingKind kind)
{
    static const char *labels[] = {
        "Shutdown", "Brightness",      "WIFI",       "Display",     "Themes",
        "Tweaks",   "Change language", "Menu sound", "Sleep timer", "About device"};
    static const int ids[] = {86, 26, 131, 118, 125, 407, 23, 25, 115, 30};
    return kind >= 0 && kind < SET_COUNT ? mainui_translate(ids[kind], labels[kind]) : "";
}

const char *mainui_stock_setting_icon(MainUISettingKind kind)
{
    static const char *icons[] = {"skin/icon-Shutdown.png",       "skin/icon-brightness-48.png",
                                  "skin/icon-setting-wifi.png",   "skin/color.png",
                                  "skin/icon-theme.png",          "skin/fixit.png",
                                  "skin/icon-language-48.png",    "skin/sound-icon.png",
                                  "skin/icon-device-info-48.png", "skin/icon-device-info-48.png"};
    return kind >= 0 && kind < SET_COUNT ? icons[kind] : "";
}

static int kind_for(const char *key)
{
    for (int i = 0; i < SET_COUNT; i++) {
        if (!strcmp(key, keys[i])) {
            return i;
        }
    }
    if (!strcmp(key, "wi-fi")) {
        return SET_WIFI;
    }
    if (!strcmp(key, "theme")) {
        return SET_THEMES;
    }
    if (!strcmp(key, "fixes")) {
        return SET_TWEAKS;
    }
    if (!strcmp(key, "menuSound") || !strcmp(key, "menu_sound")) {
        return SET_SOUND;
    }
    if (!strcmp(key, "sleepTimer") || !strcmp(key, "sleep_timer")) {
        return SET_SLEEP;
    }
    if (!strcmp(key, "aboutDevice") || !strcmp(key, "about_device")) {
        return SET_ABOUT;
    }
    return -1;
}

static bool exists(const char *sd, const char *relative)
{
    char path[4096];
    int length = snprintf(path, sizeof path, "%s/%s", sd, relative);
    if (length < 0 || length >= (int)sizeof path) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fclose(file);
    return true;
}

void mainui_stock_settings_load(MainUIStockSettings *settings, const char *directory,
                                const char *sd, int model)
{
    *settings = (MainUIStockSettings){0};
    bool seen[SET_COUNT] = {0}, enabled[SET_COUNT];
    int order[SET_COUNT], count = 0;
    for (int i = 0; i < SET_COUNT; i++) {
        enabled[i] = true;
    }
    char path[4096];
    char *text = NULL;
    int length = directory ? snprintf(path, sizeof path, "%s/main-menu.json", directory) : -1;
    if (length > 0 && length < (int)sizeof path) {
        text = mainui_read_text(path, 128 * 1024);
    }
    cJSON *root = mainui_menu_json(text);
    free(text);
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (cJSON_IsObject(object)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, object)
        {
            int kind = kind_for(item->string);
            if (kind < 0) {
                continue;
            }
            if (!seen[kind]) {
                order[count++] = kind;
            }
            seen[kind] = true;
            enabled[kind] = cJSON_IsTrue(item);
        }
    }
    if (cJSON_IsArray(object)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, object)
        {
            if (!cJSON_IsString(item)) {
                continue;
            }
            int kind = kind_for(item->valuestring);
            if (kind >= 0 && !seen[kind]) {
                order[count++] = kind;
                seen[kind] = true;
            }
        }
    }
    else {
        for (int i = 0; i < SET_COUNT; i++) {
            if (!seen[i]) {
                order[count++] = i;
            }
        }
    }
    /* Original Mini hardware has no Wi-Fi, even if the allowlist requests it.
     * Unknown desktop models retain the existing visible default. */
    bool wifi = model != 283;
    enabled[SET_WIFI] &= wifi;
    enabled[SET_THEMES] &= exists(sd, "App/ThemeSwitcher/launch.sh");
    enabled[SET_TWEAKS] &= exists(sd, "App/Tweaks/launch.sh");
    for (int i = 0; i < count; i++) {
        if (enabled[order[i]]) {
            settings->rows[settings->count++] = order[i];
        }
    }
    /* An unusable allowlist must not strand the user in an empty Settings page. */
    if (!settings->count) {
        for (int i = 0; i < SET_COUNT; i++) {
            if ((i != SET_WIFI || wifi) &&
                (i != SET_THEMES || exists(sd, "App/ThemeSwitcher/launch.sh")) &&
                (i != SET_TWEAKS || exists(sd, "App/Tweaks/launch.sh"))) {
                settings->rows[settings->count++] = i;
            }
        }
    }
    cJSON_Delete(root);
    cJSON *system = mainui_system_read(sd);
    const MainUISettingKind numeric[] = {SET_BRIGHTNESS, SET_SOUND, SET_SLEEP};
    const char *names[] = {"brightness", "bgmvol", "hibernate"};
    const int defaults[] = {7, 20, 5}, maximum[] = {10, 20, 30};
    for (int i = 0; i < 3; i++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(system, names[i]);
        int number = cJSON_IsNumber(value) ? value->valueint : defaults[i];
        /* keymon's live value is the one in effect, also when a failed save
         * could not put it back; the file's is the fallback. */
        int live = 0;
        if (mainui_system_live_value(sd, names[i], &live) && live >= 0 && live <= maximum[i]) {
            number = live;
        }
        if (number < 0 || number > maximum[i]) {
            number = defaults[i];
        }
        if (numeric[i] == SET_SLEEP && number != 0 && number != 5 && number != 15 && number != 30) {
            number = 0;
        }
        settings->values[numeric[i]] = number;
    }
    cJSON_Delete(system);
}

MainUISettingsResult mainui_stock_setting_adjust(MainUIStockSettings *settings, const char *sd,
                                                 int delta)
{
    if (settings->selected < 0 || settings->selected >= settings->count) {
        return MAINUI_SETTINGS_NOT_SAVED;
    }
    MainUISettingKind kind = settings->rows[settings->selected];
    const char *key = kind == SET_BRIGHTNESS ? "brightness"
                      : kind == SET_SOUND    ? "bgmvol"
                      : kind == SET_SLEEP    ? "hibernate"
                                             : NULL;
    if (!key) {
        return MAINUI_SETTINGS_NOT_SAVED;
    }
    int maximum = kind == SET_BRIGHTNESS ? 10 : kind == SET_SOUND ? 20 : 30;
    int value = settings->values[kind] + (delta < 0 ? -1 : 1);
    if (kind == SET_SLEEP) {
        /* fix-sleep-timer-left-right reverses the stock four-value cycle. */
        const int cycle[] = {0, 5, 15, 30};
        int index = 0;
        for (int i = 0; i < 4; i++) {
            if (cycle[i] == settings->values[kind]) {
                index = i;
            }
        }
        value = cycle[(index + (delta < 0 ? 3 : 1)) % 4];
    }
    if (value < 0) {
        value = 0;
    }
    if (value > maximum) {
        value = maximum;
    }
    if (value == settings->values[kind]) {
        return MAINUI_SETTINGS_SAVED;
    }
    cJSON *number = cJSON_CreateNumber(value);
    MainUISettingsResult result =
        number ? mainui_system_write_result(sd, key, number) : MAINUI_SETTINGS_NOT_SAVED;
    cJSON_Delete(number);
    int live = 0;
    if (result == MAINUI_SETTINGS_SAVED) {
        settings->values[kind] = value;
    }
    else if (result == MAINUI_SETTINGS_PARTLY && mainui_system_live_value(sd, key, &live)) {
        settings->values[kind] = live; /* show and apply what is in effect */
    }
    return result;
}
