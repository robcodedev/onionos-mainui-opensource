/* SPDX-License-Identifier: GPL-3.0-only */
#include "app/device_job.h"
#include <stdio.h>
#include <string.h>

static bool cancelled(void *context)
{
    return atomic_load(&((MainUIDeviceJob *)context)->cancel);
}

static int perform(void *context)
{
    MainUIDeviceJob *job = context;
    MainUICancel cancel = {cancelled, job};
    job->success = job->operation == 5 || job->operation == 6
                       ? mainui_device_wifi_enable(&job->adapter, job->operation == 5, cancel)
                   : job->operation == 3
                       ? mainui_device_connect(&job->adapter, job->ssid, job->password, cancel)
                   : job->operation == 4 ? mainui_device_scan(&job->adapter, cancel)
                                         : mainui_device_refresh(&job->adapter, cancel);
    memset(job->password, 0, sizeof job->password);
    atomic_store_explicit(&job->done, true, memory_order_release);
    SDL_Event event = {.type = SDL_USEREVENT};
    /* A status refresh only changes the header, which repaint checks compare. */
    event.user.code = job->operation == 0 ? MAINUI_STATUS_CODE : 0;
    SDL_PushEvent(&event);
    return 0;
}

bool mainui_device_job_start(MainUIDeviceJob *job, const MainUIDeviceAdapter *adapter,
                             int operation, const char *ssid, const char *password)
{
    if ((operation != 0 && operation != 3 && operation != 4 && operation != 5 && operation != 6) ||
        (ssid && strlen(ssid) > 32) || (password && strlen(password) > 63)) {
        return false;
    }
    if (job->thread) {
        bool toggle = operation == 5 || operation == 6;
        bool connect_after_scan = operation == 3 && job->operation == 4;
        if (!toggle &&
            ((!connect_after_scan && job->operation != 0) || !operation || job->queued_operation)) {
            return false;
        }
        /* Power transitions finish before the latest toggle runs. Cancelling
         * midway can leave a powered-down radio with a live supplicant. */
        job->queued_operation = operation;
        memset(job->queued_ssid, 0, sizeof job->queued_ssid);
        memset(job->queued_password, 0, sizeof job->queued_password);
        if (ssid) {
            strcpy(job->queued_ssid, ssid);
        }
        if (password) {
            strcpy(job->queued_password, password);
        }
        if (job->operation != 5 && job->operation != 6) {
            atomic_store(&job->cancel, true);
        }
        return true;
    }
    *job = (MainUIDeviceJob){.adapter = *adapter, .operation = operation};
    *job->adapter.error = 0;
    atomic_init(&job->cancel, false);
    atomic_init(&job->done, false);
    if (ssid) {
        strcpy(job->ssid, ssid);
    }
    if (password) {
        strcpy(job->password, password);
    }
    job->thread = SDL_CreateThread(perform, job);
    if (!job->thread) {
        memset(job->password, 0, sizeof job->password);
    }
    return job->thread != NULL;
}

int mainui_device_job_take(MainUIDeviceJob *job)
{
    if (!job->thread || !atomic_load_explicit(&job->done, memory_order_acquire)) {
        return -1;
    }
    SDL_WaitThread(job->thread, NULL);
    job->thread = NULL;
    if (job->queued_operation) {
        int operation = job->queued_operation;
        char ssid[33], password[64];
        strcpy(ssid, job->queued_ssid);
        strcpy(password, job->queued_password);
        bool started = mainui_device_job_start(job, &job->adapter, operation, ssid, password);
        memset(password, 0, sizeof password);
        return started ? -1 : 0;
    }
    return job->success && !atomic_load(&job->cancel);
}

/* Upper bound for finishing Wi-Fi power work at exit or launch. Normal
 * transitions take well under a second; this only caps a stuck helper.
 * After cancellation the join is bounded by the adapter, not here: every
 * wait in device_adapter.c checks the cancel flag or ends within about a
 * second (a socket reply, a helper's SIGTERM/SIGKILL grace). A transport
 * that ignores cancellation delays close by its own duration. */
#define DEVICE_CLOSE_BUDGET_MS 5000u

static bool close_deadline_passed(void *context)
{
    const Uint32 *deadline = context;
    return (Sint32)(SDL_GetTicks() - *deadline) >= 0;
}

void mainui_device_job_close(MainUIDeviceJob *job)
{
    Uint32 started = SDL_GetTicks();
    Uint32 deadline = started + DEVICE_CLOSE_BUDGET_MS;
    if (job->thread) {
        if (job->operation != 5 && job->operation != 6) {
            atomic_store(&job->cancel, true);
        }
        /* Let a power transition finish, but not indefinitely. */
        while (!atomic_load_explicit(&job->done, memory_order_acquire) &&
               !close_deadline_passed(&deadline)) {
            SDL_Delay(10);
        }
        if (!atomic_load_explicit(&job->done, memory_order_acquire)) {
            fprintf(stderr, "Wi-Fi power change exceeded %u ms at exit; cancelling\n",
                    DEVICE_CLOSE_BUDGET_MS);
            atomic_store(&job->cancel, true);
        }
        SDL_WaitThread(job->thread, NULL);
    }
    /* Settings already persisted this request. Finish the last requested power
     * state even when it was queued behind a cancelled scan/connect, within
     * whatever remains of the budget. */
    if (job->queued_operation == 5 || job->queued_operation == 6) {
        bool enable = job->queued_operation == 5;
        if (close_deadline_passed(&deadline) ||
            !mainui_device_wifi_enable(&job->adapter, enable,
                                       (MainUICancel){close_deadline_passed, &deadline})) {
            fprintf(stderr, "Queued Wi-Fi %s did not complete at exit\n",
                    enable ? "enable" : "disable");
        }
    }
    Uint32 elapsed = SDL_GetTicks() - started;
    if (elapsed > 500) {
        fprintf(stderr, "Device job close took %u ms\n", (unsigned)elapsed);
    }
    memset(job, 0, sizeof *job);
}
