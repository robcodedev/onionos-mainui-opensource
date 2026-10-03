/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_SYSTEM_CONFIG_H
#define MAINUI_SYSTEM_CONFIG_H
#include "cJSON.h"
#include <stdbool.h>
/* Preserve unknown system.json members. Failed saves leave the old file intact. */
/* Caller owns the object. A missing file yields an empty object; malformed or
 * unreadable input returns NULL, so it cannot be overwritten as an empty file. */
cJSON *mainui_system_read(const char *sd);
bool mainui_system_write(const char *sd, const char *key, const cJSON *value);
/* Numeric values also go to the live copy keymon keeps (the monitor), before
 * the file, since keymon writes that copy back to the file. When the file
 * cannot be replaced, the save fails and the live values it changed are put
 * back, unless something else changed them meanwhile. Nothing that can fail
 * without side effects is left until after the live values change. */
bool mainui_system_patch(const char *sd, const cJSON *values);

typedef enum {
    MAINUI_SETTINGS_SAVED,     /* the file was replaced (perhaps not flushed) */
    MAINUI_SETTINGS_NOT_SAVED, /* nothing changed: the old values are in effect */
    MAINUI_SETTINGS_PARTLY     /* not saved, but a live value could not be put
                                * back or was changed meanwhile: it may differ
                                * from the file; read it with
                                * mainui_system_live_value() */
} MainUISettingsResult;

MainUISettingsResult mainui_system_patch_result(const char *sd, const cJSON *values);
MainUISettingsResult mainui_system_write_result(const char *sd, const char *key,
                                                const cJSON *value);
/* The live value of `key` (keymon's copy); false when none is kept. */
bool mainui_system_live_value(const char *sd, const char *key, int *value);

/* The live copy: get is false for a key it does not keep or cannot read; set
 * is true for a key it does not keep, and only when a kept value was
 * confirmed. NULL restores the default: keymon on the device's own card. */
typedef struct {
    bool (*get)(void *context, const char *key, int *value);
    bool (*set)(void *context, const char *key, int value);
    void *context;
} MainUISettingsMonitor;

void mainui_system_set_monitor(const MainUISettingsMonitor *monitor);
/* system.json exists but its content cannot be used (not a JSON object, NUL
 * bytes, too large), as opposed to missing, blank or an I/O error. Nothing
 * resets or rewrites it: it holds settings other programs share. */
bool mainui_system_damaged(const char *sd);
#endif
