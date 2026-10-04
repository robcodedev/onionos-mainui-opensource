/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/gamelist.h"
#include "catalog/catalog.h"
#include "catalog/pinyin.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define XML_LIMIT (16u * 1024u * 1024u)
#define XML_DEPTH 32

typedef struct {
    uint32_t hash, offset, length;
} MetadataRecord;

#define IMPORT_FOLDER_BUCKETS 256
#define IMPORT_INDEX_BYTES (32u * 1024u * 1024u)

typedef struct ImportFile {
    struct ImportFile *next;
    unsigned char type;
    char name[];
} ImportFile;

typedef struct ImportFolder {
    struct ImportFolder *next;
    ImportFile **files;
    size_t buckets, count;
    bool fallback;
    char path[];
} ImportFolder;

typedef struct {
    sqlite3 *database;
    sqlite3_stmt *insert, *folder;
    const char *sd, *root, *saved_root;
    char values[6][MAINUI_PATH_MAX];
    size_t lengths[6];
    bool seen[6];
    /* A value cut at its buffer's end: fine for text, never for a path. */
    bool overflow[6];
    MainUIMetadata *metadata;
    MetadataRecord *index;
    uint32_t record_start, record_end;
    const char *filename;
    unsigned records;
    /* A dry pass only checks that the list is usable; nothing is inserted. */
    bool dry, closed;
    MainUICancel cancel;
    ImportFolder *folders[IMPORT_FOLDER_BUCKETS];
    size_t index_bytes;
} Import;

static bool xml_character(unsigned value)
{
    return value == 9 || value == 10 || value == 13 || (value >= 32 && value <= 0xd7ff) ||
           (value >= 0xe000 && value <= 0xfffd) || (value >= 0x10000 && value <= 0x10ffff);
}

/* Windows-1252 code points for 0x80-0x9f; 0 where it defines none. Text that
 * is not UTF-8 is read as this superset of Latin-1, the usual encoding of a
 * hand-edited or scraped gamelist, so typographic quotes come out right. */
static const unsigned short cp1252[32] = {
    0x20ac, 0,      0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017d, 0,      0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0,      0x017e, 0x0178};

/* Length of the valid UTF-8 sequence at text, or 0: overlong forms,
 * surrogates and characters XML forbids are not valid here. */
static size_t utf8_sequence(const unsigned char *text, size_t size, unsigned *out)
{
    unsigned value = text[0], extra, minimum;
    if (value >= 0xc2 && value <= 0xdf) {
        extra = 1;
        minimum = 0x80;
        value &= 0x1f;
    }
    else if (value >= 0xe0 && value <= 0xef) {
        extra = 2;
        minimum = 0x800;
        value &= 0x0f;
    }
    else if (value >= 0xf0 && value <= 0xf4) {
        extra = 3;
        minimum = 0x10000;
        value &= 7;
    }
    else {
        return 0;
    }
    if (extra >= size) {
        return 0;
    }
    for (size_t i = 1; i <= extra; i++) {
        if ((text[i] & 0xc0) != 0x80) {
            return 0;
        }
        value = (value << 6) | (text[i] & 0x3f);
    }
    if (value < minimum || !xml_character(value)) {
        return 0;
    }
    *out = value;
    return extra + 1;
}

/* Append one character as UTF-8. Characters XML forbids (control codes) are
 * dropped. A value too long for its field is cut at a character boundary,
 * always leaving room for the longest character so the cut is final, and
 * *overflow records the cut. */
static void put_character(unsigned value, char *out, size_t *length, bool *overflow)
{
    if (!out || !xml_character(value)) {
        return;
    }
    if (*length + 4 >= MAINUI_PATH_MAX) {
        *overflow = true;
        return;
    }
    size_t count = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
    char *bytes = out + *length;
    unsigned remaining = value;
    for (size_t j = count - 1; j; j--) {
        bytes[j] = (char)(0x80 | (remaining & 63));
        remaining >>= 6;
    }
    bytes[0] = (char)(count == 1   ? remaining
                      : count == 2 ? 0xc0 | remaining
                      : count == 3 ? 0xe0 | remaining
                                   : 0xf0 | remaining);
    *length += count;
    out[*length] = 0;
}

/* The character a reference names, or 0 when the text at text[0] == '&' is
 * not a reference: a bare & stays text, as stock reads it. Only predefined
 * entities and numeric references are decoded; DTDs and external entities
 * are never read, so XML cannot trigger additional file reads. */
static size_t reference(const char *text, size_t size, unsigned *out)
{
    size_t end = 1;
    while (end < size && end <= 17 && text[end] != ';') {
        end++;
    }
    if (end >= size || text[end] != ';') {
        return 0;
    }
    const char *entity = text + 1;
    size_t n = end - 1;
    unsigned value = 0;
    if (n && entity[0] == '#') {
        size_t digit = 1;
        unsigned base = 10;
        if (digit < n && entity[digit] == 'x') {
            base = 16;
            digit++;
        }
        if (digit == n) {
            return 0;
        }
        for (; digit < n; digit++) {
            unsigned char c = (unsigned char)entity[digit];
            unsigned d = c >= '0' && c <= '9'   ? c - '0' + 0u
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10u
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10u
                                                : 99u;
            if (d >= base || value > (0x10ffff - d) / base) {
                return 0;
            }
            value = value * base + d;
        }
        if (!xml_character(value)) {
            return 0;
        }
    }
    else if (n == 3 && !memcmp(entity, "amp", n)) {
        value = '&';
    }
    else if (n == 2 && !memcmp(entity, "lt", n)) {
        value = '<';
    }
    else if (n == 2 && !memcmp(entity, "gt", n)) {
        value = '>';
    }
    else if (n == 4 && !memcmp(entity, "quot", n)) {
        value = '"';
    }
    else if (n == 4 && !memcmp(entity, "apos", n)) {
        value = '\'';
    }
    else {
        return 0;
    }
    *out = value;
    return end + 1;
}

/* Decode text leniently, as stock reads gamelists: nothing here fails. An
 * unknown or unfinished reference stays as written, and a byte that does not
 * start valid UTF-8 is read as Windows-1252. */
static void append_text(const char *text, size_t size, bool entities, char *out, size_t *length,
                        bool *overflow)
{
    if (!out) {
        return;
    }
    for (size_t i = 0; i < size;) {
        unsigned char c = (unsigned char)text[i];
        unsigned value = c;
        size_t used = 1;
        if (entities && c == '&') {
            size_t n = reference(text + i, size - i, &value);
            used = n ? n : 1;
        }
        else if (c >= 0x80) {
            size_t n = utf8_sequence((const unsigned char *)text + i, size - i, &value);
            if (n) {
                used = n;
            }
            else if (c < 0xa0 && cp1252[c - 0x80]) {
                value = cp1252[c - 0x80];
            }
        }
        put_character(value, out, length, overflow);
        i += used;
    }
}

static bool insert_row(Import *import, const char *label, const char *path, const char *image,
                       int type, const char *parent)
{
    char pinyin[MAINUI_PATH_MAX];
    pinyin[0] = 0;
    if (!type) {
        mainui_pinyin(import->sd, label, pinyin, sizeof pinyin);
    }
    sqlite3_stmt *statement = import->insert;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return sqlite3_bind_text(statement, 1, label, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
           sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
           sqlite3_bind_text(statement, 3, image, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
           sqlite3_bind_int(statement, 4, type) == SQLITE_OK &&
           sqlite3_bind_text(statement, 5, parent, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
           sqlite3_bind_text(statement, 6, pinyin, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
           sqlite3_bind_text(statement, 7, pinyin, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
           sqlite3_step(statement) == SQLITE_DONE;
}

static bool regular_file(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

/* Exact filename keys preserve case-sensitive host semantics. */
static uint32_t file_hash(const char *name)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        hash = (hash ^ *p) * 16777619u;
    }
    return hash;
}

static void clear_folder_index(Import *import, ImportFolder *folder)
{
    for (size_t i = 0; i < folder->buckets; ++i) {
        ImportFile *entry = folder->files[i];
        while (entry) {
            ImportFile *next = entry->next;
            import->index_bytes -= sizeof *entry + strlen(entry->name) + 1;
            free(entry);
            entry = next;
        }
    }
    import->index_bytes -= folder->buckets * sizeof *folder->files;
    free(folder->files);
    folder->files = NULL;
    folder->buckets = folder->count = 0;
}

static void close_folder_indexes(Import *import)
{
    for (size_t i = 0; i < IMPORT_FOLDER_BUCKETS; ++i) {
        ImportFolder *folder = import->folders[i];
        while (folder) {
            ImportFolder *next = folder->next;
            clear_folder_index(import, folder);
            free(folder);
            folder = next;
        }
    }
}

static bool index_file(Import *import, ImportFolder *folder, const struct dirent *entry)
{
    if (!folder->buckets || folder->count >= folder->buckets * 2) {
        size_t count = folder->buckets ? folder->buckets * 2 : 64;
        size_t bytes = count * sizeof *folder->files;
        if (bytes > IMPORT_INDEX_BYTES - import->index_bytes) {
            return false;
        }
        ImportFile **files = calloc(count, sizeof *files);
        if (!files) {
            return false;
        }
        for (size_t i = 0; i < folder->buckets; ++i) {
            ImportFile *item = folder->files[i];
            while (item) {
                ImportFile *next = item->next;
                size_t bucket = file_hash(item->name) % count;
                item->next = files[bucket];
                files[bucket] = item;
                item = next;
            }
        }
        import->index_bytes += bytes - folder->buckets * sizeof *files;
        free(folder->files);
        folder->files = files;
        folder->buckets = count;
    }
    size_t bytes = sizeof(ImportFile) + strlen(entry->d_name) + 1;
    if (bytes > IMPORT_INDEX_BYTES - import->index_bytes) {
        return false;
    }
    ImportFile *item = malloc(bytes);
    if (!item) {
        return false;
    }
    strcpy(item->name, entry->d_name);
    item->type = entry->d_type;
    size_t bucket = file_hash(item->name) % folder->buckets;
    item->next = folder->files[bucket];
    folder->files[bucket] = item;
    folder->count++;
    import->index_bytes += bytes;
    return true;
}

/* Keep one directory snapshot per import. On enumeration/allocation failure,
 * use the old stat path rather than silently dropping valid XML records. */
static bool indexed_regular_file(Import *import, const char *path)
{
    char parent[MAINUI_PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        return regular_file(path);
    }
    size_t length = (size_t)(slash - path);
    memcpy(parent, path, length);
    parent[length] = 0;
    size_t bucket = file_hash(parent) % IMPORT_FOLDER_BUCKETS;
    ImportFolder *folder = import->folders[bucket];
    while (folder && strcmp(folder->path, parent)) {
        folder = folder->next;
    }
    if (!folder) {
        size_t bytes = sizeof *folder + length + 1;
        if (bytes > IMPORT_INDEX_BYTES - import->index_bytes) {
            return regular_file(path);
        }
        folder = calloc(1, bytes);
        if (!folder) {
            return regular_file(path);
        }
        strcpy(folder->path, parent);
        folder->next = import->folders[bucket];
        import->folders[bucket] = folder;
        import->index_bytes += bytes;
        DIR *dir = opendir(parent);
        folder->fallback = !dir;
        if (dir) {
            for (;;) {
                if (mainui_cancelled(import->cancel)) {
                    folder->fallback = true;
                    break;
                }
                errno = 0;
                struct dirent *entry = readdir(dir);
                if (!entry) {
                    folder->fallback = errno != 0;
                    break;
                }
                if (!index_file(import, folder, entry)) {
                    folder->fallback = true;
                    break;
                }
            }
            if (closedir(dir)) {
                folder->fallback = true;
            }
        }
        if (folder->fallback) {
            clear_folder_index(import, folder);
        }
    }
    if (mainui_cancelled(import->cancel)) {
        return false;
    }
    if (folder->fallback) {
        return regular_file(path);
    }
    const char *name = slash + 1;
    ImportFile *entry = folder->buckets ? folder->files[file_hash(name) % folder->buckets] : NULL;
    for (; entry; entry = entry->next) {
        if (!strcmp(entry->name, name)) {
            if (entry->type == DT_UNKNOWN || entry->type == DT_LNK) {
                return regular_file(path);
            }
            return entry->type == DT_REG;
        }
    }
    /* A case-insensitive filesystem can resolve a differently spelled name.
     * Preserve that behavior on misses; exact regular-file hits avoid stat. */
    return regular_file(path);
}

/* Compare the key relative to this XML directory, accepting the reference
 * ./ or / prefix and ASCII case folding. Root fallback keys include subfolders. */
static uint32_t metadata_hash(const char *path)
{
    if (!strncmp(path, "./", 2)) {
        path += 2;
    }
    else if (*path == '/') {
        path++;
    }
    uint32_t hash = 2166136261u;
    while (*path) {
        hash = (hash ^ (unsigned char)tolower((unsigned char)*path++)) * 16777619u;
    }
    return hash;
}

static bool metadata_game(Import *import)
{
    if (import->index) {
        import->index[import->records - 1] =
            (MetadataRecord){metadata_hash(import->values[0]), import->record_start,
                             import->record_end - import->record_start};
        return true;
    }
    const char *path = import->values[0], *filename = import->filename;
    if (!strncmp(path, "./", 2)) {
        path += 2;
    }
    else if (*path == '/') {
        path++;
    }
    while (*path && *filename &&
           tolower((unsigned char)*path) == tolower((unsigned char)*filename)) {
        path++;
        filename++;
    }
    if (*path || *filename || import->metadata->found) {
        return true;
    }
    MainUIMetadata *result = import->metadata;
    result->found = true;
    strcpy(result->genre, import->values[3]);
    strcpy(result->description, import->values[5]);
    const char *rating = import->values[4];
    while (isspace((unsigned char)*rating)) {
        rating++;
    }
    if (*rating >= '0' && *rating <= '9') {
        unsigned whole = 0, fraction = 0, divisor = 1, digits = 0;
        /* Preserve the reference numeric-prefix rule without integer overflow. */
        while (*rating >= '0' && *rating <= '9') {
            whole = whole || *rating != '0';
            rating++;
        }
        if (*rating == '.') {
            rating++;
            while (*rating >= '0' && *rating <= '9' && digits++ < 3) {
                fraction = fraction * 10 + (unsigned)(*rating++ - '0');
                divisor *= 10;
            }
        }
        unsigned score = whole ? 10 : (fraction * 10 + divisor / 2) / divisor;
        snprintf(result->rating, sizeof result->rating, "%u/10", score);
    }
    return true;
}

static bool import_game(Import *import)
{
    if (++import->records > (import->metadata ? 32768u : 1000000u)) {
        return false;
    }
    /* A path or image cut at the buffer's end names a different file, so it
     * is never used: the game is skipped, or keeps no image. */
    if (import->overflow[0]) {
        if (!import->dry && !import->metadata) {
            fprintf(stderr, "%s: skipped a game whose path is longer than %d bytes\n", import->root,
                    MAINUI_PATH_MAX - 1);
        }
        import->values[0][0] = 0;
    }
    if (import->overflow[2]) {
        import->values[2][0] = 0;
    }
    if (import->metadata) {
        return metadata_game(import);
    }
    const char *path = import->values[0], *label = import->values[1];
    if (import->dry || !*path || !*label) {
        return true;
    }
    char resolved[MAINUI_PATH_MAX], relative[MAINUI_PATH_MAX], parent[MAINUI_PATH_MAX] = ".";
    if (!mainui_catalog_path(resolved, import->sd, import->root, path)) {
        return true;
    }
    size_t root_length = strlen(import->root);
    /* Outside-root records have no navigable parent in this catalog. Skip them
     * along with missing ROM files; this boundary is documented explicitly. */
    if (strncmp(resolved, import->root, root_length) || resolved[root_length] != '/' ||
        !indexed_regular_file(import, resolved)) {
        return true;
    }
    int n = snprintf(relative, sizeof relative, "%s", resolved + root_length + 1);
    if (n < 0 || n >= (int)sizeof relative) {
        return true; /* A record that cannot be represented is skipped. */
    }
    int depth = 0;
    for (const char *slash = strchr(relative, '/'); slash; slash = strchr(slash + 1, '/')) {
        if (++depth >= MAINUI_STACK_MAX - 1) {
            return true;
        }
    }
    for (char *slash = strchr(relative, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        sqlite3_reset(import->folder);
        if (sqlite3_bind_text(import->folder, 1, relative, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(import->folder) != SQLITE_DONE) {
            return false;
        }
        /* XML subfolders and deeper nesting use inferred stock scan rules. */
        char folder_path[MAINUI_PATH_MAX];
        int size = snprintf(folder_path, sizeof folder_path, "%s/%s", import->saved_root, relative);
        const char *name = strrchr(relative, '/');
        if (size < 0 || size >= (int)sizeof folder_path ||
            (sqlite3_changes(import->database) &&
             !insert_row(import, name ? name + 1 : relative, folder_path, folder_path, 1,
                         parent))) {
            return false;
        }
        strcpy(parent, relative);
        *slash = '/';
    }
    /* The patcher requires an empty image for missing/null text, never the
     * previous game's image. Each game starts with zeroed field storage. */
    char stored[MAINUI_PATH_MAX], image[MAINUI_PATH_MAX] = "";
    /* Preserve decoded XML text, including ./; absolute XML paths already
     * carry their prefix and must remain launchable. Validation above uses host paths. */
    n = snprintf(stored, sizeof stored, "%s%s%s", *path == '/' ? "" : import->saved_root,
                 *path == '/' ? "" : "/", path);
    if (n < 0 || n >= (int)sizeof stored) {
        return true;
    }
    const char *art = import->values[2];
    if (*art) {
        n = snprintf(image, sizeof image, "%s%s%s", *art == '/' ? "" : import->saved_root,
                     *art == '/' ? "" : "/", art);
        if (n < 0 || n >= (int)sizeof image) {
            image[0] = 0; /* Too long to store: no image, as when none is given. */
        }
    }
    return insert_row(import, label, stored, image, 0, parent);
}

static void whitespace(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' || **cursor == '\n') {
        (*cursor)++;
    }
}

/* An element name; one longer than 63 bytes is read whole and kept cut. */
static bool name(const char **cursor, char out[64])
{
    const char *start = *cursor;
    if (!isalpha((unsigned char)**cursor) && **cursor != '_') {
        return false;
    }
    while (**cursor && (isalnum((unsigned char)**cursor) || strchr("_:-.", **cursor))) {
        (*cursor)++;
    }
    size_t size = (size_t)(*cursor - start);
    if (size > 63) {
        size = 63;
    }
    memcpy(out, start, size);
    out[size] = 0;
    return true;
}

/* At "</": the depth the end tag closes to (the innermost open element of its
 * name), or -1 when it is no end tag of an open element. *after is past '>'. */
static int end_tag(const char *cursor, char stack[][64], int depth, const char **after)
{
    char tag[64];
    cursor += 2;
    if (!name(&cursor, tag)) {
        return -1;
    }
    whitespace(&cursor);
    if (*cursor != '>') {
        return -1;
    }
    *after = cursor + 1;
    for (int i = depth - 1; i >= 0; i--) {
        if (!strcmp(stack[i], tag)) {
            return i;
        }
    }
    return -1;
}

/* Close open elements down to depth `to`; a <game> that closes is imported.
 * cursor is just past the markup that closed it. */
static bool close_to(Import *import, char stack[][64], int *depth, int to, const char *base,
                     const char *cursor)
{
    for (; *depth > to; (*depth)--) {
        if (*depth == 2 && !strcmp(stack[1], "game")) {
            import->record_end = (uint32_t)(cursor - base);
            if (!import_game(import)) {
                return false;
            }
        }
    }
    if (!*depth) {
        import->closed = true;
    }
    return true;
}

/* Bounded event parser retaining one record, not a DOM. It reads gamelists
 * leniently, as stock does, since many are written without escaping: inside
 * a field only an end tag of an open element is markup, so "A < B" and
 * "Sonic & Tails" are text; an end tag closes up to its open element and a
 * stray one is ignored; anything outside the <gameList> root is ignored.
 * Returns false only for a list that has no usable <gameList> (one that was
 * neither closed nor held a game), the nesting bound, or an import error. */
static bool parse(Import *import, const char *cursor)
{
    const char *base = cursor;
    char stack[XML_DEPTH][64];
    int depth = 0, field = -1;
    bool root_seen = false;
    import->closed = false;
    if (!strncmp(cursor, "\xef\xbb\xbf", 3)) {
        cursor += 3;
    }
    while (*cursor) {
        if (mainui_cancelled(import->cancel)) {
            return false;
        }
        if (*cursor != '<') {
            const char *end = strchr(cursor, '<');
            if (!end) {
                end = cursor + strlen(cursor);
            }
            if (field >= 0) {
                append_text(cursor, (size_t)(end - cursor), true, import->values[field],
                            &import->lengths[field], &import->overflow[field]);
            }
            cursor = end;
            continue;
        }
        if (!strncmp(cursor, "<!--", 4)) {
            const char *end = strstr(cursor + 4, "-->");
            if (!end) {
                break;
            }
            cursor = end + 3;
            continue;
        }
        if (!strncmp(cursor, "<![CDATA[", 9)) {
            const char *end = strstr(cursor + 9, "]]>");
            if (!end) {
                end = cursor + strlen(cursor);
            }
            if (field >= 0) {
                append_text(cursor + 9, (size_t)(end - cursor - 9), false, import->values[field],
                            &import->lengths[field], &import->overflow[field]);
            }
            cursor = *end ? end + 3 : end;
            continue;
        }
        const char *after = NULL;
        int closes = cursor[1] == '/' ? end_tag(cursor, stack, depth, &after) : -1;
        if (field >= 0 && closes < 0) {
            append_text("<", 1, false, import->values[field], &import->lengths[field],
                        &import->overflow[field]);
            cursor++;
            continue;
        }
        if (closes >= 0) {
            field = -1;
            if (!close_to(import, stack, &depth, closes, base, after)) {
                return false;
            }
            cursor = after;
            if (!depth) {
                break; /* The root closed; whatever follows is ignored. */
            }
            continue;
        }
        if (cursor[1] == '?' || cursor[1] == '!' || cursor[1] == '/') {
            /* Declarations such as DOCTYPE (never expanded), processing
             * instructions and stray end tags. */
            const char *end = strstr(cursor, cursor[1] == '?' ? "?>" : ">");
            if (!end) {
                break;
            }
            cursor = end + (cursor[1] == '?' ? 2 : 1);
            continue;
        }
        const char *tag_start = cursor;
        const char *p = cursor + 1;
        char tag[64];
        if (!name(&p, tag)) {
            cursor++; /* A lone '<' outside a field: text, ignored here. */
            continue;
        }
        /* Attributes are not used; skip to the end of the tag. */
        const char *end = strchr(p, '>');
        if (!end) {
            break;
        }
        bool empty = end > p && end[-1] == '/';
        cursor = end + 1;
        if (!depth && strcmp(tag, "gameList")) {
            continue; /* Outside the root. */
        }
        if (!depth) {
            root_seen = true;
            if (empty) {
                import->closed = true;
                break;
            }
        }
        if (depth == XML_DEPTH) {
            return false;
        }
        int opened = -1;
        if (depth == 1 && !strcmp(tag, "game")) {
            import->record_start = (uint32_t)(tag_start - base);
            memset(import->values, 0, sizeof import->values);
            memset(import->lengths, 0, sizeof import->lengths);
            memset(import->seen, 0, sizeof import->seen);
            memset(import->overflow, 0, sizeof import->overflow);
        }
        if (depth == 2 && !strcmp(stack[1], "game")) {
            if (!strcmp(tag, "path")) {
                opened = 0;
            }
            else if (!strcmp(tag, "name")) {
                opened = 1;
            }
            else if (!strcmp(tag, "image")) {
                opened = 2;
            }
            else if (import->metadata && !strcmp(tag, "genre")) {
                opened = 3;
            }
            else if (import->metadata && !strcmp(tag, "rating")) {
                opened = 4;
            }
            else if (import->metadata && !strcmp(tag, "desc")) {
                opened = 5;
            }
            if (opened >= 0) {
                /* A repeated field keeps its first value. */
                if (import->seen[opened]) {
                    opened = -1;
                }
                else {
                    import->seen[opened] = true;
                }
            }
        }
        if (empty) {
            continue;
        }
        strcpy(stack[depth++], tag);
        field = opened;
    }
    return root_seen && (import->closed || import->records);
}

/* An unusable list is not imported: the caller lists the ROM files instead. */
static bool unusable(const char *path, const char *reason, bool *present)
{
    fprintf(stderr, "%s is unusable (%s); listing the ROM files instead\n", path, reason);
    *present = false;
    return true;
}

/* A list that cannot be opened or read (I/O, permission) may be fine: the
 * build fails, so the previous cache is kept rather than replaced by one made
 * from the ROM files. */
static bool unreadable(const char *path, int error)
{
    fprintf(stderr, "%s could not be read (%s); the previous cache is kept\n", path,
            strerror(error));
    return false;
}

bool mainui_gamelist_import_control(sqlite3 *database, sqlite3_stmt *insert, const char *sd,
                                    const char *root, const char *saved_root, bool *present,
                                    MainUICancel cancel)
{
    *present = true;
    char path[MAINUI_PATH_MAX];
    if (!mainui_catalog_path(path, sd, root, "miyoogamelist.xml")) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) {
            *present = false;
            return true;
        }
        return unreadable(path, errno);
    }
    char *data = malloc(XML_LIMIT + 1);
    if (!data) {
        fclose(file);
        return false;
    }
    size_t size = fread(data, 1, XML_LIMIT, file);
    bool large = !ferror(file) && fgetc(file) != EOF;
    int error = ferror(file) ? (errno ? errno : EIO) : 0;
    fclose(file);
    data[size] = 0;
    if (error) {
        free(data);
        return unreadable(path, error);
    }
    if (large) {
        free(data);
        return unusable(path, "larger than 16 MiB", present);
    }
    Import *import = calloc(1, sizeof *import);
    if (!import) {
        free(data);
        return false;
    }
    import->database = database;
    import->insert = insert;
    import->sd = sd;
    import->root = root;
    import->saved_root = saved_root;
    import->cancel = cancel;
    /* A dry pass first, so a list found unusable halfway leaves no rows. */
    import->dry = true;
    bool ok = parse(import, data);
    if (!ok) {
        bool cancelled = mainui_cancelled(cancel);
        free(import);
        free(data);
        return cancelled ? false : unusable(path, "no readable <gameList>", present);
    }
    import->dry = false;
    import->records = 0;
    /* Folder deduplication lives in SQLite instead of a growing heap model.
     * This temporary table never reaches the published cache file. */
    ok = sqlite3_exec(database, "CREATE TEMP TABLE mainui_xml_folders(path TEXT PRIMARY KEY)", NULL,
                      NULL, NULL) == SQLITE_OK &&
         sqlite3_prepare_v2(database, "INSERT OR IGNORE INTO mainui_xml_folders VALUES (?1)", -1,
                            &import->folder, NULL) == SQLITE_OK;
    if (ok) {
        ok = parse(import, data);
    }
    sqlite3_finalize(import->folder);
    close_folder_indexes(import);
    free(import);
    free(data);
    return ok;
}

/* UI-thread cache: sixteen XML paths, at most 2 MiB of compact record locations.
 * Full buffers exist only while building; selected records are read on demand. */
#define METADATA_BYTES (2u * 1024u * 1024u)
#define METADATA_RECORDS 32768u
#define METADATA_READ 32768u

typedef struct {
    char path[MAINUI_PATH_MAX];
    MainUIFileStamp stamp;
    MetadataRecord *records;
    size_t count;
    uint64_t used;
    bool valid;
} MetadataIndex;

static MetadataIndex metadata_cache[16];
static uint64_t metadata_clock;
static size_t metadata_bytes;
#ifdef MAINUI_METADATA_TEST
static size_t metadata_builds, metadata_reads;

void mainui_gamelist_metadata_stats(size_t *builds, size_t *reads, size_t *bytes)
{
    *builds = metadata_builds;
    *reads = metadata_reads;
    *bytes = metadata_bytes;
}
#endif

static void discard_index(MetadataIndex *index)
{
    metadata_bytes -= index->count * sizeof *index->records;
    free(index->records);
    *index = (MetadataIndex){0};
}

void mainui_gamelist_metadata_close(void)
{
    for (size_t i = 0; i < 16; i++) {
        discard_index(&metadata_cache[i]);
    }
    metadata_clock = 0;
}

static int record_compare(const void *a, const void *b)
{
    const MetadataRecord *x = a, *y = b;
    if (x->hash != y->hash) {
        return x->hash < y->hash ? -1 : 1;
    }
    return (x->offset > y->offset) - (x->offset < y->offset);
}

static MetadataIndex *get_index(const char *path, MainUIFileStamp stamp)
{
    MetadataIndex *slot = &metadata_cache[0];
    for (size_t i = 0; i < 16; i++) {
        MetadataIndex *item = &metadata_cache[i];
        if (!strcmp(item->path, path)) {
            if (mainui_file_stamp_equal(item->stamp, stamp)) {
                item->used = ++metadata_clock;
                return item;
            }
            slot = item;
            break;
        }
        if (item->used < slot->used) {
            slot = item;
        }
    }
    discard_index(slot);
    strcpy(slot->path, path);
    slot->stamp = stamp;
    slot->used = ++metadata_clock;
    if (!stamp.exists || stamp.size > 8u * 1024u * 1024u) {
        return slot;
    }
#ifdef MAINUI_METADATA_TEST
    metadata_builds++;
#endif
    char *text = mainui_read_text(path, 8u * 1024u * 1024u);
    MetadataRecord *records = malloc(METADATA_RECORDS * sizeof *records);
    MainUIMetadata *unused = calloc(1, sizeof *unused);
    Import *import = calloc(1, sizeof *import);
    if (import) {
        import->metadata = unused;
        import->index = records;
    }
    bool ok = text && records && import && unused && parse(import, text) &&
              mainui_file_stamp_equal(stamp, mainui_file_stamp(path));
    size_t record_count = import ? import->records : 0;
    free(import);
    free(unused);
    free(text);
    if (!ok) {
        free(records);
        return slot;
    }
    size_t bytes = record_count * sizeof *records;
    if (!bytes) {
        free(records);
        records = NULL;
    }
    else {
        MetadataRecord *compact = realloc(records, bytes);
        if (!compact) {
            free(records);
            return slot;
        }
        records = compact;
        qsort(records, record_count, sizeof *records, record_compare);
    }
    while (metadata_bytes + bytes > METADATA_BYTES) {
        MetadataIndex *oldest = NULL;
        for (size_t i = 0; i < 16; i++) {
            MetadataIndex *item = &metadata_cache[i];
            if (item != slot && item->records && (!oldest || item->used < oldest->used)) {
                oldest = item;
            }
        }
        if (!oldest) {
            free(records);
            return slot;
        }
        discard_index(oldest);
    }
    slot->records = records;
    slot->count = record_count;
    slot->valid = true;
    metadata_bytes += bytes;
    return slot;
}

static bool read_metadata(const char *directory, size_t directory_length, const char *key,
                          MainUIMetadata *result, bool *missing)
{
    *missing = false;
    *result = (MainUIMetadata){0};
    char path[MAINUI_PATH_MAX];
    if (directory_length >= MAINUI_PATH_MAX) {
        return false;
    }
    int length = snprintf(path, sizeof path, "%.*s/gamelist.xml", (int)directory_length, directory);
    if (length < 0 || length >= (int)sizeof path) {
        return false;
    }
    MainUIFileStamp stamp = mainui_file_stamp(path);
    *missing = !stamp.exists;
    MetadataIndex *index = get_index(path, stamp);
    if (!index->valid) {
        return false;
    }
    uint32_t hash = metadata_hash(key);
    size_t low = 0, high = index->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (index->records[mid].hash < hash) {
            low = mid + 1;
        }
        else {
            high = mid;
        }
    }
    for (; low < index->count && index->records[low].hash == hash; low++) {
        MetadataRecord record = index->records[low];
        if (record.length > METADATA_READ) {
            return false;
        }
        FILE *file = fopen(path, "rb");
        if (!file) {
            return false;
        }
#ifdef MAINUI_METADATA_TEST
        metadata_reads += record.length;
#endif
        char *text = malloc(record.length + 22);
        bool ok = text && !fseek(file, (long)record.offset, SEEK_SET);
        if (ok) {
            memcpy(text, "<gameList>", 10);
            ok = fread(text + 10, 1, record.length, file) == record.length && !ferror(file);
            memcpy(text + 10 + record.length, "</gameList>", 12);
        }
        fclose(file);
        MainUIMetadata pending = {0};
        Import *import = calloc(1, sizeof *import);
        if (import) {
            import->metadata = &pending;
            import->filename = key;
        }
        ok = ok && import && parse(import, text) &&
             mainui_file_stamp_equal(stamp, mainui_file_stamp(path));
        free(import);
        free(text);
        if (!ok) {
            discard_index(index);
            return false;
        }
        if (pending.found) {
            *result = pending;
            return true;
        }
    }
    return true;
}

bool mainui_gamelist_metadata(const char *rom, const char *root, MainUIMetadata *result)
{
    *result = (MainUIMetadata){0};
    const char *filename = strrchr(rom, '/');
    if (!filename) {
        return false;
    }
    bool missing = false;
    bool ok = read_metadata(rom, (size_t)(filename - rom), filename + 1, result, &missing);
    if (result->found || (!ok && !missing) || !root || !*root) {
        return ok;
    }
    size_t length = strlen(root);
    while (length && root[length - 1] == '/') {
        length--;
    }
    /* Never search above the configured console, or use a sibling's basename. */
    if (!length || length >= (size_t)(filename - rom) || strncmp(rom, root, length) ||
        rom[length] != '/') {
        return ok;
    }
    return read_metadata(root, length, rom + length + 1, result, &missing);
}

bool mainui_gamelist_import(sqlite3 *database, sqlite3_stmt *insert, const char *sd,
                            const char *root, const char *saved_root, bool *present)
{
    return mainui_gamelist_import_control(database, insert, sd, root, saved_root, present,
                                          (MainUICancel){0});
}
