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
- Clock (timezone hardcoded to `Asia/Jakarta`)

Uses Nerd Font icons. Make sure your dwm font supports them.

## Build

```bash
git clone https://github.com/kafudz/dwmstatus.git
cd dwmstatus
make
