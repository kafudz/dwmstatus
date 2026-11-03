/*
dwmstatus - status bar for dwm (based on 20h's dwmstatus)
Requires a Nerd Font in dwm's config.h, e.g.
static const char *fonts[] = { "JetBrainsMono Nerd Font:size=10" };
Shows: Caps Lock | Brightness | Volume | Battery | Temperature | Time
Optional runtime tool: wpctl (PipeWire) for volume.
Brightness, battery, and temperature are read straight from sysfs (no subprocess).
Segments that are not available on the machine are hidden instead of printing "n/a".
Build: make   (or: cc -std=c99 -D_DEFAULT_SOURCE -O2 -Wall -o dwmstatus dwmstatus.c -lX11)
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
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

/* Update every 0.5 seconds for sub-second responsiveness */
#define INTERVAL_MS 500

/* wpctl normally answers in a few ms; never let a hung one freeze the bar */
#define VOLUME_TIMEOUT_MS 300

/* Local timezone */
static const char *tzlocal = "Asia/Jakarta";

#define PSDIR "/sys/class/power_supply"
#define BLDIR "/sys/class/backlight"
#define HWDIR "/sys/class/hwmon"

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
#define ICON_TEMP         "\xef\x8b\x87"  /* U+F2C7 nf-fa-thermometer_full */

/* runline() failure codes */
#define RUN_FAIL     (-1)
#define RUN_NOTFOUND (-2)

extern char **environ;

static Display *dpy;
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
    XStoreName(dpy, DefaultRootWindow(dpy), str);
    XFlush(dpy);
}

/* Reads sysfs file using raw open/read/close to avoid libc FILE buffering overhead */
static int
readfile(const char *dir, const char *file, char *buf, size_t size)
{
    char path[PATH_MAX];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if ((fd = open(path, O_RDONLY)) < 0)
        return -1;

    n = read(fd, buf, size - 1);
    close(fd);

    if (n <= 0)
        return -1;

    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

/* Reads an integer from dir/file. 0 on success. */
static int
readlong(const char *dir, const char *file, long *out)
{
    char buf[64], *end;
    if (readfile(dir, file, buf, sizeof(buf)) < 0)
        return -1;
    *out = strtol(buf, &end, 10);
    return end == buf ? -1 : 0;
}

static long long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Runs argv (no shell) and stores the first line of its stdout in buf.
 * Gives up after timeout_ms and kills the child, so a stuck helper can
 * never block the status loop. 0 on success, RUN_NOTFOUND if the program
 * does not exist, RUN_FAIL otherwise.
 */
static int
runline(char *const argv[], char *buf, size_t size, int timeout_ms)
{
    posix_spawn_file_actions_t fa;
    struct pollfd pfd;
    long long deadline;
    size_t len = 0;
    ssize_t n;
    pid_t pid;
    int fds[2], err, left;

    if (pipe(fds) < 0)
        return RUN_FAIL;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);

    if (err != 0) {
        close(fds[0]);
        return err == ENOENT ? RUN_NOTFOUND : RUN_FAIL;
    }

    deadline = now_ms() + timeout_ms;
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    while (len < size - 1) {
        left = (int)(deadline - now_ms());
        if (left <= 0 || poll(&pfd, 1, left) <= 0)
            break;
        if ((n = read(fds[0], buf + len, size - 1 - len)) <= 0)
            break;
        len += (size_t)n;
        if (memchr(buf, '\n', len))
            break;
    }
    close(fds[0]);
    buf[len] = '\0';
    buf[strcspn(buf, "\n")] = '\0';

    /* reap the child; if it is still running (hung), kill it first */
    if (waitpid(pid, NULL, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    return len > 0 ? 0 : RUN_FAIL;
}

/* Finds the first system battery. 0 on success. */
static int
find_battery(char *out, size_t size)
{
    DIR *d;
    struct dirent *e;
    char path[PATH_MAX], val[32];

    if (!(d = opendir(PSDIR)))
        return -1;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;

        snprintf(path, sizeof(path), "%s/%s", PSDIR, e->d_name);
        if (readfile(path, "type", val, sizeof(val)) < 0 ||
            strcmp(val, "Battery") != 0)
            continue;

        /* skip batteries of peripherals (mouse, headset, ...) */
        if (readfile(path, "scope", val, sizeof(val)) == 0 &&
            strcmp(val, "Device") == 0)
            continue;

        snprintf(out, size, "%s", path);
        closedir(d);
        return 0;
    }
    closedir(d);
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

    if (readlong(base, "capacity", &cap) == 0)
        return cap < 0 ? 0 : cap > 100 ? 100 : (int)cap;

    for (i = 0; i < 2; i++) {
        if (readlong(base, now[i], &n) < 0)
            continue;
        if ((readlong(base, full[i], &f) < 0 || f <= 0) &&
            (readlong(base, dsgn[i], &f) < 0 || f <= 0))
            continue;
        /* 64-bit math: energy_now in uWh * 100 overflows a 32-bit long */
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
    if (base == NULL)
        return;

    if ((capacity = battery_percent(base)) < 0)
        return; /* hide segment on failure */

    if (readfile(base, "status", status, sizeof(status)) == 0)
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

    /* Added extra spaces after icons for better visual separation */
    snprintf(buf, size, "%s%s  %d%%", charging ? ICON_BOLT "  " : "", icon, capacity);
}

/* First backlight device in sysfs. 0 on success. */
static int
find_backlight(char *out, size_t size)
{
    DIR *d;
    struct dirent *e;

    if (!(d = opendir(BLDIR)))
        return -1;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;

        snprintf(out, size, "%s/%s", BLDIR, e->d_name);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

static void
get_brightness(const char *dev, char *buf, size_t size)
{
    long cur, max;

    buf[0] = '\0';
    if (dev == NULL)
        return; /* no backlight (desktop): hide segment */

    if (readlong(dev, "brightness", &cur) < 0 ||
        readlong(dev, "max_brightness", &max) < 0 || max <= 0)
        return;

    snprintf(buf, size, "%s  %lld%%", ICON_BRIGHTNESS,
             ((long long)cur * 100 + max / 2) / max);
}

static void
get_volume(char *buf, size_t size)
{
    static int notfound; /* wpctl is not installed: stop trying */
    static char *const argv[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    char line[128], *end;
    const char *icon;
    float vol;
    int volume, r;

    buf[0] = '\0'; /* never leave a stale or uninitialised segment behind */
    if (notfound)
        return;

    if ((r = runline(argv, line, sizeof(line), VOLUME_TIMEOUT_MS)) < 0) {
        notfound = (r == RUN_NOTFOUND);
        return; /* hide segment if wpctl is missing or failing */
    }

    /* expected: "Volume: 0.50" or "Volume: 0.50 [MUTED]" */
    if (strncmp(line, "Volume:", 7) != 0)
        return;
    vol = strtof(line + 7, &end);
    if (end == line + 7)
        return;

    if (strstr(end, "MUTED")) {
        snprintf(buf, size, "%s  muted", ICON_VOL_OFF);
        return;
    }

    volume = (int)(vol * 100 + 0.5f);
    if (volume == 0)
        icon = ICON_VOL_OFF;
    else if (volume < 50)
        icon = ICON_VOL_LOW;
    else
        icon = ICON_VOL_HIGH;

    snprintf(buf, size, "%s  %d%%", icon, volume);
}

/*
 * Finds the CPU temperature sensor. hwmon numbers are not stable between
 * boots or kernels, so match by driver name instead of hardcoding hwmon0.
 * Earlier entries in names[] win. Add your driver here if yours is missing.
 * 0 on success.
 */
static int
find_temperature(char *out, size_t size)
{
    static const char *names[] = {
        "coretemp", "k10temp", "zenpower", "cpu_thermal", "acpitz"
    };
    DIR *d;
    struct dirent *e;
    char path[PATH_MAX], val[32];
    long t;
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(d = opendir(HWDIR)))
            return -1;

        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;

            snprintf(path, sizeof(path), "%s/%s", HWDIR, e->d_name);
            if (readfile(path, "name", val, sizeof(val)) < 0 ||
                strcmp(val, names[i]) != 0)
                continue;
            if (readlong(path, "temp1_input", &t) < 0)
                continue;

            snprintf(out, size, "%s", path);
            closedir(d);
            return 0;
        }
        closedir(d);
    }
    return -1;
}

/* Native Temperature reading */
static void
get_temperature(const char *dir, char *buf, size_t size)
{
    long temp;

    buf[0] = '\0';
    if (dir == NULL || readlong(dir, "temp1_input", &temp) < 0)
        return; /* hide segment if no sensor was found */

    snprintf(buf, size, "%s  %ld°C", ICON_TEMP, temp / 1000);
}

static void
get_time(char *buf, size_t size)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    if (tm == NULL || strftime(buf, size, ICON_CLOCK "  %a %d %b %H:%M", tm) == 0)
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

/*
 * Appends seg to dst, preceded by " | " if dst is not empty.
 * A segment that does not fit completely is dropped, so a UTF-8 icon is
 * never cut in half. dst must be NUL-terminated. Returns the new length.
 */
static size_t
append(char *dst, size_t size, size_t len, const char *seg)
{
    static const char sep[] = " | ";
    size_t seg_len, sep_len;

    if (!seg || seg[0] == '\0')
        return len;

    seg_len = strlen(seg);
    sep_len = len > 0 ? sizeof(sep) - 1 : 0;
    if (len + sep_len + seg_len >= size)
        return len;

    memcpy(dst + len, sep, sep_len);
    len += sep_len;
    memcpy(dst + len, seg, seg_len + 1); /* includes the NUL */
    return len + seg_len;
}

int
main(int argc, char *argv[])
{
    struct sigaction sa;
    struct timespec ts = { INTERVAL_MS / 1000, (INTERVAL_MS % 1000) * 1000000L };
    char bat_path[PATH_MAX], bl_path[PATH_MAX], hw_path[PATH_MAX];
    char status[512], last[512];
    char caps[32], bri[64], vol[64], bat[64], temp[64], tim[64];
    const char *batpath = NULL, *blpath = NULL, *hwpath = NULL;
    size_t len;
    int drawn = 0;

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

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onsignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    setenv("TZ", tzlocal, 1);
    tzset();

    /* hardware is looked up once; absent segments simply stay hidden */
    if (find_battery(bat_path, sizeof(bat_path)) == 0)
        batpath = bat_path;
    if (find_backlight(bl_path, sizeof(bl_path)) == 0)
        blpath = bl_path;
    if (find_temperature(hw_path, sizeof(hw_path)) == 0)
        hwpath = hw_path;

    while (running) {
        get_capslock(caps, sizeof(caps));
        get_brightness(blpath, bri, sizeof(bri));
        get_volume(vol, sizeof(vol));
        get_battery(batpath, bat, sizeof(bat));
        get_temperature(hwpath, temp, sizeof(temp));
        get_time(tim, sizeof(tim));

        len = 0;
        status[0] = '\0';

        len = append(status, sizeof(status), len, caps);
        len = append(status, sizeof(status), len, bri);
        len = append(status, sizeof(status), len, vol);
        len = append(status, sizeof(status), len, bat);
        len = append(status, sizeof(status), len, temp);
        len = append(status, sizeof(status), len, tim);

        /* only touch the root window when something changed: every write makes dwm redraw its bar */
        if (!drawn || strcmp(status, last) != 0) {
            setstatus(status);
            snprintf(last, sizeof(last), "%s", status);
            drawn = 1;
        }

        if (running)
            nanosleep(&ts, NULL); /* a signal cuts the sleep short */
    }

    setstatus("");
    XCloseDisplay(dpy);
    return 0;
}
