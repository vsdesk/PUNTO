#pragma once

#include "../core/AutoSwitchHeuristic.h"
#include "../engine/InputTracker.h"
#include "DaemonConfig.h"
#include "EvdevUinput.h"
#include "LayoutController.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace punto {

struct HotkeySpec {
    uint32_t     modMask = 0; // 1=Alt 2=Shift 4=Ctrl 8=Super
    unsigned int keyCode = 0;
    /// Some keyboards emit KEY_GRAVE instead of KEY_APOSTROPHE for the same physical key.
    unsigned int altKeyCode = 0;
};

class PuntoDaemon {
public:
    explicit PuntoDaemon(DaemonConfig cfg);

    int run(); // 0 ok, 1 error

private:
    DaemonConfig          cfg_;
    EvdevUinput           io_;
    LayoutController      layout_;
    AutoSwitchHeuristic   heuristic_;
    InputTracker          tracker_;

    HotkeySpec hkSwapLast_{};
    HotkeySpec hkSwapSel_{};
    HotkeySpec hkToggle_{};
    HotkeySpec hkUndo_{};

    std::array<uint8_t, 1024> keyState_{};
    std::unordered_set<unsigned int> swallowed_;

    ::xkb_context* xkbCtx_   = nullptr;
    ::xkb_keymap*  xkbMap_   = nullptr;
    ::xkb_state*   xkbState_ = nullptr;

    // Modifier-sequence tracking: detect Ctrl+Shift / Alt+Shift layout-switch combos.
    // KDE updates its layout index asynchronously via uinput events, so we must wait
    // ~15 ms before querying the new index (otherwise we decode the first letter with
    // the old layout and put wrong characters in wordBuffer, e.g. 'п' instead of 'g').
    bool onlyModifiersSinceNormalKey_ = false;
    bool pendingLayoutRefresh_        = false;
    int  lastKnownLayoutIndex_        = -1;
    // Set to true when the daemon itself calls setEnglish()/setRussian() so that the
    // layout-change detection in processEvent does NOT reset the tracker (which would
    // erase lastWord and break the manual Alt+' hotkey right after an auto-switch).
    bool daemonSwitchedLayout_        = false;

    bool setupXkb();
    void teardownXkb();

    bool parseHotkey(const std::string& spec, HotkeySpec& out) const;
    bool matchMods(uint32_t mask) const;
    bool matchHotkey(const HotkeySpec& hk, unsigned int code, int value) const;
    // Returns the current KDE layout index (or -1), updating xkb state.
    int syncXkbLayoutFromKde();

    void processEvent(const input_event& ev);

    bool doSwapLastWord();
    bool doSwapSelection();
    void doToggleAutoSwitch();
    bool doUndoLast();

    void checkAutoSwitch(const std::string& word);

    void tapBackspaces(int count);
    void applyLayoutForWord(CharMapping::Layout typedLayout);

    /// Wayland: wl-copy + Ctrl+V first (KWin often rejects wtype), then wtype; X11: wtype then paste.
    bool injectText(const std::string& utf8);
    bool injectViaWlCopyPaste(const std::string& utf8);

    struct InternalGuard {
        InputTracker* t = nullptr;
        explicit InternalGuard(InputTracker* tr) : t(tr) {
            if (t) t->beginInternal();
        }
        ~InternalGuard() {
            if (t) t->endInternal();
        }
    };
};

} // namespace punto
