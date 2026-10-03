/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/render.h"
#include "app/screen_events.h"
#include "menus/stock_settings.h"
#include "platform/audio.h"
#include "platform/system_config.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int saved(const char *sd, const char *key)
{
    cJSON *system = mainui_system_read(sd);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(system, key);
    int result = cJSON_IsNumber(value) ? value->valueint : -1;
    cJSON_Delete(system);
    return result;
}

/* One press on a value row: exactly one change sound, also at a limit, and
 * none from the selection logic on the following frame. Returns the menu
 * volume that was requested before the sound. */
static int press(MainUIApp *ui, SDLKey key)
{
    unsigned before = mainui_audio_change_requests(NULL);
    assert(mainui_screen_settings_open(ui, key));
    int volume = -2;
    assert(mainui_audio_change_requests(&volume) == before + 1);
    mainui_prepare_frame(ui);
    assert(mainui_audio_change_requests(NULL) == before + 1);
    assert(!*ui->message_title);
    return volume;
}

/* Brightness, Menu sound and Sleep timer click on every Left/Right (#10). */
static int sounds(const char *sd)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/system.json", sd);
    FILE *file = fopen(path, "wb");
    assert(file && fputs("{\"brightness\":9,\"bgmvol\":1,\"hibernate\":30}\n", file) >= 0);
    assert(fclose(file) == 0);
    MainUIApp *ui = calloc(1, sizeof *ui);
    assert(ui);
    ui->running = ui->settings_open = true;
    ui->confirmation = -1;
    ui->sd = sd;
    ui->settings.count = 3;
    ui->settings.rows[0] = SET_BRIGHTNESS;
    ui->settings.rows[1] = SET_SOUND;
    ui->settings.rows[2] = SET_SLEEP;
    ui->settings.values[SET_BRIGHTNESS] = 9;
    ui->settings.values[SET_SOUND] = 1;
    ui->settings.values[SET_SLEEP] = 30;
    mainui_prepare_frame(ui); /* initial presentation is silent */
    unsigned start = mainui_audio_change_requests(NULL);
    assert(start == 0);

    press(ui, SDLK_RIGHT);
    assert(ui->settings.values[SET_BRIGHTNESS] == 10 && saved(sd, "brightness") == 10);
    press(ui, SDLK_RIGHT); /* at the maximum: unchanged, still one click */
    assert(ui->settings.values[SET_BRIGHTNESS] == 10);
    press(ui, SDLK_LEFT);
    assert(saved(sd, "brightness") == 9);

    /* Moving the selection keeps its single sound from the frame logic. */
    assert(mainui_screen_settings_open(ui, SDLK_DOWN));
    assert(mainui_audio_change_requests(NULL) == start + 3);
    mainui_prepare_frame(ui);
    assert(mainui_audio_change_requests(NULL) == start + 4);

    /* Menu sound plays at the new volume: silent at 0, also at the minimum. */
    assert(press(ui, SDLK_LEFT) == 0 && saved(sd, "bgmvol") == 0);
    assert(press(ui, SDLK_LEFT) == 0 && ui->settings.values[SET_SOUND] == 0);
    assert(press(ui, SDLK_RIGHT) == 1 && saved(sd, "bgmvol") == 1);

    assert(mainui_screen_settings_open(ui, SDLK_DOWN));
    mainui_prepare_frame(ui);
    press(ui, SDLK_RIGHT); /* the sleep timer wraps 30 -> 0 */
    assert(saved(sd, "hibernate") == 0);
    press(ui, SDLK_LEFT);
    assert(saved(sd, "hibernate") == 30);
    assert(mainui_audio_change_requests(NULL) == start + 10);

    /* Other rows stay silent on Left/Right. */
    ui->settings.rows[2] = SET_ABOUT;
    assert(mainui_screen_settings_open(ui, SDLK_RIGHT));
    assert(mainui_audio_change_requests(NULL) == start + 10);
    free(ui);
    puts("Settings value rows click once per press, at limits too, after the volume change");
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    if (argc == 4 && !strcmp(argv[3], "sounds")) {
        return sounds(argv[2]);
    }
    MainUIStockSettings settings;
    mainui_stock_settings_load(&settings, argv[1], argv[2], 0);
    if (argc == 4) {
        assert(settings.count == 8);
        assert(settings.rows[0] == SET_SHUTDOWN && settings.rows[7] == SET_ABOUT);
        const int models[] = {283, 354, 284};
        for (unsigned i = 0; i < sizeof models / sizeof *models; ++i) {
            mainui_stock_settings_load(&settings, argv[1], argv[2], models[i]);
            bool wifi = false;
            for (int row = 0; row < settings.count; ++row) {
                wifi |= settings.rows[row] == SET_WIFI;
            }
            assert(wifi == (models[i] != 283));
            assert(settings.count == (models[i] == 283 ? 7 : 8));
        }
        puts("Empty Settings allowlist restores model-appropriate defaults");
        return 0;
    }
    assert(settings.count == 3);
    assert(settings.rows[0] == SET_DISPLAY && settings.rows[1] == SET_BRIGHTNESS &&
           settings.rows[2] == SET_ABOUT);
    assert(!strcmp(mainui_stock_setting_icon(SET_SHUTDOWN), "skin/icon-Shutdown.png"));
    assert(!strcmp(mainui_stock_setting_label(SET_SOUND), "Menu sound"));
    puts("Stock Settings whitelist and reference labels verified");
    return 0;
}
