# dwmstatus

My personal status bar for [dwm](https://dwm.suckless.org/).  
It writes system info to the X11 root window name, which dwm shows in its bar.

Based on the classic `dwmstatus` by **20h**.

## Personal use

This repo is for **my own setup**.  
It is not a general-purpose project, and I don’t guarantee it will work on your machine.  
Feel free to read/copy it, but expect to edit the source for your own system.

## What it shows

- Caps Lock state
- Brightness (from `/sys/class/backlight`)
- Volume (via `wpctl`; hidden if unavailable)
- Battery (from `/sys/class/power_supply`)
- Temperature (from `/sys/class/hwmon`, with `/sys/class/thermal` fallback)
- Clock (timezone defaults to `Asia/Jakarta`; override with `DWMSTATUS_TZ`)

Uses Nerd Font icons. Make sure your dwm font supports them.

The root window name is only rewritten when the status actually changes, so dwm
does not redraw its bar on every poll. Caps Lock and the clock are checked every
0.5 s, battery/brightness/temperature every 1 s, volume every 2 s, and sysfs
devices are re-scanned every 30 s so hotplug still works.

## Build

```bash
git clone https://github.com/kafudz/dwmstatus.git
cd dwmstatus
make
sudo make install
```

Then start it from your X session (`dwmstatus &`) or with `dwmstatus-restart`.
