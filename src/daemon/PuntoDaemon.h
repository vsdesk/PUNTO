#pragma once

#include "../core/AutoSwitchHeuristic.h"
#include "../engine/InputTracker.h"
#include "DaemonConfig.h"
#include "EvdevUinput.h"
#include "LayoutController.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace punto {

struct HotkeySpec {
    uint32_t modMask = 0; // 1=Alt 2=Shift 4=Ctrl 8=Super
    /// All evdev KEY_* codes that should trigger this binding (e.g. APOSTROPHE+GRAVE aliases).
    std::vector<unsigned int> codes;
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
    bool pendingUrlSecondSlash_       = false;
    bool capsLockOn_                  = false;
    int  lastKnownLayoutIndex_        = -1;
    // Set to true when the daemon itself calls setEnglish()/setRussian() so that the
    // layout-change detection in processEvent does NOT reset the tracker (which would
    // erase lastWord and break the manual Alt+' hotkey right after an auto-switch).
    bool daemonSwitchedLayout_        = false;
    // Guard against rare lost key-up on Enter: if we see a long repeat storm,
    // synthesize key release and stop forwarding repeats.
    std::chrono::steady_clock::time_point enterRepeatWindowStart_{};
    int  enterRepeatCount_ = 0;
    /// Lost key-up after suspend/USB can cause endless letter repeats; force release.
    unsigned int                          letterRepeatCode_ = 0;
    std::chrono::steady_clock::time_point letterRepeatWindowStart_{};
    int                                   letterRepeatCount_ = 0;

    // Used for silence-based device reacquire after resume/connection delay.
    std::chrono::steady_clock::time_point lastInputEventAt_ = std::chrono::steady_clock::now();

    bool setupXkb();
    void teardownXkb();

    bool parseHotkey(const std::string& spec, HotkeySpec& out) const;
    void logResolvedHotkeys() const;
    bool matchMods(uint32_t mask) const;
    bool matchHotkey(const HotkeySpec& hk, unsigned int code, int value) const;
    // Returns the current KDE layout index (or -1), updating xkb state.
    int syncXkbLayoutFromKde();
    /// Match xkb Lock modifier to kernel Caps LED (fixes wrong case after restart / resume).
    void seedCapsLockFromHardware();

    /// Best-effort "unstick": release any keys we believe are currently held.
    /// Used before in-process reinit/exit to prevent apps from seeing stuck keys
    /// after suspend/resume or device-grab recovery.
    void releaseAllDownKeys();
    /// Release grabs, reset tracker/key state, reopen keyboards (suspend/resume, topology, etc.).
    /// Returns false if io_.init fails (caller should teardown xkb and exit).
    bool reinitKeyboardDevices(const char* logReason);

    void processEvent(const input_event& ev);

    bool doSwapLastWord();
    bool doSwapSelection();
    void doToggleAutoSwitch();
    bool doUndoLast();

    // Returns true when caller should consume (not forward) current boundary key event.
    bool checkAutoSwitch(const std::string& word, const std::string& boundaryUtf8);

    void tapBackspaces(int count);
    void applyLayoutForWord(CharMapping::Layout typedLayout);
    void recoverStuckModifiersFromXkb();

    /// Wayland: wtype then wl-copy + Ctrl+V by default; daemon.ini inject_paste_first reverses order.
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
