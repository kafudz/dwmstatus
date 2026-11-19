"""Shared backends for the dwmstatus helpers: volume, brightness, battery,
NetworkManager, Bluetooth (BlueZ) and the power menu."""

import os
import re
import shlex
import shutil
import subprocess

SINK = "@DEFAULT_AUDIO_SINK@"
PULSE_SINK = "@DEFAULT_SINK@"
BLDIR = "/sys/class/backlight"
PSDIR = "/sys/class/power_supply"
DEVNULL = subprocess.DEVNULL
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def run(args, timeout=3):
    """Run a query command and return its stdout, or '' on any failure."""
    try:
        result = subprocess.run(args, stdout=subprocess.PIPE, stderr=DEVNULL,
                                text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return ""
    return ANSI.sub("", result.stdout)


def run_rc(args, timeout=20):
    """Run a command that changes state.

    Returns (ok, message): ok is True when it exited with status 0, message is
    its (stdout + stderr) text without colour codes.  Never raises.
    """
    try:
        result = subprocess.run(args, stdin=DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired:
        return (False, "timed out")
    except (OSError, subprocess.SubprocessError) as error:
        return (False, str(error))
    return (result.returncode == 0, ANSI.sub("", result.stdout).strip())


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


# --- network (NetworkManager) ---------------------------------------------

def split_terse(line):
    """Split a line of `nmcli -t` output on unescaped ':'.

    nmcli escapes a literal ':' and '\\' inside a value with a backslash.
    """
    fields, current, i = [], [], 0
    while i < len(line):
        char = line[i]
        if char == "\\" and i + 1 < len(line):
            current.append(line[i + 1])
            i += 2
            continue
        if char == ":":
            fields.append("".join(current))
            current = []
        else:
            current.append(char)
        i += 1
    fields.append("".join(current))
    return fields


def nm_available():
    """True when nmcli exists and NetworkManager is running."""
    if not shutil.which("nmcli"):
        return False
    return run(["nmcli", "-t", "-f", "RUNNING", "general"]).strip() == "running"


def nm_state():
    """Return {'wifi': bool|None, 'devices': [...]} or None if unreadable.

    'wifi' is the Wi-Fi radio state (None when there is no Wi-Fi hardware).
    Each device is a dict with device, type, state and connection.
    """
    out = run(["nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION",
               "device", "status"])
    if not out.strip():
        return None
    devices = []
    for line in out.splitlines():
        fields = split_terse(line)
        if len(fields) < 4:
            continue
        device, kind, state, connection = fields[:4]
        # loopback, bridges, tunnels, p2p and friends are not "the network"
        if kind not in ("wifi", "ethernet"):
            continue
        devices.append({"device": device, "type": kind, "state": state,
                        "connection": "" if connection == "--" else connection})
    wifi = None
    if any(d["type"] == "wifi" for d in devices):
        wifi = run(["nmcli", "radio", "wifi"]).strip() == "enabled"
    return {"wifi": wifi, "devices": devices}


def nm_summary(state):
    """Return a short human text for the headline of the network section."""
    if state is None:
        return "Unknown"
    for kind, label in (("ethernet", "Ethernet"), ("wifi", "Wi-Fi")):
        for dev in state["devices"]:
            if dev["type"] == kind and dev["state"] == "connected":
                return f"{label}: {dev['connection']}" if dev["connection"] \
                    else label
    if state["wifi"] is False:
        return "Wi-Fi off"
    return "Disconnected"


def nm_ip(device):
    """First IPv4 address of a device (e.g. '192.168.1.5/24'), or ''."""
    out = run(["nmcli", "-t", "-f", "IP4.ADDRESS", "device", "show", device])
    for line in out.splitlines():
        fields = split_terse(line)
        if len(fields) >= 2 and fields[1]:
            return fields[1]
    return ""


def nm_wifi_list(limit=8):
    """Visible Wi-Fi networks, strongest first, one entry per SSID.

    Uses NetworkManager's cached scan results (--rescan no) so it is fast.
    """
    out = run(["nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY",
               "device", "wifi", "list", "--rescan", "no"], timeout=6)
    best = {}
    for line in out.splitlines():
        fields = split_terse(line)
        if len(fields) < 4 or not fields[1]:
            continue
        try:
            signal = int(fields[2])
        except ValueError:
            signal = 0
        entry = {"ssid": fields[1], "signal": signal,
                 "secure": fields[3] not in ("", "--"),
                 "active": fields[0] == "*"}
        old = best.get(entry["ssid"])
        if old is None or entry["active"] or \
                (not old["active"] and entry["signal"] > old["signal"]):
            best[entry["ssid"]] = entry
    networks = sorted(best.values(),
                      key=lambda n: (not n["active"], -n["signal"], n["ssid"]))
    return networks[:limit]


def nm_wifi_rescan():
    """Ask NetworkManager for a fresh scan; the result shows up in the list."""
    run_rc(["nmcli", "device", "wifi", "rescan"], timeout=10)


def nm_set_wifi(enabled):
    return run_rc(["nmcli", "radio", "wifi", "on" if enabled else "off"],
                  timeout=10)[0]


def nm_connect(ssid, password=None):
    """Connect to a Wi-Fi network; returns (ok, message).

    A known or open network needs no password.  For a new secured network
    nmcli is given the password on its command line (visible in `ps` while it
    runs), which is the only non-interactive interface it offers.
    """
    if not ssid or ssid.startswith("-"):
        return (False, "invalid network name")
    args = ["nmcli", "--wait", "20", "device", "wifi", "connect", ssid]
    if password:
        args += ["password", password]
    return run_rc(args, timeout=30)


def needs_password(message):
    text = message.lower()
    return ("secrets were required" in text or
            ("password" in text and "required" in text))


def nm_disconnect(device):
    return run_rc(["nmcli", "device", "disconnect", device], timeout=15)[0]


# --- bluetooth (BlueZ) ----------------------------------------------------

MAC = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")
BT_DEVICE = re.compile(r"^Device\s+([0-9A-Fa-f:]{17})\s+(.*)$")


def bt_available():
    return bool(shutil.which("bluetoothctl"))


def bt_state():
    """Return {'powered': bool} or None when there is no controller."""
    out = run(["bluetoothctl", "show"], timeout=5)
    if "Powered:" not in out:
        return None
    return {"powered": bool(re.search(r"Powered:\s*yes", out))}


def _parse_bt_devices(out):
    devices = []
    for line in out.splitlines():
        match = BT_DEVICE.match(line.strip())
        if match:
            devices.append({"mac": match.group(1).upper(),
                            "name": match.group(2).strip() or match.group(1),
                            "connected": False})
    return devices


def bt_devices(limit=8):
    """Paired devices as dicts with mac, name and connected."""
    devices = _parse_bt_devices(
        run(["bluetoothctl", "devices", "Paired"], timeout=5))
    if not devices:
        # bluez < 5.65 has no filter argument
        devices = _parse_bt_devices(
            run(["bluetoothctl", "paired-devices"], timeout=5))
    devices = devices[:limit]
    for dev in devices:
        info = run(["bluetoothctl", "info", dev["mac"]], timeout=5)
        dev["connected"] = bool(re.search(r"Connected:\s*yes", info))
    devices.sort(key=lambda d: (not d["connected"], d["name"].lower()))
    return devices


def bt_set_power(on):
    """Power the adapter on or off; also lifts an rfkill block when powering on."""
    if on and shutil.which("rfkill"):
        run_rc(["rfkill", "unblock", "bluetooth"], timeout=5)
    return run_rc(["bluetoothctl", "power", "on" if on else "off"],
                  timeout=10)[0]


def bt_connect(mac, connect=True):
    """Connect or disconnect a paired device; returns (ok, message)."""
    if not MAC.match(mac or ""):
        return (False, "invalid address")
    ok, message = run_rc(["bluetoothctl", "connect" if connect
                          else "disconnect", mac], timeout=25)
    if "fail" in message.lower():
        ok = False
    return (ok, message)


# --- power menu -----------------------------------------------------------

POWER_ACTIONS = ("lock", "logout", "suspend", "reboot", "poweroff")


def power_command(action):
    """Command (argv list) for a power action, or None if there is none.

    DWMSTATUS_LOCK / _LOGOUT / _SUSPEND / _REBOOT / _POWEROFF override the
    defaults; the value is split like a shell would, without running a shell.
    """
    custom = os.environ.get("DWMSTATUS_" + action.upper())
    if custom:
        try:
            argv = shlex.split(custom)
        except ValueError:
            return None
        return argv or None
    if action == "lock":
        for argv in (["slock"], ["xsecurelock"], ["i3lock"],
                     ["loginctl", "lock-session"],
                     ["xdg-screensaver", "lock"]):
            if shutil.which(argv[0]):
                return argv
        return None
    if action == "logout":
        session = os.environ.get("XDG_SESSION_ID")
        if session and shutil.which("loginctl"):
            return ["loginctl", "terminate-session", session]
        return ["pkill", "-TERM", "-x", "dwm"]
    if action in ("suspend", "reboot", "poweroff"):
        return ["systemctl", action]
    return None


def power_available(action):
    argv = power_command(action)
    return argv is not None and bool(shutil.which(argv[0]))


def run_power(action):
    """Start a power action detached from the popup. True if it started."""
    argv = power_command(action)
    if argv is None:
        return False
    try:
        subprocess.Popen(argv, stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL,
                         start_new_session=True)
    except OSError:
        return False
    return True


def uptime_text():
    """'3h 12m' style uptime, or '' when /proc/uptime is unreadable."""
    try:
        with open("/proc/uptime") as handle:
            seconds = int(float(handle.read().split()[0]))
    except (OSError, ValueError, IndexError):
        return ""
    days, rest = divmod(seconds, 86400)
    hours, rest = divmod(rest, 3600)
    minutes = rest // 60
    if days:
        return f"{days}d {hours}h"
    if hours:
        return f"{hours}h {minutes}m"
    return f"{minutes}m"
