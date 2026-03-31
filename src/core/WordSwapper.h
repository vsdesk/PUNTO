#pragma once
#include <string>
#include <optional>

namespace punto {

// WordSwapper operates on UTF-8 strings and understands Punto-style
// word boundaries: whitespace, standard punctuation, newline.
//
// It provides two public operations matching Punto's hotkeys:
//   1. swapLastWord   — swap the word immediately before the cursor
//   2. swapSelection  — swap an arbitrary selected fragment

struct SwapResult {
    std::string swapped;      // replacement text (UTF-8)
    int         deleteBack;   // how many UTF-8 bytes to delete before cursor
};

class WordSwapper {
public:
    // Given the text before the cursor (surroundingText up to cursor), extract
    // the last word/fragment (up to a word boundary) and return its swapped
    // version together with the byte count to delete.
    //
    // "Word boundary" chars: space, tab, \n, \r, and punctuation from
    // BOUNDARY_PUNCT (defined in .cpp).  Hyphen and apostrophe are
    // intentionally kept as word chars (same as Punto).
    //
    // Returns nullopt if there is no swappable content (e.g. cursor at start
    // or immediately after a boundary).
    static std::optional<SwapResult> swapLastWord(const std::string& textBeforeCursor);

    // Swap an explicitly selected fragment.  The entire fragment is treated as
    // a single token regardless of word boundaries.
    static std::string swapSelection(const std::string& selectedText);

    // Low-level: is this codepoint a word boundary?
    static bool isWordBoundary(char32_t c);

    // For input tracking: punctuation keys like , . ; [ ] ` are boundaries between words,
    // but on JCUKEN they share keys with RU letters (б/ю/ж/х/ъ/ё).
    // - when the in-progress word is empty, treat them as the first character of the next word
    //   (otherwise ",уква" never appears in wordBuffer);
    // - when previous tracked char is a letter, treat them as word-internal too (e.g. "Lj,hsq").
    // When \a pendingUrlSecondSlash is true (after "реезыЖ" → https:), the next "." must still
    // be a boundary so "реезыЖ.." → https:// completes.
    static bool isWordBoundaryForTracker(char32_t c, bool wordBufferEmpty,
                                       bool prevIsLetter = false,
                                       bool pendingUrlSecondSlash = false);

    /// True when the next character is a Latin letter and the buffer's last letter is Cyrillic
    /// (or the reverse). Callers should only finalize the token (onBoundary), not run
    /// auto-switch + inject on this edge — short words caused inject/backspace loops.
    static bool shouldSplitScriptBeforeAddChar(const std::string& wordBufferUtf8, char32_t ch);
};

} // namespace punto
