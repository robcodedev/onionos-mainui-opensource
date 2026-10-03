/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "app/settings_page.h"
#include "platform/device_request.h"
#include "platform/files.h"
#include "platform/system_config.h"
#include "ui/drawing.h"
#ifdef MAINUI_ONION
#include <SDL_loadso.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *display_keys[] = {"lumination", "hue", "saturation", "contrast"};

static void read_line(char *out, size_t size, const char *directory, const char *name)
{
    char path[4096];
    int n = directory ? snprintf(path, sizeof path, "%s/%s", directory, name) : -1;
    char *text = n > 0 && n < (int)sizeof path ? mainui_read_text(path, size - 1) : NULL;
    if (text) {
        text[strcspn(text, "\r\n")] = 0;
    }
    snprintf(out, size, "%s", text && *text ? text : "Unknown");
    free(text);
}

static bool scan(MainUISettingsPage *page, const char *runtime)
{
    char path[4096];
    int n = runtime ? snprintf(path, sizeof path, "%s/wifi-scan.txt", runtime) : -1;
    char *text = n > 0 && n < (int)sizeof path ? mainui_read_text(path, 16384) : NULL;
    page->network_count = 0;
    if (!text) {
        return false;
    }
    char *line = strchr(text, '\n');
    while (line && *++line && page->network_count < 32) {
        char *next = strchr(line, '\n');
        if (next) {
            *next = 0;
        }
        char *ssid = line;
        for (int i = 0; i < 4 && ssid; i++) {
            ssid = strchr(ssid, '\t');
            if (ssid) {
                ssid++;
            }
        }
        if (ssid) {
            ssid[strcspn(ssid, "\r\n")] = 0;
            bool duplicate = false;
            for (int i = 0; i < page->network_count; i++) {
                duplicate |= !strcmp(page->networks[i], ssid);
            }
            if (*ssid && strlen(ssid) <= 32 && !duplicate) {
                const char *flags = line;
                for (int field = 0; field < 3 && flags; ++field) {
                    flags = strchr(flags, '\t');
                    if (flags) {
                        ++flags;
                    }
                }
                const char *signal = strchr(line, '\t');
                signal = signal ? strchr(signal + 1, '\t') : NULL;
                char *signal_end = NULL;
                errno = 0;
                long parsed = signal ? strtol(signal + 1, &signal_end, 10) : -100;
                int dbm = signal && signal_end != signal + 1 && *signal_end == '\t' && !errno &&
                                  parsed >= -126 && parsed <= 127
                              ? (int)parsed
                              : -100;
                page->network_signal[page->network_count] = mainui_wifi_signal_level(dbm);
                page->network_secure[page->network_count] =
                    flags && ((strstr(flags, "WPA") && strstr(flags, "WPA") < ssid) ||
                              (strstr(flags, "WEP") && strstr(flags, "WEP") < ssid));
                strcpy(page->networks[page->network_count++], ssid);
            }
        }
        line = next;
    }
    free(text);
    return true;
}

void mainui_settings_page_open(MainUISettingsPage *page, MainUISettingKind kind, const char *sd,
                               const char *runtime)
{
    *page = (MainUISettingsPage){.open = true, .kind = kind};
    cJSON *root = mainui_system_read(sd);
    for (int i = 0; i < 4; i++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, display_keys[i]);
        page->values[i] = cJSON_IsNumber(value) && value->valuedouble == value->valueint &&
                                  value->valueint >= 0 && value->valueint <= 20
                              ? value->valueint
                          : i ? 10
                              : 7;
        /* keymon's live value is the one in effect; see stock settings. */
        int live = 0;
        if (mainui_system_live_value(sd, display_keys[i], &live) && live >= 0 && live <= 20) {
            page->values[i] = live;
        }
    }
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    page->wifi = cJSON_IsNumber(wifi) && wifi->valuedouble == 1;
    cJSON_Delete(root);
    if (kind == SET_ABOUT) {
        read_line(page->version, sizeof page->version, sd, ".tmp_update/onionVersion/version.txt");
        size_t version_end = strlen(page->version);
        if (version_end > 20) {
            version_end = 20;
            while (version_end && ((unsigned char)page->version[version_end] & 0xc0) == 0x80) {
                --version_end;
            }
        }
        page->version[version_end] = 0;
        snprintf(page->max_resolution, sizeof page->max_resolution, "Unknown");
        read_line(page->firmware, sizeof page->firmware, runtime, "firmware-version.txt");
        char model[24];
        read_line(model, sizeof model, runtime, "deviceModel");
        char *end = NULL;
        long value = strtol(model, &end, 10);
        MainUIDevice device =
            mainui_device(end && !*end && value >= 0 && value <= 1000 ? (int)value : 0);
        snprintf(page->device_name, sizeof page->device_name, "%s", device.name);
        if (device.id) {
            snprintf(page->model, sizeof page->model, "MY%d", device.id);
        }
        else {
            snprintf(page->model, sizeof page->model, "Unknown");
        }
        read_line(page->serial, sizeof page->serial, runtime, "serial-number.txt");
        read_line(page->cpu, sizeof page->cpu, runtime, "cpu-frequency.txt");
        read_line(page->memory, sizeof page->memory, runtime, "memory-size.txt");
        read_line(page->storage, sizeof page->storage, runtime, "storage-usage.txt");
    }
    if (kind == SET_WIFI) {
        scan(page, runtime);
    }
}

static void max_resolution(char out[32])
{
    FILE *stream = popen("dmesg 2>/dev/null", "r");
    if (!stream) {
        return;
    }
    unsigned best_width = 0, best_height = 0, timing_width = 0, timing_height = 0;
    char line[2048];
    while (fgets(line, sizeof line, stream)) {
        for (char *p = line; *p; ++p) {
            *p = (char)tolower((unsigned char)*p);
        }
        const char *width_key = strstr(line, "fb_timming_width=");
        if (!width_key) {
            width_key = strstr(line, "fb_timing_width=");
        }
        const char *height_key = strstr(line, "fb_timming_height=");
        if (!height_key) {
            height_key = strstr(line, "fb_timing_height=");
        }
        if (width_key && isdigit((unsigned char)strchr(width_key, '=')[1])) {
            timing_width = (unsigned)strtoul(strchr(width_key, '=') + 1, NULL, 10);
        }
        if (height_key && isdigit((unsigned char)strchr(height_key, '=')[1])) {
            timing_height = (unsigned)strtoul(strchr(height_key, '=') + 1, NULL, 10);
        }
        if (!strstr(line, "lcd") && !strstr(line, "panel") && !strstr(line, "display") &&
            !strstr(line, "framebuffer") && !strstr(line, "fb0") && !strstr(line, "resolution")) {
            continue;
        }
        unsigned width = 0, height = 0;
        for (const char *p = line; *p; ++p) {
            if (isdigit((unsigned char)*p) && (p == line || !isdigit((unsigned char)p[-1])) &&
                (sscanf(p, "%ux%u", &width, &height) == 2 ||
                 sscanf(p, "%u x %u", &width, &height) == 2)) {
                if (width >= 320 && width <= 4096 && height >= 240 && height <= 4096 &&
                    width * height > best_width * best_height) {
                    best_width = width;
                    best_height = height;
                }
            }
        }
        char *w = strstr(line, "width"), *h = strstr(line, "height");
        if (w && h) {
            while (*w && !isdigit((unsigned char)*w)) {
                ++w;
            }
            while (*h && !isdigit((unsigned char)*h)) {
                ++h;
            }
            width = (unsigned)strtoul(w, NULL, 10);
            height = (unsigned)strtoul(h, NULL, 10);
            if (width >= 320 && width <= 4096 && height >= 240 && height <= 4096 &&
                width * height > best_width * best_height) {
                best_width = width;
                best_height = height;
            }
        }
    }
    pclose(stream);
    if (timing_width >= 320 && timing_width <= 4096 && timing_height >= 240 &&
        timing_height <= 4096) {
        best_width = timing_width;
        best_height = timing_height;
    }
    if (best_width && best_height) {
        snprintf(out, 32, "%ux%u", best_width, best_height);
    }
}

void mainui_settings_page_device_info(MainUISettingsPage *page, const MainUIDeviceAdapter *adapter,
                                      const char *sd)
{
    if (page->kind != SET_ABOUT) {
        return;
    }
    if (adapter->backend == DEVICE_ONION) {
        max_resolution(page->max_resolution);
        if (!strcmp(page->max_resolution, "Unknown")) {
            char path[4096];
            int n = snprintf(path, sizeof path, "%s/screen_resolution", adapter->runtime);
            char *text = n > 0 && n < (int)sizeof path ? mainui_read_text(path, 64) : NULL;
            unsigned width, height;
            if (text && sscanf(text, "%ux%u", &width, &height) == 2 && width >= 320 &&
                width <= 4096 && height >= 240 && height <= 4096) {
                snprintf(page->max_resolution, sizeof page->max_resolution, "%ux%u", width, height);
            }
            free(text);
        }
        /* Onion runtime.sh reads this bootloader variable, not a /tmp fixture. */
        FILE *firmware = popen("/etc/fw_printenv miyoo_version 2>/dev/null", "r");
        if (firmware) {
            char line[128];
            if (fgets(line, sizeof line, firmware)) {
                const char *prefix = "miyoo_version=";
                char *version = line + strlen(prefix);
                if (!strncmp(line, prefix, strlen(prefix))) {
                    version[strcspn(version, "\r\n")] = 0;
                    size_t length = strlen(version);
                    if (length > 0 && length < sizeof page->firmware &&
                        strspn(version, "0123456789") == length) {
                        snprintf(page->firmware, sizeof page->firmware, "%s", version);
                    }
                }
            }
            pclose(firmware);
        }
    }
    MainUIDeviceInfo info;
    mainui_device_info(adapter, sd, NULL, &info);
    strcpy(page->serial, info.serial);
    strcpy(page->cpu, info.cpu);
    strcpy(page->memory, info.memory);
    strcpy(page->storage, info.storage);
}

static bool apply_display(const MainUISettingsPage *page)
{
#ifdef MAINUI_ONION
    if (!page->managed_device) {
        return false;
    }
    /* Stock 0x1bea4: preserve the 24-byte LCD structure, replace only its four
     * color fields. Load optionally so unsupported devices still start normally. */
    static void *module;
    static int (*get_lcd)(int, uint32_t *), (*set_lcd)(int, const uint32_t *);
    if (!module) {
        module = SDL_LoadObject("libmi_disp.so");
        if (!module) {
            return false;
        }
        void *get = SDL_LoadFunction(module, "MI_DISP_GetLcdParam");
        void *set = SDL_LoadFunction(module, "MI_DISP_SetLcdParam");
        if (sizeof get_lcd != sizeof get || sizeof set_lcd != sizeof set) {
            return false;
        }
        memcpy(&get_lcd, &get, sizeof get_lcd);
        memcpy(&set_lcd, &set, sizeof set_lcd);
    }
    if (!get_lcd || !set_lcd) {
        return false;
    }
    uint32_t parameters[6] = {0};
    if (get_lcd(0, parameters)) {
        return false;
    }
    parameters[1] = (uint32_t)(page->values[0] + 35);
    parameters[3] = (uint32_t)(page->values[1] * 5);
    parameters[4] = (uint32_t)(page->values[2] * 5);
    parameters[2] = (uint32_t)((page->values[3] + 15) * 2);
    return set_lcd(0, parameters) == 0;
#else
    (void)page;
    return false;
#endif
}

/* Stock WifiMenu contains the power toggle followed by scanned networks. */
static int wifi_control(const MainUISettingsPage *page, int row)
{
    (void)page;
    return row == 0 ? 0 : row + 4;
}

int mainui_settings_page_key(MainUISettingsPage *page, SDLKey key, const char *sd,
                             const char *runtime)
{
    (void)runtime;
    if (key == SDLK_ESCAPE) {
        memset(page, 0, sizeof *page);
        return 0;
    }
    int count = page->kind == SET_DISPLAY ? 4
                : page->kind == SET_WIFI  ? (page->wifi ? 1 + page->network_count : 1)
                : page->kind == SET_ABOUT ? 10
                                          : 0;
    if ((key == SDLK_UP || key == SDLK_DOWN) && count) {
        page->selected = (page->selected + (key == SDLK_UP ? count - 1 : 1)) % count;
        if (page->kind == SET_ABOUT || page->kind == SET_WIFI) {
            int rows = page->kind == SET_ABOUT ? 6 : 5;
            if (page->selected < page->start) {
                page->start = page->selected;
            }
            else if (page->selected >= page->start + rows) {
                page->start = page->selected - rows + 1;
            }
        }
    }
    if (page->kind == SET_DISPLAY) {
        int before[4];
        memcpy(before, page->values, sizeof before);
        if (key == SDLK_LEFT || key == SDLK_RIGHT) {
            int *value = &page->values[page->selected];
            *value += key == SDLK_LEFT ? -1 : 1;
            if (*value < 0) {
                *value = 0;
            }
            if (*value > 20) {
                *value = 20;
            }
        }
        if (key == SDLK_RETURN || key == SDLK_LEFT || key == SDLK_RIGHT) {
            cJSON *values = cJSON_CreateObject();
            bool ok = values != NULL;
            for (int i = 0; ok && i < 4; i++) {
                ok = cJSON_AddNumberToObject(values, display_keys[i], page->values[i]) != NULL;
            }
            MainUISettingsResult result =
                ok ? mainui_system_patch_result(sd, values) : MAINUI_SETTINGS_NOT_SAVED;
            ok = result == MAINUI_SETTINGS_SAVED;
            cJSON_Delete(values);
            /* Not saved: show the values in effect, not the ones asked for. */
            for (int i = 0; !ok && i < 4; i++) {
                if (result != MAINUI_SETTINGS_PARTLY ||
                    !mainui_system_live_value(sd, display_keys[i], &page->values[i])) {
                    page->values[i] = before[i];
                }
            }
            bool applied = ok && apply_display(page);
            snprintf(page->message, sizeof page->message, "%s",
                     !ok && mainui_system_damaged(sd)
                         ? "system.json is damaged, so settings are not saved."
                     : result == MAINUI_SETTINGS_PARTLY
                         ? "Could not save system.json, and the change could not be fully undone."
                     : !ok     ? "Could not save system.json."
                     : applied ? ""
                               : mainui_translate(
                                     123, "Display settings saved. Reboot the device to apply."));
        }
    }
    if (page->kind == SET_WIFI) {
        int control = wifi_control(page, page->selected);
        if (control == 0 && (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_RETURN)) {
            cJSON *value = cJSON_CreateNumber(!page->wifi);
            bool ok = value && mainui_system_write(sd, "wifi", value);
            cJSON_Delete(value);
            if (ok) {
                page->wifi = !page->wifi;
            }
            page->message[0] = 0;
            if (ok) {
                page->network_count = 0;
                page->selected = page->start = 0;
            }
            if (ok && page->managed_device) {
                return page->wifi ? 5 : 6;
            }
        }
        else if (key == SDLK_RETURN && page->wifi && control >= 5 &&
                 control - 5 < page->network_count) {
            strcpy(page->ssid, page->networks[control - 5]);
            memset(page->password, 0, sizeof page->password);
            if (!page->network_secure[control - 5]) {
                if (page->managed_device) {
                    return 3;
                }
                snprintf(page->message, sizeof page->message, "Connection requires the device.");
                return 0;
            }
            page->connect_after_password = true;
            return 2;
        }
    }
    return 0;
}

static void draw_status(SDL_Surface *screen, MainUITheme *theme, const char *text)
{
    char remaining[256];
    /* Stock translated notices contain layout newlines. Reflow their text
     * into the two-line status area instead of drawing control characters. */
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && used + 1 < sizeof remaining;
         ++p) {
        if (isspace(*p)) {
            if (used && remaining[used - 1] != ' ') {
                remaining[used++] = ' ';
            }
        }
        else {
            remaining[used++] = (char)*p;
        }
    }
    remaining[used] = 0;
    for (int line = 0; line < 2 && *remaining; line++) {
        size_t length = strlen(remaining), take = length, space = 0, fit = 0;
        for (size_t i = 1; i <= length; i++) {
            if ((remaining[i] & 0xc0) == 0x80) {
                continue;
            }
            char saved = remaining[i];
            remaining[i] = 0;
            int width = 0, height = 0;
            TTF_SizeUTF8(theme->menu_font, remaining, &width, &height);
            remaining[i] = saved;
            if (width > 590) {
                take = space ? space : fit ? fit : i;
                break;
            }
            fit = i;
            if (saved == ' ') {
                space = i;
            }
        }
        char saved = remaining[take];
        remaining[take] = 0;
        mainui_label(screen, theme->menu_font, theme->color, remaining, 20, 354 + line * 30);
        remaining[take] = saved;
        while (remaining[take] == ' ') {
            take++;
        }
        memmove(remaining, remaining + take, strlen(remaining + take) + 1);
    }
}

/* About extends the stock six-row viewport with device and Onion details. */
static void draw_about(const MainUISettingsPage *page, SDL_Surface *screen, MainUITheme *theme)
{
    const int ids[] = {59, 408, 409, 60, 61, 410, 62, 64, 66, 155};
    const char *labels[] = {"Model number",  "Model name",       "Max resolution",
                            "Serial number", "Firmware version", "Onion version",
                            "CPU frequency", "Memory size",      "Used/total storage",
                            "Website"};
    const char *values[] = {
        page->model,   page->device_name, page->max_resolution, page->serial,  page->firmware,
        page->version, page->cpu,         page->memory,         page->storage, "www.lomiyoo.com"};
    const MainUISettingsArtwork *art = mainui_theme_settings_artwork(theme);
    /* DeviceInfoMenu 0x2ac38 uses the same div-line-h asset (0x1821c8). */
    SDL_Rect content = {0, 60, 640, 360};
    SDL_SetClipRect(screen, &content);
    for (int i = 2; i < 7; i++) {
        mainui_blit(screen, theme->divider, 0, i * 60);
    }
    int start = page->start;
    for (int i = start; i < 10 && i < start + 6; i++) {
        int y = 62 + (i - start) * 60;
        SDL_Rect clip = {0, (Sint16)y, 640, (Uint16)(y + 60 > 420 ? 420 - y : 60)};
        SDL_SetClipRect(screen, &clip);
        if (i == page->selected) {
            mainui_blit(screen, art->selection, 0, y);
        }
        int label_width = 0, value_width = 0;
        const char *label = mainui_translate(ids[i], labels[i]);
        TTF_SizeUTF8(theme->menu_font, label, &label_width, NULL);
        const char *value =
            !strcmp(values[i], "Unknown") ? mainui_translate(74, "unknown") : values[i];
        TTF_SizeUTF8(theme->menu_font, value, &value_width, NULL);
        int text_y = y + (60 - TTF_FontHeight(theme->menu_font)) / 2;
        if (page->kind == SET_WIFI && i > 0) {
            SDL_Rect label_clip = {20, (Sint16)y, 430, 60};
            SDL_SetClipRect(screen, &label_clip);
            mainui_label(screen, theme->menu_font, theme->color, label, 20, text_y);
            SDL_SetClipRect(screen, &clip);
            /* Stock thresholds, but fix stock's weak-signal full-bars list bug. */
            int level = page->network_signal[i - 1];
            SDL_Surface *signal = theme->wifi_signal[level >= 2 && level <= 3 ? level : 1];
            if (page->wifi_connection == 1 &&
                !strcmp(page->connected_ssid, page->networks[i - 1]) && theme->wifi_connected) {
                mainui_blit(screen, theme->wifi_connected, 470 - theme->wifi_connected->w / 2,
                            y + (60 - theme->wifi_connected->h) / 2);
            }
            if (page->network_secure[i - 1] && theme->wifi_locked) {
                mainui_blit(screen, theme->wifi_locked, 520 - theme->wifi_locked->w / 2,
                            y + (60 - theme->wifi_locked->h) / 2);
            }
            if (signal) {
                mainui_blit(screen, signal, 580 - signal->w / 2, y + (60 - signal->h) / 2);
            }
        }
        else {
            mainui_label(screen, theme->menu_font, theme->color, label, 20, text_y);
        }
        int left = 20 + label_width;
        if (left < 620) {
            clip.x = (Sint16)left;
            clip.w = (Uint16)(620 - left);
            SDL_SetClipRect(screen, &clip);
            mainui_label(screen, theme->menu_font, theme->color, value, 620 - value_width, text_y);
        }
    }
    SDL_SetClipRect(screen, NULL);
}

void mainui_settings_page_draw(const MainUISettingsPage *page, SDL_Surface *screen,
                               MainUITheme *theme)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 24, 24, 24));
    mainui_blit(screen, theme->background, 0, 0);
    mainui_draw_header(screen, theme, mainui_stock_setting_label(page->kind));
    if (page->kind == SET_DISPLAY) {
        mainui_draw_action_footer(screen, theme, mainui_translate(124, "Apply changes"),
                                  mainui_translate(89, "BACK"));
    }
    else {
        mainui_draw_footer(screen, theme, 0, -1);
    }
    if (page->kind == SET_ABOUT) {
        draw_about(page, screen, theme);
        return;
    }
    const char *names[] = {"Luminance", "Hue", "Saturation", "Contrast"};
    const char *wifi_names[] = {"WIFI", "SSID", "Password", "Connect", "Scan"};
    int count = page->kind == SET_DISPLAY ? 4
                : page->kind == SET_WIFI  ? (page->wifi ? 1 + page->network_count : 1)
                                          : 0;
    int start = page->start;
    const MainUISettingsArtwork *art = mainui_theme_settings_artwork(theme);
    for (int i = start; i < count && i < start + 5; i++) {
        int row_height = 60;
        int y = 62 + (i - start) * row_height;
        SDL_Rect clip = {0, (Sint16)y, 640, (Uint16)row_height};
        SDL_SetClipRect(screen, &clip);
        if (i == page->selected && page->kind != SET_ABOUT) {
            mainui_blit(screen, art->selection, 0, y);
        }
        char label[256] = "", value[160] = "";
        if (page->kind == SET_DISPLAY) {
            snprintf(label, sizeof label, "%s", mainui_translate(119 + i, names[i]));
            snprintf(value, sizeof value, "%02d/20", page->values[i]);
        }
        else if (page->kind == SET_WIFI) {
            int control = wifi_control(page, i);
            snprintf(label, sizeof label, "%s",
                     control < 5 ? mainui_translate((int[]){131, 135, 136, 133, 140}[control],
                                                    wifi_names[control])
                                 : page->networks[control - 5]);
            if (control == 0) {
                snprintf(value, sizeof value, "%s",
                         mainui_translate(page->wifi ? 104 : 105, page->wifi ? "ON" : "OFF"));
            }
            if (control == 1) {
                snprintf(value, sizeof value, "%s", page->ssid);
            }
            if (control == 2 && *page->password) {
                snprintf(value, sizeof value, "********");
            }
        }
        int text_y =
            page->kind == SET_DISPLAY ? y + (60 - TTF_FontHeight(theme->menu_font)) / 2 : y + 12;
        if (page->kind == SET_WIFI && i > 0) {
            SDL_Rect label_clip = {20, (Sint16)y, 430, 60};
            SDL_SetClipRect(screen, &label_clip);
            mainui_label(screen, theme->menu_font, theme->color, label, 20, text_y);
            SDL_SetClipRect(screen, &clip);
            /* Stock thresholds, but fix stock's weak-signal full-bars list bug. */
            int level = page->network_signal[i - 1];
            SDL_Surface *signal = theme->wifi_signal[level >= 2 && level <= 3 ? level : 1];
            if (page->wifi_connection == 1 &&
                !strcmp(page->connected_ssid, page->networks[i - 1]) && theme->wifi_connected) {
                mainui_blit(screen, theme->wifi_connected, 470 - theme->wifi_connected->w / 2,
                            y + (60 - theme->wifi_connected->h) / 2);
            }
            if (page->network_secure[i - 1] && theme->wifi_locked) {
                mainui_blit(screen, theme->wifi_locked, 520 - theme->wifi_locked->w / 2,
                            y + (60 - theme->wifi_locked->h) / 2);
            }
            if (signal) {
                mainui_blit(screen, signal, 580 - signal->w / 2, y + (60 - signal->h) / 2);
            }
        }
        else {
            mainui_label(screen, theme->menu_font, theme->color, label, 20, text_y);
        }
        if (page->kind == SET_DISPLAY || (page->kind == SET_WIFI && i == 0)) {
            int right_width = art->right ? art->right->w : 24;
            if (art->left) {
                mainui_blit(screen, art->left, 640 - right_width - art->left->w - 240,
                            y + (60 - art->left->h) / 2);
            }
            if (art->right) {
                mainui_blit(screen, art->right, 640 - right_width - 40,
                            y + (60 - art->right->h) / 2);
            }
            int width = 0;
            TTF_SizeUTF8(theme->menu_font, value, &width, NULL);
            mainui_label(screen, theme->menu_font, theme->color, value,
                         640 - right_width - 240 + (200 - width) / 2, text_y);
        }
        else {
            mainui_label(screen, theme->menu_font, theme->color, value, 320, text_y);
        }
    }
    SDL_SetClipRect(screen, NULL);
    SDL_Rect message_clip = {20, 352, 600, 64};
    SDL_SetClipRect(screen, &message_clip);
    if (page->kind != SET_WIFI || page->wifi) {
        draw_status(
            screen, theme,
            page->kind == SET_WIFI
                ? (!strcmp(page->message, "Scanning...") ? mainui_translate(83, "Scanning...") : "")
                : page->message);
    }
    SDL_SetClipRect(screen, NULL);
}

void mainui_settings_page_scan_results(MainUISettingsPage *page, const char *runtime)
{
    char selected[33] = "";
    if (page->selected > 0 && page->selected <= page->network_count) {
        strcpy(selected, page->networks[page->selected - 1]);
    }
    int previous = page->selected;
    scan(page, runtime);
    /* Keep a vanished network's row, clamping to the last remaining network.
     * Only an empty list (or an already selected toggle) selects the toggle. */
    page->selected = previous > page->network_count ? page->network_count : previous;
    for (int i = 0; *selected && i < page->network_count; ++i) {
        if (!strcmp(selected, page->networks[i])) {
            page->selected = i + 1;
            break;
        }
    }
    int last_start = page->network_count >= 4 ? page->network_count - 4 : 0;
    if (page->start > last_start) {
        page->start = last_start;
    }
    if (page->selected < page->start) {
        page->start = page->selected;
    }
    else if (page->selected >= page->start + 5) {
        page->start = page->selected - 4;
    }
}

static int about_read(void *context)
{
    MainUIAboutJob *job = context;
    mainui_settings_page_open(&job->pending, SET_ABOUT, job->sd, job->adapter.runtime);
    mainui_settings_page_device_info(&job->pending, &job->adapter, job->sd);
    atomic_store_explicit(&job->done, true, memory_order_release);
    SDL_Event event = {.type = SDL_USEREVENT};
    SDL_PushEvent(&event);
    return 0;
}

void mainui_about_start(MainUIAboutJob *job, const MainUIDeviceAdapter *adapter, const char *sd)
{
    if (job->thread || !sd || strlen(sd) >= sizeof job->sd) {
        return;
    }
    job->adapter = *adapter;
    strcpy(job->sd, sd);
    atomic_init(&job->done, false);
    job->thread = SDL_CreateThread(about_read, job);
}

void mainui_about_update(MainUIAboutJob *job, MainUISettingsPage *page)
{
    if (job->thread && atomic_load_explicit(&job->done, memory_order_acquire)) {
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
        if (job->ready && !strcmp(job->pending.max_resolution, "Unknown") &&
            strcmp(job->cached.max_resolution, "Unknown") && *job->cached.max_resolution) {
            strcpy(job->pending.max_resolution, job->cached.max_resolution);
        }
        job->cached = job->pending;
        job->ready = true;
    }
    if (job->ready && page->open && page->kind == SET_ABOUT) {
        /* Preserve selection while updated readings arrive in the background. */
        int selected = page->selected, start = page->start;
        bool managed = page->managed_device;
        *page = job->cached;
        page->selected = selected;
        page->start = start;
        page->managed_device = managed;
    }
}

void mainui_about_close(MainUIAboutJob *job)
{
    if (job->thread) {
        SDL_WaitThread(job->thread, NULL);
    }
    memset(job, 0, sizeof *job);
}
