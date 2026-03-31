#include "AutoSwitchHeuristic.h"
#include "CharMapping.h"
#include "Utf8Utils.h"
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace punto {

// ---------------------------------------------------------------------------
// Russian bigrams (frequency corpus + extensions, lowercase)
// A tiny top-N list misses many normal words (e.g. "буква", "букву", "других"),
// so wrong-layout Latin keeps a high accidental English bigram score ("er", "de")
// while the Cyrillic target scores low — auto-switch never fires. Keep this list
// broad; plausible-RU guard still blocks real Russian text from being swapped.
// ---------------------------------------------------------------------------
static const char32_t* RU_BIGRAMS_RAW[] = {
    U"ст", U"ен", U"но", U"на", U"ра", U"то", U"ко", U"ни",
    U"эт", U"ту",
    U"та", U"ть", U"ро", U"по", U"во", U"ие", U"ли", U"ле",
    U"ит", U"ри", U"ин", U"ор", U"ве", U"те", U"де", U"ал",
    U"пр", U"ов", U"из", U"ат", U"ва", U"ме",
    U"ан", U"ло", U"ла", U"ос", U"лн", U"не", U"ка", U"им",
    U"ас", U"оп", U"ил", U"ки", U"ти", U"го", U"ог", U"да",
    U"ам", U"ер",
    U"ет", U"ив", U"вы", U"ми", U"об", U"до", U"ем", U"ой", U"ая",
    // Common stems / clusters absent from the first block
    U"бу", U"ук", U"кв", U"ву", U"сл", U"др", U"ру", U"уг", U"ги",
    U"их", U"че", U"бы", U"мо", U"тв", U"ня", U"чт", U"зн", U"ны", U"гр",
    U"се", U"ек", U"кт", U"тс", U"ся", U"бо", U"ом", U"нн", U"ку", U"ут",
    U"тр", U"ма", U"яв", U"вн", U"яз", U"ей", U"щи", U"ши", U"жа",
    U"ах", U"ох", U"ух", U"ыв", U"вь", U"ьн", U"нь", U"мя", U"ят", U"ик",
    U"кс", U"ср", U"рт", U"ьо", U"йт", U"вл", U"рм",
    U"ще", U"ещ", U"йс", U"йн", U"ье", U"ее", U"мь", U"ае", U"ео",
    U"оа", U"уа", U"ио", U"оу", U"уе", U"еи", U"ию", U"юч", U"чн", U"нк",
    U"кл", U"лс", U"см", U"мн", U"нт", U"тк", U"кр", U"рг", U"гн", U"нс",
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

static std::u32string toLowerBasic(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t c : s) {
        if (c >= U'А' && c <= U'Я') out.push_back(c + 0x20);
        else if (c == U'Ё') out.push_back(U'ё');
        else if (c >= U'A' && c <= U'Z') out.push_back(c + 32);
        else out.push_back(c);
    }
    return out;
}

// Borrowed from common Punto-like switchers: force-switch obvious "technical" ASCII tokens
// (http/https/www/domain suffixes) even when bigram score is inconclusive.
static bool looksLikeAsciiTechToken(const std::u32string& s) {
    std::u32string lower = toLowerBasic(s);

    int enLetters = 0;
    int ruLetters = 0;
    for (char32_t c : lower) {
        if (CharMapping::isEnglish(c)) ++enLetters;
        else if (CharMapping::isRussian(c)) ++ruLetters;
    }
    if (ruLetters > 0 || enLetters < 4) return false;

    auto contains = [&](const char32_t* needle) {
        return lower.find(needle) != std::u32string::npos;
    };

    if (contains(U"http") || contains(U"https") || contains(U"www") || contains(U"ftp")
        || contains(U"ssh") || contains(U"git"))
        return true;

    if (contains(U"://")) return true;

    if (contains(U".com") || contains(U".org") || contains(U".net") || contains(U".io")
        || contains(U".dev") || contains(U".ai") || contains(U".ru"))
        return true;

    return false;
}

// Length guard helper: count symbols that are letters OR map to a letter key on
// the opposite layout (e.g. '\'' -> э, ';' -> ж, ',' -> б, '.' -> ю).
static bool isLetterLikeForLengthGuard(char32_t c) {
    if (CharMapping::isRussian(c) || CharMapping::isEnglish(c)) return true;
    const char32_t mapped = CharMapping::swapChar(c);
    return CharMapping::isRussian(mapped) || CharMapping::isEnglish(mapped);
}

static bool allRussianLetters(const std::u32string& s) {
    for (char32_t c : s) {
        if (!CharMapping::isRussian(c)) return false;
    }
    return !s.empty();
}

// Curated 1–2 letter Russian pronouns / particles / interjections that are otherwise
// blocked by min_word_length (default 3). Compare with ё→е folded to one form.
static bool isKnownShortRussianWord(const std::u32string& ru) {
    if (ru.empty() || ru.size() > 2) return false;
    std::u32string low = toLowerBasic(ru);
    for (char32_t& c : low) {
        if (c == U'ё') c = U'е';
    }
    static const std::unordered_set<std::u32string> one = {
        U"я", U"а", U"у", U"о", U"к", U"в", U"с", U"и", U"ы",
    };
    static const std::unordered_set<std::u32string> two = {
        U"мы", U"ты", U"он", U"вы", U"ее", U"эй", U"ну", U"да", U"не", U"ни",
        U"ли", U"бы", U"же", U"ах", U"ох", U"эх", U"ой", U"ай", U"уф",
    };
    if (low.size() == 1) return one.count(low) != 0;
    return two.count(low) != 0;
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
bool AutoSwitchHeuristic::shouldBlockManualSwapPlausibleRussianOnRussianKeyboard(
    const std::u32string& w32,
    bool   keyboardLayoutIsRussian,
    double minPlausibleDominantBigramScore,
    double confidenceThreshold) {
    if (!keyboardLayoutIsRussian) return false;
    const double ruRatio = CharMapping::russianLetterRatio(w32);
    const double enRatio = CharMapping::englishLetterRatio(w32);
    if (CharMapping::dominantLayout(w32) != CharMapping::Layout::Russian) return false;
    if (ruRatio < 0.85 || enRatio > 0.05) return false;
    const double scO = bigramScore(w32, CharMapping::Layout::Russian);
    const double scS = bigramScore(CharMapping::invertLayoutWord(w32), CharMapping::Layout::English);
    return scO >= minPlausibleDominantBigramScore
        && scS <= scO + confidenceThreshold;
}

bool AutoSwitchHeuristic::isEnLayoutJcukenLeadPunctWord(const std::u32string& w32) {
    if (w32.size() < 2) return false;
    const char32_t f = w32[0];
    if (CharMapping::isEnglish(f) || CharMapping::isRussian(f)) return false;
    const char32_t mapped = CharMapping::swapChar(f);
    if (!CharMapping::isRussian(mapped)) return false;
    for (size_t i = 1; i < w32.size(); ++i) {
        if (!CharMapping::isEnglish(w32[i])) return false;
    }
    const std::u32string inv = CharMapping::invertLayoutWord(w32);
    if (inv.empty()) return false;
    for (char32_t c : inv) {
        if (!CharMapping::isRussian(c)) return false;
    }
    return true;
}

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

    return false;
}

// ---------------------------------------------------------------------------
// shouldSwitch
// ---------------------------------------------------------------------------
bool AutoSwitchHeuristic::shouldSwitch(const std::string& word,
                                       CharMapping::Layout currentLayout) const {
    if (!cfg_.enabled) return false;

    std::u32string w32 = utf8_to_utf32(word);

    // Length guard (count letters + punctuation keys that map to letters).
    size_t letterCount = 0;
    for (char32_t c : w32)
        if (isLetterLikeForLengthGuard(c)) ++letterCount;

    // URL/email/number guard
    if (isGuarded(w32)) return false;

    // Determine current layout from word content if not provided
    CharMapping::Layout effective = currentLayout;
    if (effective == CharMapping::Layout::Unknown || effective == CharMapping::Layout::Mixed) {
        effective = CharMapping::dominantLayout(w32);
    }
    if (effective == CharMapping::Layout::Unknown || effective == CharMapping::Layout::Mixed)
        return false; // can't determine what to swap to

    // 1–2 character Russian pronouns / interjections (я, мы, ты, эй, …) typed on EN layout
    // would otherwise fail min_word_length (default 3).
    if (effective == CharMapping::Layout::English && letterCount >= 1 && letterCount <= 2
        && static_cast<int>(letterCount) < cfg_.minWordLength) {
        const std::u32string swappedShort = CharMapping::invertLayoutWord(w32);
        if (!swappedShort.empty() && swappedShort.size() <= 2 && allRussianLetters(swappedShort)
            && isKnownShortRussianWord(swappedShort)) {
            return true;
        }
        return false;
    }

    if (static_cast<int>(letterCount) < cfg_.minWordLength) return false;

    if (effective == CharMapping::Layout::English && isEnLayoutJcukenLeadPunctWord(w32)) {
        return true;
    }

    // Invert layout for scoring (do not apply normalizeJcukenMixedPrefix here — that is a fix
    // for display/swapWord, not part of bigram comparison on raw typed text).
    std::u32string swapped = CharMapping::invertLayoutWord(w32);
    // Technical ASCII token heuristic (URLs/repos/protocol-like strings).
    if (effective == CharMapping::Layout::Russian && looksLikeAsciiTechToken(swapped))
        return true;
    // If user already types a technical ASCII token in EN layout ("https", "www", ".com"),
    // never auto-switch to RU.
    if (effective == CharMapping::Layout::English && looksLikeAsciiTechToken(w32))
        return false;

    CharMapping::Layout otherLayout = (effective == CharMapping::Layout::Russian)
                                       ? CharMapping::Layout::English
                                       : CharMapping::Layout::Russian;

    // Score both versions against their respective language models
    double scoreOriginal = bigramScore(w32, effective);
    double scoreSwapped  = bigramScore(swapped, otherLayout);

    const double ruRatio = CharMapping::russianLetterRatio(w32);
    const double enRatio = CharMapping::englishLetterRatio(w32);

    // Dominant script looks "plausible" only if the *other* layout does not fit much better.
    // Otherwise wrong-layout Latin with accidental EN bigrams ("er", "de" in ",erde") was
    // blocked forever because scoreOriginal alone was high enough vs minPlausible.
    if (effective == CharMapping::Layout::Russian && ruRatio >= 0.85 && enRatio <= 0.05) {
        if (scoreOriginal >= cfg_.minPlausibleDominantBigramScore
            && scoreSwapped <= scoreOriginal + cfg_.confidenceThreshold) {
            return false;
        }
    }
    if (effective == CharMapping::Layout::English && enRatio >= 0.85 && ruRatio <= 0.05) {
        if (scoreOriginal >= cfg_.minPlausibleDominantBigramScore
            && scoreSwapped <= scoreOriginal + cfg_.confidenceThreshold) {
            return false;
        }
    }

    const double diff = scoreSwapped - scoreOriginal;
    return diff > cfg_.confidenceThreshold;
}

} // namespace punto
