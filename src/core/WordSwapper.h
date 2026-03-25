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
};

} // namespace punto
