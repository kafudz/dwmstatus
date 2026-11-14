# dwmstatus

My personal status bar for [dwm](https://dwm.suckless.org/).  
It writes system info to the X11 root window name, which dwm shows in its bar.

Based on the classic `dwmstatus` by **20h**.

## Personal use

This repo is for **my own setup**.  
It is not a general-purpose project, and I don’t guarantee it will work on your machine.  
Feel free to read/copy it, but expect to edit the source for your own system.

## What it shows

Segments are shown left to right; any segment that is not available is hidden.

- Caps Lock state (icon only while active)
- Brightness (from `/sys/class/backlight`)
- Volume (via `wpctl`; hidden if unavailable)
- Battery (from `/sys/class/power_supply`)
- Clock (timezone defaults to `Asia/Jakarta`; override with `DWMSTATUS_TZ`)

The root window name is only rewritten when the status actually changes, so dwm
does not redraw its bar on every poll. Caps Lock and the clock are checked every
0.5 s, battery/brightness every 1 s, volume every 2 s, and sysfs devices are
re-scanned every 30 s so hotplug still works.

Uses Nerd Font icons. Make sure your dwm font supports them.

## Build

```bash
git clone https://github.com/kafudz/dwmstatus.git
cd dwmstatus
make
sudo make install
```

`dwmstatus -v` prints the version.

## Restart

After rebuilding, run `dwmstatus-restart` (installed next to `dwmstatus`) to
replace the running instance. Or start it from your `.xinitrc` / autostart:

```bash
dwmstatus &
```

## Click actions

dwm draws the bar and receives the clicks, so click bindings live in dwm's
`config.h`. `dwmstatus` publishes the name of every visible segment in the
`_DWMSTATUS_SEGMENTS` root window property (for example `bri,vol,bat,tim`);
the dwm patch in this setup maps a click to the segment under the pointer.
Bindings without a segment apply to the whole status text.

`dwmstatus-calendar` shows the current month in a popup. It prefers
`dunstify` / `notify-send`, then falls back to `st`, `dmenu`, or plain text.
`cal` (util-linux) is required; the display tools are optional.

`dwmstatus-volume` and `dwmstatus-brightness` show interactive GTK 3 sliders;
both use the shared `dwmstatus-slider` helper and need Python 3, PyGObject and
GTK 3 (Arch packages `python`, `python-gobject`, `gtk3`). Volume changes go
through `wpctl` (PipeWire) or `pactl` (PulseAudio); brightness goes through
`brightnessctl` or a writable sysfs backlight. Moving the volume slider while
muted also unmutes. Clicking the same segment again closes the open popup.
The command line modes work without GTK:
`dwmstatus-volume --get|--set PERCENT|--mute|--unmute|--toggle-mute` and
`dwmstatus-brightness --get|--set PERCENT`.

Install the helpers with:

```bash
make install
```

Example `config.h`:

```c
static const char *calendarcmd[]   = { "dwmstatus-calendar", NULL };
static const char *volumecmd[]     = { "dwmstatus-volume", NULL };
static const char *brightnesscmd[] = { "dwmstatus-brightness", NULL };

static const Button buttons[] = {
	/* ... */
	{ ClkStatusText, 0, Button1, spawn, {.v = calendarcmd },   "tim" },
	{ ClkStatusText, 0, Button1, spawn, {.v = volumecmd },     "vol" },
	{ ClkStatusText, 0, Button1, spawn, {.v = brightnesscmd }, "bri" },
	{ ClkStatusText, 0, Button3, spawn, {.v = termcmd } },
	/* ... */
};
```

The last field is the segment name: `caps`, `bri`, `vol`, `bat` or `tim`.
Leaving it out means "the whole status text"; a segment binding wins over a
generic one. Stock dwm does not have this field or the `_DWMSTATUS_SEGMENTS`
lookup, so the small patch to `buttonpress()` and the `Button` struct is
required for per-segment clicks.
