/* SPDX-License-Identifier: GPL-3.0-only */
/* Test preload: ROM-list row queries for a page past the first fail with
 * SQLITE_IOERR, a cache failure that is not damaged content. Counting and
 * the first page still work, so a console opens normally. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sqlite3.h>
#include <stddef.h>

static sqlite3_stmt *failing;

int sqlite3_bind_int(sqlite3_stmt *statement, int index, int value)
{
    static int (*next)(sqlite3_stmt *, int, int);
    if (!next) {
        next = (int (*)(sqlite3_stmt *, int, int))dlsym(RTLD_NEXT, "sqlite3_bind_int");
    }
    /* mainui_cache_window() binds the page offset as parameter 2. */
    if (index == 2) {
        failing = value >= 64 ? statement : failing == statement ? NULL : failing;
    }
    return next(statement, index, value);
}

int sqlite3_step(sqlite3_stmt *statement)
{
    static int (*next)(sqlite3_stmt *);
    if (!next) {
        next = (int (*)(sqlite3_stmt *))dlsym(RTLD_NEXT, "sqlite3_step");
    }
    return statement && statement == failing ? SQLITE_IOERR : next(statement);
}
