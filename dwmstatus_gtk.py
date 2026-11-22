"""Shared GTK 3 scaffolding for the dwmstatus popup windows.

The bar and its popups share the window style, the single-instance lock, the
placement next to their panel segment and the dismissal behaviour. Each popup
only supplies its own content.
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

    Every popup has its own lock, so opening one does not close a different
    one. A second invocation of the same popup asks the running one to close.
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
    below the panel, or above it for a bottom bar.  DWMSTATUS_PANEL_ALIGN
    selects "right" (right edges aligned, the default) or "center" (centred
    under the anchor, used by the clock).  Without an anchor the popup
    appears 40 px from the top, 15 px from the right edge, or in the middle
    of the monitor when centred.
    """
    anchor = None
    try:
        ax, ay, aw, ah = (int(v) for v in
                          os.environ.get("DWMSTATUS_PANEL", "").split(","))
        anchor = (ax, ay, aw, ah)
    except ValueError:
        pass

    centered = os.environ.get("DWMSTATUS_PANEL_ALIGN") == "center"

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
    place = {"below": True, "edge": 40, "right": None, "center": None,
             "avail": 600}
    if geometry is not None:
        above = anchor and ay + ah // 2 > geometry.y + geometry.height // 2
        if above:
            place["below"] = False
            place["edge"] = ay - gap
            place["avail"] = place["edge"] - geometry.y - 10
        else:
            place["edge"] = (ay + ah + gap) if anchor else geometry.y + 40
            place["avail"] = geometry.y + geometry.height - place["edge"] - 10

        if centered:
            place["center"] = (ax + aw // 2) if anchor else \
                geometry.x + geometry.width // 2
        elif anchor:
            place["right"] = ax + aw
        else:
            place["right"] = geometry.x + geometry.width - 15
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

    def position_for(width, height):
        """Final x/y for a popup size, or None when it has no anchor."""
        if geometry is None:
            return None
        if place["center"] is not None:
            x = place["center"] - width // 2
        elif place["right"] is not None:
            x = place["right"] - width
        else:
            return None
        x = max(geometry.x, min(x, geometry.x + geometry.width - width))
        y = place["edge"] if place["below"] else place["edge"] - height
        return x, max(geometry.y, y)

    # GTK cannot measure its natural size before it is mapped, but even an
    # approximate pre-map position keeps the window from flashing at (0, 0)
    # while it opens.  reposition() refines it once the real size is known.
    win.set_position(Gtk.WindowPosition.NONE)
    natural = win.get_preferred_size().natural_size
    initial = position_for(natural.width, natural.height)
    if initial is not None:
        win.move(*initial)

    # Start the grace period before mapping: show/present can emit a focus-out
    # immediately, and a zero timestamp would make that look like a late
    # dismissal and destroy the popup while it is still opening.
    shown_at[0] = GLib.get_monotonic_time()
    win.show_all()
    win.present()

    last_position = [None]

    def reposition(*_args):
        """Keep the popup glued to its segment when the content resizes."""
        target = position_for(*win.get_size())
        if target is None or last_position[0] == target:
            return False
        last_position[0] = target
        win.move(*target)
        return False

    # dwm may also place the window itself when it manages it; keep it glued
    # to the panel when the content changes size.
    win.connect("size-allocate", lambda *_a: GLib.idle_add(reposition))
    reposition()
