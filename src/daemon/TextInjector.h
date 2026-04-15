#pragma once

#include <string>

namespace punto {

// Types replacement text via wtype / xdotool (Wayland / X11).
// logWtypeErrors: set false when caller will retry (avoids journal spam after resume).
bool injectUtf8Text(const std::string& utf8, bool logWtypeErrors = true);

} // namespace punto
