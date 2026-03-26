#include "PuntoDaemon.h"

#include "../core/CharMapping.h"
#include "../core/Utf8Utils.h"
#include "../core/WordSwapper.h"
#include "SessionEnv.h"
#include "TextInjector.h"

#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <poll.h>
#include <xkbcommon/xkbcommon.h>

#include <cctype>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace punto {

namespace {

static bool puntoDebugEnabled() {
    const char* p = std::getenv("PUNTO_DEBUG");
    return p && p[0] != '\0' && std::strcmp(p, "0") != 0;
}

static bool ieq(std::string a, std::string b) {
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a == b;
}

static void splitPlus(const std::string& spec, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (start < spec.size()) {
        size_t p = spec.find('+', start);
        if (p == std::string::npos) {
            out.push_back(spec.substr(start));
            break;
        }
        out.push_back(spec.substr(start, p - start));
        start = p + 1;
    }
}

static unsigned int keyNameToCode(const std::string& name) {
    std::string n = name;
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (n == "apostrophe" || n == "'") return KEY_APOSTROPHE;
    if (n == "grave" || n == "`") return KEY_GRAVE;
    if (n == "backspace") return KEY_BACKSPACE;
    if (n == "space") return KEY_SPACE;
    if (n == "tab") return KEY_TAB;
    if (n == "return" || n == "enter") return KEY_ENTER;
    if (n.length() == 1 && n[0] >= 'a' && n[0] <= 'z') return static_cast<unsigned>(KEY_A + (n[0] - 'a'));
    if (n.length() == 1 && n[0] >= '1' && n[0] <= '9')
        return static_cast<unsigned>(KEY_1 + static_cast<unsigned>(n[0] - '1'));
    if (n == "0") return KEY_0;

    return 0;
}

static bool isRuUpperLetter(char32_t c) { return (c >= U'А' && c <= U'Я') || c == U'Ё'; }
static bool isEnUpperLetter(char32_t c) { return c >= U'A' && c <= U'Z'; }
static char32_t toLowerLetter(char32_t c) {
    if (c >= U'A' && c <= U'Z') return c + 32;
    if (c == U'Ё') return U'ё';
    if (c >= U'А' && c <= U'Я') return c + 0x20;
    return c;
}

} // namespace

PuntoDaemon::PuntoDaemon(DaemonConfig cfg) : cfg_(std::move(cfg)), layout_(cfg_) {
    HeuristicConfig h;
    h.enabled                           = cfg_.autoSwitchEnabled;
    h.minWordLength                     = cfg_.minWordLength;
    h.confidenceThreshold               = cfg_.confidenceThreshold;
    h.minPlausibleDominantBigramScore   = cfg_.minPlausibleDominantBigramScore;
    heuristic_.setConfig(h);

    parseHotkey(cfg_.swapLastSpec, hkSwapLast_);
    parseHotkey(cfg_.swapSelectionSpec, hkSwapSel_);
    parseHotkey(cfg_.toggleAutoSpec, hkToggle_);
    parseHotkey(cfg_.undoSpec, hkUndo_);
}

bool PuntoDaemon::parseHotkey(const std::string& spec, HotkeySpec& out) const {
    out.modMask = 0;
    out.keyCode = 0;
    std::vector<std::string> parts;
    splitPlus(spec, parts);
    if (parts.empty()) return false;

    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::string t = parts[i];
        for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t == "alt") out.modMask |= 1u;
        else if (t == "shift") out.modMask |= 2u;
        else if (t == "ctrl" || t == "control") out.modMask |= 4u;
        else if (t == "super" || t == "win" || t == "meta") out.modMask |= 8u;
    }
    out.keyCode = keyNameToCode(parts.back());
    if (out.keyCode == KEY_APOSTROPHE) out.altKeyCode = KEY_GRAVE;
    return out.keyCode != 0;
}

bool PuntoDaemon::setupXkb() {
    xkbCtx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkbCtx_) return false;

    // Use the layout order from KDE's kxkbrc (via DaemonConfig) so that xkb group N == KDE
    // layout index N.  Hardcoding "us,ru" caused an EN/RU inversion on systems where KDE
    // stores layouts as "ru,us" (index 0=RU, 1=EN): KDE reports index 1 for EN, we set xkb
    // group 1 which in the old keymap was "ru" → every letter decoded as Russian.
    ::xkb_rule_names rn{};
    rn.rules   = "evdev";
    rn.model   = "pc105";
    rn.layout  = cfg_.xkbLayoutString.c_str();
    rn.variant = cfg_.xkbVariantString.c_str();
    rn.options = "grp:alt_shift_toggle";

    if (puntoDebugEnabled()) {
        std::cerr << "punto-switcher-daemon: xkb layout=\"" << cfg_.xkbLayoutString
                  << "\" variant=\"" << cfg_.xkbVariantString << "\"\n" << std::flush;
    }

    xkbMap_ = xkb_keymap_new_from_names(xkbCtx_, &rn, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!xkbMap_) return false;

    xkbState_ = xkb_state_new(xkbMap_);
    return xkbState_ != nullptr;
}

void PuntoDaemon::teardownXkb() {
    if (xkbState_) {
        xkb_state_unref(xkbState_);
        xkbState_ = nullptr;
    }
    if (xkbMap_) {
        xkb_keymap_unref(xkbMap_);
        xkbMap_ = nullptr;
    }
    if (xkbCtx_) {
        xkb_context_unref(xkbCtx_);
        xkbCtx_ = nullptr;
    }
}

bool PuntoDaemon::matchMods(uint32_t mask) const {
    const auto& ks = keyState_;
    bool alt   = ks[KEY_LEFTALT] || ks[KEY_RIGHTALT];
    bool shift = ks[KEY_LEFTSHIFT] || ks[KEY_RIGHTSHIFT];
    bool ctrl  = ks[KEY_LEFTCTRL] || ks[KEY_RIGHTCTRL];
    bool super = ks[KEY_LEFTMETA] || ks[KEY_RIGHTMETA];

    auto req = [&](unsigned bit, bool on) {
        if (mask & bit) return on;
        return !on;
    };
    return req(1u, alt) && req(2u, shift) && req(4u, ctrl) && req(8u, super);
}

bool PuntoDaemon::matchHotkey(const HotkeySpec& hk, unsigned int code, int value) const {
    if (value != 1) return false;
    if (code != hk.keyCode && (hk.altKeyCode == 0 || code != hk.altKeyCode)) return false;
    return matchMods(hk.modMask);
}

int PuntoDaemon::syncXkbLayoutFromKde() {
    if (!xkbMap_ || !xkbState_) return -1;
    int li = layout_.currentLayoutIndex();
    if (li < 0) return -1;
    xkb_layout_index_t n = xkb_keymap_num_layouts(xkbMap_);
    if (static_cast<xkb_layout_index_t>(li) >= n) return -1;

    xkb_mod_mask_t dep = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t lat = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t lck = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_LOCKED);
    /* KDE reports one active group; xkb needs the same index for depressed/latched/locked layout
     * components. Passing 0,0,li left depressed_layout at US while locked was RU — inconsistent
     * state broke shift levels (e.g. Latin caps → wrong case in utf8). */
    const xkb_layout_index_t lg = static_cast<xkb_layout_index_t>(li);
    xkb_state_update_mask(xkbState_, dep, lat, lck, lg, lg, lg);
    return li;
}

void PuntoDaemon::tapBackspaces(int count) {
    // Release held modifiers so the app sees plain Backspace, not Alt+Backspace ("delete word")
    // or Shift+Backspace. This matters when tapBackspaces is called from hotkey handlers where
    // Alt (and possibly Shift) are still physically held.
    const bool altL   = keyState_[KEY_LEFTALT];
    const bool altR   = keyState_[KEY_RIGHTALT];
    const bool shiftL = keyState_[KEY_LEFTSHIFT];
    const bool shiftR = keyState_[KEY_RIGHTSHIFT];
    if (altL)   io_.emitKey(KEY_LEFTALT,   0);
    if (altR)   io_.emitKey(KEY_RIGHTALT,  0);
    if (shiftL) io_.emitKey(KEY_LEFTSHIFT, 0);
    if (shiftR) io_.emitKey(KEY_RIGHTSHIFT,0);
    if (altL || altR || shiftL || shiftR) io_.sync();

    for (int i = 0; i < count; ++i) {
        io_.emitKey(KEY_BACKSPACE, 1);
        io_.emitKey(KEY_BACKSPACE, 0);
    }
}

bool PuntoDaemon::injectViaWlCopyPaste(const std::string& utf8) {
    if (utf8.empty()) return true;
    ensureSessionEnvFromParents();
    int fd[2];
    if (pipe(fd) < 0) return false;

    // Double-fork: the intermediate child exits immediately so waitpid() never blocks.
    // The grandchild runs wl-copy and stays alive to serve the Wayland clipboard selection
    // until the compositor (or a clipboard manager like klipper) takes ownership.
    pid_t mid = fork();
    if (mid < 0) {
        close(fd[0]);
        close(fd[1]);
        return false;
    }
    if (mid == 0) {
        // --- intermediate child ---
        pid_t inner = fork();
        if (inner < 0) _exit(1);
        if (inner == 0) {
            // --- grandchild: the actual wl-copy ---
            close(fd[1]); // close write end in grandchild
            dup2(fd[0], STDIN_FILENO);
            close(fd[0]);
            // Become session leader so we don't get killed with parent
            ::setsid();
            execl("/usr/bin/wl-copy", "wl-copy", static_cast<char*>(nullptr));
            execlp("wl-copy", "wl-copy", static_cast<char*>(nullptr));
            _exit(127);
        }
        // Intermediate child exits immediately → parent waitpid returns quickly.
        _exit(0);
    }

    // Parent: write the text, close both pipe ends, wait for intermediate child.
    close(fd[0]);
    const ssize_t n = write(fd[1], utf8.data(), utf8.size());
    close(fd[1]);
    int st = 0;
    if (waitpid(mid, &st, 0) < 0) return false;
    if (n != static_cast<ssize_t>(utf8.size())) return false;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return false;

    // Give the wl-copy grandchild just enough time to connect to Wayland and claim the
    // selection before we emit the paste shortcut. 80ms is ample (socket connect ~10ms).
    ::usleep(80000);
    // Release any held modifiers (Alt/Shift) so Ctrl+V reaches the app as plain paste,
    // not Alt+Ctrl+V or some other unintended combo. This matters when called from hotkey
    // handlers where the hotkey modifier is still physically held.
    {
        if (keyState_[KEY_LEFTALT])    io_.emitKey(KEY_LEFTALT,    0);
        if (keyState_[KEY_RIGHTALT])   io_.emitKey(KEY_RIGHTALT,   0);
        if (keyState_[KEY_LEFTSHIFT])  io_.emitKey(KEY_LEFTSHIFT,  0);
        if (keyState_[KEY_RIGHTSHIFT]) io_.emitKey(KEY_RIGHTSHIFT, 0);
        io_.sync();
    }
    // Ctrl+V — paste in GUI apps (Kate, browsers, Qt/Electron apps, etc.).
    // NOTE: Do NOT also send Ctrl+Shift+V here — in many apps (VS Code, Cursor, Electron)
    // that shortcut ALSO triggers a paste, causing the text to be inserted twice.
    // Terminal support (Konsole etc.) requires wtype; when the compositor supports the
    // virtual keyboard protocol, wtype is tried first and reaches all apps including terminals.
    io_.emitKey(KEY_LEFTCTRL, 1);
    io_.emitKey(KEY_V, 1);
    io_.emitKey(KEY_V, 0);
    io_.emitKey(KEY_LEFTCTRL, 0);
    io_.sync();
    return true;
}

bool PuntoDaemon::injectText(const std::string& utf8) {
    if (utf8.empty()) return true;
    ensureSessionEnvFromParents();

    // wtype uses the Wayland virtual keyboard protocol (zwp_virtual_keyboard_v1): it injects
    // key events directly into the compositor and works in ALL applications — including
    // terminals where Ctrl+V does not paste. Try it first.
    if (injectUtf8Text(utf8)) {
        if (puntoDebugEnabled()) {
            std::cerr << "punto-switcher-daemon: text inject via wtype\n" << std::flush;
        }
        return true;
    }

    // Fallback: set clipboard via wl-copy, then send Ctrl+V.  Works in most GUI apps but
    // NOT in terminals (they use Ctrl+Shift+V).  Used when wtype is unavailable or KWin
    // rejects the virtual-keyboard protocol.
    const char* wlDisp   = std::getenv("WAYLAND_DISPLAY");
    const bool  onWayland = wlDisp && wlDisp[0];
    if (onWayland && injectViaWlCopyPaste(utf8)) {
        if (puntoDebugEnabled()) {
            std::cerr << "punto-switcher-daemon: text inject via wl-copy + Ctrl+V (wtype fallback)\n"
                      << std::flush;
        }
        return true;
    }
    return false;
}

void PuntoDaemon::applyLayoutForWord(CharMapping::Layout typedLayout) {
    if (typedLayout == CharMapping::Layout::Russian) { layout_.setEnglish(); daemonSwitchedLayout_ = true; }
    else if (typedLayout == CharMapping::Layout::English) { layout_.setRussian(); daemonSwitchedLayout_ = true; }
}

bool PuntoDaemon::checkAutoSwitch(const std::string& word, const std::string& boundaryUtf8) {
    if (tracker_.isCurrentTokenFrozen()) return false;

    const std::u32string w32   = utf8_to_utf32(word);
    CharMapping::Layout layout = CharMapping::dominantLayout(w32);

    // Mirror AutoSwitchHeuristic "plausible dominant script" guard here so we never run
    // backspace+inject when the word already fits the active language model — even if
    // layout_.currentLayoutIndex() is unknown (no DBus) or shouldSwitch regresses. Stops
    // "привет deleted then pasted привет" when inject(swapped) fails and restore(original) runs.
    const double ruRatio = CharMapping::russianLetterRatio(w32);
    const double enRatio = CharMapping::englishLetterRatio(w32);
    if (layout == CharMapping::Layout::Russian && ruRatio >= 0.85 && enRatio <= 0.05) {
        if (AutoSwitchHeuristic::bigramScore(w32, CharMapping::Layout::Russian) >=
            cfg_.minPlausibleDominantBigramScore) {
            if (puntoDebugEnabled()) {
                std::cerr << "punto-switcher-daemon: checkAutoSwitch skip (plausible RU word)\n"
                          << std::flush;
            }
            return false;
        }
    }
    if (layout == CharMapping::Layout::English && enRatio >= 0.85 && ruRatio <= 0.05) {
        if (AutoSwitchHeuristic::bigramScore(w32, CharMapping::Layout::English) >=
            cfg_.minPlausibleDominantBigramScore) {
            if (puntoDebugEnabled()) {
                std::cerr << "punto-switcher-daemon: checkAutoSwitch skip (plausible EN word)\n"
                          << std::flush;
            }
            return false;
        }
    }

    // NOTE: The hard "Cyrillic + KDE RU layout group" guard that was here has been removed.
    // It blocked conversion of non-words like "руддщ" (= "hello" typed on RU layout) even
    // though they are not plausible Russian.  The plausible-script bigram guard above already
    // correctly passes "привет" (high RU bigram score → skip) and lets "руддщ" through.

    const bool doSwitch = heuristic_.shouldSwitch(word, layout);
    if (puntoDebugEnabled()) {
        std::cerr << "punto-switcher-daemon: checkAutoSwitch word=\"" << word << "\" shouldSwitch="
                  << (doSwitch ? "yes" : "no") << "\n"
                  << std::flush;
    }
    if (!doSwitch) return false;

    std::string swapped = CharMapping::swapWord(word);
    if (swapped == word) return false;

    int wordCpCount = static_cast<int>(utf8_to_utf32(word).size());

    tracker_.undoPrevLayout         = layout;
    tracker_.undoOriginalTextUtf8   = word;
    tracker_.undoSwappedTextUtf8    = swapped;
    const auto toLowerAscii = [](std::string s) {
        for (char& ch : s) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        }
        return s;
    };
    const std::string swappedLower = toLowerAscii(swapped);
    const bool urlSchemePrefix =
        (layout == CharMapping::Layout::Russian)
        && (swappedLower == "http:" || swappedLower == "https:" || swappedLower == "ftp:")
        && boundaryUtf8 == ".";
    if (urlSchemePrefix) pendingUrlSecondSlash_ = true;

    tracker_.undoSwappedCpCount     = utf8_to_utf32(urlSchemePrefix ? (swapped + "/") : swapped).size();

    InternalGuard g(&tracker_);
    tapBackspaces(wordCpCount);
    if (!injectText(urlSchemePrefix ? (swapped + "/") : swapped)) {
        std::cerr << "punto-switcher-daemon: inject failed (wtype + wl-copy) — restoring word\n";
        if (!injectText(word)) {
            std::cerr << "punto-switcher-daemon: could not restore word after failed inject\n";
        }
        tracker_.undoPrevLayout.reset();
        tracker_.undoOriginalTextUtf8.reset();
        tracker_.undoSwappedTextUtf8.reset();
        tracker_.undoSwappedCpCount = 0;
        return false;
    }
    tracker_.freezeCurrentToken();
    // Update wordBuffer to the swapped text so that the upcoming tracker_.onBoundary()
    // (called by processEvent right after checkAutoSwitch returns) records lastWord = swapped.
    // Without this, lastWord stays "ghbdtn" (the original), and a subsequent manual Alt+'
    // would swap "ghbdtn" → "привет" again — a visual no-op that deletes the trailing space.
    // With the fix, lastWord = "привет" and Alt+' correctly reverts to "ghbdtn".
    tracker_.wordBuffer = urlSchemePrefix ? (swapped + "/") : swapped;

    if (layout == CharMapping::Layout::Russian) { layout_.setEnglish(); daemonSwitchedLayout_ = true; }
    else if (layout == CharMapping::Layout::English) { layout_.setRussian(); daemonSwitchedLayout_ = true; }
    return urlSchemePrefix;
}

bool PuntoDaemon::doSwapLastWord() {
    if (!tracker_.wordBuffer.empty()) {
        const std::string original = tracker_.wordBuffer;
        std::string swapped        = CharMapping::swapWord(original);
        if (swapped == original) return false;

        CharMapping::Layout typedLayout = CharMapping::dominantLayout(utf8_to_utf32(original));
        int cpCount                     = static_cast<int>(utf8_to_utf32(original).size());

        tracker_.undoPrevLayout       = typedLayout;
        tracker_.undoOriginalTextUtf8 = original;
        tracker_.undoSwappedTextUtf8  = swapped;
        tracker_.undoSwappedCpCount   = utf8_to_utf32(swapped).size();

        InternalGuard g(&tracker_);
        tapBackspaces(cpCount);
        if (!injectText(swapped)) {
            std::cerr << "punto-switcher-daemon: failed to type swapped text; restoring\n";
            (void)injectText(original);
            tracker_.undoPrevLayout.reset();
            tracker_.undoOriginalTextUtf8.reset();
            tracker_.undoSwappedTextUtf8.reset();
            tracker_.undoSwappedCpCount = 0;
            return false;
        }
        applyLayoutForWord(typedLayout);
        tracker_.reset(/*clearUndo=*/false);
        // Keep lastWord so Alt+' can be pressed repeatedly (toggle).
        tracker_.lastWord       = swapped;
        tracker_.lastWordByteSize = static_cast<int>(utf8_to_utf32(swapped).size());
        tracker_.lastBoundaryUtf8.clear();
        tracker_.freezeCurrentToken();
        return true;
    }

    if (!tracker_.lastWord.empty()) {
        const std::string& original = tracker_.lastWord;
        std::string swapped       = CharMapping::swapWord(original);
        if (swapped == original) return false;

        CharMapping::Layout typedLayout = CharMapping::dominantLayout(utf8_to_utf32(original));
        int wordCp                      = static_cast<int>(utf8_to_utf32(original).size());
        // If we don't have a stored boundary (empty string), then previous manual swaps already
        // removed the boundary character from the text. In that case deleting 1 extra char would
        // break repeated toggles.
        int boundaryCp = tracker_.lastBoundaryUtf8.empty() ? 0 : static_cast<int>(utf8_length(tracker_.lastBoundaryUtf8));

        tracker_.undoPrevLayout       = typedLayout;
        tracker_.undoOriginalTextUtf8 = original;
        tracker_.undoSwappedTextUtf8  = swapped;
        tracker_.undoSwappedCpCount   = utf8_to_utf32(swapped).size();

        InternalGuard g(&tracker_);
        tapBackspaces(wordCp + boundaryCp);
        if (!injectText(swapped)) {
            std::string restore = original;
            if (!tracker_.lastBoundaryUtf8.empty()) restore += tracker_.lastBoundaryUtf8;
            (void)injectText(restore);
            tracker_.undoPrevLayout.reset();
            tracker_.undoOriginalTextUtf8.reset();
            tracker_.undoSwappedTextUtf8.reset();
            tracker_.undoSwappedCpCount = 0;
            return false;
        }
        applyLayoutForWord(typedLayout);
        tracker_.reset(/*clearUndo=*/false);
        // Keep lastWord so Alt+' can be pressed repeatedly (toggle).
        tracker_.lastWord       = swapped;
        tracker_.lastWordByteSize = static_cast<int>(utf8_to_utf32(swapped).size());
        tracker_.lastBoundaryUtf8.clear(); // boundary char was deleted by tapBackspaces if it existed
        tracker_.freezeCurrentToken();
        return true;
    }

    return false;
}

bool PuntoDaemon::doSwapSelection() {
    ensureSessionEnvFromParents();

    // Release Alt/Shift if they're still physically held so Ctrl+C works as plain copy.
    if (keyState_[KEY_LEFTALT]) io_.emitKey(KEY_LEFTALT, 0);
    if (keyState_[KEY_RIGHTALT]) io_.emitKey(KEY_RIGHTALT, 0);
    if (keyState_[KEY_LEFTSHIFT]) io_.emitKey(KEY_LEFTSHIFT, 0);
    if (keyState_[KEY_RIGHTSHIFT]) io_.emitKey(KEY_RIGHTSHIFT, 0);
    io_.sync();

    // Copy current selection to clipboard.
    io_.emitKey(KEY_LEFTCTRL, 1);
    io_.emitKey(KEY_C, 1);
    io_.emitKey(KEY_C, 0);
    io_.emitKey(KEY_LEFTCTRL, 0);
    io_.sync();
    ::usleep(20000);

    // Read clipboard (Wayland).
    std::string selected;
    {
        FILE* p = popen("timeout 0.2s /usr/bin/wl-paste -n 2>/dev/null", "r");
        if (p) {
            char buf[4096];
            while (fgets(buf, sizeof(buf), p)) {
                selected += buf;
            }
            (void)pclose(p);
        }
    }

    // Trim trailing whitespace/newlines (wl-paste -n should already avoid newline, but be safe).
    while (!selected.empty() && (selected.back() == '\n' || selected.back() == '\r' || selected.back() == ' ' ||
                                 selected.back() == '\t'))
        selected.pop_back();

    if (selected.empty()) return false;

    // Keep it simple: swap only when selection looks like a single token.
    // (If it contains spaces/newlines, we bail out to avoid partial unexpected replacements.)
    if (selected.find_first_of(" \t\r\n") != std::string::npos) return false;

    std::string swapped = CharMapping::swapWord(selected);
    if (swapped == selected) return false;

    CharMapping::Layout typedLayout = CharMapping::dominantLayout(utf8_to_utf32(selected));
    tracker_.undoPrevLayout           = typedLayout;
    tracker_.undoOriginalTextUtf8   = selected;
    tracker_.undoSwappedTextUtf8     = swapped;
    tracker_.undoSwappedCpCount     = utf8_to_utf32(swapped).size();

    InternalGuard g(&tracker_);
    if (!injectText(swapped)) {
        tracker_.undoPrevLayout.reset();
        tracker_.undoOriginalTextUtf8.reset();
        tracker_.undoSwappedTextUtf8.reset();
        tracker_.undoSwappedCpCount = 0;
        return false;
    }

    tracker_.reset(/*clearUndo=*/false);
    tracker_.freezeCurrentToken();
    return true;
}

void PuntoDaemon::doToggleAutoSwitch() {
    cfg_.autoSwitchEnabled = !cfg_.autoSwitchEnabled;
    cfg_.save();
    HeuristicConfig h;
    h.enabled                           = cfg_.autoSwitchEnabled;
    h.minWordLength                     = cfg_.minWordLength;
    h.confidenceThreshold               = cfg_.confidenceThreshold;
    h.minPlausibleDominantBigramScore   = cfg_.minPlausibleDominantBigramScore;
    heuristic_.setConfig(h);
    std::cerr << "punto-switcher-daemon: AutoSwitch " << (cfg_.autoSwitchEnabled ? "ON" : "OFF") << "\n";
}

bool PuntoDaemon::doUndoLast() {
    bool did = false;
    if (tracker_.undoPrevLayout.has_value()) {
        layout_.setForCharLayout(*tracker_.undoPrevLayout);
        did = true;
    }
    if (tracker_.undoOriginalTextUtf8 && tracker_.undoSwappedTextUtf8) {
        InternalGuard g(&tracker_);
        for (size_t i = 0; i < tracker_.undoSwappedCpCount; ++i) {
            io_.emitKey(KEY_BACKSPACE, 1);
            io_.emitKey(KEY_BACKSPACE, 0);
        }
        if (injectText(*tracker_.undoOriginalTextUtf8)) did = true;
    }
    tracker_.reset(/*clearUndo=*/true);
    return did;
}

void PuntoDaemon::processEvent(const input_event& ev) {
    if (ev.type != EV_KEY) {
        io_.forward(ev);
        return;
    }

    const unsigned int code = ev.code;
    const int          val  = ev.value;

    if (puntoDebugEnabled() && (val == 1 || val == 2)) {
        std::fprintf(stderr, "punto-switcher-daemon: EV_KEY code=%u value=%d\n", code, val);
        std::fflush(stderr);
    }

    if (code >= keyState_.size()) {
        io_.forward(ev);
        return;
    }

    // Swallow paired release for consumed keys
    if (val == 0 && swallowed_.count(code)) {
        swallowed_.erase(code);
        keyState_[code] = 0;
        return;
    }

    // Update key state before hotkey match (press path)
    if (val == 1 || val == 2) keyState_[code] = 1;
    else if (val == 0)
        keyState_[code] = 0;
    if (code == KEY_CAPSLOCK && val == 1) {
        capsLockOn_ = !capsLockOn_;
    }

    if (val == 1) {
        if (matchHotkey(hkSwapLast_, code, val)) {
            swallowed_.insert(code);
            doSwapLastWord();
            return;
        }
        if (matchHotkey(hkSwapSel_, code, val)) {
            swallowed_.insert(code);
            doSwapSelection();
            return;
        }
        if (matchHotkey(hkToggle_, code, val)) {
            swallowed_.insert(code);
            doToggleAutoSwitch();
            return;
        }
        if (matchHotkey(hkUndo_, code, val)) {
            swallowed_.insert(code);
            doUndoLast();
            return;
        }
    }

    enum xkb_key_direction dir =
        (val == 0) ? XKB_KEY_UP : XKB_KEY_DOWN;

    // Detect modifier-only sequences (Ctrl+Shift, Alt+Shift, …) that are likely layout-switch
    // shortcuts.  KDE processes the uinput modifier events on its own event loop and updates
    // the active group index asynchronously.  Without a short wait, the first normal key after
    // the combo is decoded with the *old* layout (e.g. 'п' instead of 'g' right after switching
    // from RU→EN), because our DBus query of currentLayoutIndex() races with KDE's processing.
    {
        const bool isModKey = (code == KEY_LEFTCTRL  || code == KEY_RIGHTCTRL  ||
                               code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT ||
                               code == KEY_LEFTALT   || code == KEY_RIGHTALT   ||
                               code == KEY_LEFTMETA  || code == KEY_RIGHTMETA);
        if (val == 1 && isModKey) {
            onlyModifiersSinceNormalKey_ = true;
        } else if (val == 0 && isModKey) {
            // Modifier released — check whether all modifiers are now up.
            // keyState_ was already set to 0 for this code above.
            const bool anyModDown = keyState_[KEY_LEFTCTRL]  || keyState_[KEY_RIGHTCTRL]  ||
                                    keyState_[KEY_LEFTSHIFT] || keyState_[KEY_RIGHTSHIFT] ||
                                    keyState_[KEY_LEFTALT]   || keyState_[KEY_RIGHTALT]   ||
                                    keyState_[KEY_LEFTMETA]  || keyState_[KEY_RIGHTMETA];
            if (!anyModDown && onlyModifiersSinceNormalKey_) {
                pendingLayoutRefresh_ = true;
            }
        } else if (val == 1 && !isModKey) {
            // Normal key press — consume the pending refresh if any.
            if (pendingLayoutRefresh_) {
                // 15 ms: enough for KDE to process uinput modifier events and update the
                // global layout index via the virtual keyboard component.
                ::usleep(15000);
                pendingLayoutRefresh_ = false;
                if (puntoDebugEnabled()) {
                    std::cerr << "punto-switcher-daemon: layout refresh delay (post modifier-only seq)\n"
                              << std::flush;
                }
            }
            onlyModifiersSinceNormalKey_ = false;
        }
    }

    const int curLayout = syncXkbLayoutFromKde();
    // If KDE's active layout group changed, discard the current partial word: characters typed
    // before the switch were decoded with the old layout and must not be mixed with new ones.
    // Exception: if the daemon itself triggered the layout change (after an auto-switch), keep
    // the tracker intact so that lastWord is available for a subsequent manual Alt+' hotkey.
    if (curLayout >= 0 && curLayout != lastKnownLayoutIndex_) {
        if (lastKnownLayoutIndex_ >= 0) {
            if (daemonSwitchedLayout_) {
                // Daemon-initiated layout change — preserve lastWord for manual swap hotkey.
                daemonSwitchedLayout_ = false;
                if (puntoDebugEnabled()) {
                    std::cerr << "punto-switcher-daemon: layout changed " << lastKnownLayoutIndex_
                              << " → " << curLayout << " (daemon-initiated, tracker preserved)\n"
                              << std::flush;
                }
            } else {
                tracker_.reset();
                if (puntoDebugEnabled()) {
                    std::cerr << "punto-switcher-daemon: layout changed " << lastKnownLayoutIndex_
                              << " → " << curLayout << ", wordBuffer reset\n" << std::flush;
                }
            }
        }
        lastKnownLayoutIndex_ = curLayout;
    }
    xkb_state_update_key(xkbState_, code + 8, dir);

    if (val == 1 || val == 2) {
        if (code == KEY_BACKSPACE || code == KEY_DELETE) {
            tracker_.reset();
            io_.forward(ev);
            return;
        }

        bool ctrl  = keyState_[KEY_LEFTCTRL] || keyState_[KEY_RIGHTCTRL];
        bool alt   = keyState_[KEY_LEFTALT] || keyState_[KEY_RIGHTALT];
        bool super = keyState_[KEY_LEFTMETA] || keyState_[KEY_RIGHTMETA];
        if (ctrl || alt || super) {
            io_.forward(ev);
            return;
        }

        char buf[128];
        int  n = xkb_state_key_get_utf8(xkbState_, code + 8, buf, sizeof(buf));
        if (n <= 0) {
            if (puntoDebugEnabled()) {
                std::fprintf(stderr, "punto-switcher-daemon: xkb utf8 empty for evcode=%u (layout sync?)\n", code);
                std::fflush(stderr);
            }
            io_.forward(ev);
            return;
        }

        std::string utf8(buf, static_cast<size_t>(n));
        std::u32string u32 = utf8_to_utf32(utf8);
        if (u32.empty()) {
            io_.forward(ev);
            return;
        }
        char32_t ch = u32[0];

        if (WordSwapper::isWordBoundary(ch)) {
            if (puntoDebugEnabled()) {
                std::cerr << "punto-switcher-daemon: word boundary buf=\"" << tracker_.wordBuffer << "\"\n"
                          << std::flush;
            }
            bool consumeBoundary = false;
            if (pendingUrlSecondSlash_ && (utf8 == "." || utf8 == "/")) {
                // Second boundary in patterns like "реезыЖ..": make it '/' and swallow.
                // We also inject explicitly, because returning early alone only prevents forwarding.
                pendingUrlSecondSlash_ = false;
                tracker_.onBoundary("/");
                (void)injectText("/");
                return;
            }
            if (!tracker_.wordBuffer.empty() && !tracker_.isCurrentTokenFrozen())
                consumeBoundary = checkAutoSwitch(tracker_.wordBuffer, utf8);
            tracker_.onBoundary(consumeBoundary ? std::string("/") : utf8);
            if (consumeBoundary) return;
        } else {
            pendingUrlSecondSlash_ = false;
            if (puntoDebugEnabled()) {
                std::cerr << "punto-switcher-daemon: key utf8=\"" << utf8 << "\" evcode=" << code
                          << " val=" << val << "\n"
                          << std::flush;
            }
            // Guard against occasional stale uppercase decode right after hotkeys/layout races:
            // when Shift is not physically held and CapsLock is inactive, normalize letters to lower.
            const bool shiftDown = keyState_[KEY_LEFTSHIFT] || keyState_[KEY_RIGHTSHIFT];
            if (!shiftDown && !capsLockOn_ && u32.size() == 1) {
                char32_t c = u32[0];
                if (isRuUpperLetter(c) || isEnUpperLetter(c)) {
                    u32[0] = toLowerLetter(c);
                    utf8 = utf32_to_utf8(u32);
                }
            }
            tracker_.addChar(utf8);
        }
    }

    io_.forward(ev);
}

int PuntoDaemon::run() {
    if (!setupXkb()) {
        std::cerr << "punto-switcher-daemon: xkbcommon init failed\n";
        return 1;
    }

    if (!io_.init(cfg_.devicePath)) {
        std::cerr << "punto-switcher-daemon: evdev/uinput init failed (see messages above; input group, udev, modprobe uinput)\n";
        teardownXkb();
        return 1;
    }

    if (io_.devices().empty()) {
        teardownXkb();
        return 1;
    }

    std::cerr << "punto-switcher-daemon: running (grabbed " << io_.devices().size()
              << " keyboard(s)). Stop with Ctrl+C.\n";
    if (puntoDebugEnabled()) {
        std::cerr << "punto-switcher-daemon: debug on — EV_KEY lines + utf8; or run: punto-switcher-daemon --debug\n"
                  << std::flush;
    }
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        std::cerr << "punto-switcher-daemon: warning: DBUS_SESSION_BUS_ADDRESS unset — KDE layout sync/switch "
                      "will not work. With sg(1), export it from your session before sg (see loginctl show-session).\n";
    }

    while (true) {
        const auto& devs = io_.devices();
        if (devs.empty()) {
            std::cerr << "punto-switcher-daemon: no input devices opened, trying to reinitialize...\n";
            ::usleep(500000);
            if (!io_.init(cfg_.devicePath)) continue;
        }
        std::vector<struct pollfd> pfds(devs.size());
        bool needReinitAfterDeviceLoss = false;
        for (size_t i = 0; i < devs.size(); ++i) {
            pfds[i].fd     = libevdev_get_fd(devs[i]);
            pfds[i].events = POLLIN;
        }
        if (poll(pfds.data(), pfds.size(), -1) <= 0) continue;

        for (size_t i = 0; i < devs.size(); ++i) {
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::cerr << "punto-switcher-daemon: poll reports device error/hup (revents="
                          << pfds[i].revents << "), reinitializing devices\n";
                needReinitAfterDeviceLoss = true;
                break;
            }
            if (!(pfds[i].revents & POLLIN)) continue;
            libevdev* keyboard = devs[i];
            for (;;) {
                input_event ev;
                int         rc = libevdev_next_event(keyboard, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                if (rc < 0) {
                    if (rc == -EAGAIN || rc == -EWOULDBLOCK) break;
                    if (rc == -ENODEV || rc == -EIO || rc == -ENXIO) {
                        std::cerr << "punto-switcher-daemon: input device became unavailable (rc=" << rc
                                  << "), reinitializing devices\n";
                        needReinitAfterDeviceLoss = true;
                        break;
                    }
                    if (puntoDebugEnabled()) {
                        std::cerr << "punto-switcher-daemon: libevdev_next_event rc=" << rc << "\n"
                                  << std::flush;
                    }
                    break;
                }
                if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                    while (libevdev_next_event(keyboard, LIBEVDEV_READ_FLAG_SYNC, &ev)
                           == LIBEVDEV_READ_STATUS_SUCCESS) {
                        processEvent(ev);
                    }
                    continue;
                }
                if (rc != LIBEVDEV_READ_STATUS_SUCCESS) break;
                processEvent(ev);
            }
            if (needReinitAfterDeviceLoss) break;
        }
        if (needReinitAfterDeviceLoss) {
            io_.shutdown();
            tracker_.reset();
            keyState_.fill(0);
            swallowed_.clear();
            pendingLayoutRefresh_ = false;
            onlyModifiersSinceNormalKey_ = false;
            pendingUrlSecondSlash_ = false;
            capsLockOn_ = false;
            daemonSwitchedLayout_ = false;
            ::usleep(500000);
            (void)io_.init(cfg_.devicePath);
            continue;
        }
    }

    teardownXkb();
    return 0;
}

} // namespace punto
