"""Shared GTK 3 scaffolding for the dwmstatus popup windows.

The clock and calendar popups are small enough to share everything except
their content: the window style, the single-instance lock, the placement
next to the panel and the dismissal behaviour all live here.
"""

import fcntl
import os
import signal

WINDOW_NAME = "dwmstatus-popup"

# The bar and every popup use the same background; keep the widgets flat so
# the window looks like part of dwm's bar.
BASE_CSS = """
#dwmstatus-popup {
    background-color: #101113;
}
.dwmstatus-icon {
    font-family: "JetBrainsMono Nerd Font", "Symbols Nerd Font", monospace;
    font-size: 18px;
}
.dwmstatus-title {
    font-weight: bold;
}
.dwmstatus-clock {
    font-size: 22px;
    font-weight: bold;
}
.dwmstatus-small {
    font-size: 11px;
    opacity: 0.75;
}
.dwmstatus-error {
    color: #e5484d;
    font-size: 11px;
}
.dwmstatus-level trough {
    min-height: 6px;
    border-radius: 3px;
    background-color: alpha(currentColor, 0.15);
}
.dwmstatus-level block.filled {
    min-height: 6px;
    border-radius: 3px;
    background-color: #3584e4;
}
.dwmstatus-level block.empty {
    min-height: 6px;
    background-color: transparent;
}
.dwmstatus-nav {
    min-width: 32px;
    padding: 2px 8px;
}
.dwmstatus-month {
    background-color: #2b2b2b;
    border: 1px solid alpha(currentColor, 0.35);
    border-radius: 4px;
    padding: 3px 12px;
    min-width: 150px;
}
calendar:selected {
    background-color: #121314;
}
"""


def install_css(Gtk, Gdk):
    provider = Gtk.CssProvider()
    provider.load_from_data(BASE_CSS)
    Gtk.StyleContext.add_provider_for_screen(
        Gdk.Screen.get_default(), provider,
        Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)


def claim_lock(kind):
    """Claim the single-instance lock, or close the open popup and exit.

    Each popup has its own lock, so the clock and the calendar can be opened
    independently.  A second invocation of the same popup asks the running
    one to close.
    """
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    path = os.path.join(runtime, f"dwmstatus-{kind}-{os.getuid()}.lock")
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            pid = int(os.read(fd, 32).decode().strip())
            if pid > 1:
                os.kill(pid, signal.SIGTERM)
        except (OSError, ValueError):
            pass
        os.close(fd)
        return None
    os.ftruncate(fd, 0)
    os.write(fd, str(os.getpid()).encode())
    return fd


def make_window(Gtk, Gdk, GLib):
    """Create the undecorated, always-on-top window used by every popup."""
    GLib.set_prgname(WINDOW_NAME)
    Gdk.set_program_class(WINDOW_NAME)

    win = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
    # Widget name for the background colour in CSS (same as the dwm bar).
    win.set_name(WINDOW_NAME)
    # Non-empty title: dwm marks clients with an empty name as "broken".
    win.set_title("status")
    win.set_decorated(False)
    win.set_resizable(False)
    win.set_border_width(14)
    win.set_skip_taskbar_hint(True)
    win.set_skip_pager_hint(True)
    win.set_keep_above(True)
    win.set_type_hint(Gdk.WindowTypeHint.DIALOG)
    return win


def placement(display):
    """Where to put the popup.

    With DWMSTATUS_PANEL="x,y,w,h" (set by the panel) the popup goes right
    below the panel, or above it for a bottom bar, with the right edges
    aligned.  Without it it appears 40 px from the top and 15 px from the
    right edge of the monitor under the pointer.
    """
    anchor = None
    try:
        ax, ay, aw, ah = (int(v) for v in
                          os.environ.get("DWMSTATUS_PANEL", "").split(","))
        anchor = (ax, ay, aw, ah)
    except ValueError:
        pass

    if anchor:
        monitor = display.get_monitor_at_point(ax + aw // 2, ay + ah // 2)
    else:
        _, pointer_x, pointer_y = \
            display.get_default_seat().get_pointer().get_position()
        monitor = display.get_monitor_at_point(pointer_x, pointer_y)
    monitor = monitor or display.get_primary_monitor()
    geometry = monitor.get_geometry() if monitor is not None else None

    gap = 6
    # below: popup's top edge is `edge`; otherwise its bottom edge is `edge`
    place = {"below": True, "edge": 40, "right": None, "avail": 600}
    if geometry is not None:
        if anchor and ay + ah // 2 > geometry.y + geometry.height // 2:
            place["below"] = False
            place["edge"] = ay - gap
            place["right"] = ax + aw
            place["avail"] = place["edge"] - geometry.y - 10
        else:
            place["edge"] = (ay + ah + gap) if anchor else geometry.y + 40
            place["right"] = (ax + aw) if anchor \
                else geometry.x + geometry.width - 15
            place["avail"] = geometry.y + geometry.height - place["edge"] - 10
    place["avail"] = max(240, place["avail"])
    return geometry, place


def present(win, content, geometry, place, Gtk, Gdk, GLib):
    """Show the window, keep it next to the panel and wire up dismissal."""
    # Scroll instead of running off a small screen: never taller than the
    # room next to the panel.
    scrolled = Gtk.ScrolledWindow()
    scrolled.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
    scrolled.set_shadow_type(Gtk.ShadowType.NONE)
    scrolled.set_propagate_natural_height(True)
    scrolled.set_max_content_height(place["avail"])
    if hasattr(scrolled, "set_propagate_natural_width"):  # GTK >= 3.22
        scrolled.set_propagate_natural_width(True)
    scrolled.add(content)
    win.add(scrolled)

    def on_key_press(widget, event):
        if event.keyval in (Gdk.KEY_Escape, Gdk.KEY_Return, Gdk.KEY_KP_Enter):
            win.destroy()
            return True
        return False

    shown_at = [0]

    def on_focus_out(widget, event):
        # Ignore the focus transition caused by mapping the popup itself.
        if GLib.get_monotonic_time() - shown_at[0] > 200000:
            win.destroy()
        return False

    win.connect("key-press-event", on_key_press)
    win.connect("focus-out-event", on_focus_out)
    win.connect("destroy", Gtk.main_quit)

    win.show_all()
    win.present()

    last_position = [None]

    def reposition(*_args):
        """Right edge on the panel's right edge; below it (or above it)."""
        if geometry is None or place["right"] is None:
            return False
        width, height = win.get_size()
        x = max(geometry.x, place["right"] - width)
        y = place["edge"] if place["below"] else place["edge"] - height
        y = max(geometry.y, y)
        if last_position[0] != (x, y):
            last_position[0] = (x, y)
            win.move(x, y)
        return False

    # dwm may also place the window itself when it manages it; keep it glued
    # to the panel when the content changes size.
    win.connect("size-allocate", lambda *_a: GLib.idle_add(reposition))
    reposition()

    shown_at[0] = GLib.get_monotonic_time()
