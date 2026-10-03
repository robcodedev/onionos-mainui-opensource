/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/device_job.h"
#include "app/settings_page.h"
#include "platform/audio.h"
#include "platform/device_adapter.h"
#include "platform/device_info.h"
#include "platform/files.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef main

typedef struct {
    int calls, fail_at;
    bool block, block_scan;
    int ignore_cancel_ms; /* non-cooperating backend: finish late regardless */
    char commands[32][256];
} Fake;

static int finish_power_transition(void *context)
{
    MainUIDeviceJob *job = context;
    for (int i = 0; i < 30; ++i) {
        assert(!atomic_load(&job->cancel));
        SDL_Delay(1);
    }
    return 0;
}

static bool transport(void *context, const char *command, char *reply, size_t size,
                      MainUICancel cancel)
{
    Fake *fake = context;
    assert(fake->calls < 32);
    snprintf(fake->commands[fake->calls++], 256, "%s", command);
    if (fake->ignore_cancel_ms) {
        SDL_Delay((Uint32)fake->ignore_cancel_ms);
        return false;
    }
    if (fake->block || (fake->block_scan && !strcmp(command, "SCAN"))) {
        for (int i = 0; i < 1000 && !mainui_cancelled(cancel); ++i) {
            SDL_Delay(1);
        }
        return false;
    }
    if (mainui_cancelled(cancel)) {
        return false;
    }
    const char *text =
        fake->calls == fake->fail_at      ? "FAIL\n"
        : !strcmp(command, "ADD_NETWORK") ? "7\n"
        : !strcmp(command, "SCAN_RESULTS")
            ? "bssid / frequency / signal level / flags / ssid\naa\t2412\t-50\t[WPA2]\tHost Wi-Fi\n"
        : !strcmp(command, "SIGNAL_POLL") ? "RSSI=-81\nLINKSPEED=54\n"
        : !strcmp(command, "STATUS")
            ? "wpa_state=COMPLETED\nssid=Host Wi-Fi\nip_address=192.0.2.1\n"
            : "OK\n";
    snprintf(reply, size, "%s", text);
    return true;
}

static void put(const char *root, const char *name, const char *text)
{
    char file[4096];
    snprintf(file, sizeof file, "%s/%s", root, name);
    assert(mainui_write_text_atomic(file, text));
}

static bool always_cancel(void *context)
{
    (void)context;
    return true;
}

typedef struct {
    const char *serial, *model, *maximum, *current, *memory;
    uint64_t used, total, pll;
    int pll_calls;
} InfoFixture;

static bool info_text(void *context, const char *path, char *out, size_t size)
{
    InfoFixture *fixture = context;
    const char *text = NULL;
    if (!strcmp(path, "/runtime/deviceSN")) {
        text = fixture->serial;
    }
    else if (!strcmp(path, "/runtime/deviceModel")) {
        text = fixture->model;
    }
    else if (!strcmp(path, "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq")) {
        text = fixture->maximum;
    }
    else if (!strcmp(path, "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq")) {
        text = fixture->current;
    }
    else if (!strcmp(path, "/proc/meminfo")) {
        text = fixture->memory;
    }
    if (!text || strlen(text) >= size) {
        return false;
    }
    strcpy(out, text);
    return true;
}

static bool info_storage(void *context, const char *sd, uint64_t *used, uint64_t *total)
{
    InfoFixture *fixture = context;
    assert(!strcmp(sd, "/test-sd"));
    *used = fixture->used;
    *total = fixture->total;
    return true;
}

static bool info_cpu(void *context, uint64_t *mhz)
{
    InfoFixture *fixture = context;
    fixture->pll_calls++;
    *mhz = fixture->pll;
    return true;
}

static void about_readings(void)
{
    MainUIDeviceAdapter adapter = {.backend = DEVICE_ONION, .runtime = "/runtime"};
    InfoFixture fixture = {.serial = "012345aBcDeF\n",
                           .model = "354\n",
                           .maximum = "1200000\n",
                           .current = "800000\n",
                           .memory = "MemTotal:  112640 kB\nMemFree:  2048 kB\n",
                           .used = 3ull * 1073741824 + 700,
                           .total = 32ull * 1073741824,
                           .pll = 1600};
    MainUIDeviceInfoSource source = {&fixture, info_text, info_storage, info_cpu};
    MainUIDeviceInfo info;
    mainui_device_info(&adapter, "/test-sd", &source, &info);
    assert(!strcmp(info.serial, "012345aBcDeF"));
    assert(!strcmp(info.cpu, "1200MHz") && fixture.pll_calls == 0);
    assert(!strcmp(info.memory, "128M"));
    assert(!strcmp(info.storage, "3G/32G"));
    fixture.maximum = NULL;
    mainui_device_info(&adapter, "/test-sd", &source, &info);
    assert(!strcmp(info.cpu, "800MHz"));
    fixture.current = NULL;
    mainui_device_info(&adapter, "/test-sd", &source, &info);
    assert(!strcmp(info.cpu, "1600MHz") && fixture.pll_calls == 1);
    fixture.model = "285";
    fixture.serial = "bad serial";
    fixture.memory = "MemTotal: 18446744073709551616 kB";
    fixture.used = fixture.total + 1;
    mainui_device_info(&adapter, "/test-sd", &source, &info);
    assert(!strcmp(info.serial, "Unknown") && !strcmp(info.cpu, "Unknown"));
    assert(!strcmp(info.memory, "Unknown") && !strcmp(info.storage, "Unknown"));
    assert(fixture.pll_calls == 1); /* Never map the Mini PLL on another model. */
    fixture.maximum = "1200000 trailing";
    fixture.current = "-800000";
    fixture.memory = "MemTotal: 112640 MB";
    fixture.total = fixture.used = 0;
    mainui_device_info(&adapter, "/test-sd", &source, &info);
    assert(!strcmp(info.cpu, "Unknown") && !strcmp(info.memory, "Unknown"));
    assert(!strcmp(info.storage, "Unknown"));
    assert(mainui_device_pll_mhz(3019898 & 65535, 3019898 >> 16, 1, 0) == 1200);
    assert(mainui_device_pll_mhz(0, 0, 1, 3019898) == 1200);
    assert(mainui_device_pll_mhz(0, 0, 1, 0) == 0);
    assert(mainui_device_pll_mhz(1, 0, 0, 0) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    about_readings();
    const char *root = argv[1];
    MainUIDeviceAdapter adapter;
    assert(mainui_device_adapter_open(&adapter, DEVICE_SIMULATED, root));
    assert(mainui_device_brightness(&adapter, 0));
    assert(mainui_device_brightness(&adapter, 10));
    assert(!mainui_device_brightness(&adapter, -1));
    assert(!mainui_device_brightness(&adapter, 11));
    char duty_path[4096];
    snprintf(duty_path, sizeof duty_path, "%s/duty_cycle", root);
    char *duty = mainui_read_text(duty_path, 32);
    assert(duty && !strcmp(duty, "100"));
    free(duty);
    MainUIDeviceStatus state = mainui_device_status(&adapter);
    assert(!state.model.id && state.battery == -1 && state.wifi == -1);
    assert(!mainui_device_power_off(&adapter));
    put(root, "deviceModel", "285\n");
    put(root, "percBat", "63\n");
    put(root, "lid", "0\n");
    put(root, "sleeping", "1\n");
    state = mainui_device_status(&adapter);
    assert(state.model.lid && state.battery == 63 && state.lid == 0 && state.sleeping == 1);
    put(root, "charging", "1");
    assert(mainui_device_status(&adapter).battery == 500);
    put(root, "charging", "0");
    put(root, "percBat", "101");
    assert(mainui_device_status(&adapter).battery == -1);
    put(root, "percBat", "500");
    assert(mainui_device_status(&adapter).battery == 500);
    put(root, "percBat", "12junk");
    put(root, "lid", "2");
    put(root, "sleeping", "0");
    state = mainui_device_status(&adapter);
    assert(state.battery == -1 && state.lid == -1 && state.sleeping == 0);
    put(root, "deviceModel", "283");
    assert(!mainui_device_connect(&adapter, "Host", "password", (MainUICancel){0}));
    put(root, "deviceModel", "354");
    Fake fake = {0};
    adapter.transport = transport;
    adapter.transport_context = &fake;
    assert(mainui_device_connect(&adapter, "Bob's $Wi-Fi", "pass'word", (MainUICancel){0}));
    assert(fake.calls == 6);
    assert(!strcmp(fake.commands[1], "SET_NETWORK 7 ssid \"Bob's $Wi-Fi\""));
    assert(!strcmp(fake.commands[2], "SET_NETWORK 7 psk \"pass'word\""));
    assert(!strcmp(fake.commands[5], "SAVE_CONFIG"));
    fake = (Fake){0};
    assert(mainui_device_connect(&adapter, "Open", "", (MainUICancel){0}));
    assert(!strcmp(fake.commands[2], "SET_NETWORK 7 key_mgmt NONE"));
    for (int failure = 1; failure <= 6; ++failure) {
        fake = (Fake){.fail_at = failure};
        assert(!mainui_device_connect(&adapter, "Host", "password", (MainUICancel){0}));
        if (failure > 1) {
            assert(!strcmp(fake.commands[fake.calls - 1], "REMOVE_NETWORK 7"));
        }
    }
    fake = (Fake){0};
    assert(!mainui_device_connect(&adapter, "bad\"SSID", "password", (MainUICancel){0}));
    assert(
        !mainui_device_connect(&adapter, "Host", "password", (MainUICancel){always_cancel, NULL}));
    assert(!fake.calls);
    assert(mainui_device_scan(&adapter, (MainUICancel){0}));
    state = mainui_device_status(&adapter);
    assert(state.wifi == 1 && !strcmp(state.ssid, "Host Wi-Fi") &&
           !strcmp(state.address, "192.0.2.1"));
    assert(state.wifi_signal == 1);
    fake = (Fake){.fail_at = 2}; /* SIGNAL_POLL failure must retain connection state. */
    assert(mainui_device_refresh(&adapter, (MainUICancel){0}));
    state = mainui_device_status(&adapter);
    assert(state.wifi == 1 && state.wifi_signal == 0);
    const int dbm[] = {-127, -126, -81, -80, -71, -70, 127, 128};
    const int levels[] = {0, 1, 1, 2, 2, 3, 3, 0};
    for (size_t i = 0; i < sizeof dbm / sizeof *dbm; ++i) {
        char status_text[128];
        snprintf(status_text, sizeof status_text, "wpa_state=COMPLETED\nRSSI=%d\n", dbm[i]);
        put(root, "wifi-status.txt", status_text);
        assert(mainui_device_status(&adapter).wifi_signal == levels[i]);
    }
    put(root, "wifi-status.txt", "wpa_state=COMPLETED\nRSSI=bad\n");
    assert(mainui_device_status(&adapter).wifi_signal == 0);
    MainUISettingsPage page = {0};
    put(root, "wifi-scan.txt",
        "bssid / frequency / signal level / flags / ssid\n"
        "aa\t2412\t-81\t[WPA2]\tWeak\n"
        "bb\t2412\t-80\t[WPA2]\tMedium\n"
        "cc\t2412\t-70\t[WPA2]\tStrong\n");
    mainui_settings_page_scan_results(&page, root);
    assert(page.network_count == 3 && page.network_signal[0] == 1 && page.network_signal[1] == 2 &&
           page.network_signal[2] == 3);
    page.selected = 2;
    put(root, "wifi-scan.txt",
        "bssid / frequency / signal level / flags / ssid\n"
        "bb\t2412\t-71\t[WPA2]\tMedium\n"
        "aa\t2412\t-90\t[WPA2]\tWeak\n");
    mainui_settings_page_scan_results(&page, root);
    assert(page.selected == 1 && !strcmp(page.networks[0], "Medium"));
    put(root, "wifi-scan.txt",
        "bssid / frequency / signal level / flags / ssid\n"
        "aa\t2412\t-90\t[WPA2]\tWeak\n");
    mainui_settings_page_scan_results(&page, root);
    assert(page.selected == 1 && !strcmp(page.networks[0], "Weak"));
    /* A vanished last row clamps to the last remaining network. */
    put(root, "wifi-scan.txt",
        "bssid / frequency / signal level / flags / ssid\n"
        "aa\t2412\t-90\t[WPA2]\tWeak\n"
        "bb\t2412\t-70\t[WPA2]\tStrong\n");
    mainui_settings_page_scan_results(&page, root);
    page.selected = 2;
    put(root, "wifi-scan.txt",
        "bssid / frequency / signal level / flags / ssid\n"
        "aa\t2412\t-90\t[WPA2]\tWeak\n");
    mainui_settings_page_scan_results(&page, root);
    assert(page.selected == 1);
    page.selected = 0;
    mainui_settings_page_scan_results(&page, root);
    assert(page.selected == 0); /* Preserve an intentionally selected toggle. */
    page.selected = 1;
    put(root, "wifi-scan.txt", "bssid / frequency / signal level / flags / ssid\n");
    mainui_settings_page_scan_results(&page, root);
    assert(page.network_count == 0 && page.selected == 0);
    const char *bad_signals[] = {"999999999999999999999999",
                                 "-999999999999999999999999",
                                 "128",
                                 "-127",
                                 "-70junk",
                                 "",
                                 "bad"};
    for (size_t i = 0; i < sizeof bad_signals / sizeof *bad_signals; ++i) {
        char scan_text[256];
        snprintf(scan_text, sizeof scan_text,
                 "bssid / frequency / signal level / flags / ssid\n"
                 "aa\t2412\t%s\t[WPA2]\tMalformed signal\n",
                 bad_signals[i]);
        put(root, "wifi-scan.txt", scan_text);
        mainui_settings_page_scan_results(&page, root);
        assert(page.network_count == 1 && page.network_signal[0] == 1);
    }
    fake = (Fake){.fail_at = 1};
    assert(!mainui_device_refresh(&adapter, (MainUICancel){0}));
    assert(mainui_device_status(&adapter).wifi == -1);
    put(root, "wifi-status.txt", "wpa_state=not-a-real-state\n");
    assert(mainui_device_status(&adapter).wifi == -1);
    assert(mainui_device_power_off(&adapter));
    assert(!mainui_device_power_off(&adapter)); /* no duplicate/overwrite */
    assert(mainui_device_settings_changed(&adapter));
    assert(!mainui_audio_available());
    mainui_audio_pause(true);
    mainui_audio_pause(false);
    mainui_audio_close();
    assert(SDL_Init(SDL_INIT_TIMER) == 0);
    fake = (Fake){.block = true};
    MainUIDeviceJob job = {0};
    assert(mainui_device_job_start(&job, &adapter, 3, "Host", "password"));
    SDL_Delay(10);
    Uint32 started = SDL_GetTicks();
    mainui_device_job_close(&job);
    assert(SDL_GetTicks() - started < 500 && !job.thread && !*job.password);
    /* A backend that ignores cancellation delays close by its own duration
     * only; close then still joins and clears the job. */
    fake = (Fake){.ignore_cancel_ms = 400};
    assert(mainui_device_job_start(&job, &adapter, 4, NULL, NULL));
    SDL_Delay(10);
    started = SDL_GetTicks();
    mainui_device_job_close(&job);
    Uint32 waited = SDL_GetTicks() - started;
    assert(waited >= 300 && waited < 1500 && !job.thread && !job.operation);
    assert(fake.calls == 1);
    fake = (Fake){.block_scan = true};
    assert(mainui_device_job_start(&job, &adapter, 4, NULL, NULL));
    SDL_Delay(10);
    assert(mainui_device_job_start(&job, &adapter, 3, "Host", "password"));
    assert(job.queued_operation == 3);
    int completed = -1;
    started = SDL_GetTicks();
    while (completed < 0 && SDL_GetTicks() - started < 2000) {
        completed = mainui_device_job_take(&job);
        SDL_Delay(1);
    }
    assert(completed == 1 && job.operation == 3);
    assert(!strcmp(fake.commands[fake.calls - 1], "SAVE_CONFIG"));
    mainui_device_job_close(&job);
    for (int operation = 5; operation <= 6; ++operation) {
        job = (MainUIDeviceJob){.adapter = adapter, .operation = operation};
        atomic_init(&job.cancel, false);
        job.thread = SDL_CreateThread(finish_power_transition, &job);
        assert(job.thread);
        mainui_device_job_close(&job);
        assert(!job.thread);
    }
    SDL_Quit();
    puts("Device status, Wi-Fi failure, cancellation and power contracts passed");
    return 0;
}
