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
bool mainui_system_patch(const char *sd, const cJSON *values);
/* system.json exists but its content cannot be used (not a JSON object, NUL
 * bytes, too large), as opposed to missing, blank or an I/O error. Nothing
 * resets or rewrites it: it holds settings other programs share. */
bool mainui_system_damaged(const char *sd);
#endif
