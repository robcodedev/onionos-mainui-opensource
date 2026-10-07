/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/input.h"
#include "platform/menu_button.h"
#include "ui/drawing.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void presentation_case(int depth, int width, int height, int mode)
{
    Uint32 red = depth == 16 ? 0xf800 : 0xff0000;
    Uint32 green = depth == 16 ? 0x07e0 : 0xff00;
    SDL_Surface *frame = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, depth, red, green,
                                              depth == 16 ? 0x1f : 0xff, 0);
    SDL_Surface *display = SDL_CreateRGBSurface(
        SDL_SWSURFACE, width + 2, height + 2, mode == 2 ? 16 : depth, mode == 2 ? 0xf800 : red,
        mode == 2 ? 0x07e0 : green, mode == 2 || depth == 16 ? 0x1f : 0xff,
        /* Mode 5 mirrors the device: an opaque frame on an ARGB display. */
        mode == 5 && depth == 32 ? 0xff000000 : 0);
    assert(frame && display);
    SDL_Surface *reference = SDL_ConvertSurface(display, display->format, SDL_SWSURFACE);
    assert(reference);
    size_t size = (size_t)frame->pitch * frame->h;
    Uint8 *original = malloc(size);
    assert(original);
    for (size_t i = 0; i < size; ++i) {
        original[i] = (Uint8)(i * 37 + i / 11);
    }
    memcpy(frame->pixels, original, size);
    if (mode == 1) {
        SDL_Rect clip = {1, 1, (Uint16)(width - 1), (Uint16)(height - 1)};
        SDL_SetClipRect(display, &clip);
        SDL_SetClipRect(reference, &clip);
    }
    if (mode == 3) {
        SDL_SetAlpha(frame, SDL_SRCALPHA, 128);
    }
    if (mode == 4) {
        SDL_SetColorKey(frame, SDL_SRCCOLORKEY, 0);
    }
    for (int repeat = 0; repeat < 2; ++repeat) {
        memset(display->pixels, 0x5a, display->pitch * display->h);
        memset(reference->pixels, 0x5a, reference->pitch * reference->h);
        mainui_rotate_frame(frame);
        assert(SDL_BlitSurface(frame, NULL, reference, NULL) == 0);
        mainui_rotate_frame(frame);
        assert(mainui_blit_rotated(frame, display) == 0);
        assert(!memcmp(frame->pixels, original, size));
        assert(display->pitch == reference->pitch);
        assert(!memcmp(display->pixels, reference->pixels, display->pitch * display->h));
    }
    free(original);
    SDL_FreeSurface(reference);
    SDL_FreeSurface(display);
    SDL_FreeSurface(frame);
}

/* Rotating one changed row into the previous display image equals rotating the
 * whole frame: marquee-only frames present just their row. */
static void partial_presentation(bool alpha)
{
    SDL_Surface *frame =
        SDL_CreateRGBSurface(SDL_SWSURFACE, 640, 480, 32, 0xff0000, 0xff00, 0xff, 0);
    SDL_Surface *display =
        SDL_CreateRGBSurface(SDL_SWSURFACE, 640, 480, 32, 0xff0000, 0xff00, 0xff, 0xff000000);
    SDL_Surface *expected =
        SDL_CreateRGBSurface(SDL_SWSURFACE, 640, 480, 32, 0xff0000, 0xff00, 0xff, 0xff000000);
    assert(frame && display && expected);
    size_t size = (size_t)frame->pitch * frame->h;
    for (size_t i = 0; i < size; ++i) {
        ((Uint8 *)frame->pixels)[i] = (Uint8)(i * 37 + i / 11);
    }
    if (alpha) {
        /* Forces the SDL blit fallback, which must honour the clip as well. */
        SDL_SetAlpha(frame, SDL_SRCALPHA, SDL_ALPHA_OPAQUE);
    }
    assert(mainui_blit_rotated(frame, display) == 0);
    SDL_Rect row = {0, 120, 640, 60};
    for (int y = row.y; y < row.y + row.h; ++y) {
        memset((Uint8 *)frame->pixels + y * frame->pitch, y, (size_t)frame->pitch);
    }
    SDL_Rect rotated = mainui_rotated_rect(frame, row);
    assert(rotated.x == 0 && rotated.y == 300 && rotated.w == 640 && rotated.h == 60);
    SDL_SetClipRect(display, &rotated);
    assert(mainui_blit_rotated(frame, display) == 0);
    SDL_SetClipRect(display, NULL);
    assert(mainui_blit_rotated(frame, expected) == 0);
    assert(!memcmp(display->pixels, expected->pixels, (size_t)display->pitch * display->h));
    SDL_FreeSurface(expected);
    SDL_FreeSurface(display);
    SDL_FreeSurface(frame);
}

static void presentation(int depth)
{
    for (int mode = 0; mode < 6; ++mode) {
        presentation_case(depth, 3, 3, mode);
        presentation_case(depth, 6, 4, mode);
    }
    presentation_case(depth, 1, 1, 0);
    presentation_case(depth, 640, 480, 0);
    presentation_case(depth, 640, 480, 5);
}

#ifdef __linux__
/* Keymon's long press of Menu is a lone release; the reader counts every Menu
 * event and every Menu release, and nothing else, from a FIFO standing in for the input device. */
static void menu_releases(void)
{
    char path[] = "/tmp/mainui-menu-XXXXXX";
    assert(mkdtemp(path));
    char fifo[64];
    snprintf(fifo, sizeof fifo, "%s/event0", path);
    assert(mkfifo(fifo, 0600) == 0);
    assert(mainui_menu_button_open(fifo));
    int writer = open(fifo, O_WRONLY);
    assert(writer >= 0);
    struct input_event events[] = {
        {.type = EV_KEY, .code = KEY_ESC, .value = 0}, /* the lone release */
        {.type = EV_SYN},
        {.type = EV_KEY, .code = KEY_ESC, .value = 1},
        {.type = EV_KEY, .code = KEY_ESC, .value = 2},
        {.type = EV_KEY, .code = KEY_SPACE, .value = 0},
        {.type = EV_KEY, .code = KEY_ESC, .value = 0}, /* the physical release */
    };
    assert(write(writer, events, sizeof events) == (ssize_t)sizeof events);
    for (int i = 0; i < 200 && mainui_menu_button_releases() < 2; i++) {
        SDL_Delay(5);
    }
    SDL_Delay(20);
    assert(mainui_menu_button_releases() == 2 && mainui_menu_button_events() == 4);
    /* Keymon's L1 right after a Menu event is dropped; later ones are not. */
    assert(mainui_menu_button_recent(500));
    SDL_Delay(60);
    assert(!mainui_menu_button_recent(50));
    mainui_menu_button_close();
    close(writer);
    assert(unlink(fifo) == 0 && rmdir(path) == 0);
    /* No device: nothing is read, and closing is still safe. */
    assert(!mainui_menu_button_open("/nonexistent/event0"));
    mainui_menu_button_close();
}
#endif

int mainui_suite_input(void)
{
#ifdef __linux__
    menu_releases();
#endif
    presentation(16);
    presentation(24);
    presentation(32);
    partial_presentation(false);
    partial_presentation(true);
    /* Odd dimensions and 24-bit row padding catch width/pitch mistakes. */
    SDL_Surface *frame = SDL_CreateRGBSurface(SDL_SWSURFACE, 3, 3, 24, 0xff0000, 0xff00, 0xff, 0);
    assert(frame);
    for (int i = 0; i < 9; ++i) {
        Uint8 *pixel = (Uint8 *)frame->pixels + (i / 3) * frame->pitch + (i % 3) * 3;
        pixel[0] = (Uint8)i;
    }
    mainui_rotate_frame(frame);
    for (int i = 0; i < 9; ++i) {
        Uint8 *pixel = (Uint8 *)frame->pixels + (i / 3) * frame->pitch + (i % 3) * 3;
        assert(pixel[0] == 8 - i);
    }
    SDL_FreeSurface(frame);
    assert(mainui_input_key(SDLK_s) == SDLK_RCTRL);
    assert(mainui_input_key(SDLK_x) == SDLK_LSHIFT);
    assert(mainui_input_key(SDLK_y) == SDLK_LALT);
    assert(mainui_input_key(SDLK_m) == SDLK_F1);
    assert(mainui_input_key(SDLK_RETURN) == SDLK_RETURN);
    assert(mainui_input_key(SDLK_ESCAPE) == SDLK_ESCAPE);
    assert(mainui_input_key(SDLK_SPACE) == SDLK_RETURN);
    assert(mainui_input_key(SDLK_LCTRL) == SDLK_ESCAPE);
    const char actions[] = "SXYTM1234";
    const char *names[] = {"SELECT", "X", "Y", "START", "MENU", "L1", "R1", "L2", "R2"};
    for (int i = 0; i < 9; i++) {
        assert(!strcmp(mainui_input_button(mainui_input_script(actions[i])), names[i]));
    }
    const SDLKey physical[] = {SDLK_UP,     SDLK_DOWN,   SDLK_LEFT, SDLK_RIGHT, SDLK_SPACE,
                               SDLK_LCTRL,  SDLK_LSHIFT, SDLK_LALT, SDLK_RCTRL, SDLK_RETURN,
                               SDLK_ESCAPE, SDLK_e,      SDLK_t,    SDLK_TAB,   SDLK_BACKSPACE};
    const char scripts[] = "UDLRABXYSTM1234";
    for (size_t i = 0; i < sizeof physical / sizeof *physical; i++) {
        for (int up = 0; up < 2; up++) {
            SDL_Event event = {0};
            event.type = up ? SDL_KEYUP : SDL_KEYDOWN;
            event.key.keysym.sym = physical[i];
            event.key.keysym.unicode = 'e';
            mainui_input_device_event(&event);
            assert(event.type == (up ? SDL_KEYUP : SDL_KEYDOWN));
            assert(event.key.keysym.sym == mainui_input_script(scripts[i]));
            assert(event.key.keysym.unicode == 0);
            assert(mainui_input_key(event.key.keysym.sym) == event.key.keysym.sym);
        }
    }
    SDL_Event focus = {0}, original;
    focus.type = SDL_ACTIVEEVENT;
    focus.active.gain = 0;
    focus.active.state = SDL_APPINPUTFOCUS;
    original = focus;
    mainui_input_device_event(&focus);
    assert(!memcmp(&focus, &original, sizeof focus));
    assert(mainui_input_script('?') == SDLK_UNKNOWN);
    assert(!mainui_input_button(SDLK_UP));
    return 0;
}
