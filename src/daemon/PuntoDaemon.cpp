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
#include <xkbcommon/xkbcommon-names.h>

#include <cctype>
#include <csignal>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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

volatile std::sig_atomic_t gStopRequested = 0;

void handleStopSignal(int) {
    gStopRequested = 1;
}

static double monotonicBootSeconds() {
    timespec ts{};
    if (::clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0.0;
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

static bool ieq(std::string a, std::string b) {
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a == b;
}

static std::string toLowerAsciiCopy(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return s;
}

static void trimInPlace(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
}

static void splitPlus(const std::string& spec, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (start < spec.size()) {
        size_t p = spec.find('+', start);
        if (p == std::string::npos) {
            std::string chunk = spec.substr(start);
            trimInPlace(chunk);
            if (!chunk.empty()) out.push_back(std::move(chunk));
            break;
        }
        std::string chunk = spec.substr(start, p - start);
        trimInPlace(chunk);
        if (!chunk.empty()) out.push_back(std::move(chunk));
        start = p + 1;
    }
}

/// True for evdev codes of Latin letter keys. Linux KEY_A..KEY_Z are not contiguous (e.g. KEY_E=18,
/// KEY_A=30); range checks miss Q, W, E, R, T, Y, U, I, O, P and break bare-hotkey guards.
static bool isLatinLetterEvdevKey(unsigned int code) {
    switch (code) {
    case KEY_A:
    case KEY_B:
    case KEY_C:
    case KEY_D:
    case KEY_E:
    case KEY_F:
    case KEY_G:
    case KEY_H:
    case KEY_I:
    case KEY_J:
    case KEY_K:
    case KEY_L:
    case KEY_M:
    case KEY_N:
    case KEY_O:
    case KEY_P:
    case KEY_Q:
    case KEY_R:
    case KEY_S:
    case KEY_T:
    case KEY_U:
    case KEY_V:
    case KEY_W:
    case KEY_X:
    case KEY_Y:
    case KEY_Z:
        return true;
    default:
        return false;
    }
}

/// Letter/digit/punctuation keys must not be used as bare hotkeys (modMask==0), or every
/// press is swallowed by matchHotkey() without io_.forward() — e.g. toggle_auto=e or undo=BackSpace.
static bool bareKeyNeedsModifiers(unsigned int code) {
    if (isLatinLetterEvdevKey(code)) return true;
    if ((code >= KEY_1 && code <= KEY_9) || code == KEY_0) return true;
    switch (code) {
    case KEY_BACKSPACE:
    case KEY_SPACE:
    case KEY_TAB:
    case KEY_ENTER:
    case KEY_APOSTROPHE:
    case KEY_GRAVE:
    case KEY_COMMA:
    case KEY_DOT:
    case KEY_SLASH:
    case KEY_SEMICOLON:
    case KEY_MINUS:
    case KEY_EQUAL:
    case KEY_LEFTBRACE:
    case KEY_RIGHTBRACE:
    case KEY_BACKSLASH:
    case KEY_102ND:
        return true;
    default:
        return false;
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
    if (n == "escape" || n == "esc") return KEY_ESC;
    if (n == "insert") return KEY_INSERT;
    if (n == "home") return KEY_HOME;
    if (n == "end") return KEY_END;
    if (n == "pageup" || n == "pgup") return KEY_PAGEUP;
    if (n == "pagedown" || n == "pgdn") return KEY_PAGEDOWN;
    if (n == "delete" || n == "del") return KEY_DELETE;
    if (n == "capslock") return KEY_CAPSLOCK;
    if (n == "semicolon" || n == ";") return KEY_SEMICOLON;
    if (n == "comma" || n == ",") return KEY_COMMA;
    if (n == "period" || n == ".") return KEY_DOT;
    if (n == "slash" || n == "/") return KEY_SLASH;
    if (n == "backslash" || n == "\\") return KEY_BACKSLASH;
    if (n == "bracketleft" || n == "[") return KEY_LEFTBRACE;
    if (n == "bracketright" || n == "]") return KEY_RIGHTBRACE;
    if (n == "minus" || n == "-") return KEY_MINUS;
    if (n == "equal" || n == "=") return KEY_EQUAL;
    if (n.size() >= 2 && n[0] == 'f') {
        int fn = std::atoi(n.c_str() + 1);
        if (fn >= 1 && fn <= 12) return static_cast<unsigned int>(KEY_F1 + (fn - 1));
    }
    if (n == "return" || n == "enter") return KEY_ENTER;
    if (n.length() == 1 && n[0] >= 'a' && n[0] <= 'z') {
        // Must not use KEY_A + offset: evdev letter scancodes are not alphabetical order.
        switch (n[0]) {
        case 'a':
            return KEY_A;
        case 'b':
            return KEY_B;
        case 'c':
            return KEY_C;
        case 'd':
            return KEY_D;
        case 'e':
            return KEY_E;
        case 'f':
            return KEY_F;
        case 'g':
            return KEY_G;
        case 'h':
            return KEY_H;
        case 'i':
            return KEY_I;
        case 'j':
            return KEY_J;
        case 'k':
            return KEY_K;
        case 'l':
            return KEY_L;
        case 'm':
            return KEY_M;
        case 'n':
            return KEY_N;
        case 'o':
            return KEY_O;
        case 'p':
            return KEY_P;
        case 'q':
            return KEY_Q;
        case 'r':
            return KEY_R;
        case 's':
            return KEY_S;
        case 't':
            return KEY_T;
        case 'u':
            return KEY_U;
        case 'v':
            return KEY_V;
        case 'w':
            return KEY_W;
        case 'x':
            return KEY_X;
        case 'y':
            return KEY_Y;
        case 'z':
            return KEY_Z;
        default:
            return 0;
        }
    }
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

static bool isSuspiciousControlUtf8(const std::u32string& u32) {
    if (u32.size() != 1) return false;
    const char32_t c = u32[0];
    // Ctrl+letter artifacts are usually U+0001..U+001A. Exclude tab/newline.
    if (c == U'\t' || c == U'\n' || c == U'\r') return false;
    return c > 0 && c < 0x20;
}

static void clearTransientXkbMods(::xkb_state* xkbState, int layoutIndex) {
    if (!xkbState) return;
    const xkb_mod_mask_t locked = xkb_state_serialize_mods(xkbState, XKB_STATE_MODS_LOCKED);
    const xkb_layout_index_t lg = layoutIndex >= 0 ? static_cast<xkb_layout_index_t>(layoutIndex) : 0;
    xkb_state_update_mask(xkbState, 0, 0, locked, lg, lg, lg);
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
    // If daemon.ini / GUI produced an unparseable spec, fall back so hotkeys never silently die.
    if (hkSwapLast_.codes.empty()) (void)parseHotkey("Alt+apostrophe", hkSwapLast_);
    if (hkSwapSel_.codes.empty()) (void)parseHotkey("Alt+Shift+apostrophe", hkSwapSel_);
    if (hkToggle_.codes.empty()) (void)parseHotkey("Alt+Shift+a", hkToggle_);
    if (hkUndo_.codes.empty()) (void)parseHotkey("Alt+Shift+BackSpace", hkUndo_);
}

bool PuntoDaemon::parseHotkey(const std::string& spec, HotkeySpec& out) const {
    out.modMask = 0;
    out.codes.clear();
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
    const unsigned primary = keyNameToCode(parts.back());
    if (primary == 0) {
        std::cerr << "punto-switcher-daemon: hotkey \"" << spec << "\" — unknown key (check spelling)\n";
        return false;
    }
    auto addCode = [&](unsigned c) {
        if (c == 0) return;
        for (unsigned x : out.codes)
            if (x == c) return;
        out.codes.push_back(c);
    };
    addCode(primary);
    // Same OEM key often comes as KEY_APOSTROPHE or KEY_GRAVE depending on driver/firmware.
    if (primary == KEY_APOSTROPHE) addCode(KEY_GRAVE);
    else if (primary == KEY_GRAVE) addCode(KEY_APOSTROPHE);

    if (out.modMask == 0 && bareKeyNeedsModifiers(primary)) {
        std::cerr << "punto-switcher-daemon: hotkey \"" << spec
                  << "\" needs Alt/Ctrl/Shift/Super — bare key would block normal typing (ignored)\n";
        out.codes.clear();
        return false;
    }
    return true;
}

void PuntoDaemon::logResolvedHotkeys() const {
    auto dump = [](const char* label, const std::string& ini, const HotkeySpec& hk) {
        std::cerr << "punto-switcher-daemon: hotkey " << label << " ini=\"" << ini << "\" modMask=" << hk.modMask
                  << " evdev";
        if (hk.codes.empty()) {
            std::cerr << "=(INVALID — no key codes)\n";
            return;
        }
        for (unsigned c : hk.codes) {
            std::cerr << ' ' << c;
            const char* nm = libevdev_event_code_get_name(EV_KEY, c);
            if (nm && nm[0]) std::cerr << '(' << nm << ')';
        }
        std::cerr << '\n';
    };
    dump("swap_last", cfg_.swapLastSpec, hkSwapLast_);
    dump("swap_selection", cfg_.swapSelectionSpec, hkSwapSel_);
    dump("toggle_auto", cfg_.toggleAutoSpec, hkToggle_);
    dump("undo", cfg_.undoSpec, hkUndo_);
    std::cerr << std::flush;
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
    const bool alt   = ks[KEY_LEFTALT] || ks[KEY_RIGHTALT];
    const bool shift = ks[KEY_LEFTSHIFT] || ks[KEY_RIGHTSHIFT];
    const bool ctrl  = ks[KEY_LEFTCTRL] || ks[KEY_RIGHTCTRL];
    const bool super = ks[KEY_LEFTMETA] || ks[KEY_RIGHTMETA];

    // Only modifiers listed in the hotkey mask must match. Requiring *unlisted* modifiers to be
    // up (the old "!on" rule) breaks common cases: e.g. Alt+apostrophe with Shift still held after
    // typing a capital letter, or AltGr-style chords — swap_last would never fire.
    if (mask & 1u) {
        if (!alt) return false;
    }
    if (mask & 2u) {
        if (!shift) return false;
    }
    if (mask & 4u) {
        if (!ctrl) return false;
    }
    if (mask & 8u) {
        if (!super) return false;
    }
    return true;
}

bool PuntoDaemon::matchHotkey(const HotkeySpec& hk, unsigned int code, int value) const {
    if (value != 1) return false;
    if (hk.codes.empty()) return false;
    bool codeOk = false;
    for (unsigned c : hk.codes) {
        if (c == code) {
            codeOk = true;
            break;
        }
    }
    if (!codeOk) return false;
    const unsigned primary = hk.codes.front();
    // Defense in depth: never match typing keys without modifiers (bad daemon.ini / stale hk).
    if (hk.modMask == 0 && bareKeyNeedsModifiers(primary)) return false;
    return matchMods(hk.modMask);
}

int PuntoDaemon::syncXkbLayoutFromKde() {
    if (!xkbMap_ || !xkbState_) return -1;
    int li = layout_.currentLayoutIndex();
    // Right after resume, getLayout often fails once and currentLayoutIndex() enters a multi-second
    // backoff — without a mask update xkb stays on a stale group and autoswitch/heuristic break.
    if (li < 0 && lastKnownLayoutIndex_ >= 0) li = lastKnownLayoutIndex_;
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

void PuntoDaemon::seedCapsLockFromHardware() {
    if (!xkbMap_ || !xkbState_) return;
    if (!io_.capsLockLedOn()) return;

    const int li = syncXkbLayoutFromKde();
    if (li < 0) return;

    const xkb_mod_index_t lockIdx = xkb_keymap_mod_get_index(xkbMap_, XKB_MOD_NAME_CAPS);
    if (lockIdx == XKB_MOD_INVALID) return;

    xkb_mod_mask_t dep = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t lat = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t lck = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_LOCKED);
    lck |= (1u << lockIdx);
    const xkb_layout_index_t lg = static_cast<xkb_layout_index_t>(li);
    xkb_state_update_mask(xkbState_, dep, lat, lck, lg, lg, lg);
    capsLockOn_ = true;
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

void PuntoDaemon::releaseAllDownKeys() {
    if (keyState_.empty()) return;
    // Emit explicit releases via uinput for any key we think is still held.
    // This helps when suspend/resume or device re-grab causes a missing key-up.
    for (size_t code = 0; code < keyState_.size(); ++code) {
        if (keyState_[code] == 0) continue;
        io_.emitKey(static_cast<unsigned int>(code), 0);
    }
    io_.sync();
    keyState_.fill(0);
    swallowed_.clear();
}

bool PuntoDaemon::reinitKeyboardDevices(const char* logReason) {
    std::cerr << "punto-switcher-daemon: " << logReason << "\n" << std::flush;
    releaseAllDownKeys();
    layout_.resetLayoutPollState();
    io_.shutdown();
    tracker_.reset();
    keyState_.fill(0);
    swallowed_.clear();
    pendingLayoutRefresh_        = false;
    onlyModifiersSinceNormalKey_ = false;
    pendingUrlSecondSlash_       = false;
    capsLockOn_                  = false;
    daemonSwitchedLayout_        = false;
    enterRepeatCount_            = 0;
    letterRepeatCount_           = 0;
    letterRepeatCode_            = 0;
    if (!io_.init(cfg_.devicePath)) {
        std::cerr << "punto-switcher-daemon: keyboard reinit failed (io_.init)\n" << std::flush;
        return false;
    }
    seedCapsLockFromHardware();
    lastKnownLayoutIndex_ = syncXkbLayoutFromKde();
    return true;
}

void PuntoDaemon::recoverStuckModifiersFromXkb() {
    if (!xkbState_) return;
    const bool shiftActive =
        xkb_state_mod_name_is_active(xkbState_, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) != 0;
    const bool ctrlActive =
        xkb_state_mod_name_is_active(xkbState_, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) != 0;
    const bool logoActive =
        xkb_state_mod_name_is_active(xkbState_, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) != 0;

    bool emitted = false;
    auto healPair = [&](unsigned int left, unsigned int right, bool activeNow) {
        if (activeNow) return;
        if (keyState_[left]) {
            keyState_[left] = 0;
            io_.emitKey(left, 0);
            emitted = true;
        }
        if (keyState_[right]) {
            keyState_[right] = 0;
            io_.emitKey(right, 0);
            emitted = true;
        }
    };

    healPair(KEY_LEFTSHIFT, KEY_RIGHTSHIFT, shiftActive);
    healPair(KEY_LEFTCTRL, KEY_RIGHTCTRL, ctrlActive);
    // Do not heal Alt from xkb vs evdev: with Russian layout group, effective Alt in xkb can
    // disagree with physical KEY_LEFTALT/KEY_RIGHTALT while Alt+hotkey is held, causing bogus
    // synthetic releases and breaking Alt+' / Alt+Shift+ chords.
    healPair(KEY_LEFTMETA, KEY_RIGHTMETA, logoActive);
    if (emitted) io_.sync();
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

    // Leading "," or "." typed in EN layout + RU letters (wrong layout on first key only).
    const std::u32string jcukenFixed = CharMapping::normalizeJcukenMixedPrefix(w32);
    if (jcukenFixed != w32) {
        const std::string fixedUtf8 = utf32_to_utf8(jcukenFixed);
        const int       wordCpCount   = static_cast<int>(w32.size());
        tracker_.undoPrevLayout       = layout;
        tracker_.undoOriginalTextUtf8 = word;
        tracker_.undoSwappedTextUtf8  = fixedUtf8;
        tracker_.undoSwappedCpCount   = static_cast<int>(jcukenFixed.size());
        InternalGuard g(&tracker_);
        tapBackspaces(wordCpCount);
        if (!injectText(fixedUtf8)) {
            std::cerr << "punto-switcher-daemon: inject failed (JCUKEN mixed-prefix fix)\n";
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
        tracker_.wordBuffer = fixedUtf8;
        layout_.setRussian();
        daemonSwitchedLayout_ = true;
        return false;
    }

    // Hard force path for mistyped URL scheme: "реезыЖ." -> "https:/", then next '.' -> '/'.
    // Keep this path independent of heuristic scores so URL typing is deterministic.
    {
        std::string inv = utf32_to_utf8(CharMapping::invertLayoutWord(w32));
        std::string invLower = toLowerAsciiCopy(inv);
        if (layout == CharMapping::Layout::Russian
            && boundaryUtf8 == "."
            && (invLower == "http:" || invLower == "https:" || invLower == "ftp:")) {
            const int wordCpCount = static_cast<int>(w32.size());
            pendingUrlSecondSlash_ = true;
            tracker_.undoPrevLayout       = layout;
            tracker_.undoOriginalTextUtf8 = word;
            tracker_.undoSwappedTextUtf8  = inv + "/";
            tracker_.undoSwappedCpCount   = utf8_to_utf32(inv + "/").size();
            InternalGuard g(&tracker_);
            tapBackspaces(wordCpCount);
            if (!injectText(inv + "/")) return false;
            tracker_.freezeCurrentToken();
            tracker_.wordBuffer = inv + "/";
            layout_.setEnglish();
            daemonSwitchedLayout_ = true;
            return true;
        }
    }

    // Mirror AutoSwitchHeuristic "plausible dominant script" guard here so we never run
    // backspace+inject when the word already fits the active language model — even if
    // layout_.currentLayoutIndex() is unknown (no DBus) or shouldSwitch regresses. Stops
    // "привет deleted then pasted привет" when inject(swapped) fails and restore(original) runs.
    const double ruRatio = CharMapping::russianLetterRatio(w32);
    const double enRatio = CharMapping::englishLetterRatio(w32);
    if (layout == CharMapping::Layout::Russian && ruRatio >= 0.85 && enRatio <= 0.05) {
        const double scO = AutoSwitchHeuristic::bigramScore(w32, CharMapping::Layout::Russian);
        const double scS = AutoSwitchHeuristic::bigramScore(
            CharMapping::invertLayoutWord(w32), CharMapping::Layout::English);
        if (scO >= cfg_.minPlausibleDominantBigramScore
            && scS <= scO + cfg_.confidenceThreshold) {
            if (puntoDebugEnabled()) {
                std::cerr << "punto-switcher-daemon: checkAutoSwitch skip (plausible RU word)\n"
                          << std::flush;
            }
            return false;
        }
    }
    if (layout == CharMapping::Layout::English && enRatio >= 0.85 && ruRatio <= 0.05) {
        if (!AutoSwitchHeuristic::isEnLayoutJcukenLeadPunctWord(w32)) {
            const double scO = AutoSwitchHeuristic::bigramScore(w32, CharMapping::Layout::English);
            const double scS = AutoSwitchHeuristic::bigramScore(
                CharMapping::invertLayoutWord(w32), CharMapping::Layout::Russian);
            if (scO >= cfg_.minPlausibleDominantBigramScore
                && scS <= scO + cfg_.confidenceThreshold) {
                if (puntoDebugEnabled()) {
                    std::cerr << "punto-switcher-daemon: checkAutoSwitch skip (plausible EN word)\n"
                              << std::flush;
                }
                return false;
            }
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
    const std::string swappedLower = toLowerAsciiCopy(swapped);
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

    // Track last non-release key event to implement silence-based reacquire logic.
    if (val != 0) lastInputEventAt_ = std::chrono::steady_clock::now();

    if (puntoDebugEnabled() && (val == 1 || val == 2)) {
        std::fprintf(stderr, "punto-switcher-daemon: EV_KEY code=%u value=%d\n", code, val);
        std::fflush(stderr);
    }

    if (code >= keyState_.size()) {
        io_.forward(ev);
        return;
    }

    if (cfg_.passthroughOnly) {
        io_.forward(ev);
        return;
    }

    const auto isConfiguredHotkeyTriggerCode = [&](unsigned int c) {
        auto match = [c](const HotkeySpec& hk) {
            for (unsigned x : hk.codes) {
                if (x == c) return true;
            }
            return false;
        };
        return match(hkSwapLast_) || match(hkSwapSel_) || match(hkToggle_) || match(hkUndo_);
    };

    // Swallow paired release only for actual configured hotkey trigger keys.
    // If a non-hotkey code accidentally appears in swallowed_ (stale state), fail open.
    if (val == 0 && swallowed_.count(code)) {
        swallowed_.erase(code);
        keyState_[code] = 0;
        if (isConfiguredHotkeyTriggerCode(code)) return;
        io_.forward(ev);
        return;
    }

    // Update key state before hotkey match (press path)
    if (val == 1 || val == 2) keyState_[code] = 1;
    else if (val == 0)
        keyState_[code] = 0;

    if (val == 1) {
        // Only swallow the trigger key when the action actually ran. Otherwise the press was
        // dropped and the user sees "dead" Alt+' / Alt+Shift+' / undo (e.g. swap blocked, empty
        // buffer, wl-paste empty).
        // More specific chords first (Alt+Shift+…) so they win over Alt-only when using matchMods
        // that ignores extra modifiers not listed in each hotkey mask.
        bool hotkeyConsumed = false;
        if (matchHotkey(hkSwapSel_, code, val)) {
            if (doSwapSelection()) {
                if (isConfiguredHotkeyTriggerCode(code)) swallowed_.insert(code);
                hotkeyConsumed = true;
            }
        } else if (matchHotkey(hkToggle_, code, val)) {
            if (isConfiguredHotkeyTriggerCode(code)) swallowed_.insert(code);
            doToggleAutoSwitch();
            hotkeyConsumed = true;
        } else if (matchHotkey(hkUndo_, code, val)) {
            if (doUndoLast()) {
                if (isConfiguredHotkeyTriggerCode(code)) swallowed_.insert(code);
                hotkeyConsumed = true;
            }
        } else if (matchHotkey(hkSwapLast_, code, val)) {
            if (doSwapLastWord()) {
                if (isConfiguredHotkeyTriggerCode(code)) swallowed_.insert(code);
                hotkeyConsumed = true;
            }
        }
        if (hotkeyConsumed) return;
    }

    // Kernel autorepeat (val==2): forward only. Running syncXkbLayoutFromKde + tracker +
    // autoswitch on every repeat tick corrupts xkb/wordBuffer and can produce runaway
    // repeated letters or broken typing in password fields.
    // Do not call xkb_state_update_key(KEY_DOWN) here: the key is already down from val==1.
    // Re-applying KEY_DOWN on every repeat tick can skew xkb state and cause wrong/extra chars.
    if (val == 2) {
        if (code == KEY_ENTER) {
            const auto now = std::chrono::steady_clock::now();
            if (enterRepeatCount_ == 0
                || (now - enterRepeatWindowStart_) > std::chrono::milliseconds(1200)) {
                enterRepeatWindowStart_ = now;
                enterRepeatCount_ = 1;
            } else {
                ++enterRepeatCount_;
            }
            // Stuck-key guard: normal intentional hold is short; a missing key-up
            // causes endless Enter repeats (new lines). Stop it proactively.
            if (enterRepeatCount_ > 30) {
                if (puntoDebugEnabled()) {
                    std::cerr << "punto-switcher-daemon: enter repeat storm detected, forcing key-up\n"
                              << std::flush;
                }
                keyState_[KEY_ENTER] = 0;
                io_.emitKey(KEY_ENTER, 0);
                enterRepeatCount_ = 0;
                return;
            }
        } else {
            enterRepeatCount_ = 0;
            if (isLatinLetterEvdevKey(code)) {
                const auto nowL = std::chrono::steady_clock::now();
                if (letterRepeatCount_ == 0 || letterRepeatCode_ != code
                    || (nowL - letterRepeatWindowStart_) > std::chrono::milliseconds(1500)) {
                    letterRepeatWindowStart_ = nowL;
                    letterRepeatCode_        = code;
                    letterRepeatCount_       = 1;
                } else {
                    ++letterRepeatCount_;
                }
                if (letterRepeatCount_ > 45) {
                    if (puntoDebugEnabled()) {
                        std::cerr << "punto-switcher-daemon: letter repeat storm, forcing key-up code="
                                  << code << "\n"
                                  << std::flush;
                    }
                    if (code < keyState_.size()) keyState_[code] = 0;
                    io_.emitKey(code, 0);
                    letterRepeatCount_ = 0;
                    return;
                }
            } else {
                letterRepeatCount_ = 0;
            }
        }
        if (code >= keyState_.size()) {
            io_.forward(ev);
            return;
        }
        io_.forward(ev);
        return;
    }
    if (code == KEY_ENTER && val == 0) enterRepeatCount_ = 0;
    if (val == 0 && isLatinLetterEvdevKey(code) && letterRepeatCode_ == code) letterRepeatCount_ = 0;

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
    if (xkbState_) {
        capsLockOn_ =
            xkb_state_mod_name_is_active(xkbState_, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) != 0;
    }

    if (val == 1) {
        const bool isModKeyPress = (code == KEY_LEFTCTRL  || code == KEY_RIGHTCTRL  ||
                                    code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT ||
                                    code == KEY_LEFTALT   || code == KEY_RIGHTALT   ||
                                    code == KEY_LEFTMETA  || code == KEY_RIGHTMETA);
        if (!isModKeyPress) recoverStuckModifiersFromXkb();
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
        if (isSuspiciousControlUtf8(u32)) {
            // Self-heal: xkb can keep Ctrl depressed after shortcut storms (paste + undo),
            // producing control bytes for normal letters. Clear transient mods and retry.
            clearTransientXkbMods(xkbState_, curLayout);
            n = xkb_state_key_get_utf8(xkbState_, code + 8, buf, sizeof(buf));
            if (n > 0) {
                utf8 = std::string(buf, static_cast<size_t>(n));
                u32 = utf8_to_utf32(utf8);
            }
        }
        char32_t ch = u32[0];

        // Latin ↔ Cyrillic without a typed space: finish the previous word so we never
        // merge "hello" and "привет" into one token (garbled autoswitch / Alt+' targets).
        // Do not run checkAutoSwitch here: short tokens (e.g. "e") + inject/backspace caused
        // repeated inserts; autoswitch still runs on real boundaries (space, punct, Enter).
        if (WordSwapper::shouldSplitScriptBeforeAddChar(tracker_.wordBuffer, ch)) {
            tracker_.onBoundary(std::string());
        }

        bool prevIsLetter = false;
        if (!tracker_.wordBuffer.empty()) {
            std::u32string wb32 = utf8_to_utf32(tracker_.wordBuffer);
            if (!wb32.empty()) {
                const char32_t prev = wb32.back();
                prevIsLetter = CharMapping::isEnglish(prev) || CharMapping::isRussian(prev);
            }
        }
        bool forceDotBoundaryForUrlPrefix = false;
        if (!pendingUrlSecondSlash_ && ch == U'.' && !tracker_.wordBuffer.empty()) {
            std::string invPrefix = utf32_to_utf8(CharMapping::invertLayoutWord(utf8_to_utf32(tracker_.wordBuffer)));
            invPrefix = toLowerAsciiCopy(invPrefix);
            forceDotBoundaryForUrlPrefix =
                (invPrefix == "http:" || invPrefix == "https:" || invPrefix == "ftp:");
        }
        if (WordSwapper::isWordBoundaryForTracker(ch, tracker_.wordBuffer.empty(),
                                                  prevIsLetter,
                                                  pendingUrlSecondSlash_ || forceDotBoundaryForUrlPrefix)) {
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
            // Safety rule: never swallow plain space. Even if autoswitch consumes a boundary
            // in some edge path, users must always get a physical space in apps.
            if (utf8 == " ") consumeBoundary = false;
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
    gStopRequested = 0;
    (void)std::signal(SIGTERM, handleStopSignal);
    (void)std::signal(SIGINT, handleStopSignal);

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

    seedCapsLockFromHardware();

    logResolvedHotkeys();

    std::cerr << "punto-switcher-daemon: running (grabbed " << io_.devices().size()
              << " keyboard(s)). Stop with Ctrl+C.\n";
    if (cfg_.passthroughOnly) {
        std::cerr << "punto-switcher-daemon: passthrough mode — swap/autoswitch disabled; keys forwarded only\n";
    }
    if (puntoDebugEnabled()) {
        std::cerr << "punto-switcher-daemon: debug on — EV_KEY lines + utf8; or run: punto-switcher-daemon --debug\n"
                  << std::flush;
    }
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        std::cerr << "punto-switcher-daemon: warning: DBUS_SESSION_BUS_ADDRESS unset — KDE layout sync/switch "
                      "will not work. With sg(1), export it from your session before sg (see loginctl show-session).\n";
    }

    auto lastInitAt          = std::chrono::steady_clock::now();
    bool didSilenceReinit    = false;
    auto lastTopologyProbe = std::chrono::steady_clock::now();
    double lastBootSeconds   = monotonicBootSeconds();
    int topologyMismatchStreak = 0;
    int consecutiveNoDeviceInits = 0;
    while (!gStopRequested) {
        const auto& devs = io_.devices();
        if (devs.empty()) {
            std::cerr << "punto-switcher-daemon: no input devices opened, trying to reinitialize...\n";
            ::usleep(500000);
            if (!io_.init(cfg_.devicePath)) {
                ++consecutiveNoDeviceInits;
                // Let systemd restart from a clean process after prolonged no-device state.
                if (consecutiveNoDeviceInits >= 20) {
                    std::cerr << "punto-switcher-daemon: could not open any keyboard for 10s, exiting for systemd restart\n";
                    teardownXkb();
                    return 1;
                }
                continue;
            }
            consecutiveNoDeviceInits = 0;
            lastInitAt = std::chrono::steady_clock::now();
            didSilenceReinit = false;
            topologyMismatchStreak = 0;
            lastTopologyProbe = lastInitAt;
            seedCapsLockFromHardware();
        } else {
            consecutiveNoDeviceInits = 0;
        }
        // Resume detection must run before poll: after wake, the first key often arrives
        // immediately (poll returns >0), so we never hit the 2s poll timeout path where
        // jump was previously checked — reinit never ran until 2s of idle.
        const double bootNow = monotonicBootSeconds();
        if (bootNow > 0.0 && lastBootSeconds > 0.0) {
            const double jump = bootNow - lastBootSeconds;
            if (jump > 8.0) {
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                              "resume detected (boottime jump %.2fs), reinitializing devices", jump);
                if (!reinitKeyboardDevices(msg)) {
                    teardownXkb();
                    return 1;
                }
                lastBootSeconds = monotonicBootSeconds();
                const auto nowResume = std::chrono::steady_clock::now();
                lastInitAt               = nowResume;
                lastTopologyProbe        = nowResume;
                didSilenceReinit         = false;
                topologyMismatchStreak   = 0;
                consecutiveNoDeviceInits = 0;
                continue;
            }
        }
        std::vector<struct pollfd> pfds(devs.size());
        bool needReinitAfterDeviceLoss = false;
        for (size_t i = 0; i < devs.size(); ++i) {
            pfds[i].fd     = libevdev_get_fd(devs[i]);
            pfds[i].events = POLLIN;
        }
        const double bootBeforePoll = monotonicBootSeconds();
        const int    pollRes        = poll(pfds.data(), pfds.size(), 2000);
        const double bootAfterPoll  = monotonicBootSeconds();
        // Suspend may happen while blocked in poll; CLOCK_BOOTTIME then advances by the sleep
        // duration, but the pre-poll jump check never ran — refresh lastBootSeconds without reinit.
        if (bootAfterPoll - bootBeforePoll > 8.0) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "resume detected (boottime gap %.2fs during poll), reinitializing devices",
                          bootAfterPoll - bootBeforePoll);
            if (!reinitKeyboardDevices(msg)) {
                teardownXkb();
                return 1;
            }
            lastBootSeconds = monotonicBootSeconds();
            const auto nowResume = std::chrono::steady_clock::now();
            lastInitAt               = nowResume;
            lastTopologyProbe        = nowResume;
            didSilenceReinit         = false;
            topologyMismatchStreak   = 0;
            consecutiveNoDeviceInits = 0;
            continue;
        }
        if (pollRes <= 0) {
            lastBootSeconds = bootAfterPoll;

            const auto now = std::chrono::steady_clock::now();

            // After suspend/resume or reconnect, a Bluetooth keyboard may appear
            // 10-15s later. If we are in a "silent" period after init/start,
            // do a single in-process reopen to pick up newly appeared devices.
            if (!didSilenceReinit
                && (now - lastInitAt) > std::chrono::seconds(12)
                && (now - lastInputEventAt_) > std::chrono::seconds(12)) {
                if (!reinitKeyboardDevices("input silence after init, reinitializing devices")) {
                    teardownXkb();
                    return 1;
                }
                lastInitAt = now;
                didSilenceReinit = true;
                topologyMismatchStreak = 0;
                lastTopologyProbe = now;
            }

            if ((now - lastTopologyProbe) > std::chrono::seconds(3)) {
                lastTopologyProbe = now;
                if (io_.hasDeviceSetChanged(cfg_.devicePath)) {
                    ++topologyMismatchStreak;
                    if (topologyMismatchStreak >= 2) {
                        if (!reinitKeyboardDevices("keyboard topology changed stably, reinitializing devices")) {
                            teardownXkb();
                            return 1;
                        }
                        topologyMismatchStreak = 0;
                        didSilenceReinit = false;
                        lastInitAt = now;
                    }
                } else {
                    topologyMismatchStreak = 0;
                }
            }
            continue;
        }

        for (size_t i = 0; i < devs.size(); ++i) {
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::cerr << "punto-switcher-daemon: poll reports device error/hup (revents="
                          << pfds[i].revents << "), exiting for systemd restart\n";
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
                                  << "), exiting for systemd restart\n";
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
            std::cerr << "punto-switcher-daemon: input device lost or hup, reinitializing\n" << std::flush;
            if (!reinitKeyboardDevices("input device lost or error, reinitializing")) {
                releaseAllDownKeys();
                teardownXkb();
                return 1;
            }
            lastBootSeconds          = monotonicBootSeconds();
            lastInitAt               = std::chrono::steady_clock::now();
            lastTopologyProbe        = lastInitAt;
            didSilenceReinit         = false;
            topologyMismatchStreak   = 0;
            consecutiveNoDeviceInits = 0;
            continue;
        }
        lastBootSeconds = monotonicBootSeconds();
    }

    // Graceful stop path (systemctl stop / SIGTERM): release any held keys first
    // so Enter/Shift/etc. never remain logically pressed after daemon shutdown.
    releaseAllDownKeys();
    io_.shutdown();
    teardownXkb();
    return 0;
}

} // namespace punto
