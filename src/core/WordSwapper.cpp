#include "WordSwapper.h"
#include "CharMapping.h"
#include "Utf8Utils.h"

namespace punto {

// Punctuation characters treated as word boundaries (Punto behaviour).
// Hyphen '-' and apostrophe '\'' are NOT included — they're word-internal.
static const char32_t BOUNDARY_PUNCT[] = {
    U' ', U'\t', U'\n', U'\r',
    U'.', U',', U'!', U'?', U';', U':',
    U'(', U')', U'[', U']', U'{', U'}',
    U'<', U'>', U'/', U'\\', U'|',
    U'@', U'#', U'$', U'%', U'^', U'&', U'*',
    U'+', U'=', U'~', U'`', U'"',
    0 // sentinel
};

bool WordSwapper::isWordBoundary(char32_t c) {
    for (size_t i = 0; BOUNDARY_PUNCT[i] != 0; ++i)
        if (BOUNDARY_PUNCT[i] == c) return true;
    return false;
}

bool WordSwapper::shouldSplitScriptBeforeAddChar(const std::string& wordBufferUtf8, char32_t ch) {
    if (wordBufferUtf8.empty()) return false;
    if (!CharMapping::isEnglish(ch) && !CharMapping::isRussian(ch)) return false;

    const std::u32string wb32 = utf8_to_utf32(wordBufferUtf8);
    char32_t lastLetter = 0;
    bool     hasLast    = false;
    for (size_t i = wb32.size(); i > 0;) {
        --i;
        const char32_t c = wb32[i];
        if (CharMapping::isEnglish(c)) {
            lastLetter = c;
            hasLast    = true;
            break;
        }
        if (CharMapping::isRussian(c)) {
            lastLetter = c;
            hasLast    = true;
            break;
        }
    }
    if (!hasLast) return false;

    const bool lastLatin = CharMapping::isEnglish(lastLetter);
    const bool nextLatin = CharMapping::isEnglish(ch);
    const bool lastCyr   = CharMapping::isRussian(lastLetter);
    const bool nextCyr   = CharMapping::isRussian(ch);
    return (lastLatin && nextCyr) || (lastCyr && nextLatin);
}

bool WordSwapper::isWordBoundaryForTracker(char32_t c, bool wordBufferEmpty,
                                           bool prevIsLetter,
                                           bool pendingUrlSecondSlash) {
    if (!isWordBoundary(c)) return false;
    // EN punctuation keys that map to RU letters on JCUKEN:
    // , -> б, . -> ю, ; -> ж, [ -> х, ] -> ъ, ` -> ё
    if ((c == U',' || c == U'.' || c == U';' || c == U'[' || c == U']' || c == U'`')
        && !pendingUrlSecondSlash
        && (wordBufferEmpty || prevIsLetter))
        return false;
    return true;
}

std::optional<SwapResult> WordSwapper::swapLastWord(const std::string& textBeforeCursor) {
    if (textBeforeCursor.empty()) return std::nullopt;

    std::u32string u32 = utf8_to_utf32(textBeforeCursor);
    if (u32.empty()) return std::nullopt;

    // Walk backwards from the cursor to find the word start.
    // Skip any trailing boundary characters first (e.g. cursor is right after space).
    size_t end = u32.size();

    // If the last char is a boundary, there is nothing to swap
    // (the cursor sits between a boundary and further content we don't see).
    // Actually Punto swaps the word BEFORE the just-typed boundary, so:
    // skip exactly one trailing boundary if present.
    size_t wordEnd = end;
    if (wordEnd > 0 && isWordBoundary(u32[wordEnd - 1])) {
        wordEnd--; // exclude the boundary itself from the word
    }

    if (wordEnd == 0) return std::nullopt;

    // Find start of word
    size_t wordStart = wordEnd;
    while (wordStart > 0 && !isWordBoundary(u32[wordStart - 1])) {
        wordStart--;
    }

    if (wordStart == wordEnd) return std::nullopt; // empty word

    std::u32string word = u32.substr(wordStart, wordEnd - wordStart);
    std::u32string swapped = CharMapping::swapWord(word);

    // If nothing changed (no mappable chars), return nullopt to avoid no-op swap
    if (swapped == word) return std::nullopt;

    // deleteBack covers the word only (not the boundary char after it)
    std::string wordUtf8 = utf32_to_utf8(word);
    std::string swappedUtf8 = utf32_to_utf8(swapped);

    return SwapResult{swappedUtf8, static_cast<int>(wordUtf8.size())};
}

std::string WordSwapper::swapSelection(const std::string& selectedText) {
    return CharMapping::swapWord(selectedText);
}

} // namespace punto
