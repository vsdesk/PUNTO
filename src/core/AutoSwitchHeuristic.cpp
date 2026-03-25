#include "AutoSwitchHeuristic.h"
#include "CharMapping.h"
#include "Utf8Utils.h"
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace punto {

// ---------------------------------------------------------------------------
// Top-50 Russian bigrams (frequency corpus, lowercase)
// ---------------------------------------------------------------------------
static const char32_t* RU_BIGRAMS_RAW[] = {
    U"ст", U"ен", U"но", U"на", U"ра", U"то", U"ко", U"ни",
    U"та", U"ть", U"ро", U"по", U"во", U"ие", U"ли", U"ле",
    U"ит", U"ри", U"ин", U"ор", U"ве", U"те", U"де", U"ал",
    U"пр", U"ов", U"из", U"ат", U"ва", U"ме",
    U"ан", U"ло", U"ла", U"ос", U"лн", U"не", U"ка", U"им",
    U"ас", U"оп", U"ил", U"ки", U"ти", U"го", U"ог", U"да",
    U"ам", U"из", U"ть", U"ер",
};

// Top-50 English bigrams (frequency corpus, lowercase)
static const char32_t* EN_BIGRAMS_RAW[] = {
    U"th", U"he", U"in", U"er", U"an", U"re", U"on", U"at",
    U"en", U"nd", U"ti", U"es", U"or", U"te", U"of", U"ed",
    U"is", U"it", U"al", U"ar", U"st", U"to", U"nt", U"ng",
    U"se", U"ha", U"as", U"ou", U"io", U"le",
    U"ve", U"co", U"me", U"de", U"hi", U"ri", U"ro", U"ic",
    U"ne", U"ea", U"ra", U"ce", U"li", U"ch", U"ll", U"be",
    U"ma", U"si", U"om", U"ur",
};

static std::unordered_set<std::u32string> buildSet(const char32_t** arr, size_t n) {
    std::unordered_set<std::u32string> s;
    for (size_t i = 0; i < n; ++i) s.insert(arr[i]);
    return s;
}

static const std::unordered_set<std::u32string>& ruBigrams() {
    static auto s = buildSet(RU_BIGRAMS_RAW, std::size(RU_BIGRAMS_RAW));
    return s;
}

static const std::unordered_set<std::u32string>& enBigrams() {
    static auto s = buildSet(EN_BIGRAMS_RAW, std::size(EN_BIGRAMS_RAW));
    return s;
}

// ---------------------------------------------------------------------------
// bigramScore
// ---------------------------------------------------------------------------
double AutoSwitchHeuristic::bigramScore(const std::u32string& text,
                                        CharMapping::Layout layout) {
    if (text.size() < 2) return 0.0;

    const auto& bag = (layout == CharMapping::Layout::Russian) ? ruBigrams() : enBigrams();

    // Normalise to lowercase for scoring
    std::u32string lower;
    lower.reserve(text.size());
    for (char32_t c : text) {
        if (c >= U'А' && c <= U'Я') lower.push_back(c + 0x20);
        else if (c == U'Ё')          lower.push_back(U'ё');
        else if (c >= U'A' && c <= U'Z') lower.push_back(c + 32);
        else                         lower.push_back(c);
    }

    size_t found = 0, total = 0;
    for (size_t i = 0; i + 1 < lower.size(); ++i) {
        std::u32string bg = {lower[i], lower[i + 1]};
        // Count only letter bigrams (skip punctuation, digits)
        bool bothLetters = (CharMapping::isRussian(lower[i]) || CharMapping::isEnglish(lower[i]))
                        && (CharMapping::isRussian(lower[i+1]) || CharMapping::isEnglish(lower[i+1]));
        if (!bothLetters) continue;
        ++total;
        if (bag.count(bg)) ++found;
    }
    return total == 0 ? 0.0 : static_cast<double>(found) / total;
}

// ---------------------------------------------------------------------------
// isGuarded — skip URLs, emails, numbers
// ---------------------------------------------------------------------------
bool AutoSwitchHeuristic::isGuarded(const std::u32string& word) {
    // Contains digit → skip
    for (char32_t c : word)
        if (c >= U'0' && c <= U'9') return true;

    // Contains @ → looks like email
    for (char32_t c : word)
        if (c == U'@') return true;

    // Contains :// → URL
    for (size_t i = 0; i + 2 < word.size(); ++i)
        if (word[i] == U':' && word[i+1] == U'/' && word[i+2] == U'/') return true;

    // Consecutive dots → domain-like
    for (size_t i = 0; i + 1 < word.size(); ++i)
        if (word[i] == U'.' && word[i+1] == U'.') return true;

    return false;
}

// ---------------------------------------------------------------------------
// shouldSwitch
// ---------------------------------------------------------------------------
bool AutoSwitchHeuristic::shouldSwitch(const std::string& word,
                                       CharMapping::Layout currentLayout) const {
    if (!cfg_.enabled) return false;

    std::u32string w32 = utf8_to_utf32(word);

    // Length guard (count letters only)
    size_t letterCount = 0;
    for (char32_t c : w32)
        if (CharMapping::isRussian(c) || CharMapping::isEnglish(c)) ++letterCount;

    if (static_cast<int>(letterCount) < cfg_.minWordLength) return false;

    // URL/email/number guard
    if (isGuarded(w32)) return false;

    // Determine current layout from word content if not provided
    CharMapping::Layout effective = currentLayout;
    if (effective == CharMapping::Layout::Unknown || effective == CharMapping::Layout::Mixed) {
        effective = CharMapping::dominantLayout(w32);
    }
    if (effective == CharMapping::Layout::Unknown || effective == CharMapping::Layout::Mixed)
        return false; // can't determine what to swap to

    // Swap the word to the other layout
    std::u32string swapped = CharMapping::swapWord(w32);
    CharMapping::Layout otherLayout = (effective == CharMapping::Layout::Russian)
                                       ? CharMapping::Layout::English
                                       : CharMapping::Layout::Russian;

    // Score both versions against their respective language models
    double scoreOriginal = bigramScore(w32, effective);
    double scoreSwapped  = bigramScore(swapped, otherLayout);

    // Switch only if the swapped version scores significantly better
    return (scoreSwapped - scoreOriginal) > cfg_.confidenceThreshold;
}

} // namespace punto
