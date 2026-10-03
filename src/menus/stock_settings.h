/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_STOCK_SETTINGS_H
#define MAINUI_STOCK_SETTINGS_H
#include "core/core.h"
#include "platform/system_config.h"

typedef enum {
    SET_SHUTDOWN,
    SET_BRIGHTNESS,
    SET_WIFI,
    SET_DISPLAY,
    SET_THEMES,
    SET_TWEAKS,
    SET_LANGUAGE,
    SET_SOUND,
    SET_SLEEP,
    SET_ABOUT,
    SET_COUNT
} MainUISettingKind;

typedef struct {
    MainUISettingKind rows[SET_COUNT];
    int count, selected, start;
    int values[SET_COUNT];
} MainUIStockSettings;

/* Initialize the stock row order/allowlist and saved values. Does not scan languages. */
void mainui_stock_settings_load(MainUIStockSettings *settings, const char *directory,
                                const char *sd, int model);
/* Adjust and atomically save the selected numeric setting; a value at its
 * limit is SAVED unchanged. NOT_SAVED keeps the value and file as they were
 * (also for nonnumeric rows). PARTLY: not saved, and the value now holds the
 * live setting in effect when it can be read. */
MainUISettingsResult mainui_stock_setting_adjust(MainUIStockSettings *settings, const char *sd,
                                                 int delta);
/* Borrow a translated label or immutable asset path; caller must not free it. */
const char *mainui_stock_setting_label(MainUISettingKind kind);
const char *mainui_stock_setting_icon(MainUISettingKind kind);
#endif
