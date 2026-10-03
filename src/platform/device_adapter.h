/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_DEVICE_ADAPTER_H
#define MAINUI_DEVICE_ADAPTER_H
#include "core/core.h"

typedef enum {
    DEVICE_SIMULATED,
    DEVICE_ONION
} MainUIDeviceBackend;

typedef struct {
    MainUIDevice model;
    int wifi_signal;         /* Stock level: 0 unknown, 1 weak, 2 medium, 3 strong. */
    int battery;             /* -1 unavailable, 0..100 percent, 500 charging */
    int wifi, lid, sleeping; /* -1 unknown; otherwise 0/1 */
    char address[64], ssid[33];
} MainUIDeviceStatus;

/* Injectable command transport: returns a complete, bounded NUL-terminated reply.
 * Each call must honour cancellation and a bounded timeout. */
typedef bool (*MainUIWifiTransport)(void *, const char *, char *, size_t, MainUICancel);

typedef struct {
    MainUIDeviceBackend backend;
    char runtime[4096];
    char error[160];
    MainUIWifiTransport transport;
    void *transport_context;
} MainUIDeviceAdapter;

bool mainui_device_adapter_open(MainUIDeviceAdapter *, MainUIDeviceBackend, const char *runtime);
MainUIDeviceStatus mainui_device_status(const MainUIDeviceAdapter *);
bool mainui_device_connect(MainUIDeviceAdapter *, const char *ssid, const char *password,
                           MainUICancel);
int mainui_wifi_signal_level(int dbm);
bool mainui_device_refresh(MainUIDeviceAdapter *, MainUICancel);
bool mainui_device_wifi_enable(MainUIDeviceAdapter *, bool enabled, MainUICancel);
bool mainui_device_scan(MainUIDeviceAdapter *, MainUICancel);
bool mainui_device_power_off(const MainUIDeviceAdapter *);
/* Apply the Onion backlight curve, or write a simulated duty_cycle for tests. */
/* Update the stock keymon shared-memory value before notifying its reader. */
bool mainui_device_setting_sync(const MainUIDeviceAdapter *, const char *key, int value);
/* keymon's current value of `key`; false when it keeps none or is unavailable. */
bool mainui_device_setting_value(const MainUIDeviceAdapter *, const char *key, int *value);
bool mainui_device_brightness(const MainUIDeviceAdapter *, int value);
/* Preferences are consumed by keymon; there is no guessed direct suspend IPC. */
bool mainui_device_settings_changed(const MainUIDeviceAdapter *);
/* Real runtime only. Signal handlers just set flags; UI consumes them. */
bool mainui_device_signals_install(void);
void mainui_device_signals_take(bool *resume, bool *quit);
#endif
