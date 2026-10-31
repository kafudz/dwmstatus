/*
dwmstatus - status bar for dwm (based on 20h's dwmstatus)
Requires a Nerd Font in dwm's config.h, e.g.
static const char *fonts[] = { "JetBrainsMono Nerd Font:size=10" };
Shows: Caps Lock | Brightness | Volume | Battery | Temperature | Time
Optional runtime tool: wpctl (PipeWire) for volume.
Brightness, battery, and temperature are read straight from sysfs (no subprocess).
Segments that are not available on the machine are hidden instead of printing "n/a".
Build: gcc -O2 -Wall -o dwmstatus dwmstatus.c -lX11
*/

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

/* Update every 0.5 seconds for sub-second responsiveness */
#define INTERVAL_US 500000 

/* Local timezone */
static const char *tzlocal = "Asia/Jakarta";

#define PSDIR "/sys/class/power_supply"
#define BLDIR "/sys/class/backlight"
#define HWMON "/sys/class/hwmon/hwmon0" /* Adjust if your thermal zone is different */

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
    /* XFlush is much faster than XSync because it doesn't block waiting for the X server's reply */
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
        cap = (n * 100 + f / 2) / f;
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

    snprintf(buf, size, "%s%s %d%%", charging ? ICON_BOLT " " : "", icon, capacity);
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
get_brightness(char *buf, size_t size)
{
    static char dev[PATH_MAX];
    long cur, max;

    buf[0] = '\0';
    if (dev[0] == '\0' && find_backlight(dev, sizeof(dev)) < 0)
        return; /* no backlight (desktop): hide segment */

    if (readlong(dev, "brightness", &cur) < 0 ||
        readlong(dev, "max_brightness", &max) < 0 || max <= 0)
        return;

    snprintf(buf, size, "%s %ld%%", ICON_BRIGHTNESS, (cur * 100 + max / 2) / max);
}

static void
get_volume(char *buf, size_t size)
{
    FILE *fp;
    char line[128];
    float vol = 0.0f;
    int volume;
    const char *icon;

    if (!(fp = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r")))
        return; /* hide segment if wpctl is missing */

    if (!fgets(line, sizeof(line), fp) ||
        sscanf(line, "Volume: %f", &vol) != 1) {
        pclose(fp);
        return;
    }
    pclose(fp);

    if (strstr(line, "MUTED")) {
        snprintf(buf, size, "%s muted", ICON_VOL_OFF);
        return;
    }

    volume = (int)(vol * 100 + 0.5f);
    if (volume == 0)
        icon = ICON_VOL_OFF;
    else if (volume < 50)
        icon = ICON_VOL_LOW;
    else
        icon = ICON_VOL_HIGH;

    snprintf(buf, size, "%s %d%%", icon, volume);
}

/* Native Temperature reading */
static void
get_temperature(char *buf, size_t size)
{
    long temp;

    buf[0] = '\0';
    if (readlong(HWMON, "temp1_input", &temp) < 0)
        return; /* hide segment if thermal zone not found */

    snprintf(buf, size, "%s %ld°C", ICON_TEMP, temp / 1000);
}

static void
get_time(char *buf, size_t size)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    if (tm == NULL || strftime(buf, size, ICON_CLOCK " %a %d %b %H:%M", tm) == 0)
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

/* Optimized append: tracks length manually to avoid calling strlen(dst) repeatedly */
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

int
main(void)
{
    struct sigaction sa;
    char bat_path[PATH_MAX], status[512];
    char caps[32], bri[64], vol[64], bat[64], temp[64], tim[64];
    const char *batpath = NULL;
    size_t len;

    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "dwmstatus: cannot open display.\n");
        return 1;
    }

    /* exit cleanly on SIGINT/SIGTERM */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onsignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* TZ never changes, set it once */
    setenv("TZ", tzlocal, 1);
    tzset();

    if (find_battery(bat_path, sizeof(bat_path)) == 0)
        batpath = bat_path;

    while (running) {
        get_capslock(caps, sizeof(caps));
        get_brightness(bri, sizeof(bri));
        get_volume(vol, sizeof(vol));
        get_battery(batpath, bat, sizeof(bat));
        get_temperature(temp, sizeof(temp));
        get_time(tim, sizeof(tim));

        /* Build status string */
        len = 0;
        status[0] = '\0';
        
        len = append(status, sizeof(status), len, caps);
        len = append(status, sizeof(status), len, bri);
        len = append(status, sizeof(status), len, vol);
        len = append(status, sizeof(status), len, bat);
        len = append(status, sizeof(status), len, temp);
        len = append(status, sizeof(status), len, tim);

        setstatus(status);
        
        if (running)
            usleep(INTERVAL_US); /* 0.5s sleep for <1s responsiveness */
    }

    setstatus("");
    XCloseDisplay(dpy);
    return 0;
}