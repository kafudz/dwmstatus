/*
 * dwmstatus - status bar for dwm (based on 20h's dwmstatus)
 * Requires a Nerd Font in dwm's config.h, e.g.
 *   static const char *fonts[] = { "JetBrainsMono Nerd Font:size=10" };
 *
 * Shows: Caps Lock | Brightness | Volume | Battery | Time
 * Optional runtime tool: wpctl (PipeWire) for volume.
 * Brightness and battery are read straight from sysfs (no subprocess).
 * Segments that are not available on the machine (no backlight, no
 * battery) are hidden instead of printing "n/a".
 *
 * Build: gcc -O2 -Wall -o dwmstatus dwmstatus.c -lX11
 */

#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#define INTERVAL 2 /* seconds between updates */

/* Local timezone */
static const char *tzlocal = "Asia/Jakarta";

#define PSDIR "/sys/class/power_supply"
#define BLDIR "/sys/class/backlight"

/* Nerd Font icons as UTF-8 byte sequences (survive copy/paste and editors) */
#define ICON_BRIGHTNESS "\xef\x86\x85"  /* U+F185 nf-fa-sun_o */
#define ICON_VOL_OFF    "\xef\x80\xa6"  /* U+F026 nf-fa-volume_off */
#define ICON_VOL_LOW    "\xef\x80\xa7"  /* U+F027 nf-fa-volume_down */
#define ICON_VOL_HIGH   "\xef\x80\xa8"  /* U+F028 nf-fa-volume_up */
#define ICON_BAT_FULL   "\xef\x89\x80"  /* U+F240 nf-fa-battery_full */
#define ICON_BAT_3Q     "\xef\x89\x81"  /* U+F241 nf-fa-battery_three_quarters */
#define ICON_BAT_HALF   "\xef\x89\x82"  /* U+F242 nf-fa-battery_half */
#define ICON_BAT_1Q     "\xef\x89\x83"  /* U+F243 nf-fa-battery_quarter */
#define ICON_BAT_EMPTY  "\xef\x89\x84"  /* U+F244 nf-fa-battery_empty */
#define ICON_BOLT       "\xef\x83\xa7"  /* U+F0E7 nf-fa-bolt */
#define ICON_CLOCK      "\xef\x80\x97"  /* U+F017 nf-fa-clock_o */
#define ICON_CAPS_ON    "\xef\x80\xa3"  /* U+F023 nf-fa-lock */
#define ICON_CAPS_OFF   "\xef\x82\x9c"  /* U+F09C nf-fa-unlock */

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
	XSync(dpy, False);
}

/* Reads the first line of dir/file into buf, newline stripped. 0 on success. */
static int
readfile(const char *dir, const char *file, char *buf, size_t size)
{
	char path[PATH_MAX];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	if (!(fp = fopen(path, "r")))
		return -1;
	if (!fgets(buf, (int)size, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
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

/* Finds the first system battery (BAT0, BAT1, BATT, CMB0, ...). 0 on success. */
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
		snprintf(path, sizeof(path), PSDIR "/%s", e->d_name);
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

/* Battery percentage: "capacity" if the driver has it, else derived from
 * the charge_* or energy_* files (older ACPI drivers). -1 on failure. */
static int
battery_percent(const char *base)
{
	static const char *now[]  = { "charge_now",  "energy_now" };
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

	if ((capacity = battery_percent(base)) < 0) {
		snprintf(buf, size, "n/a");
		return;
	}

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

	snprintf(buf, size, "%s%s  %d%%", charging ? ICON_BOLT " " : "", icon,
	         capacity);
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
		snprintf(out, size, BLDIR "/%s", e->d_name);
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
	    readlong(dev, "max_brightness", &max) < 0 || max <= 0) {
		snprintf(buf, size, "%s  n/a", ICON_BRIGHTNESS);
		return;
	}
	snprintf(buf, size, "%s  %ld%%", ICON_BRIGHTNESS, (cur * 100 + max / 2) / max);
}

static void
get_volume(char *buf, size_t size)
{
	FILE *fp;
	char line[128];
	float vol = 0.0f;
	int volume;
	const char *icon;

	if (!(fp = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r"))) {
		snprintf(buf, size, "%s  n/a", ICON_VOL_OFF);
		return;
	}
	if (!fgets(line, sizeof(line), fp) ||
	    sscanf(line, "Volume: %f", &vol) != 1) {
		pclose(fp);
		snprintf(buf, size, "%s  n/a", ICON_VOL_OFF);
		return;
	}
	pclose(fp);

	if (strstr(line, "MUTED")) {
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

static void
get_time(char *buf, size_t size)
{
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);

	if (tm == NULL || strftime(buf, size, ICON_CLOCK "  %a %d %b %H:%M", tm) == 0)
		buf[0] = '\0';
}

/* Caps Lock: shows a lock icon only while Caps Lock is active. */
static void
get_capslock(char *buf, size_t size)
{
	XkbStateRec state;
	int on = 0;

	if (XkbGetState(dpy, XkbUseCoreKbd, &state) == Success &&
	    (state.locked_mods & LockMask))
		on = 1;
	snprintf(buf, size, "%s", on ? ICON_CAPS_ON : ICON_CAPS_OFF);
}

/* Appends seg to dst, separated by " | ". Empty segments are skipped. */
static void
append(char *dst, size_t size, const char *seg)
{
	size_t len = strlen(dst);

	if (seg[0] == '\0')
		return;
	snprintf(dst + len, size - len, "%s%s", len ? " | " : "", seg);
}

int
main(void)
{
	struct sigaction sa;
	char bat_path[PATH_MAX], body[256], status[300];
	char caps[32], bri[64], vol[64], bat[64], tim[64];
	const char *batpath = NULL;

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "dwmstatus: cannot open display.\n");
		return 1;
	}

	/* exit cleanly on SIGINT/SIGTERM; no SA_RESTART so sleep() is interrupted */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = onsignal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* TZ never changes, set it once instead of on every refresh */
	setenv("TZ", tzlocal, 1);
	tzset();

	if (find_battery(bat_path, sizeof(bat_path)) == 0)
		batpath = bat_path;

	while (running) {
		get_capslock(caps, sizeof(caps));
		get_brightness(bri, sizeof(bri));
		get_volume(vol, sizeof(vol));
		get_battery(batpath, bat, sizeof(bat));
		get_time(tim, sizeof(tim));

		/* Caps Lock | Brightness | Volume | Battery | Time */
		body[0] = '\0';
		append(body, sizeof(body), caps);
		append(body, sizeof(body), bri);
		append(body, sizeof(body), vol);
		append(body, sizeof(body), bat);
		append(body, sizeof(body), tim);

		snprintf(status, sizeof(status), " %s ", body);
		setstatus(status);

		if (running)
			sleep(INTERVAL);
	}

	setstatus("");
	XCloseDisplay(dpy);
	return 0;
}
