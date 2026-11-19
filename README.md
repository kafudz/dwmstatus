# dwmstatus

My personal status panel for [dwm](https://dwm.suckless.org/).  
A small, polybar-style panel: its own X window that sits over the right end of
dwm's bar, with its own click handling. It does **not** write dwm's status
text and needs **no dwm patch**.

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
| caps | Caps Lock (only while active) | X keyboard events, no polling |
| net | wired / Wi-Fi link, dimmed when disconnected | `/sys/class/net` |
| bt | Bluetooth adapter; dimmed when blocked, highlighted while a device is connected | `/sys/class/bluetooth`, rfkill |
| vol | volume, dimmed when muted | `wpctl` |
| bat | battery; green while charging, red when low | `/sys/class/power_supply` |
| pwr | power button | always shown |

Percentages, the clock and everything interactive are in the popup, described
under [Click actions](#click-actions).

The panel finds dwm's bar (window class `dwm`), copies its height, sits at its
right end, follows it to the bottom if `topbar = 0`, and hides while dwm's bar
is hidden. Without dwm it falls back to the top-right of the screen.
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
| `DWMSTATUS_FG`, `_DIM`, `_ACCENT`, `_WARN`, `_OK` | see source | `#rrggbb` colours |
| `DWMSTATUS_HEIGHT` | `24` | panel height when no dwm bar is found |
| `DWMSTATUS_POPUP` | `dwmstatus-popup` | command run on click |
| `DWMSTATUS_TZ` | `Asia/Jakarta` | time zone of the popup clock |
| `DWMSTATUS_LOCK`, `_LOGOUT`, `_SUSPEND`, `_REBOOT`, `_POWEROFF` | auto | power menu commands |

## Click actions

A left click on a segment opens one combined GTK 3 popup, `dwmstatus-popup`,
focused on that section: `caps` opens the clock/calendar, `net`, `bt`, `vol`,
`bat` and `pwr` open their own section. The popup opens right below the panel
(above it for a bottom bar), aligned to the panel's right edge. Its window title
is `status`. Clicking a segment again closes it, as do `Esc`, `Return` and
clicking elsewhere.

Sections, top to bottom (those without a working backend are hidden):

- **Clock**, live `HH:MM`.
- **Power**: Lock, Log out, Suspend, Reboot, Power off. Log out, Reboot and
  Power off ask for confirmation first (the focus starts on *Cancel*).
  Also shows the uptime.
- **Network** (NetworkManager): current connection and IP, Wi-Fi on/off switch,
  and a list of nearby networks. Click one to connect, click the active one to
  disconnect. A new secured network asks for its password.
- **Bluetooth** (BlueZ): power switch and the paired devices; click one to
  connect or disconnect it.
- **Brightness**, **Volume** (with mute), **Battery** and a **Calendar**.

The popup needs Python 3, PyGObject and GTK 3 (Arch packages `python`,
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
- **The panel's Bluetooth icon** is driven by sysfs: adapter present, rfkill
  state, and active connections. Whether BlueZ has the adapter *powered* is only
  shown in the popup.
- To use different power commands, set e.g.
  `DWMSTATUS_LOCK="slock"` or `DWMSTATUS_LOGOUT="pkill -x dwm"`. The value is
  split like a shell would but is **not** run through a shell.

`dwmstatus-volume` and `dwmstatus-brightness` remain as single-control sliders
(thin wrappers around `dwmstatus-slider`), and `dwmstatus-calendar` still shows
the month as a notification. Their command line modes work without GTK:
`dwmstatus-volume --get|--set PERCENT|--mute|--unmute|--toggle-mute` and
`dwmstatus-brightness --get|--set PERCENT`.

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
