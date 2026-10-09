/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui/drawing.h"
#include "ui/panels.h"
#include <SDL_image.h>
#include <stdio.h>
#include <string.h>

static void footer(SDL_Surface *screen, MainUITheme *theme, int page, int total, const char *open,
                   const char *back)
{
    SDL_Rect clip = {0, 420, 640, 60};
    SDL_SetClipRect(screen, &clip);
    if (theme->background) {
        SDL_Rect destination = clip;
        SDL_BlitSurface(theme->background, &clip, screen, &destination);
    }
    mainui_blit(screen, theme->footer, 0, 420);
    int x = 20;
    const char *hints[] = {theme->hide_hints ? " " : open, theme->hide_hints ? " " : back};
    for (int i = 0; i < 2; i++) {
        SDL_Surface *button = theme->buttons[i];
        if (button) {
            mainui_blit(screen, button, x, 450 - button->h / 2);
            x += button->w + 5;
        }
        if (!theme->hide_hint_text) {
            SDL_Surface *hint =
                mainui_theme_text(theme, theme->hint_font, hints[i], theme->hint_color);
            if (hint) {
                mainui_blit(screen, hint, x, 449 - hint->h / 2);
                x += hint->w;
            }
        }
        x += 30;
    }
    if (total >= 0 && !theme->hide_hint_text) {
        char text[48];
        snprintf(text, sizeof text, "%d", total);
        SDL_Surface *last = mainui_theme_text(theme, theme->hint_font, text, theme->total_color);
        snprintf(text, sizeof text, "%d/", total ? page : 0);
        SDL_Surface *current = mainui_theme_text(theme, theme->hint_font, text, theme->page_color);
        /* Stock 0x31268 reserves another 20px for totals above 99. The
         * counter shares the hints' line, y=449, as Onion's footer draws it. */
        int edge = total > 99 ? 600 : 620;
        if (last) {
            edge -= last->w;
            mainui_blit(screen, last, edge, 449 - last->h / 2);
        }
        if (current) {
            mainui_blit(screen, current, edge - current->w, 449 - current->h / 2);
        }
    }
    SDL_SetClipRect(screen, NULL);
}

void mainui_draw_footer(SDL_Surface *screen, MainUITheme *theme, int page, int total)
{
    footer(screen, theme, page, total, mainui_translate(88, "SELECT"),
           mainui_translate(89, "BACK"));
}

static void header_battery(SDL_Surface *screen, MainUITheme *theme)
{
    int percent = theme->battery_percent;
    int index = percent == 500 ? 5
                : percent < 5  ? 0
                : percent < 30 ? 1
                : percent < 60 ? 2
                : percent < 90 ? 3
                               : 4;
    SDL_Surface *icon = theme->battery_icons[index];
    if (percent < 0 || !icon) {
        return;
    }
    bool visible = theme->battery_visible && percent != 500 && icon->w <= 640;
    char value[32];
    snprintf(value, sizeof value, "%d%%", percent);
    SDL_Surface *text =
        visible && theme->battery_font
            ? mainui_theme_text(theme, theme->battery_font, value, theme->battery_color)
            : NULL;
    int width = text ? 2 * (text->w + 5) + icon->w : icon->w;
    if (text && (theme->battery_fixed || theme->battery_align == 1)) {
        width = icon->w > text->w ? icon->w : text->w;
    }
    if (width % 2) {
        width++;
    }
    if (width < 48) {
        width = 48;
    }
    int icon_x = 0, text_x = 0;
    if (text) {
        if (theme->battery_fixed) {
            text_x = theme->battery_align == 2   ? icon->w - text->w
                     : theme->battery_align == 1 ? (icon->w - text->w) / 2
                                                 : 0;
        }
        else if (theme->battery_align == 2) {
            icon_x = text->w + 5;
        }
        else {
            text_x = theme->battery_align == 1 ? (icon->w - text->w) / 2 : icon->w + 5;
        }
    }
    /* Onion theme/render/header.h centers the battery surface at (596,30). */
    int origin = 596 - width / 2;
    int level = theme->wifi_signal_level;
    SDL_Surface *wifi = theme->wifi_signal[level >= 2 && level <= 3 ? level : 1];
    if (theme->wifi_online && wifi) {
        int edge = origin + icon_x;
        if (text && origin + text_x + theme->battery_offset_x < edge) {
            edge = origin + text_x + theme->battery_offset_x;
        }
        mainui_blit(screen, wifi, edge - 8 - wifi->w, 30 - wifi->h / 2);
    }
    /* As Onion's battery surface: an even height of at least 48 (its
     * icon->w is Onion's own, kept for the same pixels), centered at y=30,
     * with the icon and text centered in it. Rounds odd heights up, where
     * centering each at 30 would round them down. */
    int height = text && text->h > icon->h ? text->h : icon->w;
    if (!text) {
        height = icon->h;
    }
    if (height % 2) {
        height++;
    }
    if (height < 48) {
        height = 48;
    }
    int top = 30 - height / 2;
    mainui_blit(screen, icon, origin + icon_x, top + (height - icon->h) / 2);
    if (text) {
        int offset = theme->battery_offset_y;
        const char *family = TTF_FontFaceFamilyName(theme->battery_font);
        if (family && !strncmp(family, "Exo 2", 5)) {
            offset -= (int)(0.075 * TTF_FontHeight(theme->battery_font));
        }
        mainui_blit(screen, text, origin + text_x + theme->battery_offset_x,
                    top + (height - text->h) / 2 + offset);
    }
}

static void draw_header_image(SDL_Surface *screen, MainUITheme *theme, SDL_Surface *title,
                              int margin)
{
    SDL_Rect clip = {0, 0, 640, 60};
    SDL_SetClipRect(screen, &clip);
    if (theme->background) {
        SDL_Rect destination = clip;
        SDL_BlitSurface(theme->background, &clip, screen, &destination);
    }
    mainui_blit(screen, theme->titlebar, 0, 0);
    /* Lists draw their title after the shared frame/status (0x318dc). */
    if (!margin) {
        header_battery(screen, theme);
    }
    clip.x = margin;
    clip.w = 640 - 2 * margin;
    SDL_SetClipRect(screen, &clip);
    if (title) {
        if (!theme->hide_title_text) {
            mainui_blit(screen, title, margin + (clip.w - title->w) / 2, (60 - title->h) / 2);
        }
    }
    else if (theme->logo) {
        mainui_blit(screen, theme->logo, 20, (60 - theme->logo->h) / 2);
    }
    clip.x = 0;
    clip.w = 640;
    SDL_SetClipRect(screen, &clip);
    if (margin) {
        header_battery(screen, theme);
    }
    SDL_SetClipRect(screen, NULL);
}

void mainui_draw_header_image(SDL_Surface *screen, MainUITheme *theme, SDL_Surface *title)
{
    /* Shared header 0x17018: x20, width600. */
    draw_header_image(screen, theme, title, 20);
}

void mainui_draw_list_header_image(SDL_Surface *screen, MainUITheme *theme, SDL_Surface *title)
{
    /* MenuWindow::draw 0x318dc: x0, width640. */
    draw_header_image(screen, theme, title, 0);
}

void mainui_draw_header(SDL_Surface *screen, MainUITheme *theme, const char *title)
{
    bool logo = !title || !strcmp(title, "MIYOO");
    SDL_Surface *text = !logo || !theme->logo
                            ? mainui_theme_text(theme, theme->title_font, title ? title : "MIYOO",
                                                theme->title_color)
                            : NULL;
    mainui_draw_header_image(screen, theme, text);
}

void mainui_draw_empty(SDL_Surface *screen, MainUITheme *theme)
{
    /* MainUI-354 at 0x31870..0x318d8 centers against the complete screen. */
    if (theme->empty) {
        mainui_blit(screen, theme->empty, (640 - theme->empty->w) / 2, (480 - theme->empty->h) / 2);
    }
}

void mainui_draw_folder_footer(SDL_Surface *screen, MainUITheme *theme, int count)
{
    mainui_draw_footer(screen, theme, 0, -1);
    if (theme->hide_hint_text) {
        return;
    }
    char value[32];
    snprintf(value, sizeof value, "(%d)", count);
    SDL_Surface *text = mainui_theme_text(theme, theme->hint_font, value, theme->total_color);
    if (text) {
        mainui_blit(screen, text, (count > 99 ? 600 : 620) - text->w, 450 - text->h / 2);
    }
}

void mainui_draw_popup_footer(SDL_Surface *screen, MainUITheme *theme)
{
    footer(screen, theme, 0, -1, mainui_translate(46, "OK"), mainui_translate(45, "CANCEL"));
}

void mainui_draw_action_footer(SDL_Surface *screen, MainUITheme *theme, const char *action,
                               const char *back)
{
    footer(screen, theme, 0, -1, action, back);
}

void mainui_draw_detail_counter(SDL_Surface *screen, MainUITheme *theme, int ordinal)
{
    /* 0x1e868 formats %04d, caps at 9999, and centers the digit strip
     * and background in a 124x50 box. The patch aligns the first digit at x276. */
    SDL_Surface *background = theme->number_background, *digits = theme->number_digits;
    if (!background || !digits || digits->w < 10) {
        return;
    }
    if (ordinal < 0) {
        ordinal = 0;
    }
    if (ordinal > 9999) {
        ordinal = 9999;
    }
    char text[5];
    snprintf(text, sizeof text, "%04d", ordinal);
    int width = digits->w / 10;
    int x = 216 + 2 * width + (124 - background->w) / 2;
    mainui_blit(screen, background, x, 55 + (50 - background->h) / 2);
    x += (background->w - 4 * (width + 1)) / 2;
    for (int i = 0; i < 4; i++) {
        SDL_Rect source = {(Sint16)((text[i] - '0') * width), 0, (Uint16)width, (Uint16)digits->h};
        SDL_Rect destination = {(Sint16)(x + i * (width + 1)), (Sint16)(55 + (50 - digits->h) / 2),
                                0, 0};
        SDL_BlitSurface(digits, &source, screen, &destination);
    }
}
