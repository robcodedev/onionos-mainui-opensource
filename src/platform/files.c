#define _POSIX_C_SOURCE 200809L
/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/files.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* ROMs over 2 GiB exist (multi-disc PBP). With a 32-bit off_t, stat() fails
 * with EOVERFLOW on the armhf device and those files look missing. */
_Static_assert(sizeof(off_t) == 8, "build with -D_FILE_OFFSET_BITS=64");

char *mainui_read_text(const char *path, size_t max_bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return NULL;
    }
    long length = ftell(f);
    if (length < 0 || (unsigned long)length > max_bytes || fseek(f, 0, SEEK_SET)) {
        int error = length >= 0 && (unsigned long)length > max_bytes ? EFBIG : EIO;
        fclose(f);
        errno = error;
        return NULL;
    }
    char *text = malloc((size_t)length + 1);
    if (!text) {
        fclose(f);
        errno = ENOMEM;
        return NULL;
    }
    size_t n = fread(text, 1, (size_t)length, f);
    bool ok = n == (size_t)length && !ferror(f);
    int error = ok && memchr(text, 0, n) ? EINVAL : EIO;
    ok = ok && error != EINVAL;
    fclose(f);
    if (!ok) {
        free(text);
        errno = error;
        return NULL;
    }
    text[n] = 0;
    return text;
}

#ifdef MAINUI_TEST_FAULTS
void mainui_test_fault(const char *point)
{
    const char *requested = getenv("MAINUI_TEST_FAULT");
    if (requested && !strcmp(point, requested)) {
        _Exit(77);
    }
}

#define FAULT(point) mainui_test_fault(point)
#else
#define FAULT(point) ((void)0)
#endif

static bool absent(const char *path)
{
    struct stat info;
    return lstat(path, &info) && errno == ENOENT;
}

bool mainui_temporary_path(char out[4096], const char *target, const char *tag)
{
    static atomic_uint serial = 0;
    char legacy[4096];
    int n = snprintf(legacy, sizeof legacy, "%s.%s", target, tag);
    if (n < 0 || n >= (int)sizeof legacy || !absent(legacy)) {
        return false;
    }
    unsigned process = (unsigned)getpid();
    for (int attempt = 0; attempt < 32; attempt++) {
        n = snprintf(out, 4096, "%s.%u.%u", legacy, process,
                     atomic_fetch_add_explicit(&serial, 1, memory_order_relaxed));
        if (n < 0 || n >= 4096) {
            return false;
        }
        if (absent(out)) {
            return true;
        }
    }
    return false;
}

static MainUIWriteResult publish_bytes(const char *path, const void *data, size_t size,
                                       bool replace, bool *published)
{
    if (published) {
        *published = false;
    }
    char temporary[4096];
    if (!data || size > 16u * 1024u * 1024u || !mainui_temporary_path(temporary, path, "writing")) {
        fprintf(stderr, "[write] cannot save %s: %s\n", path,
                !data || size > 16u * 1024u * 1024u ? "no data, or too large"
                                                    : "no temporary file name is free");
        return MAINUI_WRITE_UNCHANGED;
    }
    /* Exclusive reservation avoids truncating a concurrent writer's temporary. */
    bool ok = false, owned = false;
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    FILE *file = descriptor >= 0 ? fdopen(descriptor, "wb") : NULL;
    owned = descriptor >= 0;
    if (descriptor >= 0 && !file) {
        close(descriptor);
    }
    if (file) {
        FAULT("created");
        ok = fwrite(data, 1, size, file) == size && !fflush(file) && !fsync(descriptor);
        if (ok) {
            FAULT("flushed");
        }
        if (fclose(file)) {
            ok = false;
        }
        if (ok) {
            ok = replace ? rename(temporary, path) == 0 : link(temporary, path) == 0;
            if (published) {
                *published = ok;
            }
            if (ok && !replace) {
                mainui_remove_file(temporary);
            }
        }
    }
    if (!ok) {
        int error = errno;
        if (owned) {
            mainui_remove_file(temporary);
        }
        /* A new file that exists already is an expected outcome, not a fault. */
        if (replace || error != EEXIST) {
            fprintf(stderr, "[write] cannot save %s: %s\n", path, strerror(error));
        }
        errno = error;
        return MAINUI_WRITE_UNCHANGED;
    }
    FAULT("published");
    /* The new file is visible from here on; only its durability is in doubt. */
    return mainui_sync_parent(path) ? MAINUI_WRITE_DURABLE : MAINUI_WRITE_NOT_DURABLE;
}

/* Visible but possibly not durable still counts as saved: the old contents are
 * gone, so reporting failure would misdescribe the file. Log the difference. */
static bool published_result(const char *path, MainUIWriteResult result)
{
    if (result == MAINUI_WRITE_NOT_DURABLE) {
        fprintf(stderr, "Saved %s, but flushing its folder failed: %s\n", path, strerror(errno));
    }
    return result != MAINUI_WRITE_UNCHANGED;
}

MainUIWriteResult mainui_write_text_atomic_result(const char *path, const char *text)
{
    return text ? publish_bytes(path, text, strlen(text), true, NULL) : MAINUI_WRITE_UNCHANGED;
}

bool mainui_write_text_atomic(const char *path, const char *text)
{
    return published_result(path, mainui_write_text_atomic_result(path, text));
}

bool mainui_write_bytes_new(const char *path, const void *data, size_t size)
{
    return published_result(path, publish_bytes(path, data, size, false, NULL));
}

bool mainui_write_bytes_new_status(const char *path, const void *data, size_t size, bool *published)
{
    return publish_bytes(path, data, size, false, published) == MAINUI_WRITE_DURABLE;
}

bool mainui_write_bytes_new_locked(const char *path, const void *data, size_t size)
{
    return absent(path) && publish_bytes(path, data, size, true, NULL) == MAINUI_WRITE_DURABLE;
}

bool mainui_move_file_new_locked(const char *source, const char *destination)
{
    if (!absent(destination) || rename(source, destination)) {
        return false;
    }
    bool destination_synced = mainui_sync_parent(destination);
    bool source_synced = mainui_sync_parent(source);
    return destination_synced && source_synced;
}

bool mainui_move_file_new(const char *source, const char *destination)
{
    if (link(source, destination)) {
        return false;
    }
    if (!unlink(source)) {
        return mainui_sync_parent(destination) && mainui_sync_parent(source);
    }
    unlink(destination);
    return false;
}

bool mainui_path_within(const char *path, const char *root)
{
    if (!path || !root || !*root) {
        return false;
    }
    size_t base = strlen(root);
    while (base > 1 && root[base - 1] == '/') {
        --base;
    }
    return !strncmp(path, root, base) &&
           (path[base] == '/' || (base == 1 && root[0] == '/' && path[1])) &&
           !strstr(path + base, "/../") && !strstr(path + base, "/./") &&
           strcmp(strrchr(path, '/') ? strrchr(path, '/') + 1 : path, "..") &&
           strcmp(strrchr(path, '/') ? strrchr(path, '/') + 1 : path, ".");
}

static bool existing_path_within(const char *path, const char *root, bool directory)
{
    size_t base = strlen(root), length = strlen(path);
    if (length >= 4096 || !base || strncmp(path, root, base) || path[base] != '/' ||
        strstr(path + base, "/../") || strstr(path + base, "/./")) {
        return false;
    }
    char prefix[4096];
    memcpy(prefix, path, length + 1);
    for (size_t i = base; i <= length; i++) {
        if (i != length && prefix[i] != '/') {
            continue;
        }
        char saved = prefix[i];
        prefix[i] = 0;
        struct stat info;
        bool ok = !lstat(prefix, &info) &&
                  ((i != length || directory) ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
        prefix[i] = saved;
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool mainui_regular_file_within(const char *path, const char *root)
{
    return existing_path_within(path, root, false);
}

bool mainui_directory_within(const char *path, const char *root)
{
    return existing_path_within(path, root, true);
}

MainUIFileStamp mainui_file_stamp(const char *path)
{
    MainUIFileStamp stamp = {0};
    struct stat info;
    if (!stat(path, &info)) {
        stamp.exists = true;
        stamp.size = (uint64_t)info.st_size;
        stamp.modified =
            (uint64_t)info.st_mtim.tv_sec * 1000000000u + (uint64_t)info.st_mtim.tv_nsec;
        stamp.identity = (uint64_t)info.st_ino;
    }
    return stamp;
}

bool mainui_file_stamp_equal(MainUIFileStamp a, MainUIFileStamp b)
{
    return a.exists == b.exists && a.size == b.size && a.modified == b.modified &&
           a.identity == b.identity;
}

struct MainUIFileLock {
    int file;
};

static uint64_t lock_hash(const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    }
    return hash;
}

MainUIFileLock *mainui_file_lock(const char *target)
{
    if (!target || (mkdir("/tmp/mainui-locks", 0700) != 0 && errno != EEXIST)) {
        return NULL;
    }
    char path[64];
    int n = snprintf(path, sizeof path, "/tmp/mainui-locks/%016llx.lock",
                     (unsigned long long)lock_hash(target));
    if (n < 0 || n >= (int)sizeof path) {
        return NULL;
    }
    MainUIFileLock *lock = malloc(sizeof *lock);
    if (!lock) {
        return NULL;
    }
    lock->file = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock->file < 0 || flock(lock->file, LOCK_EX | LOCK_NB) < 0) {
        if (lock->file >= 0) {
            close(lock->file);
        }
        free(lock);
        return NULL;
    }
    return lock;
}

void mainui_file_unlock(MainUIFileLock *lock)
{
    if (!lock) {
        return;
    }
    close(lock->file);
    free(lock);
}

bool mainui_sync_parent(const char *path)
{
#ifdef MAINUI_TEST_FAULTS
    /* Fail one selected directory flush, allowing cleanup to sync afterward. */
    static bool failed;
    const char *requested = getenv("MAINUI_TEST_SYNC_FAILURE");
    const char *name = path ? strrchr(path, '/') : NULL;
    name = name ? name + 1 : path;
    if (!failed && requested && name && !strcmp(requested, name)) {
        failed = true;
        errno = EIO;
        return false;
    }
#endif
    char directory[4096];
    if (!path || strlen(path) >= sizeof directory) {
        return false;
    }
    strcpy(directory, path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = 0;
    }
    else {
        strcpy(directory, ".");
    }
    int parent = open(*directory ? directory : "/", O_RDONLY | O_CLOEXEC);
    bool ok = parent >= 0 && fsync(parent) == 0;
    if (parent >= 0) {
        close(parent);
    }
    return ok;
}

int mainui_remove_file(const char *path)
{
    bool removed;
    return mainui_remove_file_status(path, &removed);
}

int mainui_remove_file_status(const char *path, bool *removed)
{
    *removed = remove(path) == 0;
    return *removed && mainui_sync_parent(path) ? 0 : -1;
}
