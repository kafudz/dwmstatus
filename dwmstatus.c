/*
dwmstatus - clock status bar for dwm (based on 20h's dwmstatus)
Requires a Nerd Font in dwm's config.h, e.g.
static const char *fonts[] = { "JetBrainsMono Nerd Font:size=10" };

Shows only the time. Brightness, volume, battery and the calendar live in the
combined popup (dwmstatus-popup), which dwm opens when the clock is clicked.
The root window name is only updated when the time actually changes, so dwm
does not redraw its bar twice per second for an identical string.
The visible segment ("tim") is published in _DWMSTATUS_SEGMENTS so dwm can
route a click on the bar to the clock.

Build: gcc -O2 -Wall -o dwmstatus dwmstatus.c -lX11
*/

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

#define UPDATE_INTERVAL_MS 500

/* Local timezone (overridable with DWMSTATUS_TZ in the environment). */
static const char *tzlocal = "Asia/Jakarta";

/* Nerd Font icons as UTF-8 byte sequences */
#define ICON_CLOCK "\xef\x80\x97"  /* U+F017 nf-fa-clock_o */

static Display *dpy;
static Window root;
static Atom segatom; /* _DWMSTATUS_SEGMENTS, published for dwm's click handling */
static volatile sig_atomic_t running = 1;

static void
onsignal(int sig)
{
    (void)sig;
    running = 0;
}

/*
 * Publish the bar text and the names of the segments it contains. dwm reads
 * _DWMSTATUS_SEGMENTS to map a click to the segment under the pointer; with a
 * single "tim" segment every click on the text opens the combined popup.
 */
static void
setstatus(const char *str, const char *segments)
{
    if (segatom != None)
        XChangeProperty(dpy, root, segatom, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)segments,
                        (int)strlen(segments));
    XStoreName(dpy, root, str);
    XFlush(dpy);
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

static void
get_time(char *buf, size_t size)
{
    time_t t;
    struct tm tmv;

    buf[0] = '\0';
    t = time(NULL);
    if (t == (time_t)-1 || localtime_r(&t, &tmv) == NULL)
        return;

    if (strftime(buf, size, ICON_CLOCK "  %H:%M", &tmv) == 0)
        buf[0] = '\0';
}

int
main(int argc, char *argv[])
{
    struct sigaction sa;
    char status[64], last_status[64];
    const char *tz;
    int have_last = 0;

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
    segatom = XInternAtom(dpy, "_DWMSTATUS_SEGMENTS", False);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onsignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: we want nanosleep interrupted */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    tz = getenv("DWMSTATUS_TZ");
    if (tz == NULL || tz[0] == '\0')
        tz = tzlocal;
    if (setenv("TZ", tz, 1) == 0)
        tzset();

    last_status[0] = '\0';

    while (running) {
        get_time(status, sizeof(status));

        /*
         * Only touch the root window name when the time changed. Setting an
         * identical name still makes dwm receive PropertyNotify and redraw
         * its entire bar, which is wasted CPU every poll.
         */
        if (!have_last || strcmp(status, last_status) != 0) {
            setstatus(status, status[0] != '\0' ? "tim" : "");
            snprintf(last_status, sizeof(last_status), "%s", status);
            have_last = 1;
        }

        if (running)
            sleep_ms(UPDATE_INTERVAL_MS);
    }

    setstatus("", "");
    XCloseDisplay(dpy);
    return 0;
}
