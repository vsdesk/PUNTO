#pragma once
#include <string>
#include <unordered_map>

namespace punto {

// Physical key position mapping between Russian JCUKEN and English QWERTY.
//
// The mapping encodes: "which Russian character lives on the same physical key
// as which English character?" — this is exactly Punto's approach for swap.
//
// Source: standard Russian JCUKEN keyboard layout (ISO-8859-5 / Unicode Cyrillic).
// Ё/ё maps to `~ (backtick/tilde key), consistent with most Russian KB stickers.
//
// Uppercase pairs are derived automatically from lowercase.
class CharMapping {
public:
    // Swap a single Unicode codepoint.
    // Returns the mapped character, or the original if no mapping exists.
    static char32_t swapChar(char32_t c);

    // Swap every character in a UTF-32 string.
    static std::u32string swapWord(const std::u32string& word);

    // UTF-8 convenience wrapper.
    static std::string swapWord(const std::string& utf8word);

    // Layout detection helpers.
    static bool isRussian(char32_t c);    // Cyrillic letter
    static bool isEnglish(char32_t c);    // ASCII a-z / A-Z
    static bool isWordChar(char32_t c);   // letter, hyphen, apostrophe

    // Fraction of characters that are Russian / English letters.
    // Ignores non-letter characters (digits, punctuation, etc.).
    static double russianLetterRatio(const std::u32string& text);
    static double englishLetterRatio(const std::u32string& text);

    enum class Layout { Russian, English, Mixed, Unknown };

    // Dominant layout of the text (ignores non-letters).
    static Layout dominantLayout(const std::u32string& text);

private:
    // Lazy-initialized static maps (lowercase only; uppercase handled via case logic)
    static const std::unordered_map<char32_t, char32_t>& ruToEnLower();
    static const std::unordered_map<char32_t, char32_t>& enToRuLower();
};

} // namespace punto
