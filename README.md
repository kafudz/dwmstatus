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
- CPU temperature (from `/sys/class/hwmon`, matched by driver name:
  `coretemp`, `k10temp`, `zenpower`, `cpu_thermal`, `acpitz`; falls back to
  `/sys/class/thermal`)
- Clock (timezone defaults to `Asia/Jakarta`; override with `DWMSTATUS_TZ`)

The root window name is only rewritten when the status actually changes, so dwm
does not redraw its bar on every poll. Caps Lock and the clock are checked every
0.5 s, battery/brightness/temperature every 1 s, volume every 2 s, and sysfs
devices are re-scanned every 30 s so hotplug still works.

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
