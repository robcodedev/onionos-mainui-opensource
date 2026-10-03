/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "platform/device_adapter.h"
#include "platform/device_request.h"
#include "platform/files.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef MAINUI_ONION
#include "shmvar/shmvar.h"
#endif
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static bool path(char out[4096], const MainUIDeviceAdapter *adapter, const char *name)
{
    int n = snprintf(out, 4096, "%s/%s", adapter->runtime, name);
    return n > 0 && n < 4096;
}

static int number(const char *file)
{
    /* sysfs attributes may report a zero/4096 size: read the bounded stream. */
    char text[34], *end = NULL;
    FILE *stream = fopen(file, "rb");
    if (!stream) {
        return -1;
    }
    size_t count = fread(text, 1, sizeof text - 1, stream);
    bool readable = !ferror(stream) && count > 0 && count <= 32 && !memchr(text, 0, count);
    fclose(stream);
    if (!readable) {
        return -1;
    }
    text[count] = 0;
    errno = 0;
    long value = strtol(text, &end, 10);
    while (end && (*end == '\r' || *end == '\n' || *end == ' ')) {
        ++end;
    }
    bool ok = end != text && end && !*end && !errno && value >= 0 && value <= INT_MAX;
    return ok ? (int)value : -1;
}

static int runtime_number(const MainUIDeviceAdapter *adapter, const char *name)
{
    char file[4096];
    return path(file, adapter, name) ? number(file) : -1;
}

static void field(char *out, size_t size, const char *text, const char *key)
{
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line), prefix = strlen(key);
        if (length >= prefix && !memcmp(line, key, prefix)) {
            length -= prefix;
            if (length && line[prefix + length - 1] == '\r') {
                --length;
            }
            if (length < size) {
                memcpy(out, line + prefix, length);
                out[length] = 0;
            }
            return;
        }
        line = end ? end + 1 : NULL;
    }
}

static atomic_uint serial;

static bool unix_command(void *unused, const char *command, char *reply, size_t capacity,
                         MainUICancel cancel)
{
    (void)unused;
    if (capacity < 2 || mainui_cancelled(cancel)) {
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }
    /* Match wpa_cli: older device daemons expect a filesystem client address. */
    struct sockaddr_un local = {.sun_family = AF_UNIX}, remote = {.sun_family = AF_UNIX};
    int n = snprintf(local.sun_path, sizeof local.sun_path, "/tmp/mainui-wpa-%ld-%u",
                     (long)getpid(), atomic_fetch_add(&serial, 1));
    snprintf(remote.sun_path, sizeof remote.sun_path, "/var/run/wpa_supplicant/wlan0");
    bool ok = n > 0 && n < (int)sizeof local.sun_path &&
              bind(fd, (struct sockaddr *)&local,
                   (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1)) == 0 &&
              connect(fd, (struct sockaddr *)&remote, sizeof remote) == 0 &&
              send(fd, command, strlen(command), 0) == (ssize_t)strlen(command);
    bool ready = false;
    for (int i = 0; ok && !mainui_cancelled(cancel) && i < 20; ++i) {
        struct pollfd event = {.fd = fd, .events = POLLIN};
        int result = poll(&event, 1, 50);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 || event.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ok = false;
            break;
        }
        if (event.revents & POLLIN) {
            ready = true;
            break;
        }
    }
    ok = ok && ready && !mainui_cancelled(cancel);
    if (ok) {
        ssize_t size = recv(fd, reply, capacity - 1, MSG_TRUNC);
        ok = size > 0 && (size_t)size < capacity - 1;
        if (ok) {
            reply[size] = 0;
            ok = !memchr(reply, 0, (size_t)size);
        }
    }
    close(fd);
    if (n > 0 && n < (int)sizeof local.sun_path) {
        unlink(local.sun_path);
    }
    return ok;
}

static bool wifi_process(const char *name, bool stop)
{
    DIR *directory = opendir("/proc");
    if (!directory) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 1 || pid > INT_MAX) {
            continue;
        }
        char filename[128], command[64] = "";
        snprintf(filename, sizeof filename, "/proc/%ld/comm", pid);
        FILE *file = fopen(filename, "r");
        if (!file) {
            continue;
        }
        bool read = fgets(command, sizeof command, file) != NULL;
        fclose(file);
        command[strcspn(command, "\r\n")] = 0;
        if (read && !strcmp(command, name)) {
            found = true;
            if (stop) {
                kill((pid_t)pid, SIGTERM);
            }
        }
    }
    closedir(directory);
    return found;
}

#define SPAWN_CLOSE_LIMIT 65536

/* Signals SDL blocks in its threads, plus SIGPIPE, which may be ignored.
 * Children get default dispositions and an empty mask. */
static const int spawn_signals[] = {SIGHUP,  SIGINT,  SIGQUIT,  SIGPIPE,   SIGALRM,
                                    SIGTERM, SIGCHLD, SIGWINCH, SIGVTALRM, SIGPROF};

/* Resolve PATH before fork: the child only uses async-signal-safe calls. */
static bool program_path(char out[4096], const char *program)
{
    if (strchr(program, '/')) {
        int n = snprintf(out, 4096, "%s", program);
        return n > 0 && n < 4096 && access(out, X_OK) == 0;
    }
    const char *search = getenv("PATH");
    if (!search) {
        search = "/usr/sbin:/usr/bin:/sbin:/bin";
    }
    for (const char *part = search;;) {
        const char *end = strchr(part, ':');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (length < 4096) {
            int n = length ? snprintf(out, 4096, "%.*s/%s", (int)length, part, program)
                           : snprintf(out, 4096, "./%s", program);
            if (n > 0 && n < 4096 && access(out, X_OK) == 0) {
                return true;
            }
        }
        if (!end) {
            return false;
        }
        part = end + 1;
    }
}

static bool wifi_program(char *const arguments[], MainUICancel cancel)
{
    if (mainui_cancelled(cancel)) {
        return false;
    }
    char executable[4096];
    if (!program_path(executable, arguments[0])) {
        return false;
    }
    /* Descriptors are allocated lowest-first, so the soft limit bounds every
     * handle MainUI opened. Cap the fallback loop for huge or unlimited limits. */
    int close_limit = SPAWN_CLOSE_LIMIT;
    struct rlimit limit;
    if (!getrlimit(RLIMIT_NOFILE, &limit) && limit.rlim_cur != RLIM_INFINITY &&
        limit.rlim_cur < SPAWN_CLOSE_LIMIT) {
        close_limit = (int)limit.rlim_cur;
    }
    /* Prepared before fork so the child only makes async-signal-safe calls.
     * SDL threads block SIGTERM, SIGCHLD and others, and fork/exec keep that
     * mask, so daemons started here would otherwise ignore SIGTERM. */
    struct sigaction reset = {.sa_handler = SIG_DFL};
    sigset_t empty;
    sigemptyset(&reset.sa_mask);
    sigemptyset(&empty);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0) {
        return false;
    }
    pid_t child = fork();
    if (child == 0) {
        for (size_t i = 0; i < sizeof spawn_signals / sizeof spawn_signals[0]; ++i) {
            sigaction(spawn_signals[i], &reset, NULL);
        }
        sigprocmask(SIG_SETMASK, &empty, NULL);
        if (dup2(null, STDIN_FILENO) < 0 || dup2(null, STDOUT_FILENO) < 0 ||
            dup2(null, STDERR_FILENO) < 0) {
            _exit(127);
        }
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
        {
            /* Linux 4.9 lacks close_range; close handles explicitly there. */
            for (int fd = 3; fd < close_limit; ++fd) {
                close(fd);
            }
        }
        execve(executable, arguments, environ);
        _exit(127);
    }
    close(null);
    if (child < 0) {
        return false;
    }
    int status = 0;
    for (int i = 0; i < 60 && !mainui_cancelled(cancel); ++i) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (done < 0 && errno != EINTR) {
            return false;
        }
        poll(NULL, 0, 50);
    }
    kill(child, SIGTERM);
    for (int i = 0; i < 10; ++i) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child || (done < 0 && errno != EINTR)) {
            return false;
        }
        poll(NULL, 0, 20);
    }
    /* A child stuck in an uninterruptible kernel call (a hung Wi-Fi driver,
     * say) ignores even SIGKILL. Do not block on it: closing the device job
     * at exit or launch waits for this worker. An unreaped child is adopted
     * by init when MainUI exits. */
    kill(child, SIGKILL);
    for (int i = 0; i < 25; ++i) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child || (done < 0 && errno != EINTR)) {
            return false;
        }
        poll(NULL, 0, 20);
    }
    fprintf(stderr, "Wi-Fi helper %s did not exit after SIGKILL\n", arguments[0]);
    return false;
}

static bool wifi_configuration(void)
{
    const char *file = "/appconfigs/wpa_supplicant.conf";
    MainUIFileLock *lock = mainui_file_lock(file);
    if (!lock) {
        return false;
    }
    char *old = mainui_read_text(file, 256 * 1024);
    bool missing = !mainui_file_stamp(file).exists;
    bool ok = old || missing;
    bool control = false, update = false;
    if (old) {
        for (const char *line = old; *line;) {
            const char *end = strchr(line, '\n');
            const char *p = line;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            control |= !strncmp(p, "ctrl_interface=", 15);
            update |= !strncmp(p, "update_config=", 14);
            if (!end) {
                break;
            }
            line = end + 1;
        }
    }
    if (ok && (!control || !update)) {
        size_t size = (old ? strlen(old) : 0) + 100;
        char *text = malloc(size);
        ok = text != NULL;
        if (ok) {
            snprintf(text, size, "%s%s%s",
                     control ? "" : "ctrl_interface=/var/run/wpa_supplicant\n",
                     update ? "" : "update_config=1\n", old ? old : "");
            ok = mainui_write_text_atomic(file, text);
        }
        free(text);
    }
    free(old);
    mainui_file_unlock(lock);
    return ok;
}

static bool wifi_dhcp(MainUICancel cancel, bool restart)
{
    if (mainui_cancelled(cancel)) {
        return false;
    }
    if (restart) {
        wifi_process("udhcpc", true);
        for (int i = 0; i < 40 && wifi_process("udhcpc", false); ++i) {
            if (mainui_cancelled(cancel)) {
                return false;
            }
            poll(NULL, 0, 50);
        }
        if (wifi_process("udhcpc", false)) {
            return false;
        }
    }
    /* Also reached by scans: intentionally self-heal a missing DHCP client. */
    else if (wifi_process("udhcpc", false)) {
        return true;
    }
    /* Stock starts DHCP asynchronously with the default three 3-second tries.
     * A fixed shell command keeps lease acquisition out of the status worker. */
    char *arguments[] = {"/bin/sh", "-c",
                         "command -v udhcpc >/dev/null || exit 1; "
                         "udhcpc -i wlan0 -s /etc/init.d/udhcpc.script &",
                         NULL};
    return wifi_program(arguments, cancel);
}

static volatile sig_atomic_t resumed, quitting;

static void signal_event(int event)
{
    if (event == SIGCONT) {
        resumed = 1;
    }
    else {
        quitting = 1;
    }
}

bool mainui_device_adapter_open(MainUIDeviceAdapter *adapter, MainUIDeviceBackend backend,
                                const char *runtime)
{
    if (!adapter || !runtime || !*runtime || strlen(runtime) >= sizeof adapter->runtime) {
        return false;
    }
    *adapter = (MainUIDeviceAdapter){.backend = backend};
    strcpy(adapter->runtime, runtime);
    if (backend == DEVICE_ONION) {
        adapter->transport = unix_command;
    }
    return true;
}

int mainui_wifi_signal_level(int dbm)
{
    return dbm < -126 || dbm > 127 ? 0 : dbm >= -70 ? 3 : dbm >= -80 ? 2 : 1;
}

MainUIDeviceStatus mainui_device_status(const MainUIDeviceAdapter *adapter)
{
    MainUIDeviceStatus status = {.battery = -1, .wifi = -1, .lid = -1, .sleeping = -1};
    status.model = mainui_device(runtime_number(adapter, "deviceModel"));
    int battery = runtime_number(adapter, "percBat");
    if ((battery >= 0 && battery <= 100) || battery == 500) {
        status.battery = battery;
    }
    int charging = adapter->backend == DEVICE_SIMULATED ? runtime_number(adapter, "charging")
                   : status.model.id == 283             ? number("/sys/class/gpio/gpio59/value")
                                                        : -1;
    if (charging == 1) {
        status.battery = 500;
    }
    if (status.model.lid) {
        int lid = adapter->backend == DEVICE_SIMULATED
                      ? runtime_number(adapter, "lid")
                      : number("/sys/devices/soc0/soc/soc:hall-mh248/hallvalue");
        if (lid == 0 || lid == 1) {
            status.lid = lid;
        }
    }
    if (adapter->backend == DEVICE_SIMULATED) {
        int sleeping = runtime_number(adapter, "sleeping");
        if (sleeping == 0 || sleeping == 1) {
            status.sleeping = sleeping;
        }
    }
    if (status.model.wifi) {
        char file[4096];
        char *text = path(file, adapter, "wifi-status.txt") ? mainui_read_text(file, 16384) : NULL;
        if (text) {
            char state[32] = "";
            field(state, sizeof state, text, "wpa_state=");
            if (!strcmp(state, "COMPLETED")) {
                status.wifi = 1;
            }
            else if (!strcmp(state, "DISCONNECTED") || !strcmp(state, "INACTIVE") ||
                     !strcmp(state, "INTERFACE_DISABLED") || !strcmp(state, "SCANNING") ||
                     !strcmp(state, "AUTHENTICATING") || !strcmp(state, "ASSOCIATING") ||
                     !strcmp(state, "ASSOCIATED") || !strcmp(state, "4WAY_HANDSHAKE") ||
                     !strcmp(state, "GROUP_HANDSHAKE")) {
                status.wifi = 0;
            }
            char rssi[32] = "", *end;
            field(rssi, sizeof rssi, text, "RSSI=");
            errno = 0;
            long dbm = strtol(rssi, &end, 10);
            if (*rssi && !*end && !errno && dbm >= -126 && dbm <= 127) {
                status.wifi_signal = mainui_wifi_signal_level((int)dbm);
            }
            field(status.ssid, sizeof status.ssid, text, "ssid=");
            field(status.address, sizeof status.address, text, "ip_address=");
        }
        free(text);
    }
    return status;
}

static bool exchange(MainUIDeviceAdapter *adapter, const char *command, const char *expected,
                     MainUICancel cancel)
{
    char reply[128];
    return adapter->transport &&
           adapter->transport(adapter->transport_context, command, reply, sizeof reply, cancel) &&
           !strcmp(reply, expected);
}

bool mainui_device_wifi_enable(MainUIDeviceAdapter *adapter, bool enabled, MainUICancel cancel)
{
    *adapter->error = 0;
    if (adapter->backend == DEVICE_SIMULATED) {
        return !mainui_cancelled(cancel);
    }
    if (!mainui_device_status(adapter).model.wifi || mainui_cancelled(cancel)) {
        return false;
    }
    if (!enabled) {
        wifi_process("wpa_supplicant", true);
        wifi_process("udhcpc", true);
        /* Let the old supplicant release its control socket before another
         * enable can observe it and mistake it for a ready service. */
        for (int i = 0;
             i < 40 && !mainui_cancelled(cancel) && wifi_process("wpa_supplicant", false); ++i) {
            poll(NULL, 0, 50);
        }
        char status_file[4096];
        if (path(status_file, adapter, "wifi-status.txt")) {
            mainui_write_text_atomic(status_file, "wpa_state=INTERFACE_DISABLED\n");
        }
        char *down[] = {"ifconfig", "wlan0", "down", NULL};
        bool ok = wifi_program(down, cancel);
        char *power[] = {"/customer/app/axp_test", "wifioff", NULL};
        wifi_program(power, cancel);
        return ok;
    }
    char reply[64];
    if (adapter->transport &&
        adapter->transport(adapter->transport_context, "PING", reply, sizeof reply, cancel) &&
        !strcmp(reply, "PONG\n")) {
        return wifi_dhcp(cancel, false);
    }
    if (!wifi_configuration()) {
        snprintf(adapter->error, sizeof adapter->error, "Cannot prepare Wi-Fi configuration.");
        return false;
    }
    char *power[] = {"/customer/app/axp_test", "wifion", NULL};
    if (!wifi_program(power, cancel)) {
        snprintf(adapter->error, sizeof adapter->error, "Could not power on Wi-Fi.");
        return false;
    }
    /* Onion wifi_on waits two seconds for the radio/driver before ifconfig. */
    for (int i = 0; i < 40 && !mainui_cancelled(cancel); ++i) {
        poll(NULL, 0, 50);
    }
    if (mainui_cancelled(cancel)) {
        return false;
    }
    char *up[] = {"ifconfig", "wlan0", "up", NULL};
    if (!wifi_program(up, cancel)) {
        snprintf(adapter->error, sizeof adapter->error, "Cannot bring wlan0 up.");
        return false;
    }
    if (!wifi_process("wpa_supplicant", false)) {
        char *start[] = {
            "/mnt/SDCARD/miyoo/app/wpa_supplicant", "-B", "-D", "nl80211", "-iwlan0", "-c",
            "/appconfigs/wpa_supplicant.conf",      NULL};
        if (!wifi_program(start, cancel)) {
            snprintf(adapter->error, sizeof adapter->error, "Could not start Wi-Fi service.");
            return false;
        }
        char restart[4096];
        if (path(restart, adapter, "audioserver_restart")) {
            mainui_write_text_atomic(restart, "");
        }
    }
    for (int i = 0; i < 20 && !mainui_cancelled(cancel); ++i) {
        if (adapter->transport &&
            adapter->transport(adapter->transport_context, "PING", reply, sizeof reply, cancel) &&
            !strcmp(reply, "PONG\n")) {
            return wifi_dhcp(cancel, false);
        }
        poll(NULL, 0, 50);
    }
    snprintf(adapter->error, sizeof adapter->error, "Wi-Fi control socket is unavailable.");
    return false;
}

bool mainui_device_connect(MainUIDeviceAdapter *adapter, const char *ssid, const char *password,
                           MainUICancel cancel)
{
    /* Reuse the bounded grammar validated against stock wpa_cli arguments. */
    char *validated = mainui_device_wifi_command(ssid, password);
    bool valid = validated && !mainui_cancelled(cancel);
    if (validated) {
        memset(validated, 0, strlen(validated));
        free(validated);
    }
    if (!valid || !mainui_device_status(adapter).model.wifi) {
        return false;
    }
    if (adapter->backend == DEVICE_SIMULATED && !adapter->transport) {
        return mainui_device_wifi_request(adapter->runtime, ssid, password);
    }
    if (adapter->backend == DEVICE_ONION && !mainui_device_wifi_enable(adapter, true, cancel)) {
        return false;
    }
    /* Keep existing networks until the new credentials have been accepted.
     * ADD_NETWORK returns its actual ID; never assume the daemon allocated 0. */
    char reply[64], command[256], *end = NULL;
    if (!adapter->transport || !adapter->transport(adapter->transport_context, "ADD_NETWORK", reply,
                                                   sizeof reply, cancel)) {
        return false;
    }
    errno = 0;
    long id = strtol(reply, &end, 10);
    if (errno || end == reply || strcmp(end, "\n") || id < 0 || id > INT_MAX) {
        return false;
    }
    snprintf(command, sizeof command, "SET_NETWORK %ld ssid \"%s\"", id, ssid);
    bool ok = exchange(adapter, command, "OK\n", cancel);
    snprintf(command, sizeof command,
             *password ? "SET_NETWORK %ld psk \"%s\"" : "SET_NETWORK %ld key_mgmt NONE", id,
             password);
    ok = ok && exchange(adapter, command, "OK\n", cancel);
    memset(command, 0, sizeof command);
    snprintf(command, sizeof command, "ENABLE_NETWORK %ld", id);
    ok = ok && exchange(adapter, command, "OK\n", cancel);
    snprintf(command, sizeof command, "SELECT_NETWORK %ld", id);
    ok = ok && exchange(adapter, command, "OK\n", cancel);
    ok = ok && exchange(adapter, "SAVE_CONFIG", "OK\n", cancel);
    if (!ok) {
        snprintf(command, sizeof command, "REMOVE_NETWORK %ld", id);
        exchange(adapter, command, "OK\n", (MainUICancel){0});
    }
    if (ok && adapter->backend == DEVICE_ONION && !wifi_dhcp(cancel, true)) {
        snprintf(adapter->error, sizeof adapter->error,
                 "Connection selected, but DHCP could not start.");
        return false;
    }
    return ok;
}

bool mainui_device_scan(MainUIDeviceAdapter *adapter, MainUICancel cancel)
{
    if (!mainui_device_status(adapter).model.wifi || mainui_cancelled(cancel)) {
        return false;
    }
    char file[4096];
    if (adapter->backend == DEVICE_SIMULATED && !adapter->transport) {
        const char *script = "wpa_cli scan\nwpa_cli scan_results\n";
        return path(file, adapter, "mainui-wifi-request.sh") &&
               mainui_write_bytes_new(file, script, strlen(script));
    }
    if (adapter->backend == DEVICE_ONION && !mainui_device_wifi_enable(adapter, true, cancel)) {
        return false;
    }
    char reply[16384];
    if (!adapter->transport ||
        !adapter->transport(adapter->transport_context, "SCAN", reply, sizeof reply, cancel) ||
        (strcmp(reply, "OK\n") && strcmp(reply, "FAIL-BUSY\n"))) {
        return false;
    }
    /* The daemon scans asynchronously. Wait in the worker, checking cancellation
     * every 50 ms, before asking for the completed results. */
    for (int i = 0; i < 50 && !mainui_cancelled(cancel); ++i) {
        poll(NULL, 0, 50);
    }
    if (mainui_cancelled(cancel)) {
        return false;
    }
    bool ok =
        adapter->transport(adapter->transport_context, "SCAN_RESULTS", reply, sizeof reply, cancel);
    ok = ok && !mainui_cancelled(cancel) && !strncmp(reply, "bssid /", 7) &&
         path(file, adapter, "wifi-scan.txt") && mainui_write_text_atomic(file, reply);
    if (ok) {
        mainui_device_refresh(adapter, cancel);
    }
    return ok;
}

bool mainui_device_power_off(const MainUIDeviceAdapter *adapter)
{
    int id = mainui_device_status(adapter).model.id;
    return (id == 283 || id == 285 || id == 354) && mainui_device_shutdown(adapter->runtime);
}

#ifdef MAINUI_ONION
/* Onion settings_sync.h: keymon reads these on every MainUI input and writes
 * changed values back to system.json. Updating only the file loses. NULL when
 * `key` is not one of them; *info is NULL when the memory is unavailable. */
static const MonitorValue *monitor_slot(const char *key, KeyShmInfo **info)
{
    static const char *const keys[] = {"vol", "brightness", "bgmvol",   "hibernate", "lumination",
                                       "hue", "saturation", "contrast", "audiofix"};
    static const MonitorValue slots[] = {
        MONITOR_VOLUME,          MONITOR_BRIGHTNESS, MONITOR_BGM_VOLUME,
        MONITOR_HIBERNATE_DELAY, MONITOR_LUMINATION, MONITOR_HUE,
        MONITOR_SATURATION,      MONITOR_CONTRAST,   MONITOR_AUDIOFIX};
    static KeyShmInfo shared = {.id = -1, .addr = NULL};
    for (size_t i = 0; i < sizeof slots / sizeof *slots; ++i) {
        if (!strcmp(key, keys[i])) {
            if (!shared.addr || shared.addr == (void *)-1) {
                InitKeyShm(&shared);
            }
            *info = !shared.addr || shared.addr == (void *)-1 ? NULL : &shared;
            return &slots[i];
        }
    }
    return NULL;
}
#endif

bool mainui_device_setting_sync(const MainUIDeviceAdapter *adapter, const char *key, int value)
{
    if (adapter->backend == DEVICE_SIMULATED) {
        return true;
    }
#ifdef MAINUI_ONION
    KeyShmInfo *info = NULL;
    const MonitorValue *slot = monitor_slot(key, &info);
    if (slot) {
        if (!info) {
            return false;
        }
        SetKeyShm(info, *slot, value);
        return GetKeyShm(info, *slot) == value;
    }
#else
    (void)key;
    (void)value;
#endif
    return true;
}

bool mainui_device_setting_value(const MainUIDeviceAdapter *adapter, const char *key, int *value)
{
    if (adapter->backend == DEVICE_SIMULATED) {
        return false;
    }
#ifdef MAINUI_ONION
    KeyShmInfo *info = NULL;
    const MonitorValue *slot = monitor_slot(key, &info);
    if (slot && info) {
        *value = GetKeyShm(info, *slot);
        return true;
    }
#else
    (void)key;
    (void)value;
#endif
    return false;
}

bool mainui_device_brightness(const MainUIDeviceAdapter *adapter, int value)
{
    /* Rounded 3 * exp(0.350656 * value), Onion common/system/display.h. */
    static const int duty[] = {3, 4, 6, 9, 12, 17, 25, 35, 50, 70, 100};
    if (!adapter || value < 0 || value > 10) {
        return false;
    }
    char file[4096];
    if (adapter->backend == DEVICE_SIMULATED) {
        if (!path(file, adapter, "duty_cycle")) {
            return false;
        }
    }
    else {
        snprintf(file, sizeof file,
                 "/sys/devices/soc0/soc/1f003400.pwm/pwm/pwmchip0/pwm0/duty_cycle");
    }
    /* sysfs needs an ordinary write, never an atomic rename. */
    FILE *stream = fopen(file, "w");
    if (!stream) {
        return false;
    }
    bool ok = fprintf(stream, "%d", duty[value]) > 0;
    return fclose(stream) == 0 && ok;
}

bool mainui_device_settings_changed(const MainUIDeviceAdapter *adapter)
{
    char file[4096];
    return path(file, adapter, "settings_changed") && mainui_write_text_atomic(file, "");
}

bool mainui_device_signals_install(void)
{
    struct sigaction action = {0};
    action.sa_handler = signal_event;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGCONT, &action, NULL) == 0 && sigaction(SIGTERM, &action, NULL) == 0 &&
           sigaction(SIGINT, &action, NULL) == 0;
}

void mainui_device_signals_take(bool *resume, bool *quit)
{
    *resume = *quit = false;
    sigset_t set, previous;
    sigemptyset(&set);
    sigaddset(&set, SIGCONT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigprocmask(SIG_BLOCK, &set, &previous);
    *resume = resumed != 0;
    *quit = quitting != 0;
    resumed = quitting = 0;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

bool mainui_device_refresh(MainUIDeviceAdapter *adapter, MainUICancel cancel)
{
    if (adapter->backend == DEVICE_SIMULATED && !adapter->transport) {
        return true;
    }
    char reply[16384], file[4096];
    if (!mainui_device_status(adapter).model.wifi) {
        return true;
    }
    bool ok =
        adapter->transport &&
        adapter->transport(adapter->transport_context, "STATUS", reply, sizeof reply, cancel) &&
        strstr(reply, "wpa_state=") && !mainui_cancelled(cancel);
    if (path(file, adapter, "wifi-status.txt")) {
        if (ok) {
            char signal[1024];
            if (adapter->transport(adapter->transport_context, "SIGNAL_POLL", signal, sizeof signal,
                                   cancel)) {
                size_t used = strlen(reply);
                snprintf(reply + used, sizeof reply - used, "\n%s", signal);
            }
            ok = !mainui_cancelled(cancel) && mainui_write_text_atomic(file, reply);
        }
        else {
            mainui_remove_file(file); /* A lost daemon must not leave a stale Connected status. */
        }
    }
    return ok;
}
