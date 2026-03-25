#pragma once

namespace punto {

/// Restore WAYLAND_DISPLAY, XDG_RUNTIME_DIR, DBUS_SESSION_BUS_ADDRESS, DISPLAY from the parent
/// process chain (/proc/<pid>/environ). sg(1) often strips these so wtype and KDE busctl fail.
void ensureSessionEnvFromParents();

} // namespace punto
