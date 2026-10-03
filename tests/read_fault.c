/* SPDX-License-Identifier: GPL-3.0-only */
/* Test preload: one read error on opening a chosen file. MAINUI_READ_FAULT
 * names a control file holding "<name> eio". The next fopen() of a path ending
 * in /<name> then fails with EIO and the control file is removed, so the
 * fault happens once. stat() is untouched: the file still looks the same. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static bool take_fault(const char *path)
{
    const char *control = getenv("MAINUI_READ_FAULT");
    if (!control || !path) {
        return false;
    }
    pthread_mutex_lock(&lock);
    bool fault = false;
    char text[512] = "", name[256], action[16];
    int fd = open(control, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, text, sizeof text - 1);
        close(fd);
        text[n > 0 ? n : 0] = 0;
    }
    if (sscanf(text, "%255s %15s", name, action) == 2 && !strcmp(action, "eio")) {
        size_t length = strlen(path), size = strlen(name);
        fault =
            length > size && path[length - size - 1] == '/' && !strcmp(path + length - size, name);
        if (fault) {
            unlink(control);
            fprintf(stderr, "read-fault: failing %s\n", name);
        }
    }
    pthread_mutex_unlock(&lock);
    return fault;
}

static FILE *faulty(const char *symbol, const char *path, const char *mode)
{
    FILE *(*next)(const char *, const char *) =
        (FILE * (*)(const char *, const char *)) dlsym(RTLD_NEXT, symbol);
    if (take_fault(path)) {
        errno = EIO;
        return NULL;
    }
    return next(path, mode);
}

FILE *fopen(const char *path, const char *mode)
{
    return faulty("fopen", path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    return faulty("fopen64", path, mode);
}
