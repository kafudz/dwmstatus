# dwmstatus

My personal status bar for [dwm](https://dwm.suckless.org/).  
It writes system info to the X11 root window name, which dwm shows in its bar.

Based on the classic `dwmstatus` by **20h**.

## Personal use

This repo is for **my own setup**.  
It is not a general-purpose project, and I don’t guarantee it will work on your machine.  
Feel free to read/copy it, but expect to edit the source for your own system.

## What it shows

The bar shows Nerd Font icons only, left to right: Caps Lock (only while
active), brightness, volume, battery and clock. The icons are separated by
invisible spacing (no `|` pipes). Percentages and the clock digits are shown in
the combined popup described under [Click actions](#click-actions). The popup
clock uses `DWMSTATUS_TZ` (default `Asia/Jakarta`).

The root window name is only rewritten when the status actually changes, so dwm
does not redraw its bar on every poll. Caps Lock is checked every 0.5 s,
brightness/battery every 1 s, volume every 2 s, and sysfs devices are re-scanned
every 30 s so hotplug still works.

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
`config.h`. `dwmstatus` publishes the visible segment names in the
`_DWMSTATUS_SEGMENTS` root window property (`caps,bri,vol,bat,tim`); the dwm
patch in this setup maps a click to the segment under the pointer.

Clicking an icon opens one combined GTK 3 popup, `dwmstatus-popup`, focused on
the matching section: brightness, volume, battery, or the calendar/clock. The
Caps Lock icon also opens the clock section. The popup always appears at a fixed
position: 40 px from the top and 10 px from the right edge of the monitor. Its
window title is `status`, so dwm's title area does not mark the popup as
`broken` while it is open.

The popup starts with a large live clock (`HH:MM`), followed by a brightness
slider, a volume slider with a mute button, a read-only battery level bar with
its charging state, and a calendar whose header shows the selected date. Each
section is labelled with a Nerd Font icon (clock, sun, volume, battery,
calendar); the volume and battery icons follow the level, and a bolt appears
while charging. Clicking a status icon again closes the popup, as do `Esc`,
`Return` and clicking elsewhere. Sections without a working backend are hidden,
and the clock and calendar are always available.

The popup needs Python 3, PyGObject and GTK 3 (Arch packages `python`,
`python-gobject`, `gtk3`). Volume changes go through `wpctl` (PipeWire) or
`pactl` (PulseAudio); brightness goes through `brightnessctl` or a writable
sysfs backlight. Moving the volume slider while muted also unmutes.

`dwmstatus-volume` and `dwmstatus-brightness` remain as single-control sliders
(and thin wrappers around the shared `dwmstatus-slider`); `dwmstatus-calendar`
still shows the month as a notification for setups that prefer that. Their
command line modes work without GTK:
`dwmstatus-volume --get|--set PERCENT|--mute|--unmute|--toggle-mute` and
`dwmstatus-brightness --get|--set PERCENT`.

Install the helpers with:

```bash
make install
```

Example `config.h`:

```c
static const char *popupbricmd[] = { "dwmstatus-popup", "bri", NULL };
static const char *popupvolcmd[] = { "dwmstatus-popup", "vol", NULL };
static const char *popupbatcmd[] = { "dwmstatus-popup", "bat", NULL };
static const char *popuptimcmd[] = { "dwmstatus-popup", "tim", NULL };

static const Button buttons[] = {
	/* ... */
	{ ClkStatusText, 0, Button1, spawn, {.v = popuptimcmd }, "tim" },
	{ ClkStatusText, 0, Button1, spawn, {.v = popupvolcmd }, "vol" },
	{ ClkStatusText, 0, Button1, spawn, {.v = popupbricmd }, "bri" },
	{ ClkStatusText, 0, Button1, spawn, {.v = popupbatcmd }, "bat" },
	{ ClkStatusText, 0, Button1, spawn, {.v = popuptimcmd }, "caps" },
	{ ClkStatusText, 0, Button3, spawn, {.v = termcmd } },
	/* ... */
};
```

The optional last field is the segment name (`caps`, `bri`, `vol`, `bat`,
`tim`). Leaving it out means "the whole status text"; a segment binding wins
over a generic one. Stock dwm does not have this field or the
`_DWMSTATUS_SEGMENTS` lookup, so the small patch to `buttonpress()` and the
`Button` struct is required for segment clicks.
