#pragma once

#include <string>

namespace punto {

// Types replacement text via wtype / xdotool (Wayland / X11).
bool injectUtf8Text(const std::string& utf8);

} // namespace punto
