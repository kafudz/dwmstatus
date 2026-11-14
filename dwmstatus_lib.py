"""Shared volume, brightness and battery backends for the dwmstatus helpers."""

import os
import re
import shutil
import subprocess

SINK = "@DEFAULT_AUDIO_SINK@"
PULSE_SINK = "@DEFAULT_SINK@"
BLDIR = "/sys/class/backlight"
PSDIR = "/sys/class/power_supply"
DEVNULL = subprocess.DEVNULL


def run(args):
    """Run a query command and return its stdout, or '' on any failure."""
    try:
        result = subprocess.run(args, stdout=subprocess.PIPE, stderr=DEVNULL,
                                text=True, timeout=3)
    except (OSError, subprocess.SubprocessError):
        return ""
    return result.stdout


# --- volume ---------------------------------------------------------------

def detect_volume():
    if shutil.which("wpctl"):
        return "wpctl"
    if shutil.which("pactl"):
        return "pactl"
    return None


def volume_state(backend):
    """Return (percent, muted), or None when the backend cannot be queried."""
    if backend == "wpctl":
        out = run(["wpctl", "get-volume", SINK])
        match = re.search(r"Volume:\s*([0-9.]+)", out)
        if not match:
            return None
        return (round(float(match.group(1)) * 100), "[MUTED]" in out)

    out = run(["pactl", "get-sink-volume", PULSE_SINK])
    match = re.search(r"(\d+)%", out)
    if not match:
        return None
    muted = "yes" in run(["pactl", "get-sink-mute", PULSE_SINK]).lower()
    return (int(match.group(1)), muted)


def set_volume(backend, percent):
    percent = max(0, min(100, int(percent)))
    if backend == "wpctl":
        args = ["wpctl", "set-volume", SINK, f"{percent}%"]
    else:
        args = ["pactl", "set-sink-volume", PULSE_SINK, f"{percent}%"]
    result = subprocess.run(args, stdout=DEVNULL, stderr=DEVNULL, check=False)
    return result.returncode == 0


def set_mute(backend, muted):
    if backend == "wpctl":
        args = ["wpctl", "set-mute", SINK, "1" if muted else "0"]
    else:
        args = ["pactl", "set-sink-mute", PULSE_SINK, "1" if muted else "0"]
    result = subprocess.run(args, stdout=DEVNULL, stderr=DEVNULL, check=False)
    return result.returncode == 0


# --- brightness -----------------------------------------------------------

def backlight_dir():
    try:
        names = os.listdir(BLDIR)
    except OSError:
        return None
    for name in sorted(names):
        path = os.path.join(BLDIR, name)
        if os.access(os.path.join(path, "brightness"), os.R_OK):
            return path
    return None


def sysfs_state():
    path = backlight_dir()
    if path is None:
        return None
    try:
        with open(os.path.join(path, "brightness")) as handle:
            current = int(handle.read().strip())
        with open(os.path.join(path, "max_brightness")) as handle:
            maximum = int(handle.read().strip())
    except (OSError, ValueError):
        return None
    if maximum <= 0:
        return None
    return max(0, min(100, int(current * 100 / maximum + 0.5)))


def sysfs_set(percent):
    path = backlight_dir()
    if path is None:
        return False
    try:
        with open(os.path.join(path, "max_brightness")) as handle:
            maximum = int(handle.read().strip())
        if maximum <= 0:
            return False
        # Never write raw 0: on many panels that switches the backlight off.
        raw = max(1, int(percent * maximum / 100 + 0.5))
        with open(os.path.join(path, "brightness"), "w") as handle:
            handle.write(str(raw))
    except (OSError, ValueError):
        return False
    return True


def detect_brightness():
    if shutil.which("brightnessctl"):
        return "brightnessctl"
    if backlight_dir() is not None:
        return "sysfs"
    return None


def brightness_state(backend):
    """Return (percent, None), or None when the backend cannot be queried."""
    if backend == "brightnessctl":
        out = run(["brightnessctl", "-m", "-c", "backlight"])
        for line in out.splitlines():
            fields = line.split(",")
            if len(fields) >= 4:
                match = re.fullmatch(r"(\d+)%", fields[3].strip())
                if match:
                    return (int(match.group(1)), None)
    percent = sysfs_state()
    return None if percent is None else (percent, None)


def set_brightness(backend, percent):
    percent = max(0, min(100, int(percent)))
    if backend == "brightnessctl":
        result = subprocess.run(
            ["brightnessctl", "-q", "-c", "backlight", "set", f"{percent}%"],
            stdout=DEVNULL, stderr=DEVNULL, check=False)
        if result.returncode == 0:
            return True
    return sysfs_set(percent)


# --- battery --------------------------------------------------------------

def read_attr(path):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError:
        return None


def read_int(path):
    value = read_attr(path)
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def battery_dir():
    try:
        names = os.listdir(PSDIR)
    except OSError:
        return None
    fallback = None
    for name in sorted(names):
        path = os.path.join(PSDIR, name)
        if read_attr(os.path.join(path, "type")) != "Battery":
            continue
        # Skip batteries of peripherals (mouse, headset, ...).
        if read_attr(os.path.join(path, "scope")) == "Device":
            continue
        # Prefer a battery that actually reports capacity.
        if read_attr(os.path.join(path, "capacity")) is not None:
            return path
        if fallback is None:
            fallback = path
    return fallback


def battery_state():
    """Return (percent, status), or None when no battery is present."""
    path = battery_dir()
    if path is None:
        return None

    percent = read_int(os.path.join(path, "capacity"))
    if percent is None:
        for now, full in (("charge_now", "charge_full"),
                          ("energy_now", "energy_full")):
            current = read_int(os.path.join(path, now))
            total = read_int(os.path.join(path, full))
            if current is not None and total:
                percent = int(current * 100 / total + 0.5)
                break
    if percent is None:
        return None

    percent = max(0, min(100, percent))
    status = read_attr(os.path.join(path, "status")) or ""
    return (percent, status)
