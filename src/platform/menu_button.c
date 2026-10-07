/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/menu_button.h"
#include <SDL.h>
#include <SDL_thread.h>
#include <stdatomic.h>
#include <string.h>

static atomic_int events, releases;
static atomic_uint last_event; /* SDL_GetTicks() of the last one */

bool mainui_menu_button_recent(unsigned window_ms)
{
    return atomic_load(&events) > 0 && SDL_GetTicks() - atomic_load(&last_event) < window_ms;
}

void mainui_menu_button_record(int value)
{
    atomic_store(&last_event, SDL_GetTicks());
    atomic_fetch_add(&events, 1);
    if (value == 0) {
        atomic_fetch_add(&releases, 1);
    }
}

int mainui_menu_button_events(void)
{
    return atomic_load(&events);
}

int mainui_menu_button_releases(void)
{
    return atomic_load(&releases);
}
#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

static int input = -1, wake[2] = {-1, -1};
static SDL_Thread *reader;

static int read_input(void *unused)
{
    (void)unused;
    struct pollfd fds[2] = {{.fd = input, .events = POLLIN}, {.fd = wake[0], .events = POLLIN}};
    for (;;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (fds[1].revents) {
            return 0; /* closing */
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return 0;
        }
        struct input_event read_events[16];
        ssize_t n = read(input, read_events, sizeof read_events);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (n <= 0) {
            return 0;
        }
        for (size_t i = 0; i < (size_t)n / sizeof *read_events; i++) {
            if (read_events[i].type != EV_KEY || read_events[i].code != KEY_ESC) {
                continue;
            }
            mainui_menu_button_record(read_events[i].value);
            if (read_events[i].value == 0) {
                SDL_Event event;
                memset(&event, 0, sizeof event);
                event.type = SDL_USEREVENT;
                event.user.code = MAINUI_MENU_RELEASE_CODE;
                SDL_PushEvent(&event);
            }
        }
    }
}

bool mainui_menu_button_open(const char *device)
{
    if (reader) {
        return true;
    }
    atomic_store(&events, 0);
    atomic_store(&releases, 0);
    input = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (input < 0) {
        return false;
    }
    if (pipe(wake) != 0) {
        close(input);
        input = -1;
        return false;
    }
    reader = SDL_CreateThread(read_input, NULL);
    if (!reader) {
        mainui_menu_button_close();
        return false;
    }
    return true;
}

void mainui_menu_button_close(void)
{
    if (reader) {
        ssize_t written = write(wake[1], "x", 1);
        (void)written;
        SDL_WaitThread(reader, NULL);
        reader = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (wake[i] >= 0) {
            close(wake[i]);
            wake[i] = -1;
        }
    }
    if (input >= 0) {
        close(input);
        input = -1;
    }
}
#else
bool mainui_menu_button_open(const char *device)
{
    (void)device;
    return false;
}

void mainui_menu_button_close(void) {}
#endif
