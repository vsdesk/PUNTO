#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct xkb_keymap;

namespace punto {

// One physical key that produced (or should produce) a character.
// `shift` and `caps` are the state at the time of the press, not a request
// to change Caps Lock when the key is replayed.
struct Keystroke {
    unsigned code  = 0;
    bool     shift = false;
    bool     caps  = false;
};

struct KeyBinding {
    unsigned code  = 0;
    bool     shift = false;
};

// Turns a scancode sequence into text under a chosen xkb layout group,
// and the reverse: which key types a given codepoint in that group.
class XkbInterpreter {
public:
    void setKeymap(::xkb_keymap* map);

    std::string interpret(const std::vector<Keystroke>& keys, int layout) const;
    std::string interpret(const Keystroke& key, int layout) const;

    std::optional<KeyBinding> bindingFor(char32_t cp, int layout) const;

private:
    ::xkb_keymap* map_ = nullptr;
    uint32_t      shiftMask_ = 0;
    uint32_t      capsMask_  = 0;
    std::unordered_map<int, std::unordered_map<char32_t, KeyBinding>> rev_;

    void rebuild();
};

} // namespace punto
