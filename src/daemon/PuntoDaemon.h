#pragma once

#include "../core/AutoSwitchHeuristic.h"
#include "../engine/InputTracker.h"
#include "DaemonConfig.h"
#include "EvdevUinput.h"
#include "LayoutController.h"
#include "XkbInterpreter.h"

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

    XkbInterpreter                 keys_;
    std::vector<Keystroke>         wordKeys_;
    std::vector<Keystroke>         lastWordKeys_;
    int                            wordLayout_      = -1;
    int                            lastWordLayout_  = -1;
    std::string                    lastBoundaryUtf8_;
    bool                           lastBoundaryValid_ = false;
    bool                           wordFrozen_      = false;

    struct TypingUndo {
        bool                   valid      = false;
        bool                   selection  = false;
        /// The replacement was typed as text, not replayed as the original keys.
        bool                   textReplace = false;
        std::vector<Keystroke> keys;
        int                    layoutIndex = -1;
        std::string            boundaryUtf8;
        std::string            originalText;
        int                    deleteCpCount = 0;
    };
    TypingUndo typingUndo_;

    std::vector<int> mouseFds_;
    // Guard against rare lost key-up on Enter: if we see a long repeat storm,
    // synthesize key release and stop forwarding repeats.
    std::chrono::steady_clock::time_point enterRepeatWindowStart_{};
    int  enterRepeatCount_ = 0;
    /// Lost key-up after suspend/USB can cause endless letter repeats; force release.
    unsigned int                          letterRepeatCode_ = 0;
    std::chrono::steady_clock::time_point letterRepeatWindowStart_{};
    int                                   letterRepeatCount_ = 0;

    std::chrono::steady_clock::time_point lastTopologyProbe_{};
    int topologyMismatchStreak_ = 0;

    // Settings window and tray: STATUS / AUTO / RELOAD. -1 when the socket is down.
    int controlFd_ = -1;
    void openControl();
    void closeControl();
    void serviceControl();

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
    /// Reopen when a keyboard disappeared or a new one appeared (Bluetooth reconnect).
    /// No = nothing to do, Yes = reopened, Failed = reopen failed.
    enum class Reacquire { No, Yes, Failed };
    Reacquire reacquireIfKeyboardSetChanged();

    void processEvent(const input_event& ev);

    bool doSwapLastWord();
    bool doSwapSelection();
    void doToggleAutoSwitch();
    void setAutoSwitch(bool on);
    void applyHeuristic();
    bool doUndoLast();

    enum class KeyRole { Ignore, Word, Boundary };

    Keystroke makeKeystroke(unsigned int code) const;
    KeyRole classifyKey(const Keystroke& ks, int layout) const;
    int otherLayoutIndex(int layout) const;
    CharMapping::Layout layoutEnum(int layout) const;
    void noteLayout(int layoutIndex);
    void forgetTyping();
    void noteWordKey(const Keystroke& ks);
    void noteBackspace();
    void noteRepeat(unsigned int code);
    void releaseModifiersForInject();
    void restoreHeldModifiers();
    void replayKeystrokes(const std::vector<Keystroke>& keys);
    bool emitUtf8InLayout(const std::string& utf8, int layout);
    bool switchToLayout(int layoutIndex);
    /// Layout that can produce `c`, preferring the one that is active now.
    int layoutForCodepoint(char32_t c) const;
    /// True when every character has a key in the English or Russian group.
    bool canTypeText(const std::string& utf8) const;
    /// Type `utf8` with uinput, switching layout when the current group has no key.
    bool typeText(const std::string& utf8);
    /// Backspace the word as displayed in `fromLayout`, replay it in `toLayout`.
    /// `boundaryOnScreen` is true only when that separator was already inserted
    /// (manual swap of a finished word). Auto-switch sees the separator first and
    /// must not backspace it — that deletes the space before the word.
    bool convertKeys(const std::vector<Keystroke>& keys, int fromLayout, int toLayout,
                     const std::string& boundaryUtf8, bool boundaryOnScreen);
    void commitWord(const Keystroke& boundary);
    /// Returns true when the boundary key was replaced and must not be forwarded.
    bool onWordBoundary(const Keystroke& boundary);
    /// Backspace the typed word and type a typo fix. The boundary key is left
    /// for the caller to forward. False when nothing was changed.
    bool applyTypoCorrection(const Keystroke& boundary, const std::string& typed, int layout);

    void openMice();
    void closeMice();
    void drainMice();

    std::string readClipboard() const;
    bool setClipboard(const std::string& utf8, bool pasteOnce);
    bool clearClipboard();
    void pasteCtrlV();
    void copyCtrlC();
    /// wl-copy without --paste-once, then Ctrl+V only after wl-paste reads it back.
    bool pasteFromClipboard(const std::string& utf8);

    void tapBackspaces(int count);
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
