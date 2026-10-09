/* SPDX-License-Identifier: GPL-3.0-only
 * Asset/config conventions from Onion src/common/theme/{config,load}.h.
 */
#include "ui/theme.h"
#include "cJSON.h"
#include "platform/system_config.h"
#include "ui/artwork.h"
#include "ui/drawing.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THEME_IMAGE_BUDGET (32u * 1024u * 1024u)
/* FreeType reads fonts on demand, so file size is not memory use. The limit
 * only rejects implausible files; CJK fonts commonly exceed 16 MiB. */
#define THEME_FONT_LIMIT (64u * 1024u * 1024u)

static size_t surface_bytes(const SDL_Surface *surface)
{
    return surface ? (size_t)surface->pitch * (size_t)surface->h : 0;
}

/* Count retained artwork directly so replacing/freeing assets cannot leave
 * stale accounting. All theme loads and ownership changes use the UI thread. */
static size_t image_bytes(const MainUITheme *t)
{
    const SDL_Surface *fixed[] = {t->background,
                                  t->titlebar,
                                  t->footer,
                                  t->selection,
                                  t->icon,
                                  t->folder,
                                  t->preview_background,
                                  t->logo,
                                  t->empty,
                                  t->favorite,
                                  t->number_background,
                                  t->number_digits,
                                  t->divider,
                                  t->popup_selection,
                                  t->loading_background,
                                  t->apps_selection,
                                  t->detail_default,
                                  t->wifi_connected,
                                  t->wifi_locked,
                                  t->settings_artwork.selection,
                                  t->settings_artwork.left,
                                  t->settings_artwork.right};
    size_t bytes = 0;
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; ++i) {
        bytes += surface_bytes(fixed[i]);
    }
    for (int i = 0; i < 6; ++i) {
        bytes += surface_bytes(t->popup_backgrounds[i]) + surface_bytes(t->battery_icons[i]);
    }
    for (int i = 0; i < 4; ++i) {
        bytes += surface_bytes(t->wifi_signal[i]);
    }
    for (int i = 0; i < 2; ++i) {
        bytes +=
            surface_bytes(t->tiles[i]) + surface_bytes(t->dots[i]) + surface_bytes(t->buttons[i]);
    }
    bytes += surface_bytes(t->expert_selection) + surface_bytes(t->popup_dim);
    for (int i = 0; i < SET_COUNT; ++i) {
        bytes += surface_bytes(t->settings_artwork.icons[i]);
    }
    return bytes;
}

/* One line per rejected asset: a theme over budget shows missing artwork,
 * and the log names which asset and how far over it was. Some screens load
 * artwork on every draw, so each asset is reported once (UI thread only). */
static void budget_exceeded(const char *what, size_t retained, size_t needed)
{
    static uint32_t reported[64];
    static size_t count;
    static bool suppressed;
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)what; *p; ++p) {
        hash = (hash ^ *p) * 16777619u;
    }
    for (size_t i = 0; i < count; ++i) {
        if (reported[i] == hash) {
            return;
        }
    }
    if (count == sizeof reported / sizeof reported[0]) {
        if (!suppressed) {
            fprintf(stderr, "Theme image budget exceeded: further reports suppressed\n");
            suppressed = true;
        }
        return;
    }
    reported[count++] = hash;
    fprintf(stderr,
            "Theme image budget exceeded: %s needs %zu KiB, %zu KiB of %u KiB already "
            "retained; using missing-artwork fallback\n",
            what, needed / 1024, retained / 1024, THEME_IMAGE_BUDGET / 1024u);
}

static SDL_Surface *load_image(const MainUITheme *theme, const char *path)
{
    SDL_Surface *image = mainui_artwork_load(path);
    if (!image) {
        return NULL;
    }
    size_t retained = image_bytes(theme);
    size_t needed = (size_t)image->w * (size_t)image->h * 4;
    if (retained > THEME_IMAGE_BUDGET || needed > THEME_IMAGE_BUDGET - retained) {
        budget_exceeded(path, retained, needed);
        SDL_FreeSurface(image);
        return NULL;
    }
    if (!SDL_GetVideoSurface()) {
        return image;
    }
    SDL_Surface *converted = mainui_artwork_software(image);
    if (converted) {
        SDL_FreeSurface(image);
        image = converted;
    }
    if (image->format->BytesPerPixel == 4 && image->format->Amask && !SDL_MUSTLOCK(image)) {
        bool opaque = true;
        for (int y = 0; opaque && y < image->h; ++y) {
            const Uint32 *row = (const Uint32 *)((const Uint8 *)image->pixels + y * image->pitch);
            for (int x = 0; x < image->w; ++x) {
                if ((row[x] & image->format->Amask) != image->format->Amask) {
                    opaque = false;
                    break;
                }
            }
        }
        if (opaque) {
            SDL_SetAlpha(image, 0, SDL_ALPHA_OPAQUE);
        }
        else {
            SDL_SetAlpha(image, SDL_SRCALPHA | SDL_RLEACCEL, SDL_ALPHA_OPAQUE);
        }
    }
    return image;
}

static bool text_fits(TTF_Font *font, const char *text)
{
    int width = 0, height = 0;
    return TTF_SizeUTF8(font, text, &width, &height) == 0 && width > 0 && height > 0 &&
           width <= 8192 && height <= 256 && (size_t)width * (size_t)height <= 262144;
}

SDL_Surface *mainui_theme_text(MainUITheme *theme, TTF_Font *font, const char *text,
                               SDL_Color color)
{
    if (!font || !text) {
        return NULL;
    }
    /* Keep the catalog-sized key independent of the rendered prefix. Long labels
     * remain distinct, and clipping is governed by pixel storage, not 511 bytes. */
    char bounded[sizeof theme->text_cache[0].text];
    size_t length = strnlen(text, sizeof bounded - 1);
    while (length && ((unsigned char)text[length] & 0xc0) == 0x80) {
        --length;
    }
    memcpy(bounded, text, length);
    bounded[length] = 0;
    text = bounded;
    int slot = 0;
    for (int i = 0; i < 32; ++i) {
        if (theme->text_cache[i].font == font && theme->text_cache[i].color.r == color.r &&
            theme->text_cache[i].color.g == color.g && theme->text_cache[i].color.b == color.b &&
            !strcmp(theme->text_cache[i].text, text)) {
            theme->text_cache[i].used = ++theme->text_clock;
            return theme->text_cache[i].surface;
        }
        if (theme->text_cache[i].used < theme->text_cache[slot].used) {
            slot = i;
        }
    }
    /* Find the longest UTF-8 prefix that fits the existing 1 MiB surface bound.
     * Cache under the original key so sizing is done only on cache misses. */
    memcpy(theme->text_cache[slot].text, bounded, length + 1);
    text = theme->text_cache[slot].text;
    char *render = bounded;
    if (!text_fits(font, render)) {
        size_t low = 0, high = length, best = 0;
        while (low < high) {
            size_t middle = low + (high - low + 1) / 2;
            size_t end = middle;
            while (end && ((unsigned char)text[end] & 0xc0) == 0x80) {
                --end;
            }
            memcpy(render, text, end);
            render[end] = 0;
            if (end && text_fits(font, render)) {
                best = end;
                low = middle;
            }
            else {
                high = middle - 1;
            }
        }
        memcpy(render, text, best);
        render[best] = 0;
    }
    SDL_Surface *surface = *render ? TTF_RenderUTF8_Blended(font, render, color) : NULL;
    SDL_FreeSurface(theme->text_cache[slot].surface);
    theme->text_cache[slot].surface = surface;
    theme->text_cache[slot].font = font;
    theme->text_cache[slot].color = color;
    theme->text_cache[slot].used = ++theme->text_clock;
    return surface;
}

static bool join(char out[4096], const char *base, const char *name)
{
    int n = snprintf(out, 4096, "%s/%s", base, name);
    return n >= 0 && n < 4096;
}

/* Fall back on decode failure as well as a missing file. A corrupt active
 * asset must not prevent the ordinary built-in image from being tried.
 */
static SDL_Surface *asset(const MainUITheme *theme, const char *dir, const char *base,
                          const char *name)
{
    char path[4096];
    SDL_Surface *surface = join(path, dir, name) ? load_image(theme, path) : NULL;
    if (!surface && strcmp(dir, base)) {
        surface = join(path, base, name) ? load_image(theme, path) : NULL;
    }
    return surface;
}

static bool profile_path(const MainUITheme *theme, const char *name, char path[4096])
{
    return *theme->profile && !strncmp(name, "skin/", 5) && join(path, theme->profile, name) &&
           mainui_file_stamp(path).exists;
}

static SDL_Surface *active_asset(const MainUITheme *theme, const char *name)
{
    char path[4096];
    if (profile_path(theme, name, path)) {
        return load_image(theme, path);
    }
    return join(path, theme->directory, name) ? load_image(theme, path) : NULL;
}

SDL_Surface *mainui_theme_image(const MainUITheme *theme, const char *name)
{
    char path[4096];
    /* Onion's profile override wins by existence, even if decoding fails. */
    if (profile_path(theme, name, path)) {
        return load_image(theme, path);
    }
    if (*theme->sd && !strncmp(name, "skin/extra/", 11)) {
        SDL_Surface *surface = active_asset(theme, name);
        if (!surface) {
            char resources[4096];
            if (join(resources, theme->sd, ".tmp_update/res") && join(path, resources, name + 11)) {
                surface = load_image(theme, path);
            }
        }
        return surface;
    }
    return asset(theme, theme->directory, theme->fallback, name);
}

const MainUISettingsArtwork *mainui_theme_settings_artwork(MainUITheme *theme)
{
    MainUISettingsArtwork *art = &theme->settings_artwork;
    if (!art->loaded) {
        art->loaded = true;
        art->selection = mainui_theme_image(theme, "skin/bg-list-s.png");
        art->left = mainui_theme_image(theme, "skin/icon-left-arrow-24.png");
        art->right = mainui_theme_image(theme, "skin/icon-right-arrow-24.png");
        for (int i = 0; i < SET_COUNT; i++) {
            art->icons[i] = mainui_theme_image(theme, mainui_stock_setting_icon(i));
        }
    }
    return art;
}

/* The patched popup loader searches smaller active-theme assets before using
 * built-in artwork. Repeated row bodies retain their pixels and bottom trim. */
static SDL_Surface *popup_background(MainUITheme *theme, int rows)
{
    SDL_Surface *source = NULL;
    int source_rows = rows;
    for (; source_rows > 0; source_rows--) {
        char name[64];
        snprintf(name, sizeof name, "skin/bg-pop-menu-%d.png", source_rows);
        source = active_asset(theme, name);
        if (source) {
            break;
        }
    }
    if (!source) {
        char name[64];
        source_rows = rows;
        for (; source_rows > 0; source_rows--) {
            snprintf(name, sizeof name, "skin/bg-pop-menu-%d.png", source_rows);
            source = mainui_theme_image(theme, name);
            if (source) {
                break;
            }
        }
    }
    /* An image already tall enough for every row, such as a full-screen
     * background, is drawn as it is: repeating its top band would push the
     * rest of the picture down. */
    if (!source || source_rows == rows || source->h < source_rows * 60 || source->h >= rows * 60) {
        return source;
    }
    int trim = source->h - source_rows * 60;
    size_t retained = image_bytes(theme);
    size_t expanded_bytes = (size_t)source->w * (size_t)(rows * 60 + trim) * 4;
    if (retained > THEME_IMAGE_BUDGET || expanded_bytes > THEME_IMAGE_BUDGET - retained) {
        budget_exceeded("expanded list selection", retained, expanded_bytes);
        return source;
    }
    SDL_Surface *expanded = SDL_CreateRGBSurface(
        SDL_SWSURFACE, source->w, rows * 60 + trim, source->format->BitsPerPixel,
        source->format->Rmask, source->format->Gmask, source->format->Bmask, source->format->Amask);
    if (!expanded) {
        return source;
    }
    if (source->format->palette) {
        SDL_SetColors(expanded, source->format->palette->colors, 0,
                      source->format->palette->ncolors);
    }
    /* Disable blending only on this owned temporary so alpha bytes are copied. */
    SDL_SetAlpha(source, 0, 255);
    for (int row = 0; row < rows; row++) {
        SDL_Rect from = {0, (Sint16)((row % source_rows) * 60), (Uint16)source->w, 60};
        SDL_Rect to = {0, (Sint16)(row * 60), 0, 0};
        SDL_BlitSurface(source, &from, expanded, &to);
    }
    if (trim) {
        SDL_Rect from = {0, (Sint16)(source_rows * 60), (Uint16)source->w, (Uint16)trim};
        SDL_Rect to = {0, (Sint16)(rows * 60), 0, 0};
        SDL_BlitSurface(source, &from, expanded, &to);
    }
    SDL_FreeSurface(source);
    return expanded;
}

SDL_Surface *mainui_theme_popup_background(MainUITheme *theme, int rows)
{
    if (rows < 1 || rows > 6) {
        return NULL;
    }
    if (!theme->popup_loaded[rows - 1]) {
        theme->popup_loaded[rows - 1] = true;
        theme->popup_backgrounds[rows - 1] = popup_background(theme, rows);
    }
    return theme->popup_backgrounds[rows - 1];
}

SDL_Surface *mainui_theme_popup_selection(MainUITheme *theme)
{
    if (!theme->popup_selection_loaded) {
        theme->popup_selection_loaded = true;
        theme->popup_selection = active_asset(theme, "skin/bg-list-popup-s.png");
        if (!theme->popup_selection) {
            theme->popup_selection = mainui_theme_image(theme, "skin/bg-list-s.png");
        }
    }
    return theme->popup_selection;
}

SDL_Surface *mainui_theme_console_icon(const MainUITheme *theme, const char *path)
{
    if (!path || !*path) {
        return NULL;
    }
    SDL_Surface *surface = load_image(theme, path);
    /* Host fixtures can borrow Onion's default icon pack without copying its
     * artwork into the source tree. Real SD paths are always tried first. */
    const char *name = strrchr(path, '/');
    const char *default_pack = strstr(path, "/Icons/Default/");
    if (default_pack) {
        name = default_pack + strlen("/Icons/Default");
    }
    if (!surface && name) {
        char fallback[4096];
        int length = snprintf(fallback, sizeof fallback, "%s/../../Icons/Default/%s",
                              theme->fallback, name + 1);
        if (length > 0 && length < (int)sizeof fallback) {
            surface = load_image(theme, fallback);
        }
    }
    return surface;
}

bool mainui_theme_font_path(const MainUITheme *theme, const char *name, char out[4096])
{
    if (!name) {
        name = "Exo-2-Bold-Italic.ttf";
    }
    const char *builtin = "/mnt/SDCARD/miyoo/app/";
    if (!strncmp(name, builtin, strlen(builtin))) {
        return join(out, theme->fallback, name + strlen(builtin));
    }
    const char *customer = "/customer/app/";
    if (!strncmp(name, customer, strlen(customer))) {
        return join(out, theme->fallback, name + strlen(customer));
    }
    const char *sd = "/mnt/SDCARD/";
    if (*theme->sd && !strncmp(name, sd, strlen(sd))) {
        return join(out, theme->sd, name + strlen(sd));
    }
    if (name[0] == '/' || (strlen(name) > 1 && name[1] == ':')) {
        if (strlen(name) >= 4096) {
            return false;
        }
        strcpy(out, name);
        return true;
    }
    return join(out, theme->directory, name);
}

static TTF_Font *bounded_font(const char *path, int size)
{
    MainUIFileStamp stamp = mainui_file_stamp(path);
    if (stamp.exists && stamp.size > THEME_FONT_LIMIT) {
        fprintf(stderr, "Theme font over %u MiB, using fallback: %s\n",
                THEME_FONT_LIMIT / (1024u * 1024u), path);
    }
    return stamp.exists && stamp.size > 0 && stamp.size <= THEME_FONT_LIMIT
               ? TTF_OpenFont(path, size)
               : NULL;
}

/* Built-in fallbacks, as stock MainUI chooses them: for a non-English
 * language the multilingual WenQuanYi font comes first, otherwise Exo 2. The
 * /customer/app copies (Onion's FALLBACK_FONT) are on internal flash, so they
 * survive a damaged or incomplete SD card. */
#define LANGUAGE_FONT "wqy-microhei.ttc"
#define ASCII_FONT "Exo-2-Bold-Italic.ttf"

/* Append ", <path>" to the error, keeping the end of a long path: the file
 * name is the useful part, and a long card or build path must not hide it. */
static void append_tried(MainUITheme *theme, const char *separator, const char *path)
{
    size_t used = strlen(theme->error);
    size_t length = strlen(path);
    const char *shown = length > 100 ? path + length - 97 : path;
    snprintf(theme->error + used, sizeof theme->error - used, "%s%s%s", separator,
             length > 100 ? "..." : "", shown);
}

static TTF_Font *font_open(MainUITheme *theme, const char *name, int size)
{
    char path[4096];
    /* Only the first font that fails completely is reported. */
    bool report = !*theme->error;
    bool requested = mainui_theme_font_path(theme, name, path);
    TTF_Font *font = requested ? bounded_font(path, size) : NULL;
    bool requested_ok = font != NULL;
    if (!font && report) {
        snprintf(theme->error, sizeof theme->error, "Cannot open font %.60s (tried",
                 name ? name : "(default)");
        append_tried(theme, " ", requested ? path : "-");
    }
    const char *order[2] = {ASCII_FONT, NULL};
    if (theme->language_font) {
        order[0] = LANGUAGE_FONT;
        order[1] = ASCII_FONT;
    }
    for (int i = 0; !font && i < 2 && order[i]; i++) {
        if (join(path, theme->fallback, order[i])) {
            font = bounded_font(path, size);
            if (!font && report) {
                append_tried(theme, ", ", path);
            }
        }
    }
#ifdef MAINUI_ONION
    for (int i = 0; !font && i < 2 && order[i]; i++) {
        if (join(path, "/customer/app", order[i])) {
            font = bounded_font(path, size);
            if (!font && report) {
                append_tried(theme, ", ", path);
            }
        }
    }
#endif
    if (font && !requested_ok) {
        theme->fallback_font_used = true;
    }
    if (report) {
        if (font) {
            *theme->error = '\0';
        }
        else {
            size_t used = strlen(theme->error);
            snprintf(theme->error + used, sizeof theme->error - used, ")");
        }
    }
    return font;
}

static int json_int(const cJSON *obj, const char *name, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

/* An explicit size of 0. Other invalid sizes still fall back to a default. */
static bool zero_size(const cJSON *obj, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(item) && item->valuedouble == 0;
}

static const char *json_string(const cJSON *obj, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static SDL_Color theme_color(const cJSON *object, const char *key, SDL_Color fallback)
{
    const char *value = json_string(object, key);
    unsigned red, green, blue;
    if (value && strlen(value) == 7 && sscanf(value, "#%2x%2x%2x", &red, &green, &blue) == 3) {
        return (SDL_Color){(Uint8)red, (Uint8)green, (Uint8)blue, 0};
    }
    return fallback;
}

/* Onion's Tweaks (Appearance > Theme overrides) saves overrides in the
 * profile's theme/config.json, and Onion applies that file over the theme's
 * config field by field (theme_loadFromPath() -> theme_applyConfig()). Merge
 * it the same way: each field in an object of the overrides replaces the
 * theme's, other fields of that object stay; a top-level value replaces the
 * theme's. A missing file changes nothing; an unusable one is logged. */
static void apply_overrides(const MainUITheme *t, cJSON **root)
{
    char path[4096];
    if (!*t->profile || !join(path, t->profile, "config.json")) {
        return;
    }
    errno = 0;
    char *text = mainui_read_text(path, 1024 * 1024);
    if (!text) {
        if (errno != ENOENT) {
            fprintf(stderr, "[theme] overrides in %s not used: %s\n", path,
                    errno ? strerror(errno) : "unreadable");
        }
        return;
    }
    cJSON *overrides = cJSON_ParseWithOpts(text, NULL, true);
    free(text);
    if (!cJSON_IsObject(overrides)) {
        fprintf(stderr, "[theme] overrides in %s not used: not a JSON object\n", path);
        cJSON_Delete(overrides);
        return;
    }
    if (!cJSON_IsObject(*root)) {
        cJSON_Delete(*root);
        *root = cJSON_CreateObject();
    }
    /* Onion reads the theme's hideIconTitle into both labels before the
     * overrides; set hideLabels from it so an override of one label keeps
     * the other as the theme had it. */
    const cJSON *legacy = cJSON_GetObjectItemCaseSensitive(*root, "hideIconTitle");
    if (cJSON_GetObjectItemCaseSensitive(overrides, "hideLabels") &&
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(*root, "hideLabels")) &&
        cJSON_IsBool(legacy)) {
        cJSON *labels = cJSON_CreateObject();
        cJSON_AddBoolToObject(labels, "icons", cJSON_IsTrue(legacy));
        cJSON_AddBoolToObject(labels, "hints", cJSON_IsTrue(legacy));
        cJSON_DeleteItemFromObjectCaseSensitive(*root, "hideLabels");
        if (labels && !cJSON_AddItemToObject(*root, "hideLabels", labels)) {
            cJSON_Delete(labels);
        }
    }
    for (cJSON *item = overrides->child; item; item = item->next) {
        if (!item->string) {
            continue;
        }
        cJSON *target = cJSON_GetObjectItemCaseSensitive(*root, item->string);
        if (cJSON_IsObject(item) && cJSON_IsObject(target)) {
            for (cJSON *field = item->child; field; field = field->next) {
                cJSON *copy = field->string ? cJSON_Duplicate(field, true) : NULL;
                if (copy) {
                    cJSON_DeleteItemFromObjectCaseSensitive(target, field->string);
                    if (!cJSON_AddItemToObject(target, field->string, copy)) {
                        cJSON_Delete(copy);
                    }
                }
            }
        }
        else {
            cJSON *copy = cJSON_Duplicate(item, true);
            if (copy) {
                cJSON_DeleteItemFromObjectCaseSensitive(*root, item->string);
                if (!cJSON_AddItemToObject(*root, item->string, copy)) {
                    cJSON_Delete(copy);
                }
            }
        }
    }
    cJSON_Delete(overrides);
}

bool mainui_theme_open(MainUITheme *t, const char *dir, const char *base,
                       const MainUIConfig *config)
{
    return mainui_theme_open_sd(t, dir, base, NULL, config);
}

bool mainui_theme_open_sd(MainUITheme *t, const char *dir, const char *base, const char *sd,
                          const MainUIConfig *config)
{
    *t = (MainUITheme){.color = {255, 255, 255, 0}, .icon_margin = -1};
    if (strlen(dir) >= sizeof t->directory || strlen(base) >= sizeof t->fallback) {
        return false;
    }
    strcpy(t->directory, dir);
    strcpy(t->fallback, base);
    if (sd && *sd) {
        if (strlen(sd) >= sizeof t->sd || !join(t->profile, sd, "Saves/CurrentProfile/theme")) {
            return false;
        }
        strcpy(t->sd, sd);
        /* Stock compares the first 7 bytes of the language with "en.lang". */
        cJSON *system = mainui_system_read(sd);
        const cJSON *language = cJSON_GetObjectItemCaseSensitive(system, "language");
        t->language_font =
            cJSON_IsString(language) && strncmp(language->valuestring, "en.lang", 7) != 0;
        cJSON_Delete(system);
    }
    char path[4096];
    char *text = join(path, dir, "config.json") ? mainui_read_text(path, 1024 * 1024) : NULL;
    /* Onion theme_loadFromPath uses the built-in config only when the active
     * config is absent; it does not merge unrelated built-in fields. */
    if (!text && join(path, base, "config.json")) {
        text = mainui_read_text(path, 1024 * 1024);
    }
    cJSON *root = text ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    free(text);
    apply_overrides(t, &root);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    const cJSON *gamelist = cJSON_GetObjectItemCaseSensitive(root, "gamelist");
    const char *list_face = json_string(list, "font");
    if (!list_face) {
        list_face = json_string(title, "font");
    }
    int font_size =
        config->font_size ? config->font_size : json_int(list, "size", json_int(title, "size", 25));
    if (font_size < 1 || font_size > 120) {
        font_size = 25;
    }
    int title_size = json_int(title, "size", 25);
    if (title_size < 1 || title_size > 120) {
        title_size = 25;
    }
    int menu_size = json_int(list, "size", title_size);
    if (menu_size < 1 || menu_size > 120) {
        menu_size = 25;
    }
    t->menu_font = font_open(t, list_face, menu_size);
    t->detail_font = font_open(t, list_face, menu_size * 3 / 4 < 15 ? 15 : menu_size * 3 / 4);
    t->description_font = font_open(t, list_face, 18);
    if (t->menu_font) {
        TTF_SetFontStyle(t->menu_font, TTF_STYLE_BOLD);
    }
    if (t->description_font) {
        TTF_SetFontStyle(t->description_font, TTF_STYLE_BOLD);
    }
    t->font = font_open(t, list_face, font_size);
    t->title_font = font_open(t, json_string(title, "font"), title_size);
    t->color = theme_color(title, "color", t->color);
    const char *color = json_string(list, "color");
    unsigned r, g, b;
    if (color && strlen(color) == 7 && sscanf(color, "#%2x%2x%2x", &r, &g, &b) == 3) {
        t->color = (SDL_Color){(Uint8)r, (Uint8)g, (Uint8)b, 0};
    }
    int margin = json_int(gamelist, "iconLeftMargin", -1);
    if (margin >= 0) {
        t->icon_margin = margin > 300 ? 300 : margin;
    }
    /* Only the dedicated game-list font receives gamelist.bold. Sharing this
     * handle with titles/dialogs would leak the patch's style into other screens.
     */
    if (t->font) {
        TTF_SetFontStyle(t->font, cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(gamelist, "bold"))
                                      ? TTF_STYLE_NORMAL
                                      : TTF_STYLE_BOLD);
    }
    const cJSON *grid = cJSON_GetObjectItemCaseSensitive(root, "grid");
    const cJSON *hint = cJSON_GetObjectItemCaseSensitive(root, "hint");
    const cJSON *hide = cJSON_GetObjectItemCaseSensitive(root, "hideLabels");
    t->hide_icons = t->hide_hints =
        !cJSON_IsObject(hide) &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "hideIconTitle"));
    if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(hide, "icons"))) {
        t->hide_icons = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(hide, "icons"));
    }
    if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(hide, "hints"))) {
        t->hide_hints = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(hide, "hints"));
    }
    /* Stock parity: themes hide footer hints, home and console labels or the
     * title with size 0, where stock renders nothing. Only an explicit 0 in
     * that object counts; a hint size inherited from the title does not. */
    t->hide_title_text = zero_size(title, "size");
    t->hide_hint_text = zero_size(hint, "size");
    t->hide_grid_text = zero_size(grid, "grid1x4");
    t->hide_expert_text = zero_size(grid, "grid3x4");
    int grid_size = json_int(grid, "grid1x4", 24), hint_size = json_int(hint, "size", title_size);
    if (grid_size < 1 || grid_size > 120) {
        grid_size = 24;
    }
    if (hint_size < 1 || hint_size > 120) {
        hint_size = 40;
    }
    t->grid_font = font_open(t, json_string(grid, "font"), grid_size);
    int expert_size = json_int(grid, "grid3x4", grid_size);
    t->expert_font = font_open(t, json_string(grid, "font"),
                               expert_size > 0 && expert_size <= 120 ? expert_size : grid_size);
    if (t->expert_font) {
        TTF_SetFontStyle(t->expert_font, TTF_STYLE_BOLD);
    }
    /* Hint text (footer hints, counter, dialog actions) has its own font, as
     * stock's label font: hint.font, or without one the default font
     * (Exo 2 Bold Italic, or the language font), never the title's. Its
     * style is the font file's own. */
    const char *hint_face = json_string(hint, "font");
    t->hint_font = font_open(t, hint_face, hint_size);
    if (t->grid_font) {
        TTF_SetFontStyle(t->grid_font, TTF_STYLE_BOLD);
    }
    t->grid_color[0] = theme_color(grid, "color", (SDL_Color){104, 104, 104, 0});
    t->grid_color[1] = theme_color(grid, "selectedcolor", (SDL_Color){255, 255, 255, 0});
    t->hint_color =
        theme_color(hint, "color", theme_color(title, "color", (SDL_Color){255, 255, 255, 0}));
    t->title_color = theme_color(title, "color", (SDL_Color){255, 255, 255, 0});
    t->page_color =
        theme_color(cJSON_GetObjectItemCaseSensitive(root, "currentpage"), "color", t->hint_color);
    t->total_color =
        theme_color(cJSON_GetObjectItemCaseSensitive(root, "total"), "color", t->hint_color);
    const cJSON *battery = cJSON_GetObjectItemCaseSensitive(root, "batteryPercentage");
    const char *battery_face = json_string(battery, "font");
    if (!battery_face) {
        battery_face = hint_face ? hint_face : json_string(title, "font");
    }
    int battery_size = json_int(battery, "size", 24);
    if (battery_size < 1 || battery_size > 120) {
        battery_size = 24;
    }
    t->battery_font = font_open(t, battery_face, battery_size);
    t->battery_visible = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(battery, "visible"));
    t->battery_fixed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(battery, "fixed"));
    t->battery_color = theme_color(battery, "color", t->hint_color);
    t->battery_offset_x = json_int(battery, "offsetX", 0);
    t->battery_offset_y = json_int(battery, "offsetY", 0);
    const char *align = json_string(battery, "textAlign");
    t->battery_align = align ? (!strcmp(align, "right")    ? 2
                                : !strcmp(align, "center") ? 1
                                                           : 0)
                       : cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(battery, "onleft")) ? 2
                                                                                           : 0;
    t->battery_percent = 100; /* Deterministic value for host previews. */
    cJSON_Delete(root);
    t->tiles[0] = mainui_theme_image(t, "skin/bg-game-item-n.png");
    t->tiles[1] = mainui_theme_image(t, "skin/bg-game-item-f.png");
    /* Expert marks only its selected cell, with its own 214x120 artwork. */
    t->expert_selection = mainui_theme_image(t, "skin/bg-ra-list-item.png");
    t->dots[0] = mainui_theme_image(t, "skin/dot-n.png");
    t->dots[1] = mainui_theme_image(t, "skin/dot-a.png");
    t->buttons[0] = mainui_theme_image(t, "skin/icon-A-54.png");
    t->buttons[1] = mainui_theme_image(t, "skin/icon-B-54.png");
    t->empty = mainui_theme_image(t, "skin/Empty.png");
    /* Theme backgrounds are stored upside down: stock MainUI and Onion
     * (src/common/theme/background.h) rotate them 180 degrees on load. */
    t->background = mainui_theme_image(t, "skin/background.png");
    if (t->background) {
        mainui_rotate_frame(t->background);
    }
    const char *battery_names[] = {"skin/power-0%-icon.png",   "skin/power-20%-icon.png",
                                   "skin/power-50%-icon.png",  "skin/power-80%-icon.png",
                                   "skin/power-full-icon.png", "skin/ic-power-charge-100%.png"};
    for (int i = 0; i < 6; i++) {
        t->battery_icons[i] = mainui_theme_image(t, battery_names[i]);
    }
    t->wifi_connected = mainui_theme_image(t, "skin/icon-wifi-connected.png");
    t->wifi_locked = mainui_theme_image(t, "skin/icon-wifi-locked.png");
    for (int i = 0; i < 4; ++i) {
        char name[64];
        snprintf(name, sizeof name, "skin/icon-wifi-signal-%02d.png", i + 1);
        t->wifi_signal[i] = mainui_theme_image(t, name);
    }
    if (!t->battery_icons[4]) {
        t->battery_icons[4] = mainui_theme_image(t, "skin/power-full-icon_back.png");
    }
    t->logo = mainui_theme_image(t, "skin/miyoo-topbar.png");
    t->titlebar = mainui_theme_image(t, "skin/bg-title.png");
    t->divider = mainui_theme_image(t, "skin/div-line-h.png");
    t->footer = mainui_theme_image(t, "skin/tips-bar-bg.png");
    t->preview_background = mainui_theme_image(t, "skin/preview-bg.png");
    t->favorite = mainui_theme_image(t, "skin/ic-favorite-mark.png");
    t->number_background = mainui_theme_image(t, "skin/num-bg.png");
    t->number_digits = mainui_theme_image(t, "skin/list-num.png");
    t->folder = mainui_theme_image(t, "skin/icon-folder.png");
    t->icon = mainui_theme_image(t, "skin/icon-game.png");
    char numbered[64];
    snprintf(numbered, sizeof numbered, "skin/bg-list-s_%d.png", config->rows);
    /* Numbered selections are active-theme-only, followed by ordinary fallback. */
    t->selection = active_asset(t, numbered);
    if (!t->selection) {
        t->selection = mainui_theme_image(t, "skin/bg-list-s.png");
    }
    if (!t->font || !t->title_font || !t->grid_font || !t->hint_font || !t->menu_font ||
        !t->description_font || !t->detail_font) {
        char error[sizeof t->error];
        memcpy(error, t->error, sizeof error);
        mainui_theme_close(t);
        memcpy(t->error, error, sizeof error);
        return false;
    }
    return true;
}

SDL_Surface *mainui_theme_detail_default(MainUITheme *theme)
{
    if (!theme->detail_default_loaded) {
        theme->detail_default_loaded = true;
        theme->detail_default = mainui_theme_image(theme, "skin/thumb-default.png");
    }
    return theme->detail_default;
}

void mainui_theme_close(MainUITheme *t)
{
    SDL_FreeSurface(t->detail_default);
    for (int i = 0; i < SET_COUNT; i++) {
        SDL_FreeSurface(t->settings_artwork.icons[i]);
    }
    SDL_FreeSurface(t->wifi_connected);
    SDL_FreeSurface(t->wifi_locked);
    for (int i = 0; i < 4; ++i) {
        SDL_FreeSurface(t->wifi_signal[i]);
    }
    SDL_FreeSurface(t->settings_artwork.selection);
    SDL_FreeSurface(t->settings_artwork.left);
    SDL_FreeSurface(t->settings_artwork.right);
    for (int i = 0; i < 6; i++) {
        SDL_FreeSurface(t->popup_backgrounds[i]);
    }
    SDL_FreeSurface(t->popup_selection);
    SDL_FreeSurface(t->loading_background);
    SDL_FreeSurface(t->apps_selection);
    for (int i = 0; i < 4; ++i) {
        SDL_FreeSurface(t->app_icons[i]);
        free(t->app_icon_paths[i]);
    }
    for (int i = 0; i < 32; ++i) {
        SDL_FreeSurface(t->text_cache[i].surface);
    }
    SDL_FreeSurface(t->favorite);
    SDL_FreeSurface(t->number_background);
    SDL_FreeSurface(t->number_digits);
    SDL_FreeSurface(t->divider);
    if (t->detail_font) {
        TTF_CloseFont(t->detail_font);
    }
    if (t->battery_font) {
        TTF_CloseFont(t->battery_font);
    }
    for (int i = 0; i < 6; i++) {
        if (t->battery_icons[i]) {
            SDL_FreeSurface(t->battery_icons[i]);
        }
    }
    if (t->expert_selection) {
        SDL_FreeSurface(t->expert_selection);
    }
    if (t->popup_dim) {
        SDL_FreeSurface(t->popup_dim);
    }
    for (int i = 0; i < 2; i++) {
        if (t->tiles[i]) {
            SDL_FreeSurface(t->tiles[i]);
        }
        if (t->dots[i]) {
            SDL_FreeSurface(t->dots[i]);
        }
        if (t->buttons[i]) {
            SDL_FreeSurface(t->buttons[i]);
        }
    }
    if (t->expert_font) {
        TTF_CloseFont(t->expert_font);
    }
    if (t->grid_font) {
        TTF_CloseFont(t->grid_font);
    }
    if (t->hint_font) {
        TTF_CloseFont(t->hint_font);
    }
    if (t->empty) {
        SDL_FreeSurface(t->empty);
    }
    if (t->background) {
        SDL_FreeSurface(t->background);
    }
    if (t->logo) {
        SDL_FreeSurface(t->logo);
    }
    if (t->titlebar) {
        SDL_FreeSurface(t->titlebar);
    }
    if (t->footer) {
        SDL_FreeSurface(t->footer);
    }
    if (t->selection) {
        SDL_FreeSurface(t->selection);
    }
    if (t->preview_background) {
        SDL_FreeSurface(t->preview_background);
    }
    if (t->folder) {
        SDL_FreeSurface(t->folder);
    }
    if (t->icon) {
        SDL_FreeSurface(t->icon);
    }
    if (t->description_font) {
        TTF_CloseFont(t->description_font);
    }
    if (t->menu_font) {
        TTF_CloseFont(t->menu_font);
    }
    if (t->font) {
        TTF_CloseFont(t->font);
    }
    if (t->title_font) {
        TTF_CloseFont(t->title_font);
    }
    *t = (MainUITheme){0};
}
