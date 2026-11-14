# dwmstatus

My personal status bar for [dwm](https://dwm.suckless.org/).  
It writes system info to the X11 root window name, which dwm shows in its bar.

Based on the classic `dwmstatus` by **20h**.

## Personal use

This repo is for **my own setup**.  
It is not a general-purpose project, and I don’t guarantee it will work on your machine.  
Feel free to read/copy it, but expect to edit the source for your own system.

## What it shows

The bar shows the Caps Lock indicator (icon only while active) and the clock
(timezone defaults to `Asia/Jakarta`; override with `DWMSTATUS_TZ`). Brightness,
volume, battery and the calendar are available in the combined popup described
under [Click actions](#click-actions).

The root window name is only rewritten when the status actually changes, so dwm
does not redraw its bar on every poll. Caps Lock and the clock are checked every
0.5 s.

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
`_DWMSTATUS_SEGMENTS` root window property (`caps,tim`); the dwm patch in this
setup maps a click to the segment under the pointer. A binding without a
segment applies to the whole status text.

Clicking the status text (the Caps Lock indicator or the clock) opens one
combined GTK 3 popup, `dwmstatus-popup`. It always appears at a fixed position:
the top-right corner of the monitor, just below the bar. The popup contains a
brightness slider, a volume slider with a mute button, a read-only battery level
bar with its charging state, and a calendar. Each section is labelled with a
Nerd Font icon (sun, volume, battery, calendar); the volume and battery icons
follow the level, a bolt appears while charging, and the calendar receives
keyboard focus. Clicking the status text again closes the popup, as do `Esc`,
`Return` and clicking elsewhere. Sections without a working backend are hidden,
and the calendar is always available.

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
static const char *popupcmd[] = { "dwmstatus-popup", "tim", NULL };

static const Button buttons[] = {
	/* ... */
	{ ClkStatusText, 0, Button1, spawn, {.v = popupcmd } },
	{ ClkStatusText, 0, Button3, spawn, {.v = termcmd } },
	/* ... */
};
```

The optional last field is the segment name (`caps`, `bri`, `vol`, `bat`,
`tim`). Leaving it out means "the whole status text", which is what we want:
both the Caps Lock indicator and the clock open the same popup. A segment
binding wins over a generic one. Stock dwm does not have this field or the
`_DWMSTATUS_SEGMENTS` lookup, so the small patch to `buttonpress()` and the
`Button` struct is required for segment clicks.
