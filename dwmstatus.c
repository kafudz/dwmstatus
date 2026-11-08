/*
dwmstatus - status bar for dwm (based on 20h's dwmstatus)
Requires a Nerd Font in dwm's config.h, e.g.
static const char *fonts[] = { "JetBrainsMono Nerd Font:size=10" };
Shows: Caps Lock | Brightness | Volume | Battery | Temperature | Time
Optional runtime tool: wpctl (PipeWire) for volume.
Brightness, battery, and temperature are read straight from sysfs (no subprocess).
Segments that are not available on the machine are hidden instead of printing "n/a".
The root window name is only updated when the status actually changes, so dwm
does not redraw its bar twice per second for an identical string.

Build: gcc -O2 -Wall -o dwmstatus dwmstatus.c -lX11
*/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

/*
 * Poll intervals (milliseconds). Raising VOLUME_INTERVAL_MS makes the daemon
 * even quieter; it costs one wpctl process per update.
 */
#define UPDATE_INTERVAL_MS     500   /* Caps Lock and clock */
#define SYSFS_INTERVAL_MS     1000   /* brightness, battery, temperature */
#define VOLUME_INTERVAL_MS    2000   /* spawning wpctl is the expensive part */
#define VOLUME_TIMEOUT_MS      200   /* do not let a stuck wpctl freeze us */
#define DISCOVERY_INTERVAL_MS 30000  /* re-scan sysfs for hotplugged devices */

/* Local timezone (overridable with DWMSTATUS_TZ in the environment). */
static const char *tzlocal = "Asia/Jakarta";

#define PSDIR     "/sys/class/power_supply"
#define BLDIR     "/sys/class/backlight"
#define HWMONDIR  "/sys/class/hwmon"
#define THERMALDIR "/sys/class/thermal"

/* Nerd Font icons as UTF-8 byte sequences */
#define ICON_BRIGHTNESS   "\xef\x86\x85"  /* U+F185 nf-fa-sun_o */
#define ICON_VOL_OFF      "\xef\x80\xa6"  /* U+F026 nf-fa-volume_off */
#define ICON_VOL_LOW      "\xef\x80\xa7"  /* U+F027 nf-fa-volume_down */
#define ICON_VOL_HIGH     "\xef\x80\xa8"  /* U+F028 nf-fa-volume_up */
#define ICON_BAT_FULL     "\xef\x89\x80"  /* U+F240 nf-fa-battery_full */
#define ICON_BAT_3Q       "\xef\x89\x81"  /* U+F241 nf-fa-battery_three_quarters */
#define ICON_BAT_HALF     "\xef\x89\x82"  /* U+F242 nf-fa-battery_half */
#define ICON_BAT_1Q       "\xef\x89\x83"  /* U+F243 nf-fa-battery_quarter */
#define ICON_BAT_EMPTY    "\xef\x89\x84"  /* U+F244 nf-fa-battery_empty */
#define ICON_BOLT         "\xef\x83\xa7"  /* U+F0E7 nf-fa-bolt */
#define ICON_CLOCK        "\xef\x80\x97"  /* U+F017 nf-fa-clock_o */
#define ICON_CAPS_ON      "\xef\x80\xa3"  /* U+F023 nf-fa-lock */
#define ICON_TEMP         "\xef\x8b\x87"  /* U+F2C7 nf-fa-thermometer-half */

static Display *dpy;
static Window root;
static volatile sig_atomic_t running = 1;

static void
onsignal(int sig)
{
    (void)sig;
    running = 0;
}

static void
setstatus(const char *str)
{
    XStoreName(dpy, root, str);
    XFlush(dpy);
}

static int
join_path(char *dst, size_t size, const char *dir, const char *name)
{
    int n = snprintf(dst, size, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

/* Reads a whole (small) file. 0 on success. Handles EINTR and short reads. */
static int
read_file(const char *path, char *buf, size_t size)
{
    int fd;
    ssize_t n;
    size_t off = 0;

    if (size == 0)
        return -1;

    if ((fd = open(path, O_RDONLY)) < 0)
        return -1;

    while (off < size - 1) {
        n = read(fd, buf + off, size - 1 - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    close(fd);

    if (off == 0)
        return -1;

    buf[off] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int
read_attr(const char *dir, const char *name, char *buf, size_t size)
{
    char path[PATH_MAX];

    if (join_path(path, sizeof(path), dir, name) < 0)
        return -1;
    return read_file(path, buf, size);
}

static int
read_long_file(const char *path, long *out)
{
    char buf[64], *end;

    if (read_file(path, buf, sizeof(buf)) < 0)
        return -1;

    errno = 0;
    *out = strtol(buf, &end, 10);
    if (end == buf || errno == ERANGE)
        return -1;
    return 0;
}

static int
read_long_attr(const char *dir, const char *name, long *out)
{
    char path[PATH_MAX];

    if (join_path(path, sizeof(path), dir, name) < 0)
        return -1;
    return read_long_file(path, out);
}

static long long
now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Signal-safe-ish sleep: wakes early on SIGINT/SIGTERM. */
static void
sleep_ms(long ms)
{
    struct timespec ts;

    if (ms <= 0)
        return;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0) {
        if (errno != EINTR || !running)
            break;
    }
}

/* Finds the first real system battery. 0 on success. */
static int
find_battery(char *out, size_t size)
{
    DIR *d;
    struct dirent *e;
    char dir[PATH_MAX], val[32], fallback[PATH_MAX] = "";

    if (!(d = opendir(PSDIR)))
        return -1;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (join_path(dir, sizeof(dir), PSDIR, e->d_name) < 0)
            continue;

        if (read_attr(dir, "type", val, sizeof(val)) < 0 ||
            strcmp(val, "Battery") != 0)
            continue;

        /* skip batteries of peripherals (mouse, headset, ...) */
        if (read_attr(dir, "scope", val, sizeof(val)) == 0 &&
            strcmp(val, "Device") == 0)
            continue;

        /* Prefer a battery that actually reports capacity. */
        if (read_attr(dir, "capacity", val, sizeof(val)) == 0) {
            snprintf(out, size, "%s", dir);
            closedir(d);
            return 0;
        }
        if (fallback[0] == '\0')
            snprintf(fallback, sizeof(fallback), "%s", dir);
    }
    closedir(d);

    if (fallback[0] != '\0') {
        snprintf(out, size, "%s", fallback);
        return 0;
    }
    return -1;
}

/* Battery percentage */
static int
battery_percent(const char *base)
{
    static const char *now[] = { "charge_now", "energy_now" };
    static const char *full[] = { "charge_full", "energy_full" };
    static const char *dsgn[] = { "charge_full_design", "energy_full_design" };
    long cap, n, f;
    int i;

    if (read_long_attr(base, "capacity", &cap) == 0)
        return cap < 0 ? 0 : cap > 100 ? 100 : (int)cap;

    for (i = 0; i < 2; i++) {
        if (read_long_attr(base, now[i], &n) < 0 || n < 0)
            continue;
        if ((read_long_attr(base, full[i], &f) < 0 || f <= 0) &&
            (read_long_attr(base, dsgn[i], &f) < 0 || f <= 0))
            continue;

        /* 64-bit math: n * 100 can overflow a 32-bit long. */
        cap = (long)(((long long)n * 100 + f / 2) / f);
        return cap < 0 ? 0 : cap > 100 ? 100 : (int)cap;
    }
    return -1;
}

static void
get_battery(const char *base, char *buf, size_t size)
{
    char status[32];
    const char *icon;
    int capacity, charging = 0;

    buf[0] = '\0';
    if (base == NULL || base[0] == '\0')
        return;

    if ((capacity = battery_percent(base)) < 0)
        return; /* hide segment on failure */

    if (read_attr(base, "status", status, sizeof(status)) == 0)
        charging = (strcmp(status, "Charging") == 0);

    if (capacity >= 88)
        icon = ICON_BAT_FULL;
    else if (capacity >= 63)
        icon = ICON_BAT_3Q;
    else if (capacity >= 38)
        icon = ICON_BAT_HALF;
    else if (capacity >= 13)
        icon = ICON_BAT_1Q;
    else
        icon = ICON_BAT_EMPTY;

    snprintf(buf, size, "%s%s  %d%%", charging ? ICON_BOLT "  " : "",
             icon, capacity);
}

/* First usable backlight device in sysfs. 0 on success. */
static int
find_backlight(char *out, size_t size)
{
    DIR *d;
    struct dirent *e;
    char dir[PATH_MAX], path[PATH_MAX];

    if (!(d = opendir(BLDIR)))
        return -1;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (join_path(dir, sizeof(dir), BLDIR, e->d_name) < 0)
            continue;

        /* Ignore entries that are not actual backlight devices. */
        if (join_path(path, sizeof(path), dir, "brightness") < 0 ||
            access(path, R_OK) < 0)
            continue;
        if (join_path(path, sizeof(path), dir, "max_brightness") < 0 ||
            access(path, R_OK) < 0)
            continue;

        snprintf(out, size, "%s", dir);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

static void
get_brightness(const char *base, char *buf, size_t size)
{
    double pct;
    long cur, max;

    buf[0] = '\0';
    if (base == NULL || base[0] == '\0')
        return;

    if (read_long_attr(base, "brightness", &cur) < 0 ||
        read_long_attr(base, "max_brightness", &max) < 0 || max <= 0)
        return;

    pct = (double)cur * 100.0 / (double)max + 0.5;
    if (pct < 0.0)
        pct = 0.0;
    else if (pct > 100.0)
        pct = 100.0;

    snprintf(buf, size, "%s  %ld%%", ICON_BRIGHTNESS, (long)pct);
}

static int
is_temp_input(const char *name)
{
    size_t len = strlen(name), i;

    if (len < 11 || strncmp(name, "temp", 4) != 0 ||
        strcmp(name + len - 6, "_input") != 0)
        return 0;

    for (i = 4; i < len - 6; i++)
        if (!isdigit((unsigned char)name[i]))
            return 0;
    return 1;
}

/* Finds tempN_input inside one hwmon directory, preferring temp1. */
static int
find_temp_input(const char *dir, char *out, size_t size)
{
    DIR *d;
    struct dirent *e;
    char path[PATH_MAX];

    if (join_path(path, sizeof(path), dir, "temp1_input") == 0 &&
        access(path, R_OK) == 0) {
        snprintf(out, size, "%s", path);
        return 0;
    }

    if (!(d = opendir(dir)))
        return -1;

    while ((e = readdir(d))) {
        if (strcmp(e->d_name, "temp1_input") == 0)
            continue;
        if (!is_temp_input(e->d_name))
            continue;
        if (join_path(path, sizeof(path), dir, e->d_name) < 0 ||
            access(path, R_OK) < 0)
            continue;

        snprintf(out, size, "%s", path);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

/*
 * Locates a temperature sensor without assuming a hwmon number (hwmonN is
 * not stable across boots). Preferred CPU drivers win in the order listed
 * below, then any hwmon sensor, then /sys/class/thermal.
 */
static int
find_temperature(char *out, size_t size)
{
    static const char *names[] = {
        "coretemp", "k10temp", "zenpower", "cpu_thermal", "acpitz"
    };
    const size_t nnames = sizeof(names) / sizeof(names[0]);
    DIR *d;
    struct dirent *e;
    char dir[PATH_MAX], name[64], tpath[PATH_MAX];
    char found[PATH_MAX] = "", fallback[PATH_MAX] = "";
    size_t i, best_prio = nnames;

    if ((d = opendir(HWMONDIR))) {
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            if (join_path(dir, sizeof(dir), HWMONDIR, e->d_name) < 0)
                continue;
            if (find_temp_input(dir, tpath, sizeof(tpath)) < 0)
                continue;

            /* Remember the first sensor as a generic fallback. */
            if (found[0] == '\0')
                snprintf(found, sizeof(found), "%s", tpath);

            if (read_attr(dir, "name", name, sizeof(name)) < 0)
                continue;
            for (i = 0; i < nnames; i++)
                if (strcmp(name, names[i]) == 0)
                    break;
            if (i < best_prio) {
                best_prio = i;
                snprintf(found, sizeof(found), "%s", tpath);
            }
        }
        closedir(d);
    }

    if (found[0] != '\0') {
        snprintf(out, size, "%s", found);
        return 0;
    }

    if ((d = opendir(THERMALDIR))) {
        char type[64];

        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            if (join_path(dir, sizeof(dir), THERMALDIR, e->d_name) < 0)
                continue;
            if (join_path(tpath, sizeof(tpath), dir, "temp") < 0 ||
                access(tpath, R_OK) < 0)
                continue;

            if (read_attr(dir, "type", type, sizeof(type)) == 0 &&
                (strstr(type, "x86_pkg") || strstr(type, "cpu"))) {
                snprintf(out, size, "%s", tpath);
                closedir(d);
                return 0;
            }
            if (fallback[0] == '\0')
                snprintf(fallback, sizeof(fallback), "%s", tpath);
        }
        closedir(d);
    }

    if (fallback[0] != '\0') {
        snprintf(out, size, "%s", fallback);
        return 0;
    }
    return -1;
}

static void
get_temperature(const char *path, char *buf, size_t size)
{
    long temp;

    buf[0] = '\0';
    if (path == NULL || path[0] == '\0')
        return;

    if (read_long_file(path, &temp) < 0)
        return; /* hide segment if the sensor disappears */
    if (temp < -100000 || temp > 200000)
        return;

    snprintf(buf, size, "%s  %ld°C", ICON_TEMP, temp / 1000);
}

/* Looks up an executable in PATH. 0 on success. */
static int
find_program(const char *name, char *out, size_t size)
{
    const char *path;
    char *copy, *p, *save, candidate[PATH_MAX];

    if (strchr(name, '/')) {
        if (access(name, X_OK) < 0)
            return -1;
        snprintf(out, size, "%s", name);
        return 0;
    }

    path = getenv("PATH");
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/bin:/usr/bin:/bin";

    if ((copy = strdup(path)) == NULL)
        return -1;

    for (p = strtok_r(copy, ":", &save); p != NULL;
         p = strtok_r(NULL, ":", &save)) {
        if (join_path(candidate, sizeof(candidate), p, name) < 0)
            continue;
        if (access(candidate, X_OK) == 0) {
            snprintf(out, size, "%s", candidate);
            free(copy);
            return 0;
        }
    }
    free(copy);
    return -1;
}

/*
 * Runs a program and captures a bounded amount of stdout. The child is
 * killed if it does not finish in time, so a stuck wpctl cannot hang the
 * status bar. 0 on success.
 */
static int
run_capture(const char *prog, char *const argv[], char *buf, size_t size,
            int timeout_ms)
{
    int fds[2], status = -1, timed_out = 0, devnull;
    pid_t pid;
    size_t off = 0;
    struct pollfd pfd;

    if (size == 0)
        return -1;
    buf[0] = '\0';

    if (pipe(fds) < 0)
        return -1;

    if ((pid = fork()) < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        close(fds[0]);
        /* fds[1] can be STDOUT_FILENO if the parent had it closed. */
        if (fds[1] != STDOUT_FILENO) {
            if (dup2(fds[1], STDOUT_FILENO) < 0)
                _exit(127);
            close(fds[1]);
        }

        if ((devnull = open("/dev/null", O_RDWR)) >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }

        execv(prog, argv);
        _exit(127);
    }

    close(fds[1]);
    pfd.fd = fds[0];
    pfd.events = POLLIN;

    for (;;) {
        char discard[256];
        char *dst;
        size_t room;
        ssize_t n;
        int pr;

        dst = (off < size - 1) ? buf + off : discard;
        room = (off < size - 1) ? size - 1 - off : sizeof(discard);

        pfd.revents = 0;
        pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                if (!running) {
                    timed_out = 1;
                    break;
                }
                continue;
            }
            break;
        }
        if (pr == 0) {
            timed_out = 1;
            break;
        }

        n = read(fds[0], dst, room);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        if (off < size - 1)
            off += (size_t)n;
    }

    close(fds[0]);
    buf[off] = '\0';

    if (timed_out)
        kill(pid, SIGKILL);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            status = -1;
            break;
        }
    }

    buf[strcspn(buf, "\r\n")] = '\0';

    if (timed_out || status == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}

/* Cached path to wpctl; refreshed by discover(). */
static char wpctl_path[PATH_MAX];

static void
get_volume(char *buf, size_t size)
{
    char line[128];
    char *argv[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    double vol;
    int volume;
    const char *icon;

    buf[0] = '\0';
    if (wpctl_path[0] == '\0')
        return;

    if (run_capture(wpctl_path, argv, line, sizeof(line),
                    VOLUME_TIMEOUT_MS) < 0)
        return;

    if (sscanf(line, "Volume: %lf", &vol) != 1)
        return;

    if (strstr(line, "MUTED")) {
        snprintf(buf, size, "%s  muted", ICON_VOL_OFF);
        return;
    }

    if (vol < 0.0)
        vol = 0.0;
    else if (vol > 10.0) /* sanity clamp before the int cast */
        vol = 10.0;
    volume = (int)(vol * 100.0 + 0.5);

    if (volume == 0)
        icon = ICON_VOL_OFF;
    else if (volume < 50)
        icon = ICON_VOL_LOW;
    else
        icon = ICON_VOL_HIGH;

    snprintf(buf, size, "%s  %d%%", icon, volume);
}

static void
get_time(char *buf, size_t size)
{
    time_t t;
    struct tm tmv;

    buf[0] = '\0';
    t = time(NULL);
    if (t == (time_t)-1 || localtime_r(&t, &tmv) == NULL)
        return;

    if (strftime(buf, size, ICON_CLOCK "  %a %d %b %H:%M", &tmv) == 0)
        buf[0] = '\0';
}

/* Caps Lock: shows a lock icon ONLY while Caps Lock is active. */
static void
get_capslock(char *buf, size_t size)
{
    XkbStateRec state;

    buf[0] = '\0';
    if (XkbGetState(dpy, XkbUseCoreKbd, &state) == Success &&
        (state.locked_mods & LockMask))
        snprintf(buf, size, "%s", ICON_CAPS_ON);
}

/* Slimmer separator: single space on each side of the pipe */
static size_t
append(char *dst, size_t size, size_t len, const char *seg)
{
    size_t seg_len;

    if (!seg || seg[0] == '\0')
        return len;

    seg_len = strlen(seg);
    if (len > 0 && len + 3 < size) {
        dst[len++] = ' ';
        dst[len++] = '|';
        dst[len++] = ' ';
    }
    if (len + seg_len >= size)
        seg_len = size - len - 1;

    memcpy(dst + len, seg, seg_len);
    len += seg_len;
    dst[len] = '\0';
    return len;
}

/* Paths are cached and re-discovered periodically to survive hotplug. */
static char bat_path[PATH_MAX];
static char bl_path[PATH_MAX];
static char temp_path[PATH_MAX];

static void
discover(void)
{
    if (find_battery(bat_path, sizeof(bat_path)) < 0)
        bat_path[0] = '\0';
    if (find_backlight(bl_path, sizeof(bl_path)) < 0)
        bl_path[0] = '\0';
    if (find_temperature(temp_path, sizeof(temp_path)) < 0)
        temp_path[0] = '\0';
    if (find_program("wpctl", wpctl_path, sizeof(wpctl_path)) < 0)
        wpctl_path[0] = '\0';
}

int
main(int argc, char *argv[])
{
    struct sigaction sa;
    char status[512], last_status[512];
    char caps[32], bri[64], vol[64], bat[64], temp[64], tim[64];
    const char *tz;
    long long now, last_sysfs, last_volume, last_discovery;
    int have_last = 0;
    size_t len;

    if (argc == 2 && strcmp(argv[1], "-v") == 0) {
        puts("dwmstatus-" VERSION);
        return 0;
    } else if (argc != 1) {
        fputs("usage: dwmstatus [-v]\n", stderr);
        return 1;
    }

    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "dwmstatus: cannot open display.\n");
        return 1;
    }
    root = DefaultRootWindow(dpy);

    /* Do not leak the X connection into short-lived children (wpctl). */
    {
        int flags = fcntl(ConnectionNumber(dpy), F_GETFD);
        if (flags >= 0)
            fcntl(ConnectionNumber(dpy), F_SETFD, flags | FD_CLOEXEC);
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onsignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: we want nanosleep/poll interrupted */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    tz = getenv("DWMSTATUS_TZ");
    if (tz == NULL || tz[0] == '\0')
        tz = tzlocal;
    if (setenv("TZ", tz, 1) == 0)
        tzset();

    discover();

    caps[0] = bri[0] = vol[0] = bat[0] = temp[0] = tim[0] = '\0';
    last_status[0] = '\0';
    last_discovery = now_ms();
    last_sysfs = last_discovery - SYSFS_INTERVAL_MS;
    last_volume = last_discovery - VOLUME_INTERVAL_MS;

    while (running) {
        now = now_ms();

        /* Refresh cached sysfs/program paths occasionally (hotplug). */
        if (now - last_discovery >= DISCOVERY_INTERVAL_MS ||
            now < last_discovery) {
            discover();
            last_discovery = now;
            /* Reflect any newly found/removed device immediately. */
            last_sysfs = now - SYSFS_INTERVAL_MS;
            last_volume = now - VOLUME_INTERVAL_MS;
        }

        get_capslock(caps, sizeof(caps));

        if (now - last_sysfs >= SYSFS_INTERVAL_MS || now < last_sysfs) {
            get_brightness(bl_path, bri, sizeof(bri));
            get_battery(bat_path, bat, sizeof(bat));
            get_temperature(temp_path, temp, sizeof(temp));
            last_sysfs = now;
        }

        if (now - last_volume >= VOLUME_INTERVAL_MS ||
            now < last_volume) {
            get_volume(vol, sizeof(vol));
            last_volume = now;
        }

        get_time(tim, sizeof(tim));

        len = 0;
        status[0] = '\0';

        len = append(status, sizeof(status), len, caps);
        len = append(status, sizeof(status), len, bri);
        len = append(status, sizeof(status), len, vol);
        len = append(status, sizeof(status), len, bat);
        len = append(status, sizeof(status), len, temp);
        len = append(status, sizeof(status), len, tim);

        /*
         * Only touch the root window name when something changed. Setting
         * an identical name still makes dwm receive PropertyNotify and
         * redraw its entire bar, which is wasted CPU every poll.
         */
        if (!have_last || strcmp(status, last_status) != 0) {
            setstatus(status);
            snprintf(last_status, sizeof(last_status), "%s", status);
            have_last = 1;
        }

        if (running)
            sleep_ms(UPDATE_INTERVAL_MS);
    }

    setstatus("");
    XCloseDisplay(dpy);
    return 0;
}
