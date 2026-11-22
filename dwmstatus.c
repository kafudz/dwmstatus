/*
dwmstatus - standalone status panel for dwm (based on 20h's dwmstatus)

A small, polybar-style panel: two small Cairo-drawn X windows over dwm's bar.
The clock is centred on the bar; volume and power stay at its right end. It
does NOT write dwm's status text and needs no dwm patch; it handles its own
clicks.

Segments, left to right (a segment that is not available is hidden):
    caps  Caps Lock (only while active)        } right end of the bar
    net   network icon (Wi-Fi / Ethernet)      } right end of the bar
    vol   volume (wpctl)                       } right end of the bar
    tim   clock icon and time (opens popup)    } centre of the bar
    pwr   power button (opens the power menu)  } right end of the bar

A left click on a segment runs a popup next to it. Volume and power run
`dwmstatus-popup <name>`, the GTK popup with power, network, Bluetooth,
brightness, volume and battery. The clock runs `dwmstatus-clock`, the combined
clock and calendar popup, centred under the clock. The panel finds dwm's bar
window (WM_CLASS dwm/dwm) and follows its position and height, so it also works
with a bottom bar and hides while dwm's bar is hidden (showbar off).

Requires a Nerd Font, e.g. "JetBrainsMono Nerd Font" (override with
DWMSTATUS_FONT). Colours can be overridden with DWMSTATUS_BG, DWMSTATUS_FG and
DWMSTATUS_DIM (#rrggbb).

Build: see Makefile (needs cairo and X11).
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

/* Intervals (milliseconds). Caps Lock is event driven and needs no polling. */
#define TICK_INTERVAL_MS       1000  /* dwm bar geometry */
#define VOLUME_INTERVAL_MS     2000  /* spawning wpctl is the expensive part */
#define VOLUME_TIMEOUT_MS       200  /* do not let a stuck wpctl freeze us */
#define DISCOVERY_INTERVAL_MS 30000  /* re-scan for the wpctl path */
#define BAR_RESCAN_TICKS         10  /* full search for dwm's bar window */

#define FALLBACK_HEIGHT 24           /* when dwm's bar cannot be found */

#ifndef NETDIR
#define NETDIR "/sys/class/net"
#endif

/* Nerd Font icons as UTF-8 byte sequences */
#define ICON_VOL_OFF      "\xef\x80\xa6"  /* U+F026 nf-fa-volume_off */
#define ICON_VOL_LOW      "\xef\x80\xa7"  /* U+F027 nf-fa-volume_down */
#define ICON_VOL_HIGH     "\xef\x80\xa8"  /* U+F028 nf-fa-volume_up */
#define ICON_CLOCK        "\xef\x80\x97"  /* U+F017 nf-fa-clock_o */
#define ICON_POWER        "\xef\x80\x91"  /* U+F011 nf-fa-power_off */
#define ICON_CAPS_ON      "\xe2\x87\xaa"  /* U+21EA ⇪ CAPS LOCK */
#define ICON_WIFI         "\xef\x87\xab"  /* U+F1EB nf-fa-wifi */
#define ICON_WIRED        "\xef\x82\xac"  /* U+F0AC nf-fa-globe */

/* Gap between the centred bar clock's icon and its HH:MM text. */
#define CLOCK_ICON_GAP    "  "

enum { COL_BG, COL_FG, COL_DIM, COL_ACCENT, NCOL };

/* Two windows: the clock sits alone in the middle of the bar, the volume and
 * power buttons stay together at its right end. */
enum { PANEL_CLOCK, PANEL_RIGHT, NPANELS };

struct panel {
    Window win;
    cairo_surface_t *surf;
    int x, y, w, h;
    int mapped;
};

struct segment {
    const char *name;   /* segment name (documentation / debugging) */
    const char *arg;    /* argument passed to the popup */
    char text[32];      /* UTF-8 icon(s); empty = hidden */
    int color;
    int panel;          /* panel window this segment lives in */
    int x, w;           /* position inside the panel (valid after layout) */
};

static struct segment segs[] = {
    { "tim", "tim", "",            COL_FG,     PANEL_CLOCK, 0, 0 },
    { "caps", "", "",              COL_ACCENT, PANEL_RIGHT, 0, 0 },
    { "net", "net", "",            COL_FG,     PANEL_RIGHT, 0, 0 },
    { "vol", "vol", "",            COL_FG,     PANEL_RIGHT, 0, 0 },
    { "pwr", "pwr", ICON_POWER,    COL_FG,     PANEL_RIGHT, 0, 0 },
};
#define NSEG (sizeof(segs) / sizeof(segs[0]))

static double palette[NCOL][3] = {
    { 0x10 / 255.0, 0x11 / 255.0, 0x13 / 255.0 }, /* bg: same as the popup */
    { 0xe6 / 255.0, 0xe6 / 255.0, 0xe6 / 255.0 }, /* fg */
    { 0x6b / 255.0, 0x6f / 255.0, 0x76 / 255.0 }, /* dim: off / muted */
    { 0x35 / 255.0, 0x84 / 255.0, 0xe4 / 255.0 }, /* accent: Caps Lock on */
};

static Display *dpy;
static Window root, barwin;
static int screen;
static int xkb_ok, xkb_event_base;
static struct panel panels[NPANELS];
static volatile sig_atomic_t running = 1;

static int font_h; /* bar height the fonts were built for */

static void
onsignal(int sig)
{
    (void)sig;
    running = 0;
}

/* Never let a stale window id (dwm bar gone) kill the panel. */
static int
xerror(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadWindow || e->error_code == BadDrawable)
        return 0;
    fprintf(stderr, "dwmstatus: X error %d (request %d)\n",
            e->error_code, e->request_code);
    return 0;
}

static int
join_path(char *dst, size_t size, const char *dir, const char *name)
{
    int n = snprintf(dst, size, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

static int
path_exists(const char *dir, const char *name)
{
    char path[PATH_MAX];

    return join_path(path, sizeof(path), dir, name) == 0 &&
           access(path, F_OK) == 0;
}

/* Reads a small sysfs attribute, stripping the trailing newline. */
static int
read_attr(const char *dir, const char *name, char *buf, size_t size)
{
    char path[PATH_MAX];
    int fd;
    ssize_t n;

    if (size == 0 || join_path(path, sizeof(path), dir, name) < 0)
        return -1;
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static long long
now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Milliseconds left until deadline; clamped to what poll(2) accepts. */
static int
time_left_ms(long long deadline)
{
    long long left = deadline - now_ms();

    if (left <= 0)
        return 0;
    if (left > INT_MAX)
        return INT_MAX;
    return (int)left;
}

/* ----------------------------------------------------------------- volume */

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
 * panel. 0 on success.
 */
static int
run_capture(const char *prog, char *const argv[], char *buf, size_t size,
            int timeout_ms)
{
    int fds[2], status = -1, timed_out = 0, reaped = 0, devnull;
    pid_t pid;
    size_t off = 0;
    long long deadline;
    struct pollfd pfd;

    if (size == 0)
        return -1;
    buf[0] = '\0';
    deadline = now_ms() + timeout_ms;

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
        int pr, left;

        dst = (off < size - 1) ? buf + off : discard;
        room = (off < size - 1) ? size - 1 - off : sizeof(discard);

        /* Budget is for the whole call, not for each read. */
        left = time_left_ms(deadline);
        if (left == 0) {
            timed_out = 1;
            break;
        }

        pfd.revents = 0;
        pr = poll(&pfd, 1, left);
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

    /*
     * A child can close stdout and keep running, which looks like EOF here.
     * Reap it, but not past the deadline, so it cannot freeze the panel.
     */
    while (!reaped && !timed_out) {
        pid_t w;
        int left;

        if (!running) {
            timed_out = 1;
            break;
        }

        w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = 1;
            break;
        }
        if (w < 0) {
            if (errno == EINTR)
                continue;
            status = -1;
            reaped = 1;
            break;
        }

        left = time_left_ms(deadline);
        if (left == 0) {
            timed_out = 1;
            break;
        }
        poll(NULL, 0, left > 5 ? 5 : left);
    }

    if (!reaped) {
        /* SIGKILL cannot be caught; the blocking reap below is bounded. */
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                status = -1;
                break;
            }
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
get_volume(char *buf, size_t size, int *color)
{
    char line[128];
    char *argv[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    double vol;
    int volume;
    const char *icon;

    buf[0] = '\0';
    *color = COL_FG;
    if (wpctl_path[0] == '\0')
        return;

    if (run_capture(wpctl_path, argv, line, sizeof(line),
                    VOLUME_TIMEOUT_MS) < 0)
        return;

    if (sscanf(line, "Volume: %lf", &vol) != 1)
        return;

    if (strstr(line, "MUTED")) {
        snprintf(buf, size, "%s", ICON_VOL_OFF);
        *color = COL_DIM;
        return;
    }

    if (!(vol >= 0.0)) /* also catches NaN from a broken wpctl */
        vol = 0.0;
    else if (vol > 10.0) /* sanity clamp before the int cast */
        vol = 10.0;
    volume = (int)(vol * 100.0 + 0.5);

    if (volume == 0) {
        icon = ICON_VOL_OFF;
        *color = COL_DIM;
    } else if (volume < 50) {
        icon = ICON_VOL_LOW;
    } else {
        icon = ICON_VOL_HIGH;
    }

    /* Icon only; the percentage is shown in dwmstatus-popup. */
    snprintf(buf, size, "%s", icon);
}

/* ------------------------------------------------------------------ state */

static void
discover(void)
{
    if (find_program("wpctl", wpctl_path, sizeof(wpctl_path)) < 0)
        wpctl_path[0] = '\0';
}

/* Stores a segment's new state. Returns 1 if it changed. */
static int
setseg(struct segment *s, const char *text, int color)
{
    if (strcmp(s->text, text) == 0 && (text[0] == '\0' || s->color == color))
        return 0;
    snprintf(s->text, sizeof(s->text), "%s", text);
    s->color = color;
    return 1;
}

static struct segment *
find_segment(const char *name)
{
    size_t i;

    for (i = 0; i < NSEG; i++)
        if (strcmp(segs[i].name, name) == 0)
            return &segs[i];
    return NULL;
}

static int
refresh_volume(void)
{
    char buf[32];
    int color;
    struct segment *s = find_segment("vol");

    if (s == NULL)
        return 0;
    get_volume(buf, sizeof(buf), &color);
    return setseg(s, buf, color);
}

static int
refresh_clock(void)
{
    struct segment *s = find_segment("tim");
    struct tm tm;
    char text[16], buf[32];
    time_t now;

    if (s == NULL)
        return 0;
    now = time(NULL);
    if (localtime_r(&now, &tm) == NULL)
        return 0;
    if (strftime(text, sizeof(text), "%H:%M", &tm) == 0)
        return 0;
    snprintf(buf, sizeof(buf), "%s%s%s", ICON_CLOCK, CLOCK_ICON_GAP, text);
    return setseg(s, buf, COL_FG);
}

/* -------------------------------------------------------------- caps lock */

/* Show ⇪ only while Caps Lock is actually latched. */
static int
refresh_caps(void)
{
    XkbStateRec state;
    struct segment *s = find_segment("caps");
    const char *text = "";

    if (s == NULL)
        return 0;
    if (XkbGetState(dpy, XkbUseCoreKbd, &state) == Success &&
        (state.locked_mods & LockMask))
        text = ICON_CAPS_ON;
    return setseg(s, text, COL_ACCENT);
}

/* ------------------------------------------------------------------ network */

/* Wi-Fi or Ethernet, dimmed while the link is down; hidden if none exists. */
static int
refresh_network(void)
{
    DIR *d;
    struct dirent *e;
    struct segment *s = find_segment("net");
    char dir[PATH_MAX], val[32];
    int have_wired = 0, have_wifi = 0, wired_up = 0, wifi_up = 0;

    if (s == NULL)
        return 0;
    if ((d = opendir(NETDIR)) == NULL)
        return setseg(s, "", COL_FG);

    while ((e = readdir(d)) != NULL) {
        int wifi, up;

        if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0)
            continue;
        if (join_path(dir, sizeof(dir), NETDIR, e->d_name) < 0)
            continue;
        /* Virtual devices (docker, veth, bridges) have no "device" link. */
        if (!path_exists(dir, "device"))
            continue;
        wifi = path_exists(dir, "wireless") || path_exists(dir, "phy80211");
        up = read_attr(dir, "operstate", val, sizeof(val)) == 0 &&
             strcmp(val, "up") == 0;
        if (wifi) {
            have_wifi = 1;
            wifi_up |= up;
        } else {
            have_wired = 1;
            wired_up |= up;
        }
    }
    closedir(d);

    if (wired_up)
        return setseg(s, ICON_WIRED, COL_FG);
    if (wifi_up)
        return setseg(s, ICON_WIFI, COL_FG);
    if (have_wifi)
        return setseg(s, ICON_WIFI, COL_DIM);
    if (have_wired)
        return setseg(s, ICON_WIRED, COL_DIM);
    return setseg(s, "", COL_FG);
}

/* ------------------------------------------------------------------ fonts */

#define NFACES 5
static const char *families[NFACES] = {
    NULL, /* DWMSTATUS_FONT or the default, set in main() */
    "Symbols Nerd Font",
    "Symbols Nerd Font Mono",
    "DejaVu Sans",
    "sans-serif",
};
static cairo_scaled_font_t *faces[NFACES];
static double font_ascent, font_descent;

static void
free_fonts(void)
{
    int i;

    for (i = 0; i < NFACES; i++) {
        if (faces[i]) {
            cairo_scaled_font_destroy(faces[i]);
            faces[i] = NULL;
        }
    }
}

static void
build_fonts(int height)
{
    cairo_font_options_t *opts;
    cairo_font_extents_t fe;
    cairo_matrix_t fm, ctm;
    cairo_font_face_t *ff;
    double px_size = height * 0.6;
    int i;

    if (px_size < 8)
        px_size = 8;

    free_fonts();

    cairo_matrix_init_scale(&fm, px_size, px_size);
    cairo_matrix_init_identity(&ctm);
    opts = cairo_font_options_create();

    for (i = 0; i < NFACES; i++) {
        ff = cairo_toy_font_face_create(families[i], CAIRO_FONT_SLANT_NORMAL,
                                        CAIRO_FONT_WEIGHT_NORMAL);
        faces[i] = cairo_scaled_font_create(ff, &fm, &ctm, opts);
        cairo_font_face_destroy(ff);
        if (cairo_scaled_font_status(faces[i]) != CAIRO_STATUS_SUCCESS) {
            cairo_scaled_font_destroy(faces[i]);
            faces[i] = NULL;
        }
    }
    cairo_font_options_destroy(opts);

    font_ascent = font_descent = 0;
    for (i = 0; i < NFACES; i++) {
        if (faces[i]) {
            cairo_scaled_font_extents(faces[i], &fe);
            font_ascent = fe.ascent;
            font_descent = fe.descent;
            break;
        }
    }
    font_h = height;
}

static int
utf8_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c >> 5) == 0x6)
        return 2;
    if ((c >> 4) == 0xe)
        return 3;
    if ((c >> 3) == 0x1e)
        return 4;
    return 1;
}

/*
 * Picks the first font that has a glyph for the UTF-8 character at u. Cairo
 * does no per-glyph fallback, and the Nerd Font may lack e.g. U+21EA.
 * Returns the face and its glyphs (caller frees); NULL if there is no face.
 */
static cairo_scaled_font_t *
pick_face(const char *u, int len, cairo_glyph_t **glyphs, int *n)
{
    cairo_scaled_font_t *first = NULL;
    cairo_glyph_t *g, *first_g = NULL;
    int ng, first_n = 0, i;

    for (i = 0; i < NFACES; i++) {
        if (!faces[i])
            continue;
        g = NULL;
        ng = 0;
        if (cairo_scaled_font_text_to_glyphs(faces[i], 0, 0, u, len, &g,
                                             &ng, NULL, NULL, NULL) !=
            CAIRO_STATUS_SUCCESS || ng <= 0) {
            cairo_glyph_free(g);
            continue;
        }
        if (g[0].index != 0) { /* index 0 is .notdef: glyph missing */
            cairo_glyph_free(first_g);
            *glyphs = g;
            *n = ng;
            return faces[i];
        }
        if (!first) {
            first = faces[i];
            first_g = g;
            first_n = ng;
        } else {
            cairo_glyph_free(g);
        }
    }
    *glyphs = first_g;
    *n = first_n;
    return first;
}

/*
 * Draws text with baseline y starting at x (cr may be NULL to only measure)
 * and returns its advance width.
 */
static double
draw_text(cairo_t *cr, double x, double y, const char *text)
{
    const char *p;
    double start = x;
    int len, n, k;
    cairo_glyph_t *g;
    cairo_scaled_font_t *face;
    cairo_text_extents_t ext;

    for (p = text; *p; p += len) {
        len = utf8_len((unsigned char)*p);
        face = pick_face(p, len, &g, &n);
        if (!face)
            continue;
        cairo_scaled_font_glyph_extents(face, g, n, &ext);
        if (cr) {
            cairo_set_scaled_font(cr, face);
            for (k = 0; k < n; k++) {
                g[k].x += x;
                g[k].y += y;
            }
            cairo_show_glyphs(cr, g, n);
        }
        x += ext.x_advance;
        cairo_glyph_free(g);
    }
    return x - start;
}

/* ----------------------------------------------------------------- layout */

/* Computes the x/w of each visible segment in one panel; returns its width. */
static int
layout(int height, int panel)
{
    int pad = height * 2 / 5, x = 0;
    size_t i;

    if (pad < 5)
        pad = 5;
    for (i = 0; i < NSEG; i++) {
        struct segment *s = &segs[i];

        if (s->panel != panel)
            continue;
        s->x = s->w = 0;
        if (s->text[0] == '\0')
            continue;
        s->w = (int)(draw_text(NULL, 0, 0, s->text) + 0.5) + 2 * pad;
        s->x = x;
        x += s->w;
    }
    return x;
}

static void
set_source(cairo_t *cr, int col)
{
    cairo_set_source_rgb(cr, palette[col][0], palette[col][1],
                         palette[col][2]);
}

static void
draw(struct panel *p)
{
    cairo_t *cr;
    double baseline;
    int pad;
    size_t i;

    if (!p->surf || !p->mapped)
        return;

    cr = cairo_create(p->surf);
    set_source(cr, COL_BG);
    cairo_paint(cr);

    pad = p->h * 2 / 5;
    if (pad < 5)
        pad = 5;
    baseline = (p->h - (font_ascent + font_descent)) / 2.0 + font_ascent;

    for (i = 0; i < NSEG; i++) {
        struct segment *s = &segs[i];

        if (s->panel != p - panels || s->text[0] == '\0')
            continue;
        set_source(cr, s->color);
        draw_text(cr, s->x + pad, baseline, s->text);
    }
    cairo_destroy(cr);
    cairo_surface_flush(p->surf);
    XFlush(dpy);
}

/* ------------------------------------------------------------ dwm bar/win */

/* Finds dwm's bar window (WM_CLASS dwm/dwm), the leftmost one. 0 if none. */
static Window
find_bar(void)
{
    Window r, p, *kids = NULL, best = 0;
    unsigned int n, i;
    int best_x = 0;

    if (!XQueryTree(dpy, root, &r, &p, &kids, &n))
        return 0;

    for (i = 0; i < n; i++) {
        XClassHint ch;
        XWindowAttributes wa;
        int isbar, j, ispanel = 0;

        for (j = 0; j < NPANELS; j++)
            if (kids[i] == panels[j].win)
                ispanel = 1;
        if (ispanel)
            continue;
        ch.res_name = ch.res_class = NULL;
        if (!XGetClassHint(dpy, kids[i], &ch))
            continue;
        isbar = ch.res_name && ch.res_class &&
                strcmp(ch.res_name, "dwm") == 0 &&
                strcmp(ch.res_class, "dwm") == 0;
        if (ch.res_name)
            XFree(ch.res_name);
        if (ch.res_class)
            XFree(ch.res_class);
        if (!isbar || !XGetWindowAttributes(dpy, kids[i], &wa) ||
            wa.map_state != IsViewable)
            continue;
        if (!best || wa.x < best_x) {
            best = kids[i];
            best_x = wa.x;
        }
    }
    if (kids)
        XFree(kids);
    return best;
}

/*
 * Geometry of dwm's bar. Returns 0 when there is none. rescan forces a new
 * search; otherwise the cached window is only re-read.
 */
static int
bar_geometry(int rescan, int *x, int *y, int *w, int *h)
{
    XWindowAttributes wa;

    if (rescan || !barwin || !XGetWindowAttributes(dpy, barwin, &wa)) {
        barwin = find_bar();
        if (!barwin || !XGetWindowAttributes(dpy, barwin, &wa)) {
            barwin = 0;
            return 0;
        }
    }
    *x = wa.x;
    *y = wa.y;
    *w = wa.width;
    *h = wa.height;
    return 1;
}

static void
create_panel(struct panel *p, int x, int y, int w, int h)
{
    XSetWindowAttributes wa;
    XClassHint ch = { "dwmstatus", "dwmstatus" };
    XColor c;
    Atom type, dock;

    c.red = (unsigned short)(palette[COL_BG][0] * 65535);
    c.green = (unsigned short)(palette[COL_BG][1] * 65535);
    c.blue = (unsigned short)(palette[COL_BG][2] * 65535);
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, screen), &c);

    wa.override_redirect = True; /* dwm must not manage or tile the panel */
    wa.background_pixel = c.pixel;
    wa.event_mask = ExposureMask | ButtonPressMask;
    p->win = XCreateWindow(dpy, root, x, y, (unsigned)w, (unsigned)h, 0,
                           DefaultDepth(dpy, screen), CopyFromParent,
                           DefaultVisual(dpy, screen),
                           CWOverrideRedirect | CWBackPixel | CWEventMask,
                           &wa);
    XSetClassHint(dpy, p->win, &ch);
    XStoreName(dpy, p->win, "dwmstatus");

    type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, p->win, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&dock, 1);

    p->surf = cairo_xlib_surface_create(dpy, p->win,
                                        DefaultVisual(dpy, screen), w, h);
    p->x = x;
    p->y = y;
    p->w = w;
    p->h = h;
}

/* Moves/sizes one panel window and brings it on screen at its new place. */
static void
place_panel(struct panel *p, int x, int y, int w, int h, int force)
{
    if (!p->win) {
        create_panel(p, x, y, w, h);
        force = 1;
    } else if (x != p->x || y != p->y || w != p->w || h != p->h) {
        cairo_xlib_surface_set_size(p->surf, w, h);
        XMoveResizeWindow(dpy, p->win, x, y, (unsigned)w, (unsigned)h);
        p->x = x;
        p->y = y;
        p->w = w;
        p->h = h;
        force = 1;
    }
    if (!p->mapped) {
        XMapRaised(dpy, p->win);
        p->mapped = 1;
        force = 1;
    }
    if (force)
        draw(p);
}

/* Takes one panel off screen (dwm's bar hidden, or no segments in it). */
static void
hide_panel(struct panel *p)
{
    if (p->win && p->mapped) {
        XUnmapWindow(dpy, p->win);
        p->mapped = 0;
    }
}

/*
 * Brings both panels in line with dwm's bar and the current segments:
 * height, position (clock in the middle, volume/power at the right end),
 * visibility. Redraws when anything changed or when force is set.
 */
static void
update_panel(int rescan, int force)
{
    int bx = 0, by = 0, bw = 0, bh = 0, have_bar;
    int nh, hidden = 0, widths[NPANELS], nx[NPANELS], ny[NPANELS];
    int sw, i;

    have_bar = bar_geometry(rescan, &bx, &by, &bw, &bh);
    if (have_bar) {
        nh = bh;
        /* dwm hides its bar by moving it just off the screen */
        if (by + bh <= 0 || by >= DisplayHeight(dpy, screen))
            hidden = 1;
    } else {
        const char *h = getenv("DWMSTATUS_HEIGHT");

        nh = (h && atoi(h) > 0) ? atoi(h) : FALLBACK_HEIGHT;
    }
    if (nh < 8)
        nh = 8;

    if (nh != font_h) {
        build_fonts(nh);
        force = 1;
    }

    for (i = 0; i < NPANELS; i++)
        widths[i] = layout(nh, i);
    if (widths[PANEL_CLOCK] <= 0 && widths[PANEL_RIGHT] <= 0)
        hidden = 1;

    if (hidden) {
        for (i = 0; i < NPANELS; i++)
            hide_panel(&panels[i]);
        XFlush(dpy);
        return;
    }

    sw = DisplayWidth(dpy, screen);
    /* The clock is centred on dwm's bar (or on the screen without one). */
    nx[PANEL_CLOCK] = (have_bar ? bx + bw / 2 : sw / 2)
                      - widths[PANEL_CLOCK] / 2;
    nx[PANEL_RIGHT] = have_bar ? bx + bw - widths[PANEL_RIGHT]
                               : sw - widths[PANEL_RIGHT];
    ny[PANEL_CLOCK] = ny[PANEL_RIGHT] = have_bar ? by : 0;

    for (i = 0; i < NPANELS; i++) {
        if (widths[i] <= 0) {
            hide_panel(&panels[i]);
            continue;
        }
        if (nx[i] < 0)
            nx[i] = 0;
        if (nx[i] + widths[i] > sw)
            nx[i] = sw - widths[i];
        if (nx[i] < 0)
            nx[i] = 0;
        place_panel(&panels[i], nx[i], ny[i], widths[i], nh, force);
    }
    XFlush(dpy);
}

/* ----------------------------------------------------------------- clicks */

/*
 * Runs the popup detached (double fork, so there is never a zombie and no
 * waitpid() race with run_capture). The panel geometry is passed along so
 * the popup can open right below (or above) the panel.
 */
static void
spawn_popup(const char *arg, struct panel *p)
{
    const char *cmd;
    pid_t pid;

    /*
     * The clock has its own combined clock and calendar popup; everything
     * else opens the system popup. Both can be overridden from the
     * environment.
     */
    if (strcmp(arg, "tim") == 0) {
        cmd = getenv("DWMSTATUS_CLOCK");
        if (cmd == NULL || cmd[0] == '\0')
            cmd = "dwmstatus-clock";
    } else {
        cmd = getenv("DWMSTATUS_POPUP");
        if (cmd == NULL || cmd[0] == '\0')
            cmd = "dwmstatus-popup";
    }

    if ((pid = fork()) < 0)
        return;
    if (pid == 0) {
        char anchor[96];
        int devnull;
        pid_t g;

        if ((g = fork()) != 0)
            _exit(g < 0 ? 1 : 0);

        setsid();
        snprintf(anchor, sizeof(anchor), "%d,%d,%d,%d",
                 p->x, p->y, p->w, p->h);
        setenv("DWMSTATUS_PANEL", anchor, 1);
        if ((devnull = open("/dev/null", O_RDWR)) >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        if (strcmp(arg, "tim") == 0)
            execlp(cmd, cmd, (char *)NULL);
        else
            execlp(cmd, cmd, arg, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

static void
buttonpress(struct panel *p, XButtonEvent *ev)
{
    size_t i;

    if (ev->button != Button1)
        return;
    for (i = 0; i < NSEG; i++) {
        if (segs[i].panel != p - panels || segs[i].w == 0)
            continue;
        if (ev->x >= segs[i].x && ev->x < segs[i].x + segs[i].w) {
            spawn_popup(segs[i].arg, p);
            return;
        }
    }
}

/* ------------------------------------------------------------------- main */

static void
parse_color(const char *envname, int col)
{
    const char *s = getenv(envname);
    unsigned int r, g, b;
    char tail;

    if (s && sscanf(s, "#%2x%2x%2x%c", &r, &g, &b, &tail) == 3) {
        palette[col][0] = r / 255.0;
        palette[col][1] = g / 255.0;
        palette[col][2] = b / 255.0;
    }
}

/* One panel per user and display; a second one would only fight the first. */
static int
single_instance(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    const char *disp = getenv("DISPLAY");
    char path[PATH_MAX];
    int fd;

    if (!dir || !dir[0])
        dir = "/tmp";
    snprintf(path, sizeof(path), "%s/dwmstatus-%u-%s.lock", dir,
             (unsigned)getuid(), disp ? disp : "");
    if ((fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600)) < 0)
        return 1; /* cannot lock: carry on rather than refuse to start */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0)
        return 0;
    return 1; /* fd stays open (and locked) for the life of the process */
}

int
main(int argc, char *argv[])
{
    struct sigaction sa;
    long long now, next_tick, next_volume, next_discovery;
    struct pollfd pfd;
    int tick = 0, changed, redraw, timeout;
    size_t i;
    const char *font;

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
    if (!single_instance()) {
        fprintf(stderr, "dwmstatus: already running.\n");
        return 1;
    }
    XSetErrorHandler(xerror);
    screen = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    /* Do not leak the X connection into short-lived children. */
    {
        int flags = fcntl(ConnectionNumber(dpy), F_GETFD);
        if (flags >= 0)
            fcntl(ConnectionNumber(dpy), F_SETFD, flags | FD_CLOEXEC);
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onsignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: we want poll interrupted */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    font = getenv("DWMSTATUS_FONT");
    families[0] = (font && font[0]) ? font : "JetBrainsMono Nerd Font";
    parse_color("DWMSTATUS_BG", COL_BG);
    parse_color("DWMSTATUS_FG", COL_FG);
    parse_color("DWMSTATUS_DIM", COL_DIM);
    parse_color("DWMSTATUS_ACCENT", COL_ACCENT);

    /* The bar clock and the popup clock use the same time zone. */
    {
        const char *tz = getenv("DWMSTATUS_TZ");

        if (tz == NULL || tz[0] == '\0')
            tz = "Asia/Jakarta";
        setenv("TZ", tz, 1);
        tzset();
    }

    /*
     * Older versions put the status in dwm's bar through the root window
     * name and _DWMSTATUS_SEGMENTS. Remove those; a single space also stops
     * dwm from showing its own "dwm-x.y" banner next to the panel.
     */
    XDeleteProperty(dpy, root, XInternAtom(dpy, "_DWMSTATUS_SEGMENTS", False));
    XStoreName(dpy, root, " ");

    /* Caps Lock is event driven; fall back to polling if XKB is unavailable. */
    {
        int opcode, error, major = XkbMajorVersion, minor = XkbMinorVersion;

        if (XkbQueryExtension(dpy, &opcode, &xkb_event_base, &error, &major,
                              &minor)) {
            XkbSelectEventDetails(dpy, XkbUseCoreKbd, XkbStateNotify,
                                  XkbModifierLockMask, XkbModifierLockMask);
            xkb_ok = 1;
        }
    }

    discover();
    refresh_caps();
    refresh_network();
    refresh_volume();
    refresh_clock();
    update_panel(1, 1);

    now = now_ms();
    next_tick = now + TICK_INTERVAL_MS;
    next_volume = now + VOLUME_INTERVAL_MS;
    next_discovery = now + DISCOVERY_INTERVAL_MS;
    pfd.fd = ConnectionNumber(dpy);
    pfd.events = POLLIN;

    while (running) {
        changed = 0;
        redraw = 0;

        XFlush(dpy);
        while (XPending(dpy)) {
            XkbEvent ev;
            int i, hit = -1;

            XNextEvent(dpy, &ev.core);
            if (xkb_ok && ev.type == xkb_event_base &&
                ev.any.xkb_type == XkbStateNotify) {
                changed |= refresh_caps();
                continue;
            }
            for (i = 0; i < NPANELS; i++)
                if (ev.core.xany.window == panels[i].win)
                    hit = i;
            if (hit < 0)
                continue;
            if (ev.core.type == Expose && ev.core.xexpose.count == 0)
                redraw = 1;
            else if (ev.core.type == ButtonPress)
                buttonpress(&panels[hit], &ev.core.xbutton);
        }

        now = now_ms();
        if (now >= next_discovery) {
            discover();
            next_discovery = now + DISCOVERY_INTERVAL_MS;
            next_tick = now;
            next_volume = now;
        }
        if (now >= next_volume) {
            changed |= refresh_volume();
            next_volume = now + VOLUME_INTERVAL_MS;
        }
        if (now >= next_tick) {
            tick++;
            if (!xkb_ok)
                changed |= refresh_caps();
            changed |= refresh_network();
            changed |= refresh_clock();
            /* geometry of dwm's bar, with an occasional full search */
            update_panel(tick % BAR_RESCAN_TICKS == 0, changed || redraw);
            changed = redraw = 0;
            next_tick = now + TICK_INTERVAL_MS;
        }
        if (changed || redraw)
            update_panel(0, 1);

        if (!running)
            break;

        timeout = time_left_ms(next_tick);
        if (time_left_ms(next_volume) < timeout)
            timeout = time_left_ms(next_volume);
        if (XPending(dpy))
            continue;
        if (poll(&pfd, 1, timeout) < 0 && errno != EINTR)
            break;
    }

    XStoreName(dpy, root, "");
    free_fonts();
    for (i = 0; i < NPANELS; i++) {
        if (panels[i].surf)
            cairo_surface_destroy(panels[i].surf);
        if (panels[i].win)
            XDestroyWindow(dpy, panels[i].win);
    }
    XCloseDisplay(dpy);
    cairo_debug_reset_static_data(); /* release Cairo's global caches */
    return 0;
}
