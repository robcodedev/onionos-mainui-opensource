/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/audio.h"
#include <SDL.h>
#include <SDL_loadso.h>
#include <stdio.h>
#include <string.h>

/* Opaque SDL_mixer 1.2 objects. Resolve exported functions instead of introducing
 * an unconditional host dependency; the target ships SDL_mixer with Onion. */
static struct {
    void *module, *music, *change;
    bool opened, paused;
    bool music_failed; /* bgm.mp3 loaded but did not start playing */
    void (*pause_music)(void), (*resume_music)(void);
    void (*pause_channels)(int), (*resume_channels)(int);
    int (*open)(int, Uint16, int, int);
    void (*close)(void);
    void *(*load_music)(const char *);
    void *(*load_wave)(SDL_RWops *, int);
    int (*play_music)(void *, int);
    int (*play_channel)(int, void *, int, int);
    int (*volume_music)(int);
    int (*volume_channel)(int, int);
    int (*halt_music)(void);
    int (*halt_channel)(int);
    void (*free_music)(void *);
    void (*free_wave)(void *);
} audio;

/* Kept apart from `audio`, which close clears. UI thread only. */
static unsigned change_requests;
static int requested_volume = -1, change_volume = -1;

static bool resolve(const char *name, void *function, size_t size)
{
    void *address = SDL_LoadFunction(audio.module, name);
    if (!address || size != sizeof address) {
        return false;
    }
    /* memcpy avoids a nonportable C object/function pointer cast. Supported SDL
     * targets use equally sized pointers, checked above before publishing one. */
    memcpy(function, &address, size);
    return true;
}

static void *load_asset(const char *directory, const char *name, bool music)
{
    char path[4096];
    int length = snprintf(path, sizeof path, "%s/sound/%s", directory, name);
    if (length < 0 || length >= (int)sizeof path) {
        return NULL;
    }
    if (music) {
        return audio.load_music(path);
    }
    SDL_RWops *file = SDL_RWFromFile(path, "rb");
    return file ? audio.load_wave(file, 1) : NULL;
}

void mainui_audio_close(void)
{
    if (audio.opened) {
        audio.halt_music();
        audio.halt_channel(-1);
        if (audio.music) {
            audio.free_music(audio.music);
        }
        if (audio.change) {
            audio.free_wave(audio.change);
        }
        audio.close();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (audio.module) {
        SDL_UnloadObject(audio.module);
    }
    memset(&audio, 0, sizeof audio);
}

static void start_music(void)
{
    audio.music_failed = audio.play_music(audio.music, -1) != 0;
    if (audio.music_failed) {
        fprintf(stderr, "[audio] background music did not start: %s\n", SDL_GetError());
    }
}

void mainui_audio_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 20) {
        volume = 20;
    }
    requested_volume = volume;
    if (!audio.opened) {
        return;
    }
    int mixer_volume = volume * 128 / 20;
    audio.volume_music(mixer_volume);
    audio.volume_channel(-1, mixer_volume);
    /* Music that failed to start is tried again when Menu sound is raised
     * above 0; otherwise only the mixer volume would change. */
    if (audio.music_failed && volume > 0 && !audio.paused) {
        start_music();
    }
}

/* Report the failing step with its error before cleanup can overwrite it. */
static bool audio_failed(const char *step)
{
    fprintf(stderr, "Theme audio unavailable: %s: %s\n", step, SDL_GetError());
    return false;
}

bool mainui_audio_open(const char *theme, const char *fallback, int volume)
{
    mainui_audio_close();
    const char *library = "libSDL_mixer-1.2.so.0";
    audio.module = SDL_LoadObject(library);
    if (!audio.module) {
        return audio_failed("cannot load libSDL_mixer-1.2.so.0");
    }
#define LOAD(member, name) resolve(name, &audio.member, sizeof audio.member)
    bool linked = LOAD(open, "Mix_OpenAudio") && LOAD(close, "Mix_CloseAudio") &&
                  LOAD(load_music, "Mix_LoadMUS") && LOAD(load_wave, "Mix_LoadWAV_RW") &&
                  LOAD(play_music, "Mix_PlayMusic") && LOAD(play_channel, "Mix_PlayChannelTimed") &&
                  LOAD(volume_music, "Mix_VolumeMusic") && LOAD(volume_channel, "Mix_Volume") &&
                  LOAD(halt_music, "Mix_HaltMusic") && LOAD(halt_channel, "Mix_HaltChannel") &&
                  LOAD(pause_music, "Mix_PauseMusic") && LOAD(resume_music, "Mix_ResumeMusic") &&
                  LOAD(pause_channels, "Mix_Pause") && LOAD(resume_channels, "Mix_Resume") &&
                  LOAD(free_music, "Mix_FreeMusic") && LOAD(free_wave, "Mix_FreeChunk");
#undef LOAD
    if (!linked) {
        audio_failed("SDL_mixer is missing a function");
        mainui_audio_close();
        return false;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        audio_failed("SDL audio init failed");
        mainui_audio_close();
        return false;
    }
    /* The SSD202D audio-out runs at 48000 and keeps that rate whatever is
     * requested, and the firmware's SDL_mixer does not resample MP3. Asking for
     * 44100 played 48 kHz bgm.mp3 about 8% slow and low; 48000 plays it at its
     * own speed. A 44.1 kHz bgm.mp3 now plays slightly fast, as with stock.
     * WAV sounds are converted on load and are unaffected. */
    if (audio.open(48000, AUDIO_S16SYS, 2, 1024) != 0) {
        audio_failed("Mix_OpenAudio failed");
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        mainui_audio_close();
        return false;
    }
    audio.opened = true;
    /* Only the theme's own music, as in stock: Onion's "Mute background
     * music" renames the theme's bgm.mp3 to bgm_muted.mp3 before MainUI
     * starts (mute_theme_bgm in runtime.sh), so the fallback theme's music
     * must not play in its place. change.wav still falls back. */
    audio.music = load_asset(theme, "bgm.mp3", true);
    audio.change = load_asset(theme, "change.wav", false);
    if (!audio.change) {
        audio.change = load_asset(fallback, "change.wav", false);
    }
    mainui_audio_volume(volume);
    if (audio.music) {
        start_music();
    }
    return true;
}

void mainui_audio_change(void)
{
    change_requests++;
    change_volume = requested_volume;
    if (audio.opened && !audio.paused && audio.change) {
        audio.play_channel(-1, audio.change, 0, -1);
    }
}

bool mainui_audio_available(void)
{
    return audio.opened;
}

void mainui_audio_pause(bool paused)
{
    if (!audio.opened || audio.paused == paused) {
        return;
    }
    audio.paused = paused;
    if (paused) {
        audio.pause_music();
        audio.pause_channels(-1);
    }
    else {
        audio.resume_music();
        audio.resume_channels(-1);
    }
}

unsigned mainui_audio_change_requests(int *volume)
{
    if (volume) {
        *volume = change_volume;
    }
    return change_requests;
}
