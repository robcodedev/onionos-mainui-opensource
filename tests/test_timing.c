/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/timing.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *count_worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < 5000; ++i) {
        mainui_count_add("scan-entries", 1);
        mainui_count_add("cache-build-ms", 2);
        mainui_count_add("scan-ms", 3);
    }
    return NULL;
}

static void read_report(FILE *capture, char output[4096])
{
    assert(fflush(stdout) == 0);
    rewind(capture);
    size_t size = fread(output, 1, 4095, capture);
    output[size] = 0;
}

int mainui_suite_timing(void)
{
    assert(fflush(stdout) == 0);
    int saved_stdout = dup(STDOUT_FILENO);
    FILE *capture = tmpfile();
    assert(saved_stdout >= 0 && capture);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    mainui_mark(MAINUI_MARK_ENTRY);
    mainui_mark(MAINUI_MARK_SESSION);
    mainui_mark(MAINUI_MARK_VIDEO);
    mainui_mark(MAINUI_MARK_RESTORE);
    mainui_mark(MAINUI_MARK_READY);
    mainui_mark(MAINUI_MARK_FIRST_FRAME);
    mainui_mark(MAINUI_MARK_FIRST_FRAME);
    mainui_count("roms", 1043);
    mainui_count("cache", 1);
    mainui_count_add("frames", 12);
    mainui_count_add("draw-ms", 12);
    mainui_count_add("draw-ms", 8);
    mainui_count("unknown", 999);
    mainui_count(NULL, 0);
    pthread_t workers[4];
    for (int i = 0; i < 4; ++i) {
        assert(pthread_create(&workers[i], NULL, count_worker, NULL) == 0);
    }
    for (int i = 0; i < 4; ++i) {
        assert(pthread_join(workers[i], NULL) == 0);
    }
    assert(ftell(capture) == 0); /* No I/O from marks or counters. */
    char exchange[] = "/tmp/mainui-timing-test-XXXXXX";
    int descriptor = mkstemp(exchange);
    assert(descriptor >= 0);
    close(descriptor);
    mainui_timing_handoff(exchange);
    mainui_mark(MAINUI_MARK_EVENT);
    mainui_mark(MAINUI_MARK_HANDOFF);
    mainui_mark(MAINUI_MARK_EXIT);
    mainui_timing_report();
    mainui_timing_report();
    char output[4096];
    read_report(capture, output);
    assert(strstr(output, "[timing] ms session "));
    assert(strstr(output, "[timing] launch-ms "));
    assert(strstr(output, "frames 12 roms 1043 cache 1"));
    assert(strstr(output, "draw-ms 20"));
    assert(strstr(output, "scan-entries 20000 cache-build-ms 40000 scan-ms 60000"));
    assert(strstr(output, " discover-ms ") && strstr(output, " icon-ms "));
    assert(!strstr(output, "unknown"));
    assert(!strstr(strstr(output, "[timing] ms session ") + 1, "[timing] ms session "));
    /* A second logged session consumes the same-boot handoff once. */
    assert(ftruncate(fileno(capture), 0) == 0);
    rewind(capture);
    mainui_mark(MAINUI_MARK_ENTRY);
    mainui_timing_handoff(exchange);
    mainui_mark(MAINUI_MARK_EXIT);
    mainui_timing_report();
    read_report(capture, output);
    assert(strstr(output, "first-frame -1"));
    assert(!strstr(output, "launch-ms"));
    assert(strstr(output, "frames 0 roms 0 cache -1"));
    assert(strstr(output, "draw-ms 0"));
    assert(strstr(output, "cache-build-ms 0 scan-ms 0"));
#ifdef __linux__
    assert(!strstr(output, "away -1"));
    assert(access(exchange, F_OK) != 0);
#endif
    unlink(exchange);
    FILE *stale = fopen(exchange, "w");
    assert(stale && fputs("different-boot 0 0\n", stale) >= 0 && fclose(stale) == 0);
    assert(ftruncate(fileno(capture), 0) == 0);
    rewind(capture);
    mainui_mark(MAINUI_MARK_ENTRY);
    mainui_timing_handoff(exchange);
    mainui_mark(MAINUI_MARK_EXIT);
    mainui_timing_report();
    read_report(capture, output);
    assert(strstr(output, "away -1"));
    unlink(exchange);
    /* Exercise the duration helper without a scheduler-dependent sleep. */
    assert(ftruncate(fileno(capture), 0) == 0);
    rewind(capture);
    mainui_mark(MAINUI_MARK_ENTRY);
    struct timespec start = mainui_timing_start();
    assert(start.tv_sec >= 1);
    start.tv_sec--;
    mainui_timing_finish("scan-ms", start);
    mainui_timing_report();
    read_report(capture, output);
    const char *scan_time = strstr(output, "scan-ms ");
    long measured = -1;
    assert(scan_time && sscanf(scan_time, "scan-ms %ld", &measured) == 1);
    assert(measured >= 1000);
    /* Interim lines survive a SIGKILL: written only when counters changed,
     * and never after the final report. */
    assert(ftruncate(fileno(capture), 0) == 0);
    rewind(capture);
    mainui_mark(MAINUI_MARK_ENTRY);
    mainui_timing_interim(0);
    read_report(capture, output);
    assert(!*output);
    mainui_count_add("frames", 7);
    mainui_count_add("gap-40", 5);
    mainui_timing_interim(60000);
    read_report(capture, output);
    assert(!*output);
    mainui_timing_interim(0);
    read_report(capture, output);
    assert(strstr(output, "[timing] interim uptime-ms ") && strstr(output, " frames 7 ") &&
           strstr(output, " gap-40 5 "));
    size_t first = strlen(output);
    mainui_timing_interim(0);
    read_report(capture, output);
    assert(strlen(output) == first);
    mainui_count_add("frames", 1);
    mainui_timing_interim(0);
    read_report(capture, output);
    assert(strstr(output + first, " frames 8 "));
    mainui_timing_report();
    read_report(capture, output);
    assert(strstr(output, "[timing] peak-rss ") && strstr(output, " frames 8 "));
    size_t reported = strlen(output);
    mainui_count_add("frames", 1);
    mainui_timing_interim(0);
    read_report(capture, output);
    assert(strlen(output) == reported);
    /* Default wrapper routing suppresses collection as well as the report. */
    int null_output = open("/dev/null", O_WRONLY);
    assert(null_output >= 0 && dup2(null_output, STDOUT_FILENO) >= 0);
    close(null_output);
    mainui_mark(MAINUI_MARK_ENTRY);
    assert(mainui_timing_start().tv_sec == -1);
    mainui_timing_finish("scan-ms", (struct timespec){0, 0});
    mainui_count_add("frames", 99);
    assert(ftruncate(fileno(capture), 0) == 0);
    rewind(capture);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    mainui_timing_interim(0);
    mainui_timing_report();
    read_report(capture, output);
    assert(!*output);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    fclose(capture);
    return 0;
}
