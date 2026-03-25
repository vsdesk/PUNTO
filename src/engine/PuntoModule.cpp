#include "PuntoModule.h"
#include "../core/WordSwapper.h"
#include "../core/CharMapping.h"
#include "../core/Utf8Utils.h"

#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-utils/log.h>

#include <string>

namespace punto {

FCITX_DEFINE_LOG_CATEGORY(punto_log, "punto");
#define PUNTO_DEBUG() FCITX_LOGC(punto_log, Debug)
#define PUNTO_WARN()  FCITX_LOGC(punto_log, Warn)

namespace {
// Prevent our own injected keys (BackSpace / inserted Unicode) from confusing
// the tracker via the PreInputMethod watcher.
struct InternalInjectionGuard {
    InputTracker* tracker = nullptr;
    explicit InternalInjectionGuard(InputTracker* t) : tracker(t) {
        if (tracker) tracker->beginInternal();
    }
    ~InternalInjectionGuard() {
        if (tracker) tracker->endInternal();
    }
    InternalInjectionGuard(const InternalInjectionGuard&) = delete;
    InternalInjectionGuard& operator=(const InternalInjectionGuard&) = delete;
};

static std::optional<std::string> prefixForLayout(CharMapping::Layout layout) {
    if (layout == CharMapping::Layout::English) return std::string("keyboard-us");
    if (layout == CharMapping::Layout::Russian) return std::string("keyboard-ru");
    return std::nullopt;
}

static std::optional<std::string> prefixForOppositeLayout(CharMapping::Layout layout) {
    if (layout == CharMapping::Layout::English) return std::string("keyboard-ru");
    if (layout == CharMapping::Layout::Russian) return std::string("keyboard-us");
    return std::nullopt;
}

static bool setIMByPrefix(fcitx::Instance* instance,
                           fcitx::InputContext* ic,
                           const std::string& prefix) {
    const auto& group = instance->inputMethodManager().currentGroup();
    for (const auto& item : group.inputMethodList()) {
        if (item.name().rfind(prefix, 0) == 0) {
            instance->setCurrentInputMethod(ic, item.name(), false);
            PUNTO_DEBUG() << "IM switched to " << item.name();
            return true;
        }
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
// Helper: is this keysym a word-boundary character?
// Must stay in sync with WordSwapper::isWordBoundary.
// ---------------------------------------------------------------------------
static bool isWordBoundaryKeySym(uint32_t sym) {
    // Common boundary keysyms
    switch (sym) {
        case FcitxKey_space:
        case FcitxKey_Tab:
        case FcitxKey_Return:
        case FcitxKey_KP_Enter:
        case FcitxKey_period:
        case FcitxKey_comma:
        case FcitxKey_exclam:
        case FcitxKey_question:
        case FcitxKey_semicolon:
        case FcitxKey_colon:
        case FcitxKey_parenleft:
        case FcitxKey_parenright:
        case FcitxKey_bracketleft:
        case FcitxKey_bracketright:
        case FcitxKey_braceleft:
        case FcitxKey_braceright:
        case FcitxKey_less:
        case FcitxKey_greater:
        case FcitxKey_slash:
        case FcitxKey_backslash:
        case FcitxKey_bar:
        case FcitxKey_at:
        case FcitxKey_numbersign:
        case FcitxKey_dollar:
        case FcitxKey_percent:
        case FcitxKey_asciicircum:
        case FcitxKey_ampersand:
        case FcitxKey_asterisk:
        case FcitxKey_plus:
        case FcitxKey_equal:
        case FcitxKey_asciitilde:
        case FcitxKey_grave:
        case FcitxKey_quotedbl:
            return true;
        default:
            return false;
    }
}

static std::string keysymToBoundaryUtf8(uint32_t sym) {
    switch (sym) {
        case FcitxKey_Return:
        case FcitxKey_KP_Enter:
            return std::string("\n");
        case FcitxKey_Tab:
            return std::string("\t");
        case FcitxKey_space:
            return std::string(" ");
        default:
            break;
    }
    uint32_t unicode =
        fcitx::Key::keySymToUnicode(static_cast<fcitx::KeySym>(sym));
    if (unicode >= 0x20)
        return utf32_to_utf8(std::u32string(1, static_cast<char32_t>(unicode)));
    return std::string(" ");
}

// ---------------------------------------------------------------------------
// Construction & config
// ---------------------------------------------------------------------------

PuntoModule::PuntoModule(fcitx::Instance* instance)
    : instance_(instance),
      propertyFactory_([](fcitx::InputContext&) { return new PuntoICProperty; })
{
    instance_->inputContextManager().registerProperty("puntoSwitcherState",
                                                       &propertyFactory_);
    fcitx::readAsIni(config_, "conf/punto-switcher.conf");
    applyConfig();

    // 1. Key events — PreInputMethod.
    //    Tracks printable characters into wordBuffer directly (via keysym→unicode).
    //    This works in ALL modes: Wayland native, X11/XIM, QT_IM_MODULE, GTK_IM_MODULE.
    //    Previously we used CommitString (PostInputMethod) for tracking, but in Wayland
    //    native virtual-keyboard mode the keyboard addon forwards raw key events without
    //    firing CommitString, so the wordBuffer was never populated.
    handlers_.emplace_back(
        instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event& e) {
                onKeyEvent(static_cast<fcitx::KeyEvent&>(e));
            }));

    // 2. FocusOut — flush state
    handlers_.emplace_back(
        instance_->watchEvent(
            fcitx::EventType::InputContextFocusOut,
            fcitx::EventWatcherPhase::PostInputMethod,
            [this](fcitx::Event& e) {
                onFocusOut(static_cast<fcitx::InputContextEvent&>(e));
            }));

    PUNTO_DEBUG() << "PuntoSwitcher module loaded";
}

void PuntoModule::reloadConfig() {
    fcitx::readAsIni(config_, "conf/punto-switcher.conf");
    applyConfig();
    PUNTO_DEBUG() << "PuntoSwitcher config reloaded";
}

void PuntoModule::setConfig(const fcitx::RawConfig& cfg) {
    config_.load(cfg, true);
    fcitx::safeSaveAsIni(config_, "conf/punto-switcher.conf");
    applyConfig();
}

void PuntoModule::applyConfig() {
    HeuristicConfig hcfg;
    hcfg.enabled             = *config_.autoSwitchEnabled;
    hcfg.minWordLength       = *config_.minWordLength;
    hcfg.confidenceThreshold = *config_.confidenceThresholdPct / 100.0;
    heuristic_.setConfig(hcfg);
}

PuntoICProperty* PuntoModule::prop(fcitx::InputContext* ic) const {
    return ic->propertyAs<PuntoICProperty>("puntoSwitcherState");
}

// ---------------------------------------------------------------------------
// Key event handler (PreInputMethod)
//
// Tracks characters into wordBuffer via keysym→unicode conversion.
// This is the ONLY tracking mechanism (CommitString is not used) because
// in Wayland native virtual-keyboard mode the keyboard addon forwards
// raw key events without firing CommitString.
// ---------------------------------------------------------------------------

void PuntoModule::onKeyEvent(fcitx::KeyEvent& ev) {
    if (ev.isRelease()) return;

    auto* ic  = ev.inputContext();
    auto  key = ev.key();
    auto* p   = prop(ic);

    // If we are injecting keys as part of our own operation, do not mutate
    // tracker state (it would cause wrong/infinite re-switching).
    if (p && p->tracker.isInternal()) {
        return;
    }

    // ---- Hotkey: swap last word ----
    if (key.checkKeyList(*config_.swapLastTextKey)) {
        bool did = doSwapLastWord(ic);
        if (did) ev.filterAndAccept();
        return;
    }

    // ---- Hotkey: swap selection ----
    if (key.checkKeyList(*config_.swapSelectionKey)) {
        if (doSwapSelection(ic)) ev.filterAndAccept();
        return;
    }

    // ---- Hotkey: toggle auto-switch ----
    if (key.checkKeyList(*config_.toggleAutoSwitchKey)) {
        doToggleAutoSwitch();
        ev.filterAndAccept();
        return;
    }

    // ---- Hotkey: undo last switch ----
    if (key.checkKeyList(*config_.undoSwitchKey)) {
        if (doUndoLastSwitch(ic)) ev.filterAndAccept();
        return;
    }

    // ---- Backspace / Delete: reset tracker ----
    if (key.sym() == FcitxKey_BackSpace || key.sym() == FcitxKey_Delete) {
        if (p) p->tracker.reset();
        return;
    }

    // ---- Word boundary: auto-switch check ----
    if (isWordBoundaryKeySym(key.sym())) {
        if (p) {
            if (!p->tracker.wordBuffer.empty() && !p->tracker.isCurrentTokenFrozen())
                checkAutoSwitch(p->tracker.wordBuffer, ic);
            // onBoundary() increments tokenId (unfreezes) and clears wordBuffer.
            // Without this call, freezeCurrentToken() would prevent ALL subsequent
            // words from being auto-switched.
            p->tracker.onBoundary(keysymToBoundaryUtf8(key.sym()));
        }
        // Let the boundary key propagate.
        return;
    }

    // ---- Track printable character into wordBuffer ----
    // Skip if any modifier (Ctrl/Alt/Super/Meta) is held — those are hotkeys.
    if (key.states().test(fcitx::KeyState::Ctrl) ||
        key.states().test(fcitx::KeyState::Alt)  ||
        key.states().test(fcitx::KeyState::Super) ||
        key.states().test(fcitx::KeyState::Hyper))
        return;

    // Convert keysym to Unicode codepoint. Works for Latin and Cyrillic:
    //   - 0x0020..0x007E: ASCII (maps directly)
    //   - 0x00A0..0x00FF: Latin-1 (maps directly)
    //   - 0x01000000+cp:  Unicode keysyms (X.org extended range)
    //   - Cyrillic, Greek etc. via fcitx keySymToUnicode table
    uint32_t unicode = fcitx::Key::keySymToUnicode(key.sym());
    if (unicode < 0x20) return; // control character, not printable

    if (!p) return;

    char32_t ch = static_cast<char32_t>(unicode);
    if (WordSwapper::isWordBoundary(ch)) {
        p->tracker.onBoundary(utf32_to_utf8(std::u32string(1, ch)));
    } else {
        p->tracker.addChar(utf32_to_utf8(std::u32string(1, ch)));
    }
}

void PuntoModule::onFocusOut(fcitx::InputContextEvent& ev) {
    if (auto* p = prop(ev.inputContext())) p->tracker.reset();
}

// ---------------------------------------------------------------------------
// Action: swap last word
// ---------------------------------------------------------------------------

// Helper: forward each character of a UTF-8 string as individual key-press events.
// Uses the X11 Unicode keysym range (0x01000000 + codepoint) which is universally
// supported. All forwardKey calls go through the SAME channel as BackSpace forwardKeys,
// guaranteeing that deletion happens before insertion — no race condition.
// NOTE: forwardKey bypasses fcitx5's IM processing entirely (goes directly to the app),
// so injected chars do NOT re-trigger our onKeyEvent handlers — no re-entrancy.
static void forwardString(fcitx::InputContext* ic, const std::string& utf8str) {
    for (char32_t c : utf8_to_utf32(utf8str)) {
        auto ksym = static_cast<fcitx::KeySym>(0x01000000u + static_cast<uint32_t>(c));
        ic->forwardKey(fcitx::Key(ksym), true);
        ic->forwardKey(fcitx::Key(ksym), false);
    }
}

bool PuntoModule::doSwapLastWord(fcitx::InputContext* ic) {
    auto* p = prop(ic);

    // --- Path A: wordBuffer has the current (in-progress) word ---
    // User typed a word and presses Alt+' without a boundary first.
    // Cursor is right after the word — no trailing boundary character.
    //
    // WHY forwardKey for EVERYTHING (not deleteSurroundingText+commitString):
    //   deleteSurroundingText maps to QInputMethodEvent::setCommitString(str, from, len).
    //   Many apps (KTextEditor/Kate, Cursor/Electron, web inputs) do NOT implement the
    //   replacement-range parameters — they silently ignore `from`/`len` and just insert
    //   the commit string. Result: old word stays + new word inserted → exponential growth.
    //   Using forwardKey for BOTH deletion (BackSpace) and insertion (Unicode keysyms)
    //   keeps everything in the same key-event channel, guaranteeing ordering.
    if (p && !p->tracker.wordBuffer.empty()) {
        const std::string original = p->tracker.wordBuffer;
        std::string swapped = CharMapping::swapWord(original);
        if (swapped == original) {
            PUNTO_DEBUG() << "swap_last: nothing to swap (wordBuffer)";
            return false;
        }
        int cpCount = static_cast<int>(utf8_to_utf32(original).size());

        // Record undo state (for the "cancel last switch" hotkey).
        CharMapping::Layout typedLayout = CharMapping::dominantLayout(utf8_to_utf32(original));
        p->tracker.undoPrevLayout = typedLayout;
        p->tracker.undoOriginalTextUtf8 = original;
        p->tracker.undoSwappedTextUtf8 = swapped;
        p->tracker.undoSwappedCpCount = utf8_to_utf32(swapped).size();

        // Switch IM to match the swapped text.
        if (auto prefix = prefixForOppositeLayout(typedLayout)) {
            setIMByPrefix(instance_, ic, *prefix);
        }

        // Delete old word and insert swapped word with suppressed tracker updates.
        InternalInjectionGuard guard(&p->tracker);
        for (int i = 0; i < cpCount; ++i) {
            ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), true);
            ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), false);
        }
        // Insert swapped word.
        // For manual swaps in browsers/Wayland widgets, Unicode forwardKey
        // injection is more reliable than commitString.
        forwardString(ic, swapped);

        // Clear wordBuffer but keep undo info.
        p->tracker.reset(/*clearUndo=*/false);
        PUNTO_DEBUG() << "swap_last: done (wordBuffer+forwardKey path)";
        return true;
    }

    // --- Path B: wordBuffer empty — word was completed (boundary was pressed) ---
    // Use surroundingText to find the last word.
    const auto& st = ic->surroundingText();
    if (!st.isValid()) {
        PUNTO_WARN() << "swap_last: no wordBuffer and no surroundingText";
        return false;
    }

    std::u32string u32full = utf8_to_utf32(st.text());
    unsigned cursor = st.cursor();
    if (cursor > static_cast<unsigned>(u32full.size()))
        cursor = static_cast<unsigned>(u32full.size());

    std::u32string beforeCursor = u32full.substr(0, cursor);
    std::string beforeUtf8 = utf32_to_utf8(beforeCursor);

    auto result = WordSwapper::swapLastWord(beforeUtf8);
    if (!result) {
        PUNTO_DEBUG() << "swap_last: nothing to swap (surroundingText)";
        return false;
    }

    bool trailingBoundary = !beforeCursor.empty()
                         && WordSwapper::isWordBoundary(beforeCursor.back());

    size_t boundaryByteLen = trailingBoundary
        ? utf32_to_utf8(std::u32string(1, beforeCursor.back())).size()
        : 0;
    size_t wordByteEnd   = beforeUtf8.size() - boundaryByteLen;
    size_t wordByteStart = wordByteEnd - static_cast<size_t>(result->deleteBack);
    int cpCount = static_cast<int>(
        utf8_to_utf32(beforeUtf8.substr(wordByteStart, result->deleteBack)).size());

    // Move cursor before trailing boundary so BackSpace hits the word.
    if (trailingBoundary) {
        ic->forwardKey(fcitx::Key(FcitxKey_Left), true);
        ic->forwardKey(fcitx::Key(FcitxKey_Left), false);
    }
    const std::string swapped = result->swapped;
    const std::string original = CharMapping::swapWord(swapped);

    CharMapping::Layout typedLayout = CharMapping::dominantLayout(utf8_to_utf32(original));
    if (p) {
        p->tracker.undoPrevLayout = typedLayout;
        p->tracker.undoOriginalTextUtf8 = original;
        p->tracker.undoSwappedTextUtf8 = swapped;
        p->tracker.undoSwappedCpCount = utf8_to_utf32(swapped).size();
    }

    if (auto prefix = prefixForOppositeLayout(typedLayout)) {
        setIMByPrefix(instance_, ic, *prefix);
    }

    // Delete old word and insert swapped word with suppressed tracker updates.
    if (p) {
        InternalInjectionGuard guard(&p->tracker);
        for (int i = 0; i < cpCount; ++i) {
            ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), true);
            ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), false);
        }
        forwardString(ic, swapped);
        p->tracker.reset(/*clearUndo=*/false);
    } else {
        for (int i = 0; i < cpCount; ++i) {
            ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), true);
            ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), false);
        }
        forwardString(ic, swapped);
    }
    PUNTO_DEBUG() << "swap_last: done (surroundingText+forwardKey path)";
    return true;
}

// ---------------------------------------------------------------------------
// Action: swap selection
// ---------------------------------------------------------------------------

bool PuntoModule::doSwapSelection(fcitx::InputContext* ic) {
    const auto& st = ic->surroundingText();
    if (!st.isValid()) {
        PUNTO_WARN() << "swap_selection: surroundingText not available";
        return false;
    }

    unsigned cursor = st.cursor();
    unsigned anchor = st.anchor();

    if (cursor == anchor) {
        PUNTO_DEBUG() << "swap_selection: no selection — fallback to swap_last";
        return doSwapLastWord(ic);
    }

    unsigned selStart = std::min(cursor, anchor);
    unsigned selEnd   = std::max(cursor, anchor);

    std::u32string u32full = utf8_to_utf32(st.text());
    if (selEnd > static_cast<unsigned>(u32full.size()))
        selEnd = static_cast<unsigned>(u32full.size());

    std::u32string selected = u32full.substr(selStart, selEnd - selStart);
    if (selected.empty()) return false;

    std::string swappedUtf8 = WordSwapper::swapSelection(utf32_to_utf8(selected));
    if (swappedUtf8 == utf32_to_utf8(selected)) {
        PUNTO_DEBUG() << "swap_selection: nothing mappable in selection";
        return false;
    }

    // In Wayland text-input protocol, commitString replaces the active selection.
    // In X11/XIM mode the app typically replaces the selection on insert as well.
    ic->commitString(swappedUtf8);

    if (auto* p = prop(ic)) p->tracker.reset();
    PUNTO_DEBUG() << "swap_selection: done";
    return true;
}

// ---------------------------------------------------------------------------
// Action: toggle auto-switch
// ---------------------------------------------------------------------------

void PuntoModule::doToggleAutoSwitch() {
    bool current = *config_.autoSwitchEnabled;
    config_.autoSwitchEnabled.setValue(!current);
    fcitx::safeSaveAsIni(config_, "conf/punto-switcher.conf");
    applyConfig();
    FCITX_INFO() << "PuntoSwitcher: AutoSwitch " << (!current ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// Auto-switch check
//
// Called from onKeyEvent BEFORE the boundary character is committed.
// Always uses BackSpace injection to delete the word — deleteSurroundingText
// is NOT used because in KDE Wayland (text-input-v3) it is asynchronous and
// does not take effect before the subsequent commitString, causing both the
// original word and the swapped word to appear in the text field.
// ---------------------------------------------------------------------------

void PuntoModule::checkAutoSwitch(const std::string& word, fcitx::InputContext* ic) {
    auto* p = prop(ic);
    if (!p) return;
    if (p->tracker.isCurrentTokenFrozen()) return;

    std::u32string w32 = utf8_to_utf32(word);
    CharMapping::Layout layout = CharMapping::dominantLayout(w32);

    if (!heuristic_.shouldSwitch(word, layout)) return;

    std::string swapped = CharMapping::swapWord(word);
    int wordCpCount = static_cast<int>(w32.size());

    PUNTO_DEBUG() << "auto_switch: '" << word << "' → '" << swapped << "'";

    // Save undo state for the last switch operation.
    p->tracker.undoPrevLayout = layout; // typed layout
    p->tracker.undoOriginalTextUtf8 = word;
    p->tracker.undoSwappedTextUtf8 = swapped;
    p->tracker.undoSwappedCpCount = utf8_to_utf32(swapped).size();

    InternalInjectionGuard guard(&p->tracker);
    for (int i = 0; i < wordCpCount; ++i) {
        ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), true);
        ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), false);
    }
    // For Wayland text-input-v3, commitString is the most compatible way to
    // replace the active token at once (avoids "deleted but not re-inserted"
    // behaviour in some apps when using Unicode forwardKey injection).
    ic->commitString(swapped);
    p->tracker.freezeCurrentToken();

    // Switch the active input method to match the detected language.
    // Latin input that scored as Russian → switch to keyboard-ru, and vice versa.
    if (auto prefix = prefixForOppositeLayout(layout)) {
        setIMByPrefix(instance_, ic, *prefix);
    }
}

// ---------------------------------------------------------------------------
// Action: undo last switch (layout + best-effort text revert)
// ---------------------------------------------------------------------------
bool PuntoModule::doUndoLastSwitch(fcitx::InputContext* ic) {
    auto* p = prop(ic);
    if (!p) return false;

    bool didAnything = false;

    // 1) Undo layout switch.
    if (p->tracker.undoPrevLayout.has_value()) {
        CharMapping::Layout layout = *p->tracker.undoPrevLayout;
        if (auto prefix = prefixForLayout(layout)) {
            setIMByPrefix(instance_, ic, *prefix);
            didAnything = true;
        }
    }

    // 2) Undo text swap (best-effort; uses surroundingText when available).
    if (p->tracker.undoOriginalTextUtf8 && p->tracker.undoSwappedTextUtf8) {
        const auto& st = ic->surroundingText();
        if (st.isValid()) {
            InternalInjectionGuard guard(&p->tracker);

            std::u32string u32full = utf8_to_utf32(st.text());
            unsigned cursor = st.cursor();
            if (cursor > static_cast<unsigned>(u32full.size()))
                cursor = static_cast<unsigned>(u32full.size());

            // If the cursor is after a boundary char (space/punct), step left
            // once so BackSpace deletes the swapped word only.
            if (cursor > 0 && WordSwapper::isWordBoundary(u32full[cursor - 1])) {
                ic->forwardKey(fcitx::Key(FcitxKey_Left), true);
                ic->forwardKey(fcitx::Key(FcitxKey_Left), false);
                if (cursor > 0) --cursor;
            }
            for (size_t i = 0; i < p->tracker.undoSwappedCpCount; ++i) {
                ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), true);
                ic->forwardKey(fcitx::Key(FcitxKey_BackSpace), false);
            }
            forwardString(ic, *p->tracker.undoOriginalTextUtf8);
            didAnything = true;
        }
    }

    // After undo we lose synchronization; clear state.
    p->tracker.reset(/*clearUndo=*/true);
    return didAnything;
}

} // namespace punto

FCITX_ADDON_FACTORY(punto::PuntoModuleFactory);
