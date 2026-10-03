/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_CORE_H
#define MAINUI_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool (*requested)(void *context);
    void *context;
} MainUICancel;

static inline bool mainui_cancelled(MainUICancel cancel)
{
    return cancel.requested && cancel.requested(cancel.context);
}

/* Process-lifetime patch settings. A zero font_size inherits the theme font;
 * scroll_status matches the reference model: 1 = disabled, 2 = enabled.
 * These values configure included functionality, never select build features.
 */
typedef struct {
    int rows, row_height, font_size;
    int repeat_delay, repeat_interval;
    int scroll_status, scroll_delay, scroll_speed;
    bool case_sensitive, dynamic_favorite_position, show_recents, show_expert;
} MainUIConfig;

/* Parse nullable file contents using the patcher's prefix/read-length rules.
 * out must be non-NULL. Every call resets all fields to baseline defaults;
 * mainui_config_load subsequently fills the marker-file flags.
 */
void mainui_config_parse(MainUIConfig *out, const char *rows, const char *font, const char *repeat,
                         const char *scroll);
/* Files are read from an explicit config directory; never writes user data. */
void mainui_config_load(MainUIConfig *out, const char *directory);

typedef struct {
    int id;
    const char *name;
    bool wifi, axp, lid;
} MainUIDevice;

/* Unknown IDs expose no hardware capabilities. Callers must not infer a
 * power-control interface from an unrecognized device.
 */
MainUIDevice mainui_device(int model);

typedef struct {
    int total, selected, start, end;
} MainUIViewport;

/* Normalize untrusted persisted indices against the current list size.
 * Empty lists have selected=end=-1. Nonempty views always contain selection.
 * This is the internal normalized model, not a serializer for legacy sentinels.
 */
void mainui_viewport_restore(MainUIViewport *view, int total, int rows, int selected, int start,
                             int end);
/* Move a normalized viewport; delta may be negative or exceed the list size.
 * Single-step callers may request edge wrapping, while page moves clamp.
 */
void mainui_viewport_move(MainUIViewport *view, int rows, int delta, bool wrap);
/* Fit a window to a list that changed underneath it (an edit, a removal, a
 * reload): select `selected` (clamped), keep the old start where it still
 * shows the selection, otherwise scroll just far enough, and close any gap
 * below the last row. The old end is ignored, being stale when the size
 * changed. Never moves a window that still fits. */
void mainui_viewport_refit(MainUIViewport *view, int total, int rows, int selected);

typedef struct {
    int source_x, width, destination_x;
} MainUIBlit;

/* Returns 0..2 clipped segments for the seamless 60px-gap marquee.
 * elapsed_ms starts after the configured idle delay. out must hold two entries.
 * Segments describe source/destination rectangles; the caller owns all surfaces.
 */
int mainui_marquee(uint64_t elapsed_ms, int speed, int title_width, int visible_width,
                   MainUIBlit out[2]);

/* Caller has already established overflow against the unobstructed lane. The
 * draw viewport may be wider because preview artwork is composited afterwards. */
/* Marquee frames are paced on this period: four 10 ms kernel ticks on the device. */
#define MAINUI_MARQUEE_FRAME_MS 40
/* Whole-pixel marquee offset after moving_ms of scrolling: a constant step per
 * frame period (or one pixel every k periods), so the motion stays even. */
uint64_t mainui_marquee_pixels(uint64_t moving_ms, int speed);
/* The same offset after a number of marquee frames. */
uint64_t mainui_marquee_step_pixels(uint64_t frames, int speed);
int mainui_marquee_stream(uint64_t elapsed_ms, int speed, int title_width, int draw_width,
                          MainUIBlit out[2]);

typedef struct {
    int title, type, selected, start, end;
} MainUIFrame;

#define MAINUI_STACK_MAX 32

typedef struct {
    MainUIFrame frames[MAINUI_STACK_MAX];
    size_t count;
} MainUIStack;

/* Parse the legacy /tmp/state.json stack, retaining numeric sentinel values.
 * text must be NUL-terminated; the file-reading layer must bound input size.
 * On failure out is untouched. No viewport repair occurs in this wire codec.
 */
bool mainui_state_parse(const char *json, MainUIStack *out);
/* Returns a newly allocated legacy JSON string, or NULL on failure.
 * The caller frees the result with free(); this project uses cJSON's default
 * allocator. No file publication or disk mutation occurs here.
 */
char *mainui_state_json(const MainUIStack *state);

#endif
