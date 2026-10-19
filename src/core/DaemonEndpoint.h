#pragma once

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace punto {

// The settings window and the tray talk to the daemon through this socket.
// Same path the daemon binds: the user session runtime dir, or /tmp for this uid.
inline std::string daemonControlSocketPath() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0])
        return std::string(rt) + "/rocket-switcher-daemon.sock";
    return std::string("/tmp/rocket-switcher-daemon.") + std::to_string(getuid()) + ".sock";
}

} // namespace punto
