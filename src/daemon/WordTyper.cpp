#include "PuntoDaemon.h"

#include "../core/CharMapping.h"
#include "../core/TypoCorrector.h"
#include "../core/Utf8Utils.h"
#include "../core/WordDictionary.h"
#include "../core/WordSwapper.h"
#include "SessionEnv.h"

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace punto {

namespace {

bool debugOn() {
    const char* p = std::getenv("PUNTO_DEBUG");
    return p && p[0] != '\0' && std::strcmp(p, "0") != 0;
}

bool oneCp(const std::string& utf8, char32_t& out) {
    const std::u32string u = utf8_to_utf32(utf8);
    if (u.size() != 1 || u[0] < 0x20) return false;
    out = u[0];
    return true;
}

bool isLetterCp(char32_t c) {
    return CharMapping::isRussian(c) || CharMapping::isEnglish(c);
}

} // namespace

Keystroke PuntoDaemon::makeKeystroke(unsigned int code) const {
    Keystroke ks;
    ks.code  = code;
    ks.shift = keyState_[KEY_LEFTSHIFT] || keyState_[KEY_RIGHTSHIFT];
    ks.caps  = capsLockOn_;
    return ks;
}

PuntoDaemon::KeyRole PuntoDaemon::classifyKey(const Keystroke& ks, int layout) const {
    switch (ks.code) {
    case KEY_SPACE:
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_TAB:
        return KeyRole::Boundary;
    default:
        break;
    }
    // Same physical key, two groups (xkb ru winkeys + us):
    //   comma/dot/semicolon/apostrophe/brackets/grave → б ю ж э х ъ ё (and the shifted pair)
    //   slash → "." / "/" , shift+slash → "," / "?"
    //   shift+digit → " № ; : ? versus @ # $ ^ &
    // A key is part of the word when either group produces a letter (xneur stores
    // the keysym of the active group; a letter is never a delimiter). Punctuation
    // in both groups ends the word. The separator is then replayed in the new group.
    char32_t cur = 0;
    if (!oneCp(keys_.interpret(ks, layout), cur)) return KeyRole::Ignore;
    const int other = otherLayoutIndex(layout);
    char32_t alt = 0;
    const bool haveAlt = other >= 0 && oneCp(keys_.interpret(ks, other), alt);

    if (WordSwapper::isWordBoundary(cur)) {
        if (haveAlt && isLetterCp(alt)) return KeyRole::Word;
        return KeyRole::Boundary;
    }
    // "руддщ" is already "hello". The next key is "ю" here and "." in English,
    // so it ends the word and is retyped as a period. "работа" + "ю" stays one
    // word: the stem is Russian and does not switch.
    if (!wordFrozen_ && cfg_.autoSwitchEnabled && !wordKeys_.empty() && isLetterCp(cur)
        && haveAlt && WordSwapper::isWordBoundary(alt)) {
        const std::string typed = keys_.interpret(wordKeys_, layout);
        const std::string swapped = keys_.interpret(wordKeys_, other);
        if (!typed.empty() && heuristic_.shouldSwitch(typed, layoutEnum(layout), swapped))
            return KeyRole::Boundary;
    }
    return KeyRole::Word;
}

int PuntoDaemon::otherLayoutIndex(int layout) const {
    if (layout == cfg_.layoutIndexEnglish) return cfg_.layoutIndexRussian;
    if (layout == cfg_.layoutIndexRussian) return cfg_.layoutIndexEnglish;
    return -1;
}

CharMapping::Layout PuntoDaemon::layoutEnum(int layout) const {
    if (layout == cfg_.layoutIndexEnglish) return CharMapping::Layout::English;
    if (layout == cfg_.layoutIndexRussian) return CharMapping::Layout::Russian;
    return CharMapping::Layout::Unknown;
}

void PuntoDaemon::noteLayout(int layoutIndex) {
    if (layoutIndex < 0) return;
    if (lastKnownLayoutIndex_ >= 0 && layoutIndex != lastKnownLayoutIndex_ && !daemonSwitchedLayout_) {
        if (debugOn()) {
            std::cerr << "rocket-switcher-daemon: layout changed " << lastKnownLayoutIndex_ << " → "
                      << layoutIndex << ", word reset\n";
        }
        forgetTyping();
    }
    daemonSwitchedLayout_ = false;
    lastKnownLayoutIndex_ = layoutIndex;
}

void PuntoDaemon::forgetTyping() {
    wordKeys_.clear();
    lastWordKeys_.clear();
    wordLayout_       = -1;
    lastWordLayout_   = -1;
    lastBoundaryUtf8_.clear();
    lastBoundaryValid_ = false;
    wordFrozen_       = false;
    tracker_.reset(/*clearUndo=*/false);
}

void PuntoDaemon::noteWordKey(const Keystroke& ks) {
    if (!wordKeys_.empty() && wordLayout_ >= 0 && lastKnownLayoutIndex_ >= 0
        && lastKnownLayoutIndex_ != wordLayout_) {
        wordKeys_.clear();
        wordLayout_ = lastKnownLayoutIndex_;
    }
    if (wordLayout_ < 0) wordLayout_ = lastKnownLayoutIndex_;
    wordKeys_.push_back(ks);
    wordFrozen_ = false;
}

void PuntoDaemon::noteBackspace() {
    if (!wordKeys_.empty()) {
        wordKeys_.pop_back();
        if (wordKeys_.empty()) wordLayout_ = -1;
        return;
    }
    lastWordKeys_.clear();
    lastBoundaryValid_ = false;
    lastBoundaryUtf8_.clear();
}

void PuntoDaemon::noteRepeat(unsigned int code) {
    const bool ctrl  = keyState_[KEY_LEFTCTRL] || keyState_[KEY_RIGHTCTRL];
    const bool alt   = keyState_[KEY_LEFTALT] || keyState_[KEY_RIGHTALT];
    const bool super = keyState_[KEY_LEFTMETA] || keyState_[KEY_RIGHTMETA];
    if (ctrl || alt || super) return;
    if (code == KEY_BACKSPACE) {
        noteBackspace();
        return;
    }
    const int lay = wordLayout_ >= 0 ? wordLayout_ : lastKnownLayoutIndex_;
    if (lay < 0) return;
    const Keystroke ks = makeKeystroke(code);
    if (classifyKey(ks, lay) != KeyRole::Word) return;
    noteWordKey(ks);
}

void PuntoDaemon::releaseModifiersForInject() {
    const unsigned mods[] = {KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
                             KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTMETA, KEY_RIGHTMETA};
    bool any = false;
    for (unsigned m : mods) {
        if (!keyState_[m]) continue;
        io_.emitKey(m, 0);
        if (xkbState_) xkb_state_update_key(xkbState_, m + 8, XKB_KEY_UP);
        any = true;
    }
    if (any) io_.sync();
}

void PuntoDaemon::restoreHeldModifiers() {
    const unsigned mods[] = {KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
                             KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTMETA, KEY_RIGHTMETA};
    bool any = false;
    for (unsigned m : mods) {
        if (!keyState_[m]) continue;
        io_.emitKey(m, 1);
        if (xkbState_) xkb_state_update_key(xkbState_, m + 8, XKB_KEY_DOWN);
        any = true;
    }
    if (any) io_.sync();
}

void PuntoDaemon::replayKeystrokes(const std::vector<Keystroke>& keys) {
    for (const Keystroke& ks : keys) {
        if (ks.shift) io_.emitKey(KEY_LEFTSHIFT, 1);
        io_.emitKey(ks.code, 1);
        io_.emitKey(ks.code, 0);
        if (ks.shift) io_.emitKey(KEY_LEFTSHIFT, 0);
    }
    io_.sync();
}

int PuntoDaemon::layoutForCodepoint(char32_t c) const {
    if (c == U'\n' || c == U'\r' || c == U'\t')
        return lastKnownLayoutIndex_ >= 0 ? lastKnownLayoutIndex_ : cfg_.layoutIndexEnglish;
    auto has = [&](int lay) {
        return lay >= 0 && keys_.bindingFor(c, lay).has_value();
    };
    if (has(lastKnownLayoutIndex_)) return lastKnownLayoutIndex_;
    if (has(cfg_.layoutIndexEnglish)) return cfg_.layoutIndexEnglish;
    if (has(cfg_.layoutIndexRussian)) return cfg_.layoutIndexRussian;
    return -1;
}

bool PuntoDaemon::canTypeText(const std::string& utf8) const {
    const std::u32string u32 = utf8_to_utf32(utf8);
    if (u32.empty()) return false;
    for (char32_t c : u32)
        if (layoutForCodepoint(c) < 0) return false;
    return true;
}

bool PuntoDaemon::typeText(const std::string& utf8) {
    const std::u32string u32 = utf8_to_utf32(utf8);
    if (u32.empty()) return false;
    for (char32_t c : u32)
        if (layoutForCodepoint(c) < 0) return false;
    for (char32_t c : u32) {
        if (c == U'\n' || c == U'\r') {
            io_.emitKey(KEY_ENTER, 1);
            io_.emitKey(KEY_ENTER, 0);
            continue;
        }
        if (c == U'\t') {
            io_.emitKey(KEY_TAB, 1);
            io_.emitKey(KEY_TAB, 0);
            continue;
        }
        const int lay = layoutForCodepoint(c);
        if (lay != lastKnownLayoutIndex_ && !switchToLayout(lay)) return false;
        const auto binding = keys_.bindingFor(c, lay);
        if (!binding) return false;
        // A phrase typed in one burst loses the middle: the field is still
        // applying the previous key and drops the next ones.
        if (binding->shift) {
            io_.emitKey(KEY_LEFTSHIFT, 1);
            ::usleep(3000);
        }
        io_.emitKey(binding->code, 1);
        ::usleep(3000);
        io_.emitKey(binding->code, 0);
        if (binding->shift) io_.emitKey(KEY_LEFTSHIFT, 0);
        ::usleep(8000);
    }
    return true;
}

bool PuntoDaemon::emitUtf8InLayout(const std::string& utf8, int layout) {
    const std::u32string u32 = utf8_to_utf32(utf8);
    if (u32.empty()) return true;
    for (char32_t c : u32) {
        const auto binding = keys_.bindingFor(c, layout);
        if (!binding) {
            if (debugOn()) {
                std::cerr << "rocket-switcher-daemon: no key for U+" << std::hex << static_cast<unsigned>(c)
                          << std::dec << " in layout " << layout << "\n";
            }
            return false;
        }
        if (binding->shift) io_.emitKey(KEY_LEFTSHIFT, 1);
        io_.emitKey(binding->code, 1);
        io_.emitKey(binding->code, 0);
        if (binding->shift) io_.emitKey(KEY_LEFTSHIFT, 0);
    }
    io_.sync();
    return true;
}

bool PuntoDaemon::switchToLayout(int layoutIndex) {
    if (layoutIndex < 0) return false;
    if (lastKnownLayoutIndex_ == layoutIndex) return true;
    daemonSwitchedLayout_ = true;
    if (layoutIndex == cfg_.layoutIndexEnglish) layout_.setEnglish();
    else if (layoutIndex == cfg_.layoutIndexRussian) layout_.setRussian();
    else {
        daemonSwitchedLayout_ = false;
        return false;
    }
    if (!layout_.waitForLayoutIndex(layoutIndex, 120)) {
        std::cerr << "rocket-switcher-daemon: layout " << layoutIndex << " was not confirmed\n";
        daemonSwitchedLayout_ = false;
        return false;
    }
    lastKnownLayoutIndex_ = layoutIndex;
    if (xkbState_ && xkbMap_) {
        const xkb_mod_mask_t dep = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_DEPRESSED);
        const xkb_mod_mask_t lat = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_LATCHED);
        const xkb_mod_mask_t lck = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_LOCKED);
        const xkb_layout_index_t lg = static_cast<xkb_layout_index_t>(layoutIndex);
        xkb_state_update_mask(xkbState_, dep, lat, lck, lg, lg, lg);
    }
    return true;
}

bool PuntoDaemon::convertKeys(const std::vector<Keystroke>& keys, int fromLayout, int toLayout,
                              const std::string& boundaryUtf8, bool boundaryOnScreen) {
    if (keys.empty() || fromLayout < 0 || toLayout < 0) return false;
    const std::string typed = keys_.interpret(keys, fromLayout);
    const std::string swapped = keys_.interpret(keys, toLayout);
    if (typed.empty() || swapped.empty() || swapped == typed) return false;

    if (!switchToLayout(toLayout)) return false;

    const bool haveBoundary = !boundaryUtf8.empty();
    const int boundaryChars = haveBoundary ? static_cast<int>(utf8_to_utf32(boundaryUtf8).size()) : 0;
    const int deleteCount   = static_cast<int>(utf8_to_utf32(typed).size())
                            + (boundaryOnScreen ? boundaryChars : 0);

    releaseModifiersForInject();
    tapBackspaces(deleteCount);
    replayKeystrokes(keys);
    if (haveBoundary && !emitUtf8InLayout(boundaryUtf8, toLayout)) {
        std::cerr << "rocket-switcher-daemon: could not retype word separator\n";
    }
    restoreHeldModifiers();

    typingUndo_.valid         = true;
    typingUndo_.selection     = false;
    typingUndo_.textReplace   = false;
    typingUndo_.keys          = keys;
    typingUndo_.layoutIndex   = fromLayout;
    typingUndo_.boundaryUtf8  = boundaryUtf8;
    typingUndo_.originalText  = typed;
    typingUndo_.deleteCpCount = static_cast<int>(utf8_to_utf32(swapped).size())
                              + (haveBoundary ? static_cast<int>(utf8_to_utf32(boundaryUtf8).size()) : 0);
    return true;
}

void PuntoDaemon::commitWord(const Keystroke& boundary) {
    const int lay = wordLayout_ >= 0 ? wordLayout_ : lastKnownLayoutIndex_;
    lastWordKeys_   = wordKeys_;
    lastWordLayout_ = lay;
    lastBoundaryUtf8_ = (lay >= 0) ? keys_.interpret(boundary, lay) : std::string();
    if (lastBoundaryUtf8_.empty()) {
        if (boundary.code == KEY_SPACE) lastBoundaryUtf8_ = " ";
        else if (boundary.code == KEY_ENTER || boundary.code == KEY_KPENTER) lastBoundaryUtf8_ = "\n";
        else if (boundary.code == KEY_TAB) lastBoundaryUtf8_ = "\t";
    }
    lastBoundaryValid_ = !lastBoundaryUtf8_.empty();
    wordKeys_.clear();
    wordLayout_ = -1;
    wordFrozen_ = false;
}

bool PuntoDaemon::onWordBoundary(const Keystroke& boundary) {
    if (wordKeys_.empty()) return false;
    const int lay = wordLayout_ >= 0 ? wordLayout_ : lastKnownLayoutIndex_;
    const int other = otherLayoutIndex(lay);
    const CharMapping::Layout layEnum = layoutEnum(lay);
    const std::string typed = (lay >= 0) ? keys_.interpret(wordKeys_, lay) : std::string();

    auto finish = [&]() {
        commitWord(boundary);
        return false;
    };

    const bool canSwitch = !wordFrozen_ && cfg_.autoSwitchEnabled && lay >= 0 && other >= 0
                           && layEnum != CharMapping::Layout::Unknown && !typed.empty();
    if (!canSwitch) {
        if (!wordFrozen_ && cfg_.typoCorrectEnabled && lay >= 0 && !typed.empty()
            && applyTypoCorrection(boundary, typed, lay)) {
            return false;
        }
        return finish();
    }

    const std::string otherText = keys_.interpret(wordKeys_, other);
    if (otherText.empty() || otherText == typed
        || !heuristic_.shouldSwitch(typed, layEnum, otherText)) {
        if (debugOn()) {
            std::cerr << "rocket-switcher-daemon: keep \"" << typed << "\"\n";
        }
        if (cfg_.typoCorrectEnabled && applyTypoCorrection(boundary, typed, lay)) return false;
        return finish();
    }

    // The separator is not on screen yet. Type the same physical key in the new
    // layout: RU slash "." becomes EN "/", EN slash "/" becomes RU ".",
    // RU shift+7 "?" becomes EN "&" only when that is the key the fingers hit.
    // "реезыЖ" + slash + slash is "https://". "ццц" + the English period key
    // (ю here) is peeled above and retyped as "www.".
    std::string boundaryUtf = keys_.interpret(boundary, other);
    if (boundaryUtf.empty()) boundaryUtf = keys_.interpret(boundary, lay);
    if (boundaryUtf.empty()) {
        if (boundary.code == KEY_SPACE) boundaryUtf = " ";
        else if (boundary.code == KEY_ENTER || boundary.code == KEY_KPENTER) boundaryUtf = "\n";
        else if (boundary.code == KEY_TAB) boundaryUtf = "\t";
    }

    if (debugOn()) {
        std::cerr << "rocket-switcher-daemon: convert \"" << typed << "\" → \"" << otherText << "\"\n";
    }
    // The separator is not in the document yet: backspace only the word, then type it.
    if (!convertKeys(wordKeys_, lay, other, boundaryUtf, /*boundaryOnScreen=*/false)) return finish();

    lastWordKeys_      = wordKeys_;
    lastWordLayout_    = other;
    lastBoundaryUtf8_  = boundaryUtf;
    lastBoundaryValid_ = !boundaryUtf.empty();
    wordKeys_.clear();
    wordLayout_ = -1;
    wordFrozen_ = false;
    return true;
}

bool PuntoDaemon::applyTypoCorrection(const Keystroke& boundary, const std::string& typed, int layout) {
    const CharMapping::Layout layEnum = layoutEnum(layout);
    if (layEnum != CharMapping::Layout::Russian && layEnum != CharMapping::Layout::English)
        return false;
    const std::string fixed = TypoCorrector::instance().correct(utf8_to_utf32(typed), layEnum);
    if (fixed.empty() || fixed == typed || !canTypeText(fixed)) return false;

    std::string boundaryUtf = keys_.interpret(boundary, layout);
    if (boundaryUtf.empty()) {
        if (boundary.code == KEY_SPACE) boundaryUtf = " ";
        else if (boundary.code == KEY_ENTER || boundary.code == KEY_KPENTER) boundaryUtf = "\n";
        else if (boundary.code == KEY_TAB) boundaryUtf = "\t";
    }

    if (debugOn()) {
        std::cerr << "rocket-switcher-daemon: typo \"" << typed << "\" → \"" << fixed << "\"\n";
    }

    releaseModifiersForInject();
    tapBackspaces(static_cast<int>(utf8_to_utf32(typed).size()));
    if (!typeText(fixed)) {
        (void)typeText(typed);
        restoreHeldModifiers();
        return false;
    }
    restoreHeldModifiers();

    typingUndo_.valid         = true;
    typingUndo_.selection     = false;
    typingUndo_.textReplace   = true;
    typingUndo_.keys.clear();
    typingUndo_.layoutIndex   = layout;
    typingUndo_.boundaryUtf8  = boundaryUtf;
    typingUndo_.originalText  = typed;
    typingUndo_.deleteCpCount = static_cast<int>(utf8_to_utf32(fixed).size() + utf8_to_utf32(boundaryUtf).size());

    lastWordKeys_.clear();
    lastWordLayout_    = -1;
    lastBoundaryUtf8_  = boundaryUtf;
    lastBoundaryValid_ = !boundaryUtf.empty();
    wordKeys_.clear();
    wordLayout_ = -1;
    wordFrozen_ = false;
    return true;
}

bool PuntoDaemon::doSwapLastWord() {
    if (!wordKeys_.empty()) {
        const int lay   = wordLayout_ >= 0 ? wordLayout_ : lastKnownLayoutIndex_;
        const int other = otherLayoutIndex(lay);
        if (!convertKeys(wordKeys_, lay, other, "", false)) return false;
        wordLayout_ = other;
        wordFrozen_ = true;
        return true;
    }
    if (lastWordKeys_.empty() || lastWordLayout_ < 0) return false;
    const int other = otherLayoutIndex(lastWordLayout_);
    const std::string boundary = lastBoundaryValid_ ? lastBoundaryUtf8_ : std::string();
    if (!convertKeys(lastWordKeys_, lastWordLayout_, other, boundary, !boundary.empty())) return false;
    lastWordLayout_ = other;
    wordFrozen_     = true;
    return true;
}

std::string PuntoDaemon::readClipboard() const {
    ensureSessionEnvFromParents();
    FILE* p = popen("timeout 0.25s wl-paste -n 2>/dev/null", "r");
    if (!p) return {};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) {
        out.append(buf);
        if (out.size() > 1024 * 1024) break;
    }
    (void)pclose(p);
    return out;
}

bool PuntoDaemon::setClipboard(const std::string& utf8, bool pasteOnce) {
    ensureSessionEnvFromParents();
    int fd[2];
    if (pipe(fd) < 0) return false;

    pid_t mid = fork();
    if (mid < 0) {
        close(fd[0]);
        close(fd[1]);
        return false;
    }
    if (mid == 0) {
        pid_t inner = fork();
        if (inner < 0) _exit(1);
        if (inner == 0) {
            close(fd[1]);
            dup2(fd[0], STDIN_FILENO);
            close(fd[0]);
            ::setsid();
            if (pasteOnce) {
                execl("/usr/bin/wl-copy", "wl-copy", "--paste-once", static_cast<char*>(nullptr));
                execlp("wl-copy", "wl-copy", "--paste-once", static_cast<char*>(nullptr));
            } else {
                execl("/usr/bin/wl-copy", "wl-copy", static_cast<char*>(nullptr));
                execlp("wl-copy", "wl-copy", static_cast<char*>(nullptr));
            }
            _exit(127);
        }
        _exit(0);
    }
    close(fd[0]);
    const ssize_t n = write(fd[1], utf8.data(), utf8.size());
    close(fd[1]);
    int st = 0;
    if (waitpid(mid, &st, 0) < 0) return false;
    return n == static_cast<ssize_t>(utf8.size()) && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool PuntoDaemon::clearClipboard() {
    ensureSessionEnvFromParents();
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execl("/usr/bin/wl-copy", "wl-copy", "--clear", static_cast<char*>(nullptr));
        execlp("wl-copy", "wl-copy", "--clear", static_cast<char*>(nullptr));
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

void PuntoDaemon::copyCtrlC() {
    io_.emitKey(KEY_LEFTCTRL, 1);
    io_.emitKey(KEY_C, 1);
    io_.emitKey(KEY_C, 0);
    io_.emitKey(KEY_LEFTCTRL, 0);
    io_.sync();
}

void PuntoDaemon::pasteCtrlV() {
    io_.emitKey(KEY_LEFTCTRL, 1);
    ::usleep(3000);
    io_.emitKey(KEY_V, 1);
    ::usleep(3000);
    io_.emitKey(KEY_V, 0);
    io_.emitKey(KEY_LEFTCTRL, 0);
    io_.sync();
}

bool PuntoDaemon::pasteFromClipboard(const std::string& utf8) {
    if (utf8.empty() || !setClipboard(utf8, false)) return false;
    bool ready = false;
    for (int i = 0; i < 15; ++i) {
        ::usleep(i == 0 ? 40000 : 30000);
        if (readClipboard() == utf8) {
            ready = true;
            break;
        }
    }
    if (!ready) return false;
    pasteCtrlV();
    // The field reads the clipboard on Ctrl+V. Restoring the previous
    // contents any sooner puts the old text back into the selection.
    ::usleep(280000);
    return true;
}

bool PuntoDaemon::doSwapSelection() {
    ensureSessionEnvFromParents();
    const std::string saved = readClipboard();

    releaseModifiersForInject();
    copyCtrlC();
    std::string selected;
    for (int i = 0; i < 8 && (selected.empty() || selected == saved); ++i) {
        ::usleep(i == 0 ? 50000 : 30000);
        selected = readClipboard();
    }
    if (selected.empty() || selected == saved) {
        restoreHeldModifiers();
        return false;
    }

    const std::string swapped = CharMapping::swapWord(selected);
    auto restoreClipboard = [&]() {
        if (saved.empty()) (void)clearClipboard();
        else (void)setClipboard(saved, false);
    };
    if (swapped == selected) {
        restoreClipboard();
        restoreHeldModifiers();
        return false;
    }

    // One Ctrl+V replaces the whole highlight. Typing the phrase as a key
    // burst drops the middle ("привет … странненько" came out as
    // "ghbdtnhfyytymrj"). --paste-once is not used: Klipper consumes it and
    // the field then pastes nothing.
    bool inserted = pasteFromClipboard(swapped);
    if (!inserted && canTypeText(swapped)) {
        io_.emitKey(KEY_DELETE, 1);
        io_.emitKey(KEY_DELETE, 0);
        ::usleep(60000);
        inserted = typeText(swapped);
    }
    restoreClipboard();
    restoreHeldModifiers();
    if (!inserted) return false;

    typingUndo_.valid         = true;
    typingUndo_.selection     = true;
    typingUndo_.textReplace   = false;
    typingUndo_.keys.clear();
    typingUndo_.layoutIndex   = -1;
    typingUndo_.boundaryUtf8.clear();
    typingUndo_.originalText  = selected;
    typingUndo_.deleteCpCount = static_cast<int>(utf8_to_utf32(swapped).size());
    forgetTyping();
    typingUndo_.valid = true; // forgetTyping does not clear undo
    return true;
}

bool PuntoDaemon::doUndoLast() {
    if (!typingUndo_.valid) return false;
    if (typingUndo_.textReplace) {
        releaseModifiersForInject();
        tapBackspaces(typingUndo_.deleteCpCount);
        ::usleep(30000);
        const std::string back = typingUndo_.originalText + typingUndo_.boundaryUtf8;
        const bool restored = canTypeText(back) && typeText(back);
        restoreHeldModifiers();
        if (!restored) return false;
        typingUndo_.valid = false;
        forgetTyping();
        return true;
    }

    if (typingUndo_.selection) {
        const std::string saved = readClipboard();
        releaseModifiersForInject();
        tapBackspaces(typingUndo_.deleteCpCount);
        ::usleep(30000);
        bool restored = pasteFromClipboard(typingUndo_.originalText);
        if (!restored && canTypeText(typingUndo_.originalText))
            restored = typeText(typingUndo_.originalText);
        if (saved.empty()) (void)clearClipboard();
        else (void)setClipboard(saved, false);
        if (!restored) {
            restoreHeldModifiers();
            return false;
        }
        restoreHeldModifiers();
        typingUndo_.valid = false;
        forgetTyping();
        return true;
    }

    if (typingUndo_.keys.empty() || typingUndo_.layoutIndex < 0) {
        typingUndo_.valid = false;
        return false;
    }
    if (!switchToLayout(typingUndo_.layoutIndex)) return false;
    releaseModifiersForInject();
    tapBackspaces(typingUndo_.deleteCpCount);
    replayKeystrokes(typingUndo_.keys);
    if (!typingUndo_.boundaryUtf8.empty())
        (void)emitUtf8InLayout(typingUndo_.boundaryUtf8, typingUndo_.layoutIndex);
    restoreHeldModifiers();
    if (!wordKeys_.empty()) wordLayout_ = typingUndo_.layoutIndex;
    if (!lastWordKeys_.empty()) lastWordLayout_ = typingUndo_.layoutIndex;
    typingUndo_.valid = false;
    return true;
}

void PuntoDaemon::openMice() {
    closeMice();
    for (int i = 0; i < 64 && mouseFds_.size() < 8; ++i) {
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        libevdev* dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) != 0) {
            ::close(fd);
            continue;
        }
        const bool mouse = libevdev_has_event_code(dev, EV_KEY, BTN_LEFT)
                           && !libevdev_has_event_code(dev, EV_KEY, KEY_SPACE);
        libevdev_free(dev);
        if (!mouse) {
            ::close(fd);
            continue;
        }
        mouseFds_.push_back(fd);
    }
}

void PuntoDaemon::closeMice() {
    for (int fd : mouseFds_) {
        if (fd >= 0) ::close(fd);
    }
    mouseFds_.clear();
}

void PuntoDaemon::drainMice() {
    for (int fd : mouseFds_) {
        if (fd < 0) continue;
        input_event ev{};
        while (::read(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
            if (ev.type != EV_KEY || ev.value != 1) continue;
            if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE)
                forgetTyping();
        }
    }
}

} // namespace punto
