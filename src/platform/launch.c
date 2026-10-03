/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/launch.h"
#include "platform/files.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *string(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static bool protocol_path(const char *path)
{
    if (strncmp(path, "/mnt/SDCARD/", 12) || strlen(path) >= 4096) {
        return false;
    }
    /* runtime.sh splits literal quotes/colons and later escapes dollar signs.
     * Escaping only this producer would silently change its decoded ROM path. */
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 32 || *p == 127 || strchr("\"$`\\:", *p)) {
            return false;
        }
    }
    return true;
}

/* Search's own database contains command arguments rather than ROM paths.
 * Restrict this exception to its launcher and known commands; arguments remain
 * double-quoted and must not contain shell expansions or quote characters. */
static bool search_argument(const char *launch, const char *argument)
{
    if (strcmp(launch, "/mnt/SDCARD/App/Search/launch.sh") || strlen(argument) >= 4096) {
        return false;
    }
    if (strcmp(argument, "search") && strcmp(argument, "clear") &&
        strncmp(argument, "setstate:/mnt/SDCARD/Emu/", strlen("setstate:/mnt/SDCARD/Emu/")) &&
        strncmp(argument, "setstate:/mnt/SDCARD/RApp/", strlen("setstate:/mnt/SDCARD/RApp/"))) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)argument; *p; p++) {
        if (*p < 32 || *p == 127 || strchr("\"$`\\", *p)) {
            return false;
        }
    }
    return true;
}

char *mainui_launch_command(const cJSON *record, char error[256])
{
    error[0] = 0;
    const char *launch = string(record, "launch"), *rom = string(record, "rompath");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(record, "type");
    int type = cJSON_IsNumber(kind) && kind->valuedouble == kind->valueint ? kind->valueint : -1;
    bool app = type == 3 || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "app_action"));
    char source[4096];
    const char *separator = strstr(rom, "launch.sh:/mnt/SDCARD/");
    if (separator) {
        size_t length = (size_t)(separator - rom) + strlen("launch.sh");
        if (length >= sizeof source) {
            return NULL;
        }
        memcpy(source, rom, length);
        source[length] = 0;
        launch = source;
        rom += length + 1;
    }
    if ((type < 0 || (!app && type != 5 && type != 17)) || !protocol_path(launch) ||
        (!app && !protocol_path(rom) && !search_argument(launch, rom))) {
        snprintf(error, 256,
                 "Unsupported launch path or action type (quotes, dollars, backticks, colons and "
                 "control bytes cannot use the unchanged Onion protocol).");
        return NULL;
    }
    char *command = malloc(16384);
    if (!command) {
        return NULL;
    }
    int length;
    if (app) {
        /* The stock App producer changes cwd and the runtime extracts its
         * unquoted second token. Preserve it only for shell-safe paths. */
        for (const unsigned char *p = (const unsigned char *)launch; *p; p++) {
            if (!isalnum(*p) && !strchr("/._-", *p)) {
                free(command);
                snprintf(error, 256,
                         "App launch paths with spaces or shell metacharacters require a "
                         "coordinated Onion runtime parser change.");
                return NULL;
            }
        }
        char directory[4096];
        strcpy(directory, launch);
        char *slash = strrchr(directory, '/');
        if (!slash || !slash[1]) {
            free(command);
            return NULL;
        }
        *slash = 0;
        length = snprintf(
            command, 16384,
            "cd %s; chmod a+x %s; LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so %s\n",
            directory, launch, launch);
    }
    else {
        length = snprintf(command, 16384,
                          "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"\n",
                          launch, rom);
    }
    if (length < 0 || length >= 16384) {
        free(command);
        return NULL;
    }
    error[0] = 0;
    return command;
}

static void little32(unsigned char *out, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        out[i] = (unsigned char)(value >> (8 * i));
    }
}

static void return_text(unsigned char *out, const char *text, size_t limit)
{
    size_t length = strlen(text);
    if (length > limit) {
        length = limit;
        while (length && ((unsigned char)text[length] & 0xc0) == 0x80) {
            --length;
        }
    }
    memcpy(out, text, length);
    out[length] = 0;
}

bool mainui_launch_favorite_return(unsigned char out[MAINUI_FAVORITE_RETURN_SIZE], int type,
                                   const char *folder, const char *rom, const char *label)
{
    if (!folder || !rom || !label || !*folder || !strcmp(folder, "/")) {
        return false;
    }
    memset(out, 0, MAINUI_FAVORITE_RETURN_SIZE);
    little32(out, 0x43424631u);
    little32(out + 4, 1);
    little32(out + 8, (uint32_t)type);
    return_text(out + 12, folder, 255);
    return_text(out + 268, rom, 1023);
    return_text(out + 1804, label, 127);
    return true;
}

static bool join(char out[4096], const char *directory, const char *name)
{
    int length = snprintf(out, 4096, "%s/%s", directory, name);
    return length > 0 && length < 4096;
}

void mainui_launch_clear_search(const char *directory)
{
    char path[4096];
    if (directory && join(path, directory, "mainui-context-search-postgame")) {
        mainui_remove_file(path);
    }
}

typedef struct {
    char command_path[4096], state_path[4096], return_path[4096], favorite_path[4096];
    char search_path[4096];
    unsigned char favorite[MAINUI_FAVORITE_RETURN_SIZE];
} LaunchScratch;

static bool publish_unlocked(const char *directory, const cJSON *record, const MainUIStack *state,
                             const cJSON *resume, char error[256], LaunchScratch *scratch)
{
    if (!join(scratch->command_path, directory, "cmd_to_run.sh") ||
        !join(scratch->state_path, directory, "state.json") ||
        !join(scratch->return_path, directory, "mainui-return.json") ||
        !join(scratch->favorite_path, directory, "mainui-favourite-folder-return")) {
        return false;
    }
    FILE *pending_command = fopen(scratch->command_path, "rb");
    if (pending_command) {
        fclose(pending_command);
        snprintf(error, 256, "A pending launch request was preserved.");
        return false;
    }
    char *command = mainui_launch_command(record, error);
    char *state_text = mainui_state_json(state);
    cJSON *envelope = cJSON_CreateObject();
    cJSON *record_copy = cJSON_Duplicate(record, true),
          *resume_copy = cJSON_Duplicate(resume, true);
    bool ok = command && state_text && envelope && record_copy && resume_copy &&
              cJSON_AddNumberToObject(envelope, "schema", 1) &&
              cJSON_AddBoolToObject(envelope, "committed", false);
    if (ok && cJSON_AddItemToObject(envelope, "record", record_copy)) {
        record_copy = NULL;
    }
    else {
        ok = false;
    }
    if (ok && cJSON_AddItemToObject(envelope, "resume", resume_copy)) {
        resume_copy = NULL;
    }
    else {
        ok = false;
    }
    char *text = ok ? cJSON_PrintUnformatted(envelope) : NULL;
    bool owned = false, favorite_owned = false, search_owned = false;
    const char *folder = string(resume, "favorite_path");
    if (ok && *folder) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(record, "type");
        ok = mainui_launch_favorite_return(scratch->favorite, type->valueint, folder,
                                           string(record, "rompath"), string(record, "label"));
    }
    if (ok && text && strlen(text) <= 256u * 1024u) {
        ok = mainui_write_bytes_new_status(scratch->return_path, text, strlen(text), &owned);
    }
    else {
        ok = false;
    }
    if (ok && *folder) {
        ok = mainui_write_bytes_new_status(scratch->favorite_path, scratch->favorite,
                                           sizeof scratch->favorite, &favorite_owned);
    }
    if (ok && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(resume, "search_query"))) {
        ok = join(scratch->search_path, directory, "mainui-context-search-postgame");
        FILE *existing = ok ? fopen(scratch->search_path, "rb") : NULL;
        if (existing) {
            fclose(existing);
        }
        else if (ok) {
            ok = mainui_write_bytes_new_status(scratch->search_path, "", 0, &search_owned);
        }
    }
    if (ok) {
        /* The handoff is all-or-nothing with rollback, so it requires durability. */
        ok = mainui_write_text_atomic_result(scratch->state_path, state_text) ==
             MAINUI_WRITE_DURABLE;
    }
    bool command_owned = false;
    if (ok) {
        ok = mainui_write_bytes_new_status(scratch->command_path, command, strlen(command),
                                           &command_owned);
    }
    if (ok) {
        cJSON *commit = cJSON_GetObjectItemCaseSensitive(envelope, "committed");
        cJSON_SetBoolValue(commit, true);
        char *ready = cJSON_PrintUnformatted(envelope);
        ok = ready &&
             mainui_write_text_atomic_result(scratch->return_path, ready) == MAINUI_WRITE_DURABLE;
        free(ready);
    }
    if (!ok) {
        if (command_owned) {
            char *current = mainui_read_text(scratch->command_path, 16384);
            if (current && !strcmp(current, command)) {
                mainui_remove_file(scratch->command_path);
            }
            free(current);
        }
        if (search_owned) {
            mainui_remove_file(scratch->search_path);
        }
        if (owned) {
            mainui_remove_file(scratch->return_path);
        }
        if (favorite_owned) {
            mainui_remove_file(scratch->favorite_path);
        }
        if (!*error) {
            snprintf(error, 256,
                     "Cannot publish launch request; a pending request or unavailable handoff "
                     "directory was preserved.");
        }
    }
    free(command);
    free(state_text);
    free(text);
    cJSON_Delete(record_copy);
    cJSON_Delete(resume_copy);
    cJSON_Delete(envelope);
    return ok;
}

/* A return file that can never be read (NUL bytes or over the size limit)
 * would block every later launch, which may not replace it. Move it aside as
 * mainui-return.json.bad, or .bad-1 to .bad-9 to keep an earlier copy; the
 * caller holds the handoff lock and has seen that no command is pending. */
static void quarantine_return(const char *directory, const char *path)
{
    struct stat info;
    if (lstat(path, &info) || !S_ISREG(info.st_mode)) {
        return;
    }
    for (int i = 0; i < 10; i++) {
        char bad[4096 + 16], name[32];
        snprintf(name, sizeof name, i ? "mainui-return.json.bad-%d" : "mainui-return.json.bad", i);
        if (!join(bad, directory, name)) {
            return;
        }
        bool moved = mainui_move_file_new_locked(path, bad);
        if (moved || (lstat(path, &info) && errno == ENOENT)) {
            fprintf(stderr, "Unreadable %s moved aside as %s%s\n", path, bad,
                    moved ? "" : " (folder not flushed)");
            return;
        }
        if (lstat(bad, &info)) {
            fprintf(stderr, "Cannot move aside unreadable %s: %s\n", path, strerror(errno));
            return; /* the move itself failed: keep the file */
        }
    }
}

static cJSON *take_return_unlocked(const char *directory)
{
    char path[4096];
    if (!join(path, directory, "cmd_to_run.sh")) {
        return NULL;
    }
    /* Anything at the command path, or not knowing, means a launch may be
     * pending: its return state stays. */
    struct stat info;
    if (!lstat(path, &info) || errno != ENOENT) {
        return NULL;
    }
    if (!join(path, directory, "mainui-return.json")) {
        return NULL;
    }
    errno = 0;
    char *text = mainui_read_text(path, 256u * 1024u);
    if (!text) {
        /* Damaged content, never a read error: that may pass. */
        if (errno == EINVAL || errno == EFBIG) {
            quarantine_return(directory, path);
        }
        return NULL;
    }
    cJSON *root = cJSON_ParseWithOpts(text, NULL, true);
    free(text);
    bool removed = false;
    if (mainui_remove_file_status(path, &removed) != 0) {
        if (!removed) {
            cJSON_Delete(root);
            return NULL;
        }
        /* Consumed: the file is gone, so the parsed state is the only copy. */
        fprintf(stderr, "Took %s, but flushing its folder failed: %s\n", path, strerror(errno));
    }
    if (join(path, directory, "mainui-favourite-folder-return")) {
        mainui_remove_file(path);
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *committed = cJSON_GetObjectItemCaseSensitive(root, "committed");
    if (!cJSON_IsTrue(committed) || !cJSON_IsNumber(schema) || schema->valuedouble != 1 ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "resume")) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "record"))) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

bool mainui_launch_publish(const char *directory, const cJSON *record, const MainUIStack *state,
                           const cJSON *resume, char error[256])
{
    char path[4096];
    if (!join(path, directory, "mainui-handoff")) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    LaunchScratch *scratch = lock ? calloc(1, sizeof *scratch) : NULL;
    bool ok = scratch && publish_unlocked(directory, record, state, resume, error, scratch);
    if (lock && !scratch) {
        snprintf(error, 256, "Cannot allocate launch scratch space.");
    }
    free(scratch);
    if (!lock) {
        snprintf(error, 256, "Another runtime writer is active.");
    }
    mainui_file_unlock(lock);
    return ok;
}

bool mainui_launch_publish_restart(const char *directory, const cJSON *resume, char error[256])
{
    char path[4096], command[4096], envelope_path[4096];
    if (!join(path, directory, "mainui-handoff") || !join(command, directory, "cmd_to_run.sh") ||
        !join(envelope_path, directory, "mainui-return.json")) {
        return false;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    if (!lock) {
        snprintf(error, 256, "Another runtime writer is active.");
        return false;
    }
    bool ok = true;
    FILE *pending = fopen(command, "rb");
    if (pending) {
        fclose(pending);
        snprintf(error, 256, "A pending launch request was preserved.");
        ok = false;
    }
    /* No command: Onion starts MainUI again, which takes this return and
     * reopens the resumed screen. The record is required by the envelope
     * format but unused when resuming Settings. */
    cJSON *envelope = ok ? cJSON_CreateObject() : NULL;
    cJSON *record = envelope ? cJSON_CreateObject() : NULL;
    cJSON *copy = envelope ? cJSON_Duplicate(resume, true) : NULL;
    ok = ok && envelope && record && copy && cJSON_AddNumberToObject(envelope, "schema", 1) &&
         cJSON_AddBoolToObject(envelope, "committed", true) &&
         cJSON_AddStringToObject(record, "label", "restart") != NULL;
    if (ok && cJSON_AddItemToObject(envelope, "record", record)) {
        record = NULL;
    }
    else {
        ok = false;
    }
    if (ok && cJSON_AddItemToObject(envelope, "resume", copy)) {
        copy = NULL;
    }
    else {
        ok = false;
    }
    char *text = ok ? cJSON_PrintUnformatted(envelope) : NULL;
    bool published = false;
    ok = text && mainui_write_bytes_new_status(envelope_path, text, strlen(text), &published);
    if (!ok && published) {
        mainui_remove_file(envelope_path);
    }
    if (!ok && !*error) {
        snprintf(error, 256, "Cannot prepare the restart.");
    }
    free(text);
    cJSON_Delete(record);
    cJSON_Delete(copy);
    cJSON_Delete(envelope);
    mainui_file_unlock(lock);
    return ok;
}

cJSON *mainui_launch_take_return(const char *directory)
{
    char path[4096];
    if (!join(path, directory, "mainui-handoff")) {
        return NULL;
    }
    MainUIFileLock *lock = mainui_file_lock(path);
    cJSON *result = lock ? take_return_unlocked(directory) : NULL;
    mainui_file_unlock(lock);
    return result;
}
