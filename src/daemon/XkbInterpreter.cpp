#include "XkbInterpreter.h"

#include "../core/Utf8Utils.h"

#include <xkbcommon/xkbcommon.h>

#include <linux/input-event-codes.h>

namespace punto {

void XkbInterpreter::setKeymap(::xkb_keymap* map) {
    map_ = map;
    shiftMask_ = 0;
    capsMask_ = 0;
    rev_.clear();
    if (!map_) return;

    const xkb_mod_index_t shiftIdx = xkb_keymap_mod_get_index(map_, XKB_MOD_NAME_SHIFT);
    const xkb_mod_index_t capsIdx  = xkb_keymap_mod_get_index(map_, XKB_MOD_NAME_CAPS);
    if (shiftIdx != XKB_MOD_INVALID) shiftMask_ = 1u << shiftIdx;
    if (capsIdx != XKB_MOD_INVALID) capsMask_ = 1u << capsIdx;
    rebuild();
}

std::string XkbInterpreter::interpret(const Keystroke& key, int layout) const {
    return interpret(std::vector<Keystroke>{key}, layout);
}

std::string XkbInterpreter::interpret(const std::vector<Keystroke>& keys, int layout) const {
    if (!map_ || layout < 0) return {};
    const xkb_layout_index_t n = xkb_keymap_num_layouts(map_);
    if (static_cast<xkb_layout_index_t>(layout) >= n) return {};

    xkb_state* st = xkb_state_new(map_);
    if (!st) return {};

    std::string out;
    const xkb_layout_index_t lg = static_cast<xkb_layout_index_t>(layout);
    for (const Keystroke& ks : keys) {
        xkb_mod_mask_t depressed = 0;
        xkb_mod_mask_t locked    = 0;
        if (ks.shift) depressed |= shiftMask_;
        if (ks.caps) locked |= capsMask_;
        xkb_state_update_mask(st, depressed, 0, locked, lg, lg, lg);
        char buf[64];
        const int nchars = xkb_state_key_get_utf8(st, ks.code + 8, buf, sizeof(buf));
        if (nchars > 0) out.append(buf, static_cast<size_t>(nchars));
    }
    xkb_state_unref(st);
    return out;
}

std::optional<KeyBinding> XkbInterpreter::bindingFor(char32_t cp, int layout) const {
    const auto lit = rev_.find(layout);
    if (lit == rev_.end()) return std::nullopt;
    const auto it = lit->second.find(cp);
    if (it == lit->second.end()) return std::nullopt;
    return it->second;
}

void XkbInterpreter::rebuild() {
    rev_.clear();
    if (!map_) return;
    xkb_state* st = xkb_state_new(map_);
    if (!st) return;
    const xkb_layout_index_t layouts = xkb_keymap_num_layouts(map_);
    for (xkb_layout_index_t layout = 0; layout < layouts; ++layout) {
        auto& dest = rev_[static_cast<int>(layout)];
        for (unsigned code = 1; code < KEY_MAX; ++code) {
            for (int shift = 0; shift <= 1; ++shift) {
                const xkb_mod_mask_t depressed = shift ? shiftMask_ : 0;
                xkb_state_update_mask(st, depressed, 0, 0, layout, layout, layout);
                char buf[64];
                const int nchars = xkb_state_key_get_utf8(st, code + 8, buf, sizeof(buf));
                if (nchars <= 0) continue;
                const std::u32string u32 = utf8_to_utf32(std::string(buf, static_cast<size_t>(nchars)));
                if (u32.size() != 1) continue;
                if (dest.count(u32[0])) continue;
                dest.emplace(u32[0], KeyBinding{code, shift != 0});
            }
        }
    }
    xkb_state_unref(st);
}

} // namespace punto
