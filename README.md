# dwmstatus

My personal status panel for [dwm](https://dwm.suckless.org/).  
A small, polybar-style panel: two small X windows over dwm's bar, with its own
click handling. The clock sits in the middle of the bar; volume and power stay
at its right end. It does **not** write dwm's status text and needs **no dwm
patch**.

Based on the classic `dwmstatus` by **20h**.

## Personal use

This repo is for **my own setup**.  
It is not a general-purpose project, and I don’t guarantee it will work on your machine.  
Feel free to read/copy it, but expect to edit the source for your own system.

## What it shows

Nerd Font icons only, left to right. A segment that is not available on the
machine is hidden.

| Segment | Meaning | Source |
| ------- | ------- | ------ |
| caps | Caps Lock indicator, shown only while active (⇪) | XKB |
| net | Wi-Fi / Ethernet icon, dimmed when the link is down | sysfs |
| vol | volume, dimmed when muted | `wpctl` |
| tim | clock icon and live time; opens the clock and calendar popup | always shown |
| pwr | power button; opens the power menu | always shown |

The clock segment shows the current `HH:MM` next to its icon. Clicking it opens
one popup with the live clock, the date and a month calendar. The volume and
power popups hold the percentages, sliders and controls described under
[Click actions](#click-actions).

The panel finds dwm's bar (window class `dwm`), copies its height, centres the
clock on it and keeps network/volume/power at its right end. It follows the bar to the
bottom if `topbar = 0` and hides while dwm's bar is hidden. Without dwm the
clock is centred at the top and the buttons stay at the top-right.
Uses a Nerd Font, so make sure one is installed.

## Build

```bash
git clone https://github.com/kafudz/dwmstatus.git
cd dwmstatus
make
sudo make install
```

Needs libX11 and cairo (cairo is already pulled in by GTK 3).
`dwmstatus -v` prints the version.

## Run

Start it from your `.xinitrc` / autostart, after dwm:

```bash
dwmstatus &
```

After rebuilding, run `dwmstatus-restart` to replace the running instance.
Only one panel runs per user and display.

dwm's own status text is no longer used: the panel writes a single space to the
root window name so dwm does not show its `dwm-x.y` banner next to the panel.

### Settings (environment variables)

| Variable | Default | |
| -------- | ------- | - |
| `DWMSTATUS_FONT` | `JetBrainsMono Nerd Font` | font family (Nerd Font) |
| `DWMSTATUS_BG` | `#101113` | background; match your dwm bar |
| `DWMSTATUS_FG`, `_DIM`, `_ACCENT` | see source | `#rrggbb` colours (accent = Caps Lock on) |
| `DWMSTATUS_HEIGHT` | `24` | panel height when no dwm bar is found |
| `DWMSTATUS_POPUP` | `dwmstatus-popup` | system popup (volume, power, ...) |
| `DWMSTATUS_CLOCK` | `dwmstatus-clock` | clock and calendar popup |
| `DWMSTATUS_PANEL_ALIGN` | `right` | `center` centres a popup under its segment; the clock uses it |
| `DWMSTATUS_TZ` | `Asia/Jakarta` | time zone of the bar clock, popup and weather |
| `DWMSTATUS_LAT`, `_LON` | `-6.2088`, `106.8456` | weather location (Open-Meteo, no API key) |
| `DWMSTATUS_WEATHER_UNIT` | `C` | `C` or `F` for the popup weather temperature |
| `DWMSTATUS_LOCK`, `_LOGOUT`, `_SUSPEND`, `_REBOOT`, `_POWEROFF` | auto | power menu commands |

## Click actions

A left click on the volume or power segment opens the system GTK 3 popup,
`dwmstatus-popup`, focused on that section. A left click on the clock opens
`dwmstatus-clock`, one popup that contains both the live clock and the calendar.
All popups open right below their segment (above it for a bottom bar), with the
window title `status`. The system popup sits slightly left of its panel's right
edge; the clock popup is centred under the clock (or in the middle of the
monitor when launched directly). Clicking the same segment again closes its popup, as do
`Esc`, `Return` and clicking elsewhere.

The clock and calendar popup shows the live time and date at the top, a compact
weather summary (icon, temperature and condition) below it, then the current
month with its own `<` / `>` month navigation. Weather is fetched from
Open-Meteo in the background and cached for 10 minutes; if the network is down
the last known reading is kept, otherwise the popup simply says it is
unavailable. `dwmstatus-calendar` remains as a compatibility alias for
`dwmstatus-clock`.

Sections of the system popup, top to bottom (those without a working backend
are hidden):

- **Volume** (with mute), **Brightness** and **Battery**.
- **Network** (NetworkManager): current connection and IP, Wi-Fi on/off switch,
  and a list of nearby networks. Click one to connect, click the active one to
  disconnect. A new secured network asks for its password.
- **Bluetooth** (BlueZ): power switch and the paired devices; click one to
  connect or disconnect it.
- **Power**: Lock, Log out, Suspend, Reboot, Power off. Log out, Reboot and
  Power off ask for confirmation first (the focus starts on *Cancel*).
  Also shows the uptime.

The sliders and battery stay near the top; Network, Bluetooth and Power follow.
The popup is kept inside the monitor and scrolls when the screen is too small.

The popups need Python 3, PyGObject and GTK 3 (Arch packages `python`,
`python-gobject`, `gtk3`). Backends:

| Section | Tool |
| ------- | ---- |
| Network | `nmcli` (NetworkManager must be running) |
| Bluetooth | `bluetoothctl`; `rfkill` is used to unblock when powering on |
| Volume | `wpctl` (PipeWire) or `pactl` (PulseAudio) |
| Brightness | `brightnessctl` or a writable sysfs backlight |
| Power | `systemctl` (suspend/reboot/poweroff), `loginctl` or `pkill dwm` (log out), `slock`/`xsecurelock`/`i3lock`/`loginctl lock-session` (lock) |

Notes:

- **Wi-Fi passwords** are passed to `nmcli` on its command line (the only
  non-interactive way it offers), so they are visible in `ps` for the second or
  two the connection takes. Networks you already know need no password.
- To use different power commands, set e.g.
  `DWMSTATUS_LOCK="slock"` or `DWMSTATUS_LOGOUT="pkill -x dwm"`. The value is
  split like a shell would but is **not** run through a shell.

`dwmstatus-volume` and `dwmstatus-brightness` remain as single-control sliders
(thin wrappers around `dwmstatus-slider`). `dwmstatus-clock` is the combined
clock and calendar popup; `dwmstatus-calendar` is a compatibility alias.
Their command line modes work without GTK:
`dwmstatus-volume --get|--set PERCENT|--mute|--unmute|--toggle-mute` and
`dwmstatus-brightness --get|--set PERCENT`; `dwmstatus-clock` only has
`--help`.

```bash
make install   # installs the panel and all helpers
```

### Migrating from the old dwm-patch setup

Earlier versions drew the icons in dwm's status text and relied on a dwm patch
(`Button` segment field and the `_DWMSTATUS_SEGMENTS` property). That is gone:

- remove the `popup*cmd` / `ClkStatusText` bindings from dwm's `config.h`;
  the panel handles its own clicks, and stock dwm is enough,
- if your dwm positions windows titled `status`, that placement still applies
  to the popup.
