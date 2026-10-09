/* SPDX-License-Identifier: GPL-3.0-only */
#include "support.h"
#include "ui/panels.h"
#include "ui/theme.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int mkdir_0755(const char *path)
{
    return mkdir(path, 0755);
}

static void picture(const char *directory, const char *name, int width, int height, Uint8 red)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    SDL_Surface *surface =
        SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32, 0xff0000, 0xff00, 0xff, 0);
    assert(surface);
    SDL_FillRect(surface, NULL, SDL_MapRGB(surface->format, red, 0, 0));
    /* SDL_image detects the signature, so synthetic BMP pixels need no PNG writer. */
    assert(SDL_SaveBMP(surface, path) == 0);
    SDL_FreeSurface(surface);
}

static void initialize(MainUITheme *theme, const char *active, const char *fallback)
{
    strcpy(theme->directory, active);
    strcpy(theme->fallback, fallback);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fixture-artwork <onion-builtin-theme-dir>\n");
        return 2;
    }
    assert(SDL_Init(0) == 0);
    char root[256], active[512], fallback[512], path[1024];
    snprintf(root, sizeof root, "build/artwork-check-%ld", (long)getpid());
    assert(mkdir_0755(root) == 0);
    snprintf(active, sizeof active, "%s/active", root);
    snprintf(fallback, sizeof fallback, "%s/fallback", root);
    assert(mkdir_0755(active) == 0 && mkdir_0755(fallback) == 0);
    snprintf(path, sizeof path, "%s/skin", active);
    assert(mkdir_0755(path) == 0);
    snprintf(path, sizeof path, "%s/skin", fallback);
    assert(mkdir_0755(path) == 0);
    picture(fallback, "skin/bg-list-s.png", 320, 60, 12);
    picture(fallback, "skin/bg-pop-menu-6.png", 333, 360, 15);
    picture(active, "skin/bg-pop-menu-2.png", 111, 125, 22);
    picture(active, "skin/icon-brightness-48.png", 48, 48, 30);
    MainUITheme theme = {0};
    initialize(&theme, active, fallback);
    const MainUISettingsArtwork *settings = mainui_theme_settings_artwork(&theme);
    SDL_Surface *icon = settings->icons[SET_BRIGHTNESS];
    assert(icon && icon->w == 48);
    SDL_Surface *selection = settings->selection;
    assert(selection && selection->w == 320 && !settings->left);
    picture(active, "skin/icon-brightness-48.png", 50, 50, 30);
    assert(mainui_theme_settings_artwork(&theme)->icons[SET_BRIGHTNESS] == icon);
    /* New files are invisible within a theme lifetime, including cached misses. */
    picture(active, "skin/bg-list-s.png", 222, 60, 40);
    picture(active, "skin/icon-left-arrow-24.png", 24, 24, 40);
    assert(mainui_theme_settings_artwork(&theme)->selection == selection);
    assert(!mainui_theme_settings_artwork(&theme)->left);
    SDL_Surface *popup = mainui_theme_popup_background(&theme, 6);
    assert(popup && popup->w == 111 && popup->h == 365);
    picture(active, "skin/bg-pop-menu-6.png", 444, 360, 50);
    assert(mainui_theme_popup_background(&theme, 6) == popup);
    assert(!mainui_theme_popup_background(&theme, 0));
    assert(!mainui_theme_popup_background(&theme, 7));
    selection = mainui_theme_popup_selection(&theme);
    assert(selection && selection->w == 222);
    picture(active, "skin/bg-list-popup-s.png", 123, 60, 60);
    assert(mainui_theme_popup_selection(&theme) == selection);
    mainui_theme_close(&theme);
    initialize(&theme, active, fallback);
    assert(mainui_theme_settings_artwork(&theme)->left);
    assert(mainui_theme_settings_artwork(&theme)->icons[SET_BRIGHTNESS]->w == 50);
    assert(mainui_theme_settings_artwork(&theme)->selection->w == 222);
    assert(mainui_theme_popup_background(&theme, 6)->w == 444);
    assert(mainui_theme_popup_selection(&theme)->w == 123);
    mainui_theme_close(&theme);
    /* Corrupt exact artwork must retain the smaller-active-before-built-in rule. */
    snprintf(path, sizeof path, "%s/skin/bg-pop-menu-6.png", active);
    FILE *file = fopen(path, "wb");
    assert(file && fputs("broken", file) >= 0 && fclose(file) == 0);
    initialize(&theme, active, fallback);
    assert(mainui_theme_popup_background(&theme, 6)->w == 111);
    mainui_theme_close(&theme);
    /* A full-screen background with fewer rows is not expanded for more
     * rows: its top band would repeat and push the picture down. */
    picture(active, "skin/bg-pop-menu-4.png", 640, 480, 90);
    initialize(&theme, active, fallback);
    for (int rows = 5; rows <= 6; rows++) {
        popup = mainui_theme_popup_background(&theme, rows);
        assert(popup && popup->w == 640 && popup->h == 480);
    }
    mainui_theme_close(&theme);
    /* Profile skin images override the theme's, and are cached per open. */
    const char *parts[] = {"Saves", "Saves/CurrentProfile", "Saves/CurrentProfile/theme",
                           "Saves/CurrentProfile/theme/skin"};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        snprintf(path, sizeof path, "%s/%s", root, parts[i]);
        assert(mkdir_0755(path) == 0);
    }
    char profile[1024];
    snprintf(profile, sizeof profile, "%s/Saves/CurrentProfile/theme", root);
    picture(profile, "skin/bg-list-s.png", 201, 60, 70);
    picture(profile, "skin/bg-pop-menu-6.png", 202, 360, 70);
    initialize(&theme, active, fallback);
    strcpy(theme.sd, root);
    strcpy(theme.profile, profile);
    assert(mainui_theme_settings_artwork(&theme)->selection->w == 201);
    assert(mainui_theme_popup_background(&theme, 6)->w == 202);
    picture(profile, "skin/bg-list-s.png", 203, 60, 80);
    assert(mainui_theme_settings_artwork(&theme)->selection->w == 201);
    mainui_theme_close(&theme);
    initialize(&theme, active, fallback);
    strcpy(theme.sd, root);
    strcpy(theme.profile, profile);
    assert(mainui_theme_settings_artwork(&theme)->selection->w == 203);
    mainui_theme_close(&theme);
    TEST_PATH(path, "%s/skin/bg-list-s.png", profile);
    file = fopen(path, "wb");
    assert(file && fputs("corrupt override", file) >= 0 && fclose(file) == 0);
    initialize(&theme, active, fallback);
    strcpy(theme.profile, profile);
    assert(!mainui_theme_settings_artwork(&theme)->selection);
    assert(remove(path) == 0);
    assert(!mainui_theme_settings_artwork(&theme)->selection);
    mainui_theme_close(&theme);
    initialize(&theme, active, fallback);
    strcpy(theme.profile, profile);
    assert(mainui_theme_settings_artwork(&theme)->selection->w == 222);
    mainui_theme_close(&theme);
    assert(TTF_Init() == 0);
    const char *builtin = argv[1];
    MainUIConfig config;
    mainui_config_parse(&config, NULL, NULL, NULL, NULL);
    TEST_PATH(path, "%s/config.json", profile);
    file = fopen(path, "wb");
    assert(file && fputs("{\"list\":{\"size\":99}}", file) >= 0 && fclose(file) == 0);
    assert(mainui_theme_open_sd(&theme, active, builtin, root, &config));
    {
        /* Without hint.font, hints use the default font (Exo 2 Bold Italic,
         * italic by its own face), not the title's; with one, that font.
         * Console labels are made bold only in the default font: a theme's
         * grid.font keeps its own weight (stock would make it bold). */
        char fonts[1024], title_font[4096], name[2][4096], fonts_config[4096];
        snprintf(fonts, sizeof fonts, "%s/fonts", root);
        assert(mkdir_0755(fonts) == 0);
        TEST_PATH(title_font, "%s/BPreplayBold.otf", builtin);
        const char *configs[] = {"{\"title\":{\"font\":\"%s\"}}",
                                 "{\"title\":{\"font\":\"%s\"},\"hint\":{\"font\":\"%s\"},"
                                 "\"grid\":{\"font\":\"%s\"}}"};
        for (int i = 0; i < 2; i++) {
            TEST_PATH(fonts_config, "%s/config.json", fonts);
            file = fopen(fonts_config, "wb");
            assert(file && fprintf(file, configs[i], title_font, title_font, title_font) > 0 &&
                   fclose(file) == 0);
            MainUITheme fonted = {0};
            assert(mainui_theme_open_sd(&fonted, fonts, builtin, root, &config));
            snprintf(name[0], sizeof name[0], "%s", TTF_FontFaceFamilyName(fonted.hint_font));
            snprintf(name[1], sizeof name[1], "%s", TTF_FontFaceFamilyName(fonted.title_font));
            assert(i ? !strcmp(name[0], name[1]) : strcmp(name[0], name[1]) != 0);
            assert(i || strstr(name[0], "Exo"));
            int label_style = i ? TTF_STYLE_NORMAL : TTF_STYLE_BOLD;
            assert(TTF_GetFontStyle(fonted.grid_font) == label_style);
            /* Expert labels: the list font (here the title's, as no list.font
             * is set), never bold, whatever grid.font is. */
            assert(TTF_GetFontStyle(fonted.expert_font) == TTF_STYLE_NORMAL);
            assert(!strcmp(TTF_FontFaceFamilyName(fonted.expert_font),
                           TTF_FontFaceFamilyName(fonted.menu_font)));
            mainui_theme_close(&fonted);
        }
    }
    int font_height = TTF_FontHeight(theme.font);
    /* Labels beyond the old 511-byte cache key render in full when they fit,
     * and distinct tails cannot reuse the wrong cached surface. */
    char long_text[4096], prefix[512];
    memset(long_text, 'i', 600);
    long_text[600] = 0;
    memset(prefix, 'i', 511);
    prefix[511] = 0;
    SDL_Color white = {255, 255, 255, 0};
    SDL_Surface *text_image = mainui_theme_text(&theme, theme.description_font, long_text, white);
    int text_width, text_height;
    assert(TTF_SizeUTF8(theme.description_font, long_text, &text_width, &text_height) == 0);
    assert(text_image && text_image->w == text_width && text_image->h == text_height);
    assert(mainui_theme_text(&theme, theme.description_font, long_text, white) == text_image);
    SDL_Surface *short_image = mainui_theme_text(&theme, theme.description_font, prefix, white);
    assert(short_image && short_image != text_image && short_image->w < text_image->w);
    memcpy(long_text + 510, "\xe2\x82\xac", 3);
    long_text[513] = 0;
    text_image = mainui_theme_text(&theme, theme.description_font, long_text, white);
    assert(TTF_SizeUTF8(theme.description_font, long_text, &text_width, &text_height) == 0);
    assert(text_image && text_image->w == text_width && text_image->h == text_height);
    /* Oversized labels retain a visible, bounded prefix instead of a blank row. */
    memset(long_text, 'W', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = 0;
    text_image = mainui_theme_text(&theme, theme.font, long_text, white);
    assert(text_image && text_image->w > 0 && text_image->w <= 8192);
    assert((size_t)text_image->pitch * text_image->h <= 1024u * 1024u);
    assert(mainui_theme_text(&theme, theme.font, long_text, white) == text_image);
    for (int i = 0; i < 1365; ++i) {
        memcpy(long_text + i * 3, "\xe2\x82\xac", 3);
    }
    long_text[4095] = 0;
    text_image = mainui_theme_text(&theme, theme.font, long_text, white);
    assert(text_image && (size_t)text_image->pitch * text_image->h <= 1024u * 1024u);
    char resolved[4096], expected[4096];
    assert(mainui_theme_font_path(&theme, "font/custom.ttf", resolved));
    snprintf(expected, sizeof expected, "%s/font/custom.ttf", active);
    assert(!strcmp(resolved, expected));
    assert(mainui_theme_font_path(&theme, "/mnt/SDCARD/Fonts/custom.ttf", resolved));
    snprintf(expected, sizeof expected, "%s/Fonts/custom.ttf", root);
    assert(!strcmp(resolved, expected));
    assert(mainui_theme_font_path(&theme, "/customer/app/Exo-2-Bold-Italic.ttf", resolved));
    snprintf(expected, sizeof expected, "%s/Exo-2-Bold-Italic.ttf", builtin);
    assert(!strcmp(resolved, expected));
    assert(mainui_theme_font_path(&theme, "/mnt/SDCARD/miyoo/app/Exo-2-Bold-Italic.ttf", resolved));
    assert(!strcmp(resolved, expected));
    assert(mainui_theme_font_path(&theme, "C:/Fonts/custom.ttf", resolved));
    assert(!strcmp(resolved, "C:/Fonts/custom.ttf"));
    mainui_theme_close(&theme);
    assert(remove(path) == 0);
    assert(mainui_theme_open_sd(&theme, active, builtin, root, &config));
    /* The profile's config.json (Tweaks' theme overrides) applied list.size 99. */
    assert(TTF_FontHeight(theme.font) < font_height);
    mainui_theme_close(&theme);
    snprintf(path, sizeof path, "%s/config.json", active);
    file = fopen(path, "wb");
    assert(file && fputs("{\"list\":{\"font\":\"/mnt/SDCARD/Fonts/missing.ttf\"}}", file) >= 0 &&
           fclose(file) == 0);
    assert(mainui_theme_open_sd(&theme, active, builtin, root, &config));
    assert(theme.font && TTF_FontFaceFamilyName(theme.font));
    mainui_theme_close(&theme);
    /* Tweaks' overrides apply field by field, as Onion's theme_applyConfig():
     * the battery size changes, its alignment and colour stay the theme's, and
     * a hideLabels override of icons keeps hints from hideIconTitle. */
    file = fopen(path, "wb");
    assert(file &&
           fputs("{\"hideIconTitle\":true,\"batteryPercentage\":{\"visible\":true,"
                 "\"size\":20,\"textAlign\":\"right\",\"color\":\"#102030\"}}",
                 file) >= 0 &&
           fclose(file) == 0);
    assert(mainui_theme_open_sd(&theme, active, builtin, root, &config));
    int theme_battery = TTF_FontHeight(theme.battery_font);
    assert(theme.hide_icons && theme.hide_hints);
    mainui_theme_close(&theme);
    char overrides[1024];
    TEST_PATH(overrides, "%s/config.json", profile);
    file = fopen(overrides, "wb");
    assert(file &&
           fputs("{\"batteryPercentage\":{\"size\":40},\"hideLabels\":{\"icons\":false}}", file) >=
               0 &&
           fclose(file) == 0);
    assert(mainui_theme_open_sd(&theme, active, builtin, root, &config));
    assert(TTF_FontHeight(theme.battery_font) > theme_battery);
    assert(theme.battery_visible && theme.battery_align == 2 && theme.battery_color.r == 0x10);
    assert(!theme.hide_icons && theme.hide_hints);
    mainui_theme_close(&theme);
    /* An override file that is not a JSON object is ignored. */
    file = fopen(overrides, "wb");
    assert(file && fputs("not json", file) >= 0 && fclose(file) == 0);
    assert(mainui_theme_open_sd(&theme, active, builtin, root, &config));
    assert(TTF_FontHeight(theme.battery_font) == theme_battery && theme.hide_icons);
    mainui_theme_close(&theme);
    assert(remove(overrides) == 0);
    /* The Apps list keeps the decoded icons of its visible rows: a redraw
     * reuses them, and an icon is freed once its row is no longer shown. */
    assert(mainui_theme_open_sd(&theme, active, builtin, root, &config));
    char app_icons[3][1024];
    MainUIEntry apps_entries[3];
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof name, "app-%d.png", i);
        picture(active, name, 60, 60, (Uint8)(10 + i));
        TEST_PATH(app_icons[i], "%s/%s", active, name);
        apps_entries[i] = (MainUIEntry){.label = "App", .icon = app_icons[i]};
    }
    MainUICatalog *apps = calloc(1, sizeof *apps);
    assert(apps);
    apps->pages[0].entries = apps_entries;
    apps->pages[0].count = apps->pages[0].loaded = 3;
    SDL_Surface *screen =
        SDL_CreateRGBSurface(SDL_SWSURFACE, 640, 480, 32, 0xff0000, 0xff00, 0xff, 0);
    assert(screen);
    MainUIViewport apps_view = {.total = 3, .selected = 0, .start = 0, .end = 1};
    mainui_draw_apps(screen, &theme, apps, &apps_view);
    SDL_Surface *kept = NULL;
    for (int i = 0; i < 4; i++) {
        if (theme.app_icon_paths[i] && !strcmp(theme.app_icon_paths[i], app_icons[0])) {
            kept = theme.app_icons[i];
        }
    }
    assert(kept && kept->w == 60);
    kept->refcount++;
    mainui_draw_apps(screen, &theme, apps, &apps_view);
    int holders = 0;
    for (int i = 0; i < 4; i++) {
        holders += theme.app_icons[i] == kept;
    }
    assert(holders == 1 && kept->refcount == 2);
    apps_view = (MainUIViewport){.total = 3, .selected = 2, .start = 1, .end = 2};
    mainui_draw_apps(screen, &theme, apps, &apps_view);
    assert(kept->refcount == 1);
    SDL_FreeSurface(kept);
    int shown = 0;
    for (int i = 0; i < 4; i++) {
        assert(!theme.app_icon_paths[i] || strcmp(theme.app_icon_paths[i], app_icons[0]));
        shown += theme.app_icons[i] != NULL;
    }
    assert(shown == 2);
    mainui_theme_close(&theme);
    SDL_FreeSurface(screen);
    free(apps);
    TTF_Quit();
    SDL_Quit();
    puts("Artwork reuse, cached misses, theme invalidation, popup fallback and theme overrides "
         "passed");
    return 0;
}
