/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/timing.h"
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static struct timespec marks[MAINUI_MARK_COUNT];
static bool valid[MAINUI_MARK_COUNT], enabled, reported;
static const char *const names[] = {"frames",      "roms",         "cache",          "cache-hits",
                                    "cache-scans", "scan-entries", "cache-build-ms", "scan-ms",
                                    "draw-ms",     "gap-under35",  "gap-40",         "gap-50-60",
                                    "gap-over65",  "discover-ms",  "icon-ms"};
static atomic_long counters[sizeof names / sizeof *names];
static char exchange[4096], boot_id[64];
static int64_t away_ms = -1;
/* Interim reports: last emission and the counters it showed. */
static struct timespec interim_at;
static long interim_counters[sizeof names / sizeof *names];

static int64_t elapsed(struct timespec from, struct timespec to)
{
    int64_t seconds = (int64_t)to.tv_sec - (int64_t)from.tv_sec;
    return (seconds * INT64_C(1000000000) + to.tv_nsec - from.tv_nsec) / 1000000;
}

static int64_t duration(MainUIMark from, MainUIMark to)
{
    return valid[from] && valid[to] ? elapsed(marks[from], marks[to]) : -1;
}

void mainui_mark(MainUIMark mark)
{
    if (mark < 0 || mark >= MAINUI_MARK_COUNT) {
        return;
    }
    if (mark == MAINUI_MARK_ENTRY) {
        struct stat output, null_device;
        enabled = !(fstat(STDOUT_FILENO, &output) == 0 && stat("/dev/null", &null_device) == 0 &&
                    S_ISCHR(output.st_mode) && output.st_rdev == null_device.st_rdev);
        reported = false;
        memset(valid, 0, sizeof valid);
        exchange[0] = boot_id[0] = 0;
        away_ms = -1;
        for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
            atomic_store_explicit(&counters[i], !strcmp(names[i], "cache") ? -1 : 0,
                                  memory_order_relaxed);
            interim_counters[i] = !strcmp(names[i], "cache") ? -1 : 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &interim_at);
    }
    if (!enabled || reported || (mark == MAINUI_MARK_FIRST_FRAME && valid[mark]) ||
        (mark == MAINUI_MARK_EVENT && valid[MAINUI_MARK_HANDOFF])) {
        return;
    }
    valid[mark] = clock_gettime(CLOCK_MONOTONIC, &marks[mark]) == 0;
}

static void counter(const char *name, long value, bool add)
{
    if (!enabled || !name) {
        return;
    }
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        if (!strcmp(name, names[i])) {
            if (add) {
                atomic_fetch_add_explicit(&counters[i], value, memory_order_relaxed);
            }
            else {
                atomic_store_explicit(&counters[i], value, memory_order_relaxed);
            }
            return;
        }
    }
}

void mainui_count(const char *name, long value)
{
    counter(name, value, false);
}

void mainui_count_add(const char *name, long value)
{
    counter(name, value, true);
}

struct timespec mainui_timing_start(void)
{
    struct timespec start = {-1, 0};
    if (enabled && clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        start.tv_sec = -1;
    }
    return start;
}

void mainui_timing_finish(const char *name, struct timespec start)
{
    struct timespec end;
    if (!enabled || start.tv_sec < 0 || clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return;
    }
    int64_t ms = elapsed(start, end);
    if (ms >= 0) {
        mainui_count_add(name, ms > LONG_MAX ? LONG_MAX : (long)ms);
    }
}

static bool current_boot(void)
{
    FILE *file = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!file) {
        return false;
    }
    bool ok = fgets(boot_id, sizeof boot_id, file) != NULL;
    fclose(file);
    boot_id[strcspn(boot_id, "\r\n")] = 0;
    return ok && strlen(boot_id) == 36;
}

void mainui_timing_handoff(const char *path)
{
    if (!enabled || !valid[MAINUI_MARK_ENTRY] || !path || strlen(path) >= sizeof exchange ||
        !current_boot()) {
        return;
    }
    strcpy(exchange, path);
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        return;
    }
    struct stat status;
    if (fstat(descriptor, &status) || !S_ISREG(status.st_mode) || status.st_uid != getuid() ||
        status.st_size > 128) {
        close(descriptor);
        return;
    }
    FILE *file = fdopen(descriptor, "r");
    if (!file) {
        close(descriptor);
        return;
    }
    char previous_boot[64];
    int64_t seconds;
    long nanoseconds;
    if (fscanf(file, "%63s %" SCNd64 " %ld", previous_boot, &seconds, &nanoseconds) == 3 &&
        !strcmp(previous_boot, boot_id) && seconds >= 0 &&
        seconds <= (int64_t)marks[MAINUI_MARK_ENTRY].tv_sec && nanoseconds >= 0 &&
        nanoseconds < 1000000000 &&
        (seconds < (int64_t)marks[MAINUI_MARK_ENTRY].tv_sec ||
         nanoseconds <= marks[MAINUI_MARK_ENTRY].tv_nsec)) {
        struct timespec previous = {(time_t)seconds, nanoseconds};
        int64_t interval = elapsed(previous, marks[MAINUI_MARK_ENTRY]);
        if (interval >= 0) {
            away_ms = interval;
        }
    }
    fclose(file);
    /* Consume once; a failed or ordinary run cannot reuse an older interval. */
    unlink(path);
}

static void save_handoff(void)
{
    if (!*exchange || !valid[MAINUI_MARK_HANDOFF] || !valid[MAINUI_MARK_EXIT]) {
        return;
    }
    int descriptor = open(exchange, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return;
    }
    struct stat status;
    if (fstat(descriptor, &status) || !S_ISREG(status.st_mode) || status.st_uid != getuid() ||
        ftruncate(descriptor, 0)) {
        close(descriptor);
        return;
    }
    FILE *file = fdopen(descriptor, "w");
    if (!file) {
        close(descriptor);
        return;
    }
    fprintf(file, "%s %" PRId64 " %ld\n", boot_id, (int64_t)marks[MAINUI_MARK_EXIT].tv_sec,
            marks[MAINUI_MARK_EXIT].tv_nsec);
    fclose(file); /* tmpfs diagnostic only; no fsync or SD write */
}

static long peak_rss(void)
{
    FILE *file = fopen("/proc/self/status", "r");
    if (!file) {
        return -1;
    }
    char line[256];
    long value = -1;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "VmHWM: %ld kB", &value) == 1) {
            break;
        }
    }
    fclose(file);
    return value;
}

/* " peak-rss N KB name value ..." for the current counters. */
static int format_counters(char *out, size_t size, long snapshot[])
{
    int used = snprintf(out, size, " peak-rss %ld KB", peak_rss());
    for (size_t i = 0; i < sizeof names / sizeof *names && used > 0 && (size_t)used < size; ++i) {
        snapshot[i] = atomic_load_explicit(&counters[i], memory_order_relaxed);
        used += snprintf(out + used, size - (size_t)used, " %s %ld", names[i], snapshot[i]);
    }
    return used;
}

void mainui_timing_interim(long interval_ms)
{
    if (!enabled || reported) {
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || elapsed(interim_at, now) < interval_ms) {
        return;
    }
    bool changed = false;
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        changed |= atomic_load_explicit(&counters[i], memory_order_relaxed) != interim_counters[i];
    }
    if (!changed) {
        return;
    }
    interim_at = now;
    char line[1024];
    int used = snprintf(line, sizeof line, "[timing] interim uptime-ms %" PRId64,
                        valid[MAINUI_MARK_ENTRY] ? elapsed(marks[MAINUI_MARK_ENTRY], now) : -1);
    used += format_counters(line + used, sizeof line - (size_t)used, interim_counters);
    if (used > 0 && (size_t)used < sizeof line - 1) {
        line[used++] = '\n';
        fwrite(line, 1, (size_t)used, stdout);
        fflush(stdout);
    }
}

void mainui_timing_report(void)
{
    if (!enabled || reported) {
        return;
    }
    reported = true;
    char report[2048];
    int used = snprintf(report, sizeof report,
                        "[timing] ms session %" PRId64 " video %" PRId64 " restore %" PRId64
                        " ready %" PRId64 " first-frame %" PRId64 " away %" PRId64 "\n",
                        duration(MAINUI_MARK_ENTRY, MAINUI_MARK_SESSION),
                        duration(MAINUI_MARK_SESSION, MAINUI_MARK_VIDEO),
                        duration(MAINUI_MARK_VIDEO, MAINUI_MARK_RESTORE),
                        duration(MAINUI_MARK_RESTORE, MAINUI_MARK_READY),
                        duration(MAINUI_MARK_ENTRY, MAINUI_MARK_FIRST_FRAME), away_ms);
    if (valid[MAINUI_MARK_HANDOFF]) {
        used += snprintf(report + used, sizeof report - (size_t)used,
                         "[timing] launch-ms key->handoff %" PRId64 " handoff->exit %" PRId64
                         " total %" PRId64 "\n",
                         duration(MAINUI_MARK_EVENT, MAINUI_MARK_HANDOFF),
                         duration(MAINUI_MARK_HANDOFF, MAINUI_MARK_EXIT),
                         duration(MAINUI_MARK_EVENT, MAINUI_MARK_EXIT));
    }
    used += snprintf(report + used, sizeof report - (size_t)used, "[timing]");
    used += format_counters(report + used, sizeof report - (size_t)used, interim_counters);
    report[used++] = '\n';
    fwrite(report, 1, (size_t)used, stdout);
    save_handoff();
}
