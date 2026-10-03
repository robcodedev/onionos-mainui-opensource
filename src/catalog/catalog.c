/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/catalog.h"
#include "cJSON.h"
#include "catalog/cache.h"
#include "catalog/delete.h"
#include "catalog/gamelist.h"
#include "catalog/pinyin.h"
#include "platform/timing.h"
#include "sqlite3.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool search_database(const MainUICatalog *, const char *);

static char *duplicate_text(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static int compare_text(const char *a, const char *b, bool sensitive)
{
    if (sensitive) {
        return strcmp(a, b);
    }
    while (*a && *b) {
        int x = tolower((unsigned char)*a++), y = tolower((unsigned char)*b++);
        if (x != y) {
            return x - y;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

bool mainui_catalog_path(char out[MAINUI_PATH_MAX], const char *sd, const char *base,
                         const char *value)
{
    char raw[MAINUI_PATH_MAX];
    int n;
    if (!strncmp(value, "/mnt/SDCARD/", 12)) {
        n = snprintf(raw, sizeof raw, "%s/%s", sd, value + 12);
    }
    else if (!strcmp(value, "/mnt/SDCARD")) {
        n = snprintf(raw, sizeof raw, "%s", sd);
    }
    else if (*value == '/' || *value == '\\' || (strlen(value) > 1 && value[1] == ':')) {
        n = snprintf(raw, sizeof raw, "%s", value);
    }
    else {
        n = snprintf(raw, sizeof raw, "%s/%s", base, value);
    }
    if (n < 0 || n >= (int)sizeof raw) {
        return false;
    }
    for (char *p = raw; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    /* Reject paths whose meaning depends on process state. */
    if (strlen(raw) > 1 && raw[1] == ':' && raw[2] != '/') {
        return false;
    }
    /* Preserve absolute roots and unresolved leading .. for relative paths. */
    size_t length = 0, count = 0;
    /* Every offset fits within the bounded 4096-byte normalized path. */
    uint16_t starts[MAINUI_PATH_MAX / 2];
    bool absolute = raw[0] == '/' || (strlen(raw) > 2 && raw[1] == ':');
    char *p = raw;
    if (raw[0] == '/') {
        out[length++] = *p++;
    }
    else if (strlen(raw) > 2 && raw[1] == ':') {
        out[length++] = *p++;
        out[length++] = *p++;
        out[length++] = '/';
        if (*p == '/') {
            p++;
        }
    }
    while (*p) {
        while (*p == '/') {
            p++;
        }
        char *part = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t size = (size_t)(p - part);
        if (!size || (size == 1 && part[0] == '.')) {
            continue;
        }
        if (size == 2 && !strncmp(part, "..", 2)) {
            if (count && strcmp(out + starts[count - 1], "..")) {
                length = starts[--count];
                if (length && out[length - 1] == '/' &&
                    length > (absolute ? (raw[1] == ':' ? 3u : 1u) : 0u)) {
                    length--;
                }
                out[length] = 0;
                continue;
            }
            if (absolute) {
                continue;
            }
        }
        if (length && out[length - 1] != '/') {
            out[length++] = '/';
        }
        starts[count++] = length;
        memcpy(out + length, part, size);
        length += size;
        out[length] = 0;
    }
    if (!length) {
        out[length++] = '.';
    }
    out[length] = 0;
    return true;
}

/* Entry fields always use the default C allocator, regardless of their source. */
void mainui_entry_close(MainUIEntry *entry)
{
    free(entry->raw_rompath);
    free(entry->raw_imgpath);
    free(entry->config);
    free(entry->label);
    free(entry->path);
    free(entry->extensions);
    free(entry->images);
    free(entry->stored_path);
    free(entry->stored_image);
    free(entry->cache_key);
    free(entry->artwork);
    free(entry->icon);
    free(entry->icon_selected);
    free(entry->description);
    free(entry->launch);
    *entry = (MainUIEntry){0};
}

static void close_page(MainUICatalogPage *page)
{
    for (int i = 0; i < page->loaded; i++) {
        mainui_entry_close(&page->entries[i]);
    }
    mainui_cache_close(page->cache);
    free(page->entries);
    *page = (MainUICatalogPage){0};
}

void mainui_catalog_close(MainUICatalog *catalog)
{
    mainui_names_close(&catalog->names);
    for (int i = 0; i < MAINUI_STACK_MAX; i++) {
        close_page(&catalog->pages[i]);
    }
}

static bool add(MainUICatalogPage *page, const char *label, const char *path, bool directory,
                const char *extensions, const char *images)
{
    if (page->count >= MAINUI_SCAN_ENTRY_LIMIT) {
        return false;
    }
    MainUIEntry e = {.label = duplicate_text(label),
                     .path = duplicate_text(path),
                     .extensions = duplicate_text(extensions),
                     .images = duplicate_text(images),
                     .directory = directory};
    if (!e.label || !e.path || !e.extensions || !e.images) {
        mainui_entry_close(&e);
        return false;
    }
    if (page->count == page->capacity) {
        int capacity = page->capacity ? page->capacity * 2 : 32;
        if (capacity > MAINUI_SCAN_ENTRY_LIMIT) {
            capacity = MAINUI_SCAN_ENTRY_LIMIT;
        }
        MainUIEntry *entries = realloc(page->entries, (size_t)capacity * sizeof e);
        if (!entries) {
            mainui_entry_close(&e);
            return false;
        }
        page->entries = entries;
        page->capacity = capacity;
    }
    page->entries[page->count++] = e;
    if (directory) {
        page->folder_count++;
    }
    page->loaded = page->count;
    return true;
}

static const char *string(const cJSON *o, const char *key, const char *fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : fallback;
}

static cJSON *read_config(const char *path, bool *read_failed)
{
    errno = 0;
    char *data = mainui_read_text(path, 1024 * 1024);
    /* Only resource exhaustion fails the whole scan, so the previous screen
     * stays. A single unreadable config (EIO, EACCES, EISDIR, ...) just hides
     * its console; at startup there is no previous screen to keep. */
    int error = errno;
    *read_failed = !data && (error == ENOMEM || error == EMFILE || error == ENFILE);
    if (!data && !*read_failed && error != ENOENT) {
        fprintf(stderr, "Skipping console with unreadable %s: %s\n", path, strerror(error));
    }
    errno = 0;
    cJSON *json = data ? cJSON_ParseWithOpts(data, NULL, true) : NULL;
    if (data && !json && errno == ENOMEM) {
        *read_failed = true;
    }
    free(data);
    return json;
}

static bool allowed(const char *name, const char *extensions)
{
    const char *ext = strrchr(name, '.');
    if (!ext || !*extensions) {
        return false;
    }
    ext++;
    while (*extensions) {
        const char *end = strchr(extensions, '|');
        size_t n = end ? (size_t)(end - extensions) : strlen(extensions);
        if (n == strlen(ext)) {
            size_t i = 0;
            while (i < n &&
                   tolower((unsigned char)ext[i]) == tolower((unsigned char)extensions[i])) {
                i++;
            }
            if (i == n) {
                return true;
            }
        }
        if (!end) {
            break;
        }
        extensions = end + 1;
    }
    return false;
}

typedef struct {
    char path[MAINUI_PATH_MAX], config[MAINUI_PATH_MAX], roms[MAINUI_PATH_MAX];
    char images[MAINUI_PATH_MAX], launch[MAINUI_PATH_MAX], resolved[MAINUI_PATH_MAX];
    char label[MAINUI_PATH_MAX];
} ScanScratch;

/* A single entry that cannot be represented is skipped, never the whole scan:
 * one bad console, app or file name must not hide everything else. Only
 * resource failures (allocation, entry limit) still fail the scan. */
static bool skip_entry(const char *where, const char *name, const char *reason)
{
    fprintf(stderr, "Skipping %.200s/%.200s: %s\n", where, name, reason);
    return true;
}

static bool visit(MainUICatalogPage *page, const char *sd, const char *name, bool directory,
                  int mode, ScanScratch *scratch)
{
    if (*name == '.') {
        return true;
    }
    /* Backslash is a path separator in configs; a name containing one would
     * resolve elsewhere. FAT forbids it, so this only happens on host disks. */
    if (strchr(name, '\\')) {
        return skip_entry(page->path, name, "name contains a backslash");
    }
    if (!mainui_catalog_path(scratch->path, sd, page->path, name)) {
        return skip_entry(page->path, name, "path too long");
    }
    if (mode) {
        if (!directory) {
            return true;
        }
        if (!mainui_catalog_path(scratch->config, sd, scratch->path, "config.json")) {
            return skip_entry(page->path, name, "path too long");
        }
        bool read_failed;
        cJSON *json = read_config(scratch->config, &read_failed);
        if (read_failed) {
            return false;
        }
        const char *rompath = string(json, "rompath", "");
        /* Expert also contains standalone launchers without a ROM-list config. */
        bool direct = mode == 2 || (mode == 3 && (!*rompath || !*string(json, "extlist", "")));
        const cJSON *hidden = cJSON_GetObjectItemCaseSensitive(json, "hide");
        if (!cJSON_IsObject(json) || !(direct ? *string(json, "launch", "") : *rompath) ||
            cJSON_IsTrue(hidden) || (cJSON_IsNumber(hidden) && hidden->valueint)) {
            cJSON_Delete(json);
            return true;
        }
        const char *icon = string(json, "icon", "");
        const char *selected = string(json, "iconsel", icon);
        /* Validate every configured path before adding the row. */
        const char *invalid = NULL;
        if (!mainui_catalog_path(scratch->roms, sd, scratch->path,
                                 direct ? string(json, "launch", "") : rompath) ||
            !mainui_catalog_path(scratch->images, sd, scratch->path,
                                 string(json, "imgpath", "Imgs")) ||
            !mainui_catalog_path(scratch->launch, sd, scratch->path,
                                 string(json, "launch", "launch.sh"))) {
            invalid = "unusable rompath, launch or imgpath";
        }
        else if ((*icon && !mainui_catalog_path(scratch->resolved, sd, scratch->path, icon)) ||
                 (*selected &&
                  !mainui_catalog_path(scratch->resolved, sd, scratch->path, selected))) {
            invalid = "unusable icon or iconsel";
        }
        else if (strlen(string(json, "extlist", "")) >= sizeof page->extensions) {
            invalid = "extlist too long";
        }
        if (invalid) {
            cJSON_Delete(json);
            return skip_entry(page->path, name, invalid);
        }
        if (!direct && !mainui_path_within(scratch->roms, sd)) {
            cJSON_Delete(json);
            return true; /* Ignore configs that escape the SD card. */
        }
        bool ok = add(page, string(json, "label", name), scratch->roms, !direct,
                      string(json, "extlist", ""), scratch->images);
        if (ok) {
            MainUIEntry *entry = &page->entries[page->count - 1];
            const cJSON *shortname = cJSON_GetObjectItemCaseSensitive(json, "shortname");
            entry->raw_rompath = duplicate_text(rompath);
            entry->raw_imgpath = duplicate_text(string(json, "imgpath", "Imgs"));
            entry->config = duplicate_text(scratch->config);
            entry->shortname = cJSON_IsNumber(shortname) && shortname->valueint != 0;
            entry->launch = duplicate_text(scratch->launch);
            if (!entry->config || !entry->raw_rompath || !entry->raw_imgpath || !entry->launch) {
                ok = false;
            }
            if (direct) {
                entry->description = duplicate_text(string(json, "description", ""));
                if (!entry->description) {
                    ok = false;
                }
            }
            if (*icon && mainui_catalog_path(scratch->resolved, sd, scratch->path, icon)) {
                entry->icon = duplicate_text(scratch->resolved);
            }
            if (*selected && mainui_catalog_path(scratch->resolved, sd, scratch->path, selected)) {
                entry->icon_selected = duplicate_text(scratch->resolved);
            }
            if ((*icon && !entry->icon) || (*selected && !entry->icon_selected)) {
                ok = false;
            }
        }
        cJSON_Delete(json);
        return ok;
    }
    if (directory &&
        (!compare_text(name, "Imgs", false) || !compare_text(scratch->path, page->images, false))) {
        return true;
    }
    if (!directory && !allowed(name, page->extensions)) {
        return true;
    }
    snprintf(scratch->label, sizeof scratch->label, "%s", name);
    if (!directory) {
        char *dot = strrchr(scratch->label, '.');
        if (dot) {
            *dot = 0;
        }
    }
    return add(page, scratch->label, scratch->path, directory, page->extensions, page->images);
}

static int order(const void *a, const void *b, bool sensitive)
{
    const MainUIEntry *x = a, *y = b;
    if (x->directory != y->directory) {
        return x->directory ? -1 : 1;
    }
    int n = compare_text(x->label, y->label, sensitive);
    return n ? n : strcmp(x->path, y->path);
}

static int order_nocase(const void *a, const void *b)
{
    return order(a, b, false);
}

static int order_case(const void *a, const void *b)
{
    return order(a, b, true);
}

/* Why a scan stopped. Only filesystem enumeration failures and the entry limit
 * are classified; allocation, path and cancel failures stay SCAN_OTHER. */
typedef enum {
    SCAN_OTHER,
    SCAN_OPEN,
    SCAN_READ,
    SCAN_STAT,
    SCAN_LIMIT,
    SCAN_LINK
} ScanFailure;

typedef struct {
    ScanFailure kind;
    int error;
    char name[256];
} ScanResult;

static bool scan_directory(MainUICatalogPage *page, const char *sd, int mode, bool sensitive,
                           MainUICancel cancel, ScanResult *result)
{
    *result = (ScanResult){SCAN_OTHER, 0, ""};
    ScanScratch *scratch = malloc(sizeof *scratch);
    if (!scratch) {
        free(scratch);
        return false;
    }
    bool ok = true;
    /* The scanned folder itself must not be a link either (Emu, App, RApp;
     * ROM roots are checked separately with their ancestors). */
    struct stat self;
    if (lstat(page->path, &self) == 0 && S_ISLNK(self.st_mode)) {
        *result = (ScanResult){SCAN_LINK, 0, ""};
        free(scratch);
        return false;
    }
    DIR *dir = opendir(page->path);
    if (!dir) {
        *result = (ScanResult){SCAN_OPEN, errno, ""};
        free(scratch);
        return false;
    }
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            ok = errno == 0;
            if (!ok) {
                *result = (ScanResult){SCAN_READ, errno, ""};
            }
            break;
        }
        if (mainui_cancelled(cancel)) {
            ok = false;
            break;
        }
        bool directory;
        if (entry->d_type == DT_DIR || entry->d_type == DT_REG) {
            directory = entry->d_type == DT_DIR;
        }
        else if (entry->d_type != DT_UNKNOWN) {
            /* Symlinks, FIFOs and devices are never listed. Following a link
             * could leave the ROM tree; FAT has none of these anyway. */
            continue;
        }
        else {
            /* Classify without following links. */
            char path[MAINUI_PATH_MAX];
            struct stat info;
            if (!mainui_catalog_path(path, sd, page->path, entry->d_name)) {
                ok = false;
                break;
            }
            if (lstat(path, &info)) {
                if (errno == ENOENT) {
                    continue; /* Removed while scanning. */
                }
                *result = (ScanResult){SCAN_STAT, errno, ""};
                snprintf(result->name, sizeof result->name, "%s", entry->d_name);
                ok = false;
                break;
            }
            if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) {
                continue;
            }
            directory = S_ISDIR(info.st_mode);
        }
        if (!visit(page, sd, entry->d_name, directory, mode, scratch)) {
            if (page->count >= MAINUI_SCAN_ENTRY_LIMIT) {
                result->kind = SCAN_LIMIT;
            }
            ok = false;
            break;
        }
    }
    closedir(dir);
    if (ok && page->count > 1) {
        qsort(page->entries, (size_t)page->count, sizeof *page->entries,
              sensitive ? order_case : order_nocase);
    }
    free(scratch);
    return ok;
}

static bool scan_result(MainUICatalogPage *page, const char *sd, int mode, bool sensitive,
                        MainUICancel cancel, ScanResult *result)
{
    struct timespec start = mainui_timing_start();
    bool ok = scan_directory(page, sd, mode, sensitive, cancel, result);
    mainui_timing_finish("scan-ms", start);
    return ok;
}

static bool scan(MainUICatalogPage *page, const char *sd, int mode, bool sensitive,
                 MainUICancel cancel)
{
    ScanResult result;
    return scan_result(page, sd, mode, sensitive, cancel, &result);
}

bool mainui_catalog_open(MainUICatalog *catalog, const char *sd, bool sensitive)
{
    catalog->case_sensitive = sensitive;
    if (!mainui_catalog_path(catalog->sd, sd, sd, ".")) {
        return false;
    }
    MainUICatalogPage *page = &catalog->pages[0];
    strcpy(page->title, "Systems");
    if (!mainui_catalog_path(page->path, sd, sd, "Emu")) {
        return false;
    }
    struct stat info;
    /* lstat: a dangling symlink is not "absent"; the scan rejects links. */
    if (lstat(page->path, &info) && errno == ENOENT) {
        return true;
    }
    ScanResult result;
    if (!scan_result(page, catalog->sd, true, sensitive, catalog->cancel, &result)) {
        switch (result.kind) {
        case SCAN_OPEN:
        case SCAN_READ:
            snprintf(catalog->error, sizeof catalog->error, "Cannot %s systems folder Emu: %s",
                     result.kind == SCAN_OPEN ? "open" : "read", strerror(result.error));
            break;
        case SCAN_STAT:
            snprintf(catalog->error, sizeof catalog->error, "Cannot stat Emu/%.100s: %s",
                     result.name, strerror(result.error));
            break;
        case SCAN_LIMIT:
            snprintf(catalog->error, sizeof catalog->error,
                     "Systems folder Emu has more than %d entries", MAINUI_SCAN_ENTRY_LIMIT);
            break;
        case SCAN_LINK:
            snprintf(catalog->error, sizeof catalog->error,
                     "Systems folder Emu is a symlink, which is not followed");
            break;
        default:
            snprintf(catalog->error, sizeof catalog->error, "Cannot read systems (Emu)");
            break;
        }
        if (result.kind != SCAN_OTHER) {
            /* Unusable Emu: leave an empty Systems page so startup can continue;
             * opening Games again reads it again. */
            catalog->unreadable = true;
            char title[sizeof page->title], path[sizeof page->path];
            memcpy(title, page->title, sizeof title);
            memcpy(path, page->path, sizeof path);
            close_page(page);
            memcpy(page->title, title, sizeof title);
            memcpy(page->path, path, sizeof path);
        }
        return false;
    }
    return true;
}

static bool optional_catalog(MainUICatalog *catalog, const char *sd, bool sensitive,
                             const char *directory, const char *title, int mode)
{
    catalog->case_sensitive = sensitive;
    if (!mainui_catalog_path(catalog->sd, sd, sd, ".")) {
        return false;
    }
    MainUICatalogPage *page = &catalog->pages[0];
    snprintf(page->title, sizeof page->title, "%s", title);
    if (!mainui_catalog_path(page->path, sd, sd, directory)) {
        return false;
    }
    struct stat info;
    /* lstat: a dangling symlink is not "absent"; the scan rejects links. */
    if (lstat(page->path, &info) && errno == ENOENT) {
        return true;
    }
    ScanResult result;
    if (!scan_result(page, catalog->sd, mode, sensitive, catalog->cancel, &result)) {
        snprintf(catalog->error, sizeof catalog->error,
                 result.kind == SCAN_LINK ? "%s is a symlink, which is not followed"
                                          : "Cannot read %s directory",
                 directory);
        return false;
    }
    return true;
}

bool mainui_catalog_apps(MainUICatalog *catalog, const char *sd, bool sensitive)
{
    return optional_catalog(catalog, sd, sensitive, "App", "Apps", 2);
}

bool mainui_catalog_expert(MainUICatalog *catalog, const char *sd, bool sensitive)
{
    return optional_catalog(catalog, sd, sensitive, "RApp", "Expert", 3);
}

MainUIEntry *mainui_catalog_entry(MainUICatalog *catalog, int index)
{
    MainUICatalogPage *page = &catalog->pages[catalog->depth];
    if (index < 0 || index >= page->count) {
        return NULL;
    }
    if (page->cache && (index < page->offset || index >= page->offset + page->loaded)) {
        MainUIEntry rows[MAINUI_CACHE_WINDOW];
        int loaded = 0, offset = (index / MAINUI_CACHE_WINDOW) * MAINUI_CACHE_WINDOW;
        if (!mainui_cache_window(page->cache, offset, rows, &loaded)) {
            strcpy(catalog->error,
                   "Cannot read cache page; return to Systems and reopen to refresh");
            return NULL;
        }
        MainUIEntry *entries = malloc(sizeof rows);
        if (!entries) {
            for (int i = 0; i < loaded; i++) {
                mainui_entry_close(&rows[i]);
            }
            strcpy(catalog->error, "Cannot allocate cache window");
            return NULL;
        }
        /* Publish only a completely decoded replacement. Until this point the
         * previous window remains usable on I/O or allocation failure.
         */
        memcpy(entries, rows, sizeof rows);
        for (int i = 0; i < page->loaded; i++) {
            mainui_entry_close(&page->entries[i]);
        }
        free(page->entries);
        page->entries = entries;
        page->loaded = loaded;
        page->offset = offset;
    }
    return &page->entries[index - page->offset];
}

static bool path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static const char *portable_path(char out[MAINUI_PATH_MAX], const char *sd, const char *host);

/* Build a persistent prefix without normalizing the config's spelling. */
static bool saved_prefix(char out[MAINUI_PATH_MAX], const MainUICatalog *catalog,
                         const MainUIEntry *system, const char *raw)
{
    if (!raw || !system->config) {
        return false;
    }
    char directory[MAINUI_PATH_MAX], portable[MAINUI_PATH_MAX];
    snprintf(directory, sizeof directory, "%s", system->config);
    char *slash = strrchr(directory, '/');
    if (!slash) {
        return false;
    }
    *slash = 0;
    const char *base = portable_path(portable, catalog->sd, directory);
    if (!*base && *raw != '/') {
        return false;
    }
    int n = snprintf(out, MAINUI_PATH_MAX, "%s%s%s", *raw == '/' ? "" : base,
                     *raw == '/' ? "" : "/", raw);
    return n >= 0 && n < MAINUI_PATH_MAX;
}

/* Cache rows carry stock saved identities. Recursion is bounded and each folder
 * releases its decoded entries before returning. ROM files are never opened. */
static bool cache_scan(sqlite3_stmt *insert, MainUICatalog *catalog, const char *root,
                       const char *path, const char *extensions, const char *images, int depth,
                       int *count, bool shortname, const char *rom_prefix, const char *image_prefix,
                       char scratch[4][MAINUI_PATH_MAX])
{
    if (depth >= MAINUI_STACK_MAX || mainui_cancelled(catalog->cancel)) {
        return false;
    }
    MainUICatalogPage *page = calloc(1, sizeof *page);
    if (!page) {
        return false;
    }
    snprintf(page->path, sizeof page->path, "%s", path);
    snprintf(page->extensions, sizeof page->extensions, "%s", extensions);
    snprintf(page->images, sizeof page->images, "%s", images);
    bool ok = scan(page, catalog->sd, 0, catalog->case_sensitive, catalog->cancel);
    mainui_count_add("scan-entries", page->count);
    char *parent = scratch[0], *relative = scratch[1], *pinyin = scratch[2], *image = scratch[3];
    for (int i = 0; ok && i < page->count; i++) {
        if (mainui_cancelled(catalog->cancel)) {
            ok = false;
            break;
        }
        MainUIEntry *entry = &page->entries[i];
        /* Nested ppath follows the verified one-level stock convention;
         * deeper folder layouts have not been verified against stock. */
        snprintf(parent, MAINUI_PATH_MAX, "%s",
                 !strcmp(path, root) ? "." : path + strlen(root) + 1);
        int length = snprintf(relative, MAINUI_PATH_MAX, "%s/%s", rom_prefix,
                              entry->path + strlen(root) + 1);
        if (length < 0 || length >= MAINUI_PATH_MAX || ++*count > 1000000) {
            ok = false;
            break;
        }
        const char *label =
            shortname && !entry->directory
                ? mainui_name_lookup(&catalog->names, catalog->sd, entry->label, entry->label)
                : entry->label;
        if (strlen(label) >= MAINUI_PATH_MAX) {
            ok = false;
            break;
        }
        const char *file = strrchr(entry->path, '/') + 1;
        const char *dot = strrchr(file, '.');
        int image_length = 0;
        if (!entry->directory) {
            image_length = snprintf(image, MAINUI_PATH_MAX, "%s/%.*s.png", image_prefix,
                                    (int)(dot ? (size_t)(dot - file) : strlen(file)), file);
        }
        if (image_length < 0 || image_length >= MAINUI_PATH_MAX) {
            ok = false;
            break;
        }
        pinyin[0] = 0;
        if (!entry->directory) {
            mainui_pinyin(catalog->sd, label, pinyin, MAINUI_PATH_MAX);
        }
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        ok = sqlite3_bind_text(insert, 1, label, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_text(insert, 2, relative, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_text(insert, 3, entry->directory ? relative : image, -1,
                               SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_int(insert, 4, entry->directory ? 1 : 0) == SQLITE_OK &&
             sqlite3_bind_text(insert, 5, parent, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_text(insert, 6, pinyin, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_text(insert, 7, pinyin, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_step(insert) == SQLITE_DONE;
        if (ok && entry->directory) {
            ok = cache_scan(insert, catalog, root, entry->path, extensions, images, depth + 1,
                            count, shortname, rom_prefix, image_prefix, scratch);
        }
    }
    close_page(page);
    free(page);
    return ok;
}

static int cancel_sql(void *context)
{
    return mainui_cancelled(((MainUICatalog *)context)->cancel);
}

/* Caller holds the cache writer lock: no live builder can own these files.
 * Match this cache's unique build prefix, including SQLite sidecars. */
static bool cleanup_cache_builds(const char *root, const char *file, MainUICancel cancel)
{
    const char *name = strrchr(file, '/');
    name = name ? name + 1 : file;
    char prefix[MAINUI_PATH_MAX];
    int length = snprintf(prefix, sizeof prefix, "%s.building.", name);
    if (length < 0 || length >= (int)sizeof prefix) {
        return false;
    }
    DIR *dir = opendir(root);
    if (!dir) {
        return false;
    }
    bool ok = true, removed = false;
    for (;;) {
        if (mainui_cancelled(cancel)) {
            ok = false;
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            ok = errno == 0;
            break;
        }
        if (strncmp(entry->d_name, prefix, (size_t)length)) {
            continue;
        }
        char stale[MAINUI_PATH_MAX];
        int size = snprintf(stale, sizeof stale, "%s/%s", root, entry->d_name);
        /* unlink never follows symlinks or removes directories. */
        if (size < 0 || size >= (int)sizeof stale || unlink(stale)) {
            ok = false;
            break;
        }
        removed = true;
    }
    if (closedir(dir)) {
        ok = false;
    }
    if (removed && !mainui_sync_parent(file)) {
        /* The stale files are gone either way; only the flush is in doubt. */
        fprintf(stderr, "Removed stale build files for %s, but flushing the folder failed: %s\n",
                file, strerror(errno));
    }
    return ok;
}

static bool rom_root_ready(MainUICatalog *catalog, const char *root)
{
    errno = 0;
    if (mainui_directory_within(root, catalog->sd)) {
        return true;
    }
    snprintf(catalog->error, sizeof catalog->error, "%s",
             errno == ENOENT ? "ROM folder is missing"
                             : "ROM folder must be a real directory within the SD card");
    return false;
}

static bool build_cache_locked(MainUICatalog *catalog, int system, bool replace, bool abandon)
{
    if (mainui_cancelled(catalog->cancel) || system < 0 || system >= catalog->pages[0].count) {
        return false;
    }
    MainUIEntry *entry = &catalog->pages[0].entries[system];
    if (!rom_root_ready(catalog, entry->path)) {
        return false;
    }
    const char *name = strrchr(entry->path, '/');
    name = name ? name + 1 : entry->path;
    char file[MAINUI_PATH_MAX], temporary[MAINUI_PATH_MAX];
    int length = snprintf(file, sizeof file, "%s/%s_cache6.db", entry->path, name);
    if (length < 0 || length >= (int)sizeof file) {
        return false;
    }
    char table[512];
    length = snprintf(table, sizeof table, "%s_roms", name);
    if (length < 0 || length >= (int)sizeof table) {
        return false;
    }
    /* A refused recovery never blocks Refresh roms (abandon): drop only the
     * journal and rebuild from the files as they are. Both ROM copies stay.
     * Automatic builds and repairs never run while a journal exists. */
    bool recovered = mainui_delete_recover(file, table, entry->path, catalog->sd) &&
                     !mainui_delete_journal_present(file);
    if (!recovered && (!abandon || !mainui_delete_abandon(file, catalog->sd))) {
        snprintf(catalog->error, sizeof catalog->error, "Pending ROM deletion requires recovery");
        return false;
    }
    if (!cleanup_cache_builds(entry->path, file, catalog->cancel)) {
        snprintf(catalog->error, sizeof catalog->error, "Cannot clean interrupted cache builds");
        return false;
    }
    if (!replace && path_exists(file)) {
        return true;
    }
    MainUIFileStamp previous = mainui_file_stamp(file);
    if (!mainui_temporary_path(temporary, file, "building")) {
        snprintf(catalog->error, sizeof catalog->error,
                 "Cache build already in progress or temporary path unavailable");
        return false;
    }
    /* Reserve our temporary path exclusively. Never reuse a stale build file. */
    FILE *reservation = fopen(temporary, "wx");
    if (!reservation) {
        return false;
    }
    fclose(reservation);
    sqlite3 *database = NULL;
    sqlite3_stmt *insert = NULL;
    bool ok = sqlite3_open_v2(temporary, &database, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_progress_handler(database, 1000, cancel_sql, catalog);
        /* This private build is discarded on failure; it needs no rollback journal.
         * The 8 MiB page-cache target applies only to this build connection. */
        ok = sqlite3_exec(database,
                          "PRAGMA journal_mode=OFF; PRAGMA synchronous=FULL; "
                          "PRAGMA cache_size=-8192; BEGIN IMMEDIATE",
                          NULL, NULL, NULL) == SQLITE_OK;
    }
    char *sql =
        sqlite3_mprintf("CREATE TABLE \"%w_roms\" (id INTEGER PRIMARY KEY AUTOINCREMENT,disp TEXT "
                        "NOT NULL,path TEXT NOT NULL,imgpath TEXT NOT NULL,type INTEGER DEFAULT "
                        "0,ppath TEXT NOT NULL,pinyin TEXT NOT NULL,cpinyin TEXT NOT NULL)",
                        name);
    if (ok) {
        ok = sql && sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK;
    }
    sqlite3_free(sql);
    sql = sqlite3_mprintf("INSERT INTO \"%w_roms\" (disp,path,imgpath,type,ppath,pinyin,cpinyin) "
                          "VALUES (?1,?2,?3,?4,?5,?6,?7)",
                          name);
    if (ok) {
        ok = sql && sqlite3_prepare_v2(database, sql, -1, &insert, NULL) == SQLITE_OK;
    }
    sqlite3_free(sql);
    int count = 0;
    bool imported = false, import_failed = false;
    char rom_prefix[MAINUI_PATH_MAX], image_prefix[MAINUI_PATH_MAX];
    ok = ok && saved_prefix(rom_prefix, catalog, entry, entry->raw_rompath) &&
         saved_prefix(image_prefix, catalog, entry, entry->raw_imgpath);
    if (ok) {
        ok = mainui_gamelist_import_control(database, insert, catalog->sd, entry->path, rom_prefix,
                                            &imported, catalog->cancel);
        import_failed = !ok;
    }
    if (ok && !imported) {
        char(*scratch)[MAINUI_PATH_MAX] = malloc(4 * sizeof *scratch);
        ok = scratch &&
             cache_scan(insert, catalog, entry->path, entry->path, entry->extensions, entry->images,
                        0, &count, entry->shortname, rom_prefix, image_prefix, scratch);
        free(scratch);
    }
    sqlite3_finalize(insert);
#ifdef MAINUI_TEST_FAULTS
    if (ok) {
        mainui_test_fault("cache-populated");
    }
#endif
    sql = sqlite3_mprintf("CREATE INDEX mainui_rom_browse ON \"%w_roms\" (ppath,type DESC,disp%s)",
                          name, catalog->case_sensitive ? "" : " COLLATE NOCASE");
    if (ok) {
        ok = sql && sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK;
    }
    sqlite3_free(sql);
    ok = ok && !mainui_cancelled(catalog->cancel);
    if (ok) {
        ok = sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    }

    if (database && sqlite3_close(database) != SQLITE_OK) {
        ok = false;
    }
    if (ok) {
        /* Explicitly flush the complete database before publishing it, without
         * relying on SQLite's journal-OFF commit sync behavior. */
        int descriptor = open(temporary, O_RDWR | O_CLOEXEC);
        ok = descriptor >= 0 && fsync(descriptor) == 0;
        if (descriptor >= 0 && close(descriptor) != 0) {
            ok = false;
        }
    }
#ifdef MAINUI_TEST_FAULTS
    if (ok) {
        mainui_test_fault("cache-prepared");
    }
#endif
    ok = ok && !mainui_cancelled(catalog->cancel) &&
         mainui_file_stamp_equal(previous, mainui_file_stamp(file));
    if (ok) {
        ok = rename(temporary, file) == 0;
        /* Installed from here on: a failed folder flush must not report the
         * previous database as retained. */
        if (ok && !mainui_sync_parent(file)) {
            fprintf(stderr, "Published %s, but flushing its folder failed: %s\n", file,
                    strerror(errno));
        }
    }
    if (!ok) {
        mainui_remove_file(temporary);
    }
    if (!ok) {
        snprintf(catalog->error, sizeof catalog->error, "%s",
                 import_failed
                     ? "Cannot import miyoogamelist.xml; check for empty, invalid or unreadable XML"
                     : "ROM cache build failed; previous database retained");
    }
    return ok;
}

static bool build_cache(MainUICatalog *catalog, int system, bool replace, bool abandon)
{
    if (!catalog || system < 0 || system >= catalog->pages[0].count) {
        return false;
    }
    const char *path = catalog->pages[0].entries[system].path;
    if (!rom_root_ready(catalog, path)) {
        return false;
    }
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char file[4096];
    int n = snprintf(file, sizeof file, "%s/%s_cache6.db", path, name);
    if (n < 0 || n >= (int)sizeof file) {
        return false;
    }
    if (search_database(catalog, path)) {
        bool exists = mainui_file_stamp(file).exists;
        if (!exists) {
            snprintf(catalog->error, sizeof catalog->error,
                     "Run Search to create its results database.");
        }
        return exists;
    }
    MainUIFileLock *lock = mainui_file_lock(file);
    if (!lock) {
        snprintf(catalog->error, sizeof catalog->error, "Another catalog writer is active");
        return false;
    }
    struct timespec start = mainui_timing_start();
    bool ok = build_cache_locked(catalog, system, replace, abandon);
    mainui_timing_finish("cache-build-ms", start);
    mainui_file_unlock(lock);
    return ok;
}

bool mainui_catalog_build_cache(MainUICatalog *catalog, int system, bool replace)
{
    return build_cache(catalog, system, replace, replace);
}

bool mainui_catalog_repair_cache(MainUICatalog *catalog, int system)
{
    return build_cache(catalog, system, true, false);
}

bool mainui_catalog_deletion_pending(const MainUICatalog *catalog)
{
    const MainUICatalogPage *page = &catalog->pages[catalog->depth ? 1 : 0];
    return catalog->depth && *page->cache_file && mainui_delete_journal_present(page->cache_file);
}

bool mainui_catalog_remove_cache(MainUICatalog *catalog, int system)
{
    if (!catalog || system < 0 || system >= catalog->pages[0].count) {
        return false;
    }
    if (!catalog->pages[0].entries[system].directory) {
        return true; /* A standalone Expert launcher has no ROM cache. */
    }
    const char *root = catalog->pages[0].entries[system].path;
    if (!rom_root_ready(catalog, root)) {
        return false;
    }
    if (search_database(catalog, root)) {
        return true; /* Search owns these results. */
    }
    const char *name = strrchr(root, '/');
    name = name ? name + 1 : root;
    char file[4096], recovery[4096];
    int n = snprintf(file, sizeof file, "%s/%s_cache6.db", root, name);
    int r = snprintf(recovery, sizeof recovery, "%s.delete.json", file);
    if (n <= 0 || n >= (int)sizeof file || r <= 0 || r >= (int)sizeof recovery) {
        return false;
    }
    if (!mainui_file_stamp(file).exists && !mainui_delete_journal_present(file)) {
        return true;
    }
    MainUIFileLock *lock = mainui_file_lock(file);
    bool ok = lock != NULL;
    if (ok && mainui_delete_journal_present(file)) {
        /* Refresh roms: recover if possible, otherwise drop only the journal
         * and leave both ROM files, as the in-list refresh does. */
        char table[512];
        int length = snprintf(table, sizeof table, "%s_roms", name);
        ok = length > 0 && length < (int)sizeof table &&
             ((mainui_delete_recover(file, table, root, catalog->sd) &&
               !mainui_delete_journal_present(file)) ||
              mainui_delete_abandon(file, catalog->sd));
    }
    /* These are SQLite's derived sidecars, not ROM metadata or media files. */
    const char *suffixes[] = {"-wal", "-shm", "-journal", ""};
    for (int i = 0; ok && i < 4; ++i) {
        char target[4096];
        n = snprintf(target, sizeof target, "%s%s", file, suffixes[i]);
        ok = n > 0 && n < (int)sizeof target;
        bool removed = false;
        if (ok && mainui_remove_file_status(target, &removed) != 0) {
            if (removed) {
                /* Gone either way; only the folder flush is in doubt. */
                fprintf(stderr, "Removed %s, but flushing its folder failed: %s\n", target,
                        strerror(errno));
            }
            else if (errno != ENOENT) {
                ok = false;
            }
        }
    }
    mainui_file_unlock(lock);
    if (!ok) {
        snprintf(catalog->error, sizeof catalog->error, "Could not remove ROM cache: %.190s", file);
    }
    return ok;
}

/* Unsupported WAL, permissions and busy readers are not evidence of corruption. */
static bool cache_needs_repair(const char *file, const char *table)
{
    /* OMIT_WAL reports a WAL database as NOTADB too. Check its format bytes
     * before interpreting that error as damage; never replace unsupported WAL. */
    FILE *input = fopen(file, "rb");
    if (!input) {
        return false;
    }
    unsigned char header[20];
    size_t size = fread(header, 1, sizeof header, input);
    bool failed = ferror(input) != 0;
    fclose(input);
    if (failed || (size == sizeof header && !memcmp(header, "SQLite format 3", 16) &&
                   (header[18] == 2 || header[19] == 2))) {
        return false;
    }
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_open_v2(file, &db, SQLITE_OPEN_READONLY, NULL);
    bool missing = false;
    if (result == SQLITE_OK) {
        result = sqlite3_prepare_v2(
            db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1, &statement, NULL);
        if (result == SQLITE_OK) {
            result = sqlite3_bind_text(statement, 1, table, -1, SQLITE_TRANSIENT);
            if (result == SQLITE_OK) {
                result = sqlite3_step(statement);
                missing = result == SQLITE_DONE;
            }
        }
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return missing || (result & 255) == SQLITE_CORRUPT || (result & 255) == SQLITE_NOTADB;
}

bool mainui_search_root(const char *sd, const char *root)
{
    char path[MAINUI_PATH_MAX];
    return root && mainui_catalog_path(path, sd, sd, "App/Search/data") && !strcmp(path, root);
}

static bool search_database(const MainUICatalog *catalog, const char *root)
{
    return mainui_search_root(catalog->sd, root);
}

bool mainui_catalog_search_system(const MainUICatalog *catalog, int index)
{
    return index >= 0 && index < catalog->pages[0].count &&
           mainui_search_root(catalog->sd, catalog->pages[0].entries[index].path);
}

/* Console roots browsed by scanning for the rest of the session. Changed on
 * the UI thread only while no catalog worker runs; workers started later read
 * it. Kept until exit, so it stays reachable. */
static char **scan_roots;
static int scan_root_count;

static bool scanned_only(const char *root)
{
    for (int i = 0; i < scan_root_count; i++) {
        if (!strcmp(scan_roots[i], root)) {
            return true;
        }
    }
    return false;
}

bool mainui_catalog_scan_only(const char *root)
{
    if (scanned_only(root)) {
        return true;
    }
    char **grown = realloc(scan_roots, (size_t)(scan_root_count + 1) * sizeof *grown);
    if (!grown) {
        return false;
    }
    scan_roots = grown;
    if (!(scan_roots[scan_root_count] = strdup(root))) {
        return false;
    }
    scan_root_count++;
    return true;
}

bool mainui_catalog_search_results(const MainUICatalog *catalog)
{
    return catalog->depth > 0 && search_database(catalog, catalog->pages[1].path);
}

bool mainui_catalog_page_damaged(const MainUICatalog *catalog)
{
    return mainui_cache_damaged(catalog->pages[catalog->depth].cache);
}

bool mainui_catalog_enter(MainUICatalog *catalog, int index)
{
    catalog->error[0] = 0;
    MainUICatalogPage *page = &catalog->pages[catalog->depth];
    MainUIEntry *e = mainui_catalog_entry(catalog, index);
    if (!e || !e->directory) {
        return false;
    }
    if (!rom_root_ready(catalog, catalog->depth ? catalog->pages[1].path : e->path)) {
        return false;
    }
    if (catalog->depth + 1 >= MAINUI_STACK_MAX) {
        strcpy(catalog->error, "Folder depth limit reached");
        return false;
    }
    MainUICatalogPage *next = &catalog->pages[catalog->depth + 1];
    snprintf(next->title, sizeof next->title, "%s", e->label);
    snprintf(next->path, sizeof next->path, "%s", e->path);
    snprintf(next->extensions, sizeof next->extensions, "%s",
             catalog->depth ? page->extensions : e->extensions);
    snprintf(next->images, sizeof next->images, "%s", catalog->depth ? page->images : e->images);
    /* Onion src/randomGamePicker/randomGamePicker.c derives both names from
     * the ROM root basename, which can differ from the emulator label.
     */
    if (!catalog->depth) {
        const char *name = strrchr(next->path, '/');
        name = name ? name + 1 : next->path;
        char filename[MAINUI_PATH_MAX];
        int n = snprintf(filename, sizeof filename, "%s_cache6.db", name);
        int t = snprintf(next->cache_table, sizeof next->cache_table, "%s_roms", name);
        if (n < 0 || n >= (int)sizeof filename || t < 0 || t >= (int)sizeof next->cache_table ||
            !mainui_catalog_path(next->cache_file, catalog->sd, next->path, filename)) {
            close_page(next);
            strcpy(catalog->error, "Cache path exceeds supported length");
            return false;
        }
    }
    else {
        strcpy(next->cache_file, page->cache_file);
        strcpy(next->cache_table, page->cache_table);
    }
    char journal[4096];
    int journal_length = snprintf(journal, sizeof journal, "%s.delete.json", next->cache_file);
    bool pending_delete = journal_length > 0 && journal_length < (int)sizeof journal &&
                          mainui_delete_journal_present(next->cache_file);
    if (pending_delete && !catalog->depth) {
        MainUIFileLock *lock = mainui_file_lock(next->cache_file);
        /* Busy or refused recovery never blocks browsing. Keep the journal and
         * retry on the next console entry; mutations still require successful recovery. */
        if (lock) {
            mainui_delete_recover(next->cache_file, next->cache_table, next->path, catalog->sd);
            mainui_file_unlock(lock);
            pending_delete = mainui_delete_journal_present(next->cache_file);
        }
    }
    /* Its cache could not be read even after a rebuild: scan the folder. A
     * scanned page's children are scanned too, so this covers the console. */
    bool forced_scan = !catalog->depth && scanned_only(next->path);
    if (forced_scan) {
        next->cache_fallback = true;
    }
    if (!forced_scan && !pending_delete && !catalog->depth && !path_exists(next->cache_file) &&
        !search_database(catalog, next->path)) {
        if (!mainui_catalog_build_cache(catalog, index, false)) {
            char xml[4096];
            struct stat info;
            int n = snprintf(xml, sizeof xml, "%s/miyoogamelist.xml", next->path);
            int j = snprintf(journal, sizeof journal, "%s.delete.json", next->cache_file);
            /* A read-only/full card can still be browsed. Do not bypass an
             * authoritative gamelist, pending recovery, or cancellation. */
            bool fallback = !mainui_cancelled(catalog->cancel) && n > 0 && n < (int)sizeof xml &&
                            j > 0 && j < (int)sizeof journal && stat(next->path, &info) == 0 &&
                            S_ISDIR(info.st_mode) && lstat(xml, &info) != 0 && errno == ENOENT &&
                            lstat(journal, &info) != 0 && errno == ENOENT;
            if (!fallback) {
                close_page(next);
                return false;
            }
            fprintf(stderr, "%s; scanning %s without a cache\n", catalog->error, next->path);
            next->cache_fallback = true;
        }
    }
    const char *root = catalog->depth ? catalog->pages[1].path : next->path;
    /* A scanned child has no verified database key; retain scan semantics. */
    if (!forced_scan && (!catalog->depth || e->cache_key) && path_exists(next->cache_file)) {
        bool rebuilt = false;
    retry_cache:
        if (mainui_cache_open(&next->cache, next->cache_file, next->cache_table,
                              catalog->depth ? e->cache_key : ".", catalog->sd, root,
                              catalog->case_sensitive, &next->count)) {
            next->folder_count = mainui_cache_folder_count(next->cache);
            next->entries = calloc(MAINUI_CACHE_WINDOW, sizeof *next->entries);
            if (next->entries &&
                mainui_cache_window(next->cache, 0, next->entries, &next->loaded)) {
                mainui_cache_changed(next->cache);
                mainui_count("cache", 1);
                mainui_count_add("cache-hits", 1);
                mainui_count("roms", next->count - next->folder_count);
                catalog->depth++;
                return true;
            }
            free(next->entries);
            next->entries = NULL;
            mainui_cache_close(next->cache);
            next->cache = NULL;
        }
        else if (!pending_delete && !catalog->depth && !rebuilt &&
                 !search_database(catalog, next->path) && !mainui_cancelled(catalog->cancel) &&
                 cache_needs_repair(next->cache_file, next->cache_table)) {
            rebuilt = true;
            if (mainui_catalog_build_cache(catalog, index, true)) {
                goto retry_cache;
            }
        }
        if (search_database(catalog, root)) {
            close_page(next);
            strcpy(catalog->error,
                   "Cannot read Search database; results preserved. Run Search again.");
            return false;
        }
        next->count = next->loaded = next->folder_count = 0;
        next->cache_fallback = true;
        fprintf(stderr, "Cache unavailable or invalid; scanning %s\n", next->path);
    }
    if (!scan(next, catalog->sd, false, catalog->case_sensitive, catalog->cancel)) {
        close_page(next);
        snprintf(catalog->error, sizeof catalog->error,
                 "Cannot read ROM folder, or more than %d entries", MAINUI_SCAN_ENTRY_LIMIT);
        return false;
    }
    mainui_count("cache", 0);
    mainui_count_add("cache-scans", 1);
    mainui_count_add("scan-entries", next->count);
    mainui_count("roms", next->count - next->folder_count);
    catalog->error[0] = 0;
    catalog->depth++;
    return true;
}

bool mainui_catalog_back(MainUICatalog *catalog)
{
    if (!catalog->depth) {
        return false;
    }
    close_page(&catalog->pages[catalog->depth--]);
    catalog->error[0] = 0;
    return true;
}

static const char *portable_path(char out[MAINUI_PATH_MAX], const char *sd, const char *host)
{
    if (!host) {
        return "";
    }
    size_t length = strlen(sd);
    if (!strncmp(host, sd, length) && host[length] == '/') {
        int n = snprintf(out, MAINUI_PATH_MAX, "/mnt/SDCARD%s", host + length);
        return n > 0 && n < MAINUI_PATH_MAX ? out : "";
    }
    return host;
}

cJSON *mainui_catalog_record(MainUICatalog *catalog, int index)
{
    MainUIEntry *entry = mainui_catalog_entry(catalog, index);
    if (!entry || entry->directory) {
        return NULL;
    }
    bool app = catalog->depth == 0;
    const char *launch = entry->launch;
    if (!app && !launch) {
        int system = catalog->pages[0].view.selected;
        if (mainui_cancelled(catalog->cancel) || system < 0 || system >= catalog->pages[0].count) {
            return NULL;
        }
        launch = catalog->pages[0].entries[system].launch;
    }
    char path[MAINUI_PATH_MAX], launcher[MAINUI_PATH_MAX], image[MAINUI_PATH_MAX];
    const char *rom = portable_path(path, catalog->sd, entry->path);
    const char *art = portable_path(image, catalog->sd, entry->artwork);
    if (!app && !entry->launch && entry->stored_path &&
        !search_database(catalog, catalog->pages[1].path)) {
        /* The row is the identity, including XML ./ and missing image strings. */
        rom = entry->stored_path;
        art = entry->stored_image;
    }
    else if (!app && !entry->launch && !search_database(catalog, catalog->pages[1].path)) {
        int system = catalog->pages[0].view.selected;
        if (system < 0 || system >= catalog->pages[0].count) {
            return NULL;
        }
        const MainUIEntry *console = &catalog->pages[0].entries[system];
        size_t root = strlen(console->path);
        if (!console->config || !console->raw_rompath || !console->raw_imgpath ||
            strncmp(entry->path, console->path, root) || entry->path[root] != '/') {
            return NULL;
        }
        char base[MAINUI_PATH_MAX], portable[MAINUI_PATH_MAX];
        snprintf(base, sizeof base, "%s", console->config);
        char *slash = strrchr(base, '/');
        if (!slash) {
            return NULL;
        }
        *slash = 0;
        const char *dir = portable_path(portable, catalog->sd, base);
        const char *file = strrchr(entry->path, '/') + 1;
        const char *dot = strrchr(file, '.');
        /* Write the stock spelling 1:1: Favorites, Recents and GameSwitcher
         * store it as is. Never normalize it here or depend on artwork lookup;
         * comparisons go through mainui_rom_key() instead. */
        /* Absolute config paths already contain their prefix. Preserve them
         * verbatim too; prepending the console directory would break launch. */
        bool absolute_rom = console->raw_rompath[0] == '/';
        bool absolute_image = console->raw_imgpath[0] == '/';
        int r = snprintf(path, sizeof path, "%s%s%s/%s", absolute_rom ? "" : dir,
                         absolute_rom ? "" : "/", console->raw_rompath, entry->path + root + 1);
        int i = snprintf(image, sizeof image, "%s%s%s/%.*s.png", absolute_image ? "" : dir,
                         absolute_image ? "" : "/", console->raw_imgpath,
                         (int)(dot ? (size_t)(dot - file) : strlen(file)), file);
        if (r < 0 || r >= (int)sizeof path || i < 0 || i >= (int)sizeof image) {
            return NULL;
        }
        rom = path;
        art = image;
    }
    cJSON *record = cJSON_CreateObject();
    if (!record) {
        return NULL;
    }
    if (!cJSON_AddStringToObject(record, "label", entry->label) ||
        !cJSON_AddStringToObject(record, "rompath", rom) ||
        !cJSON_AddStringToObject(record, "launch", portable_path(launcher, catalog->sd, launch)) ||
        !cJSON_AddStringToObject(record, "imgpath", art) ||
        !cJSON_AddNumberToObject(record, "type", app ? 3 : 5)) {
        cJSON_Delete(record);
        return NULL;
    }
    return record;
}
