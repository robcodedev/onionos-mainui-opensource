/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog/delete.h"
#include "cJSON.h"
#include "platform/files.h"
#include "sqlite3.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool journal_path(char out[4096], const char *cache)
{
    int n = snprintf(out, 4096, "%s.delete.json", cache);
    return n > 0 && n < 4096;
}

static bool staging_path(char out[4096], const char *original)
{
    static atomic_uint serial = 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
        uint64_t nonce;
        int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        ssize_t count = random >= 0 ? read(random, &nonce, sizeof nonce) : -1;
        if (random >= 0) {
            close(random);
        }
        if (count != (ssize_t)sizeof nonce) {
            struct timespec now = {0};
            clock_gettime(CLOCK_REALTIME, &now);
            nonce = ((uint64_t)now.tv_sec << 32) ^ (uint64_t)now.tv_nsec ^
                    ((uint64_t)(unsigned)getpid() << 16) ^
                    atomic_fetch_add_explicit(&serial, 1, memory_order_relaxed);
        }
        int n =
            snprintf(out, 4096, "%s.mainui-delete.%016llx", original, (unsigned long long)nonce);
        struct stat info;
        if (n < 0 || n >= 4096) {
            return false;
        }
        if (lstat(out, &info) != 0) {
            return errno == ENOENT;
        }
    }
    return false;
}

/* An unreadable journal cannot identify its ROM. Search the console tree
 * conservatively, without following symlinks. Any uncertainty preserves it. */
static bool no_staged_files(const char *root, unsigned depth)
{
    if (depth > 64) {
        return false;
    }
    DIR *directory = opendir(root);
    if (!directory) {
        return false;
    }
    bool clear = true;
    struct dirent *entry;
    errno = 0;
    while (clear && (entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        if (strstr(entry->d_name, ".mainui-delete")) {
            fprintf(stderr, "Pending ROM recovery file: %s/%s\n", root, entry->d_name);
            clear = false;
            break;
        }
        char path[4096];
        int n = snprintf(path, sizeof path, "%s/%s", root, entry->d_name);
        struct stat info;
        clear = n > 0 && n < (int)sizeof path && lstat(path, &info) == 0;
        if (clear && S_ISDIR(info.st_mode)) {
            clear = no_staged_files(path, depth + 1);
        }
        errno = 0;
    }
    clear = clear && errno == 0;
    closedir(directory);
    return clear;
}

static bool journal_size(const cJSON *intent, uint64_t *size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(intent, "size");
    const char *text = cJSON_IsString(value) ? value->valuestring : NULL;
    bool legacy = false;
    if (!value) {
        value = cJSON_GetObjectItemCaseSensitive(intent, "identity");
        text = cJSON_IsString(value) ? strchr(value->valuestring, ':') : NULL;
        if (text) {
            ++text;
            legacy = true;
        }
    }
    if (!text || *text < '0' || *text > '9') {
        return false;
    }
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || (legacy ? *end != ':' : *end != 0)) {
        return false;
    }
    *size = (uint64_t)parsed;
    return true;
}

bool mainui_delete_prepare(const char *cache, const char *original, const char *key,
                           char staged[4096])
{
    char journal[4096], size[32];
    if (!journal_path(journal, cache) || !staging_path(staged, original)) {
        return false;
    }
    MainUIFileStamp stamp = mainui_file_stamp(original);
    if (!stamp.exists) {
        return false;
    }
    snprintf(size, sizeof size, "%llu", (unsigned long long)stamp.size);
    cJSON *intent = cJSON_CreateObject();
    bool ok = intent && cJSON_AddStringToObject(intent, "original", original) &&
              cJSON_AddStringToObject(intent, "key", key) &&
              cJSON_AddStringToObject(intent, "staged", staged) &&
              cJSON_AddStringToObject(intent, "size", size);
    char *text = ok ? cJSON_PrintUnformatted(intent) : NULL;
    ok = text && mainui_write_bytes_new_locked(journal, text, strlen(text));
    free(text);
    cJSON_Delete(intent);
    return ok;
}

static int recovery_budget_exhausted(void *context)
{
    int *remaining = context;
    return --*remaining <= 0;
}

static bool configure_recovery(sqlite3 *database, int *budget)
{
    sqlite3_limit(database, SQLITE_LIMIT_LENGTH, 65536);
    sqlite3_limit(database, SQLITE_LIMIT_SQL_LENGTH, 16384);
#ifdef SQLITE_DBCONFIG_DEFENSIVE
    if (sqlite3_libversion_number() >= 3026000 &&
        sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK) {
        return false;
    }
#endif
#ifdef SQLITE_DBCONFIG_TRUSTED_SCHEMA
    if (sqlite3_libversion_number() >= 3031000 &&
        sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL) != SQLITE_OK) {
        return false;
    }
#endif
    if (sqlite3_busy_timeout(database, 100) != SQLITE_OK) {
        return false;
    }
    /* Same 20-million-instruction bound as ordinary cache reads. */
    *budget = 20000;
    sqlite3_progress_handler(database, 1000, recovery_budget_exhausted, budget);
    return sqlite3_exec(database,
                        "PRAGMA query_only=ON; PRAGMA cache_size=-256; PRAGMA temp_store=MEMORY",
                        NULL, NULL, NULL) == SQLITE_OK;
}

static bool recover(const char *cache, const char *table, const char *root, const char *sd)
{
    char journal[4096], temporary[4096];
    if (!journal_path(journal, cache)) {
        return false;
    }
    /* lstat semantics: a dangling symlink is a journal entry. It then fails the
     * regular-file check below, so recovery refuses rather than reporting success. */
    if (!mainui_delete_journal_present(cache)) {
        return true;
    }
    /* Validate ancestors before scanning malformed journals or opening SQLite.
     * The console root alone cannot detect a symlink above that root. */
    if (!mainui_directory_within(root, sd) || !mainui_regular_file_within(journal, sd)) {
        return false;
    }
    char *text = mainui_read_text(journal, 16384);
    cJSON *intent = text ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    free(text);
    const cJSON *original = cJSON_GetObjectItemCaseSensitive(intent, "original");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(intent, "key");
    const cJSON *saved = cJSON_GetObjectItemCaseSensitive(intent, "staged");
    uint64_t size = 0;
    bool ok = cJSON_IsString(original) && original->valuestring[0] && cJSON_IsString(key) &&
              key->valuestring[0] && journal_size(intent, &size) &&
              (!saved || cJSON_IsString(saved));
    if (ok) {
        int n = saved ? snprintf(temporary, sizeof temporary, "%s", saved->valuestring)
                      : snprintf(temporary, sizeof temporary, "%s.mainui-delete",
                                 original->valuestring);
        ok = n > 0 && n < (int)sizeof temporary && strcmp(temporary, original->valuestring) != 0;
    }
    if (ok && saved) {
        /* Only ever touch a staging name this code could have generated. */
        size_t length = strlen(original->valuestring);
        ok = strlen(temporary) == length + 31 &&
             !strncmp(temporary, original->valuestring, length) &&
             !strncmp(temporary + length, ".mainui-delete.", 15) &&
             strspn(temporary + length + 15, "0123456789abcdef") == 16;
    }
    if (!ok) {
        struct stat info;
        bool saved_absent =
            !cJSON_IsString(saved) || (lstat(saved->valuestring, &info) != 0 && errno == ENOENT);
        cJSON_Delete(intent);
        return saved_absent && no_staged_files(root, 0) && mainui_remove_file(journal) == 0;
    }
    if (!mainui_path_within(original->valuestring, root) ||
        !mainui_path_within(original->valuestring, sd) || !mainui_path_within(temporary, root) ||
        !mainui_path_within(temporary, sd)) {
        cJSON_Delete(intent);
        return false;
    }
    MainUIFileStamp staged = mainui_file_stamp(temporary);
    MainUIFileStamp live = mainui_file_stamp(original->valuestring);
    MainUIFileStamp values[] = {staged, live};
    const char *paths[] = {temporary, original->valuestring};
    for (int i = 0; ok && i < 2; ++i) {
        struct stat info;
        if (lstat(paths[i], &info) != 0) {
            ok = errno == ENOENT;
            continue;
        }
        ok = values[i].exists && values[i].size == size &&
             mainui_regular_file_within(paths[i], root) && mainui_regular_file_within(paths[i], sd);
    }
    /* An original recreated after staging is not ours to delete, even if its
     * size matches. Preserve both files for manual resolution. */
    ok = ok && !(staged.exists && live.exists);
    if (ok && (staged.exists || live.exists)) {
        /* SQLite hot-journal recovery decides whether the row deletion committed. */
        sqlite3 *db = NULL;
        sqlite3_stmt *statement = NULL;
        int budget;
        ok = mainui_regular_file_within(cache, sd) &&
             sqlite3_open_v2(cache, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK &&
             configure_recovery(db, &budget);
        char *sql = sqlite3_mprintf("SELECT 1 FROM \"%w\" WHERE type=0 AND path=?1 LIMIT 1", table);
        ok = ok && sql && sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK &&
             sqlite3_bind_text(statement, 1, key->valuestring, -1, SQLITE_TRANSIENT) == SQLITE_OK;
        sqlite3_free(sql);
        int result = ok ? sqlite3_step(statement) : SQLITE_ERROR;
        sqlite3_finalize(statement);
        sqlite3_close(db);
        if (result == SQLITE_ROW) {
            ok = live.exists || mainui_move_file_new_locked(temporary, original->valuestring);
        }
        else if (result == SQLITE_DONE) {
            /* A committed deletion owns only the staged name, never a newly
             * present original. Keep the journal on that conflict as well. */
            ok = !live.exists && (!staged.exists || mainui_remove_file(temporary) == 0);
        }
        else {
            ok = false;
        }
    }
    if (ok) {
        ok = mainui_remove_file(journal) == 0;
    }
    cJSON_Delete(intent);
    return ok;
}

bool mainui_delete_recover(const char *cache, const char *table, const char *root, const char *sd)
{
    bool ok = recover(cache, table, root, sd);
    if (!ok) {
        char journal[4096];
        if (journal_path(journal, cache)) {
            char *text =
                mainui_regular_file_within(journal, sd) ? mainui_read_text(journal, 16384) : NULL;
            cJSON *intent = text ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
            const cJSON *staged = cJSON_GetObjectItemCaseSensitive(intent, "staged");
            const cJSON *original = cJSON_GetObjectItemCaseSensitive(intent, "original");
            fprintf(stderr, "ROM recovery refused; journal preserved: %s; staged: ", journal);
            if (cJSON_IsString(staged)) {
                fprintf(stderr, "%s\n", staged->valuestring);
            }
            else if (cJSON_IsString(original)) {
                fprintf(stderr, "%s.mainui-delete\n", original->valuestring);
            }
            else {
                fprintf(stderr, "unknown (look for .mainui-delete files under %s)\n", root);
            }
            fprintf(stderr, "Browsing remains available; Delete stays blocked until Refresh "
                            "roms, which keeps both ROM files. See docs/TROUBLESHOOTING.md.\n");
            cJSON_Delete(intent);
            free(text);
        }
    }
    return ok;
}

/* Names the staged ROM from a journal, or the journal itself when unreadable. */
static const char *pending_file(const char *journal, const char *sd, char out[4096])
{
    char *text = mainui_regular_file_within(journal, sd) ? mainui_read_text(journal, 16384) : NULL;
    cJSON *intent = text ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    const cJSON *staged = cJSON_GetObjectItemCaseSensitive(intent, "staged");
    const cJSON *original = cJSON_GetObjectItemCaseSensitive(intent, "original");
    const char *kind = "journal";
    int n = snprintf(out, 4096, "%s", journal);
    if (cJSON_IsString(staged) && *staged->valuestring) {
        n = snprintf(out, 4096, "%s", staged->valuestring);
        kind = "staged file";
    }
    else if (cJSON_IsString(original) && *original->valuestring) {
        n = snprintf(out, 4096, "%s.mainui-delete", original->valuestring);
        kind = "staged file";
    }
    if (n <= 0 || n >= 4096) {
        snprintf(out, 4096, "%s", journal);
        kind = "journal";
    }
    cJSON_Delete(intent);
    free(text);
    return kind;
}

bool mainui_delete_journal_present(const char *cache)
{
    char journal[4096];
    struct stat info;
    return !journal_path(journal, cache) || lstat(journal, &info) == 0 || errno != ENOENT;
}

bool mainui_delete_abandon(const char *cache, const char *sd)
{
    char journal[4096], pending[4096];
    if (!journal_path(journal, cache)) {
        return false;
    }
    /* lstat, not stat: a dangling symlink is still an entry that blocks a
     * later Delete. Removal below unlinks the entry, never its target. */
    struct stat info;
    if (lstat(journal, &info) != 0) {
        return errno == ENOENT;
    }
    const char *kind = pending_file(journal, sd, pending);
    bool removed = false;
    if (mainui_remove_file_status(journal, &removed) != 0) {
        if (!removed && errno != ENOENT) {
            fprintf(stderr, "Cannot drop ROM deletion journal %s: %s\n", journal, strerror(errno));
            return false;
        }
        /* Best effort: if the removal is lost in a power cut, the journal
         * simply blocks Delete again until the next Refresh roms. */
        if (removed) {
            fprintf(stderr, "Dropped ROM deletion journal %s, but flushing its folder failed: %s\n",
                    journal, strerror(errno));
        }
    }
    fprintf(stderr,
            "Refresh roms dropped pending ROM deletion journal %s; ROM files left untouched. "
            "Check the %s: %s\n",
            journal, kind, pending);
    return true;
}

void mainui_delete_pending_error(const char *cache, const char *sd, char error[256])
{
    char journal[4096], buffer[4096];
    if (!journal_path(journal, cache)) {
        snprintf(error, 256, "Pending deletion: run Refresh roms to continue.");
        return;
    }
    const char *kind = pending_file(journal, sd, buffer);
    const char *path = buffer;
    /* Keep the identifying filename when a path exceeds the popup's buffer. */
    bool clipped = strlen(path) > 180;
    if (clipped) {
        path += strlen(path) - 180;
        while (((unsigned char)*path & 0xc0) == 0x80) {
            ++path;
        }
    }
    snprintf(error, 256, "Pending deletion; run Refresh roms. %s: %s%.180s", kind,
             clipped ? "..." : "", path);
}
