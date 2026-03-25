#include "CharMapping.h"
#include "Utf8Utils.h"
#include <cctype>

namespace punto {

// ---------------------------------------------------------------------------
// JCUKEN ↔ QWERTY physical key correspondence
// Format: {Russian codepoint, English codepoint} — lowercase only.
// ---------------------------------------------------------------------------
static const std::pair<char32_t, char32_t> MAPPING_TABLE[] = {
    // Row 1 (top letter row)
    {U'й', U'q'}, {U'ц', U'w'}, {U'у', U'e'}, {U'к', U'r'},
    {U'е', U't'}, {U'н', U'y'}, {U'г', U'u'}, {U'ш', U'i'},
    {U'щ', U'o'}, {U'з', U'p'}, {U'х', U'['}, {U'ъ', U']'},
    // Row 2 (home row)
    {U'ф', U'a'}, {U'ы', U's'}, {U'в', U'd'}, {U'а', U'f'},
    {U'п', U'g'}, {U'р', U'h'}, {U'о', U'j'}, {U'л', U'k'},
    {U'д', U'l'}, {U'ж', U';'}, {U'э', U'\''},
    // Row 3 (bottom letter row)
    {U'я', U'z'}, {U'ч', U'x'}, {U'с', U'c'}, {U'м', U'v'},
    {U'и', U'b'}, {U'т', U'n'}, {U'ь', U'm'}, {U'б', U','},
    {U'ю', U'.'}, {U'ё', U'`'},
};

// Uppercase Russian codepoints: add 0x20 to lowercase (same as Latin uppercase offset).
// Russian lowercase: U+0430–U+044F (а–я), uppercase: U+0410–U+042F (А–Я).
// Ё: uppercase U+0401, lowercase U+0451 (special case).
static char32_t ruUpper(char32_t lower) {
    if (lower == U'ё') return U'Ё';
    if (lower >= U'а' && lower <= U'я') return lower - 0x20;
    return lower;
}

static char32_t ruLower(char32_t upper) {
    if (upper == U'Ё') return U'ё';
    if (upper >= U'А' && upper <= U'Я') return upper + 0x20;
    return upper;
}

const std::unordered_map<char32_t, char32_t>& CharMapping::ruToEnLower() {
    static std::unordered_map<char32_t, char32_t> m = []() {
        std::unordered_map<char32_t, char32_t> tmp;
        for (auto& p : MAPPING_TABLE) tmp[p.first] = p.second;
        return tmp;
    }();
    return m;
}

const std::unordered_map<char32_t, char32_t>& CharMapping::enToRuLower() {
    static std::unordered_map<char32_t, char32_t> m = []() {
        std::unordered_map<char32_t, char32_t> tmp;
        for (auto& p : MAPPING_TABLE) tmp[p.second] = p.first;
        return tmp;
    }();
    return m;
}

char32_t CharMapping::swapChar(char32_t c) {
    // Special case: Ё/ё ↔ ~/`
    // On a Russian JCUKEN keyboard the Ё key sits on the same physical key as
    // backtick/tilde.  Unshifted: ё ↔ `   Shifted: Ё ↔ ~
    if (c == U'Ё') return U'~';
    if (c == U'~') return U'Ё';
    if (c == U'ё') return U'`';
    if (c == U'`') return U'ё';

    // --- Russian → English ---
    // lowercase Russian
    auto it = ruToEnLower().find(c);
    if (it != ruToEnLower().end()) return it->second;

    // uppercase Russian → uppercase English
    if (isRussian(c)) {
        char32_t lo = ruLower(c);
        auto it2 = ruToEnLower().find(lo);
        if (it2 != ruToEnLower().end()) {
            char32_t enLo = it2->second;
            // ASCII uppercase: subtract 32 if it's a letter
            if (enLo >= U'a' && enLo <= U'z') return enLo - 32;
            return enLo; // non-letter (;, ', [, ], ,, .) — no case change
        }
    }

    // --- English → Russian ---
    // lowercase English
    auto it3 = enToRuLower().find(c);
    if (it3 != enToRuLower().end()) return it3->second;

    // uppercase English → uppercase Russian
    if (c >= U'A' && c <= U'Z') {
        char32_t lo = c + 32; // to lowercase
        auto it4 = enToRuLower().find(lo);
        if (it4 != enToRuLower().end()) return ruUpper(it4->second);
    }

    return c; // no mapping — return original
}

std::u32string CharMapping::swapWord(const std::u32string& word) {
    std::u32string result;
    result.reserve(word.size());
    for (char32_t c : word) result.push_back(swapChar(c));
    return result;
}

std::string CharMapping::swapWord(const std::string& utf8word) {
    return utf32_to_utf8(swapWord(utf8_to_utf32(utf8word)));
}

bool CharMapping::isRussian(char32_t c) {
    return (c >= U'а' && c <= U'я') || (c >= U'А' && c <= U'Я')
        || c == U'ё' || c == U'Ё';
}

bool CharMapping::isEnglish(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

bool CharMapping::isWordChar(char32_t c) {
    return isRussian(c) || isEnglish(c) || c == U'-' || c == U'\'';
}

double CharMapping::russianLetterRatio(const std::u32string& text) {
    if (text.empty()) return 0.0;
    size_t letters = 0, ru = 0;
    for (char32_t c : text) {
        if (isRussian(c) || isEnglish(c)) {
            ++letters;
            if (isRussian(c)) ++ru;
        }
    }
    return letters == 0 ? 0.0 : static_cast<double>(ru) / letters;
}

double CharMapping::englishLetterRatio(const std::u32string& text) {
    if (text.empty()) return 0.0;
    size_t letters = 0, en = 0;
    for (char32_t c : text) {
        if (isRussian(c) || isEnglish(c)) {
            ++letters;
            if (isEnglish(c)) ++en;
        }
    }
    return letters == 0 ? 0.0 : static_cast<double>(en) / letters;
}

CharMapping::Layout CharMapping::dominantLayout(const std::u32string& text) {
    double ru = russianLetterRatio(text);
    double en = englishLetterRatio(text);
    if (ru == 0 && en == 0) return Layout::Unknown;
    if (ru > 0.8) return Layout::Russian;
    if (en > 0.8) return Layout::English;
    return Layout::Mixed;
}

} // namespace punto
