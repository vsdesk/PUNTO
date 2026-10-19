#include "AutoSwitchHeuristic.h"
#include "CharMapping.h"
#include "Utf8Utils.h"
#include "WordDictionary.h"
#include "WordSwapper.h"
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

// Force-switch obvious technical ASCII tokens (http/https/www/domain suffixes)
// even when the bigram score is inconclusive. Short tokens are exact matches:
// "www" is three letters, "ru"/"io" are two, and a substring search would
// fire inside ordinary words.
static bool looksLikeAsciiTechToken(const std::u32string& s) {
    std::u32string lower = toLowerBasic(s);

    int enLetters = 0;
    int ruLetters = 0;
    for (char32_t c : lower) {
        if (CharMapping::isEnglish(c)) ++enLetters;
        else if (CharMapping::isRussian(c)) ++ruLetters;
    }
    if (ruLetters > 0 || enLetters < 2) return false;

    if (lower == U"www" || lower == U"ftp" || lower == U"ssh" || lower == U"git"
        || lower == U"com" || lower == U"org" || lower == U"net" || lower == U"dev"
        || lower == U"ru" || lower == U"io" || lower == U"ai")
        return true;

    if (enLetters < 3) return false;

    auto contains = [&](const char32_t* needle) {
        return lower.find(needle) != std::u32string::npos;
    };

    if (contains(U"http") || contains(U"https") || contains(U"www") || contains(U"ftp")
        || contains(U"ssh") || contains(U"git"))
        return true;

    if (enLetters < 4) return false;

    if (contains(U"://")) return true;

    if (contains(U".com") || contains(U".org") || contains(U".net") || contains(U".io")
        || contains(U".dev") || contains(U".ai") || contains(U".ru"))
        return true;

    return false;
}

// '.' ',' ';' ':' '`' '[' on JCUKEN are letters on the other layout.
// A dictionary hit must see "hello", not "hello.".
static std::u32string stripTrailingLetterKeyPunct(const std::u32string& s) {
    std::u32string out = s;
    while (!out.empty() && WordSwapper::isWordBoundary(out.back())) {
        const char32_t mapped = CharMapping::swapChar(out.back());
        if (!CharMapping::isRussian(mapped) && !CharMapping::isEnglish(mapped)) break;
        out.pop_back();
    }
    return out;
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
        U"на", U"по", U"из", U"то", U"но", U"за", U"от", U"до", U"со", U"во",
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
static bool isLowerLetter(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'а' && c <= U'я') || c == U'ё';
}

static bool isUpperLetter(char32_t c) {
    return (c >= U'A' && c <= U'Z') || (c >= U'А' && c <= U'Я') || c == U'Ё';
}

// "iPhone" / "GitHub": an internal capital with a lowercase letter after it.
// A final Shift (the colon in "реезыЖ" = "https:") is not camel case.
static bool isInternalCamel(const std::u32string& s) {
    bool seenLower = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char32_t c = s[i];
        if (isLowerLetter(c)) seenLower = true;
        if (!isUpperLetter(c) || i == 0 || !seenLower) continue;
        for (size_t j = i + 1; j < s.size(); ++j) {
            if (isLowerLetter(s[j])) return true;
        }
    }
    return false;
}

bool AutoSwitchHeuristic::shouldSwitch(const std::string& word,
                                       CharMapping::Layout currentLayout,
                                       const std::string& swappedAsTyped) const {
    if (!cfg_.enabled) return false;

    std::u32string w32 = utf8_to_utf32(word);

    // Length guard (count letters + punctuation keys that map to letters).
    size_t letterCount = 0;
    for (char32_t c : w32)
        if (isLetterLikeForLengthGuard(c)) ++letterCount;

    // URL/email/number guard. Underscore marks an identifier, not prose.
    if (isGuarded(w32)) return false;
    for (char32_t c : w32)
        if (c == U'_') return false;
    if (isInternalCamel(w32)) return false;

    // Determine current layout from word content if not provided
    CharMapping::Layout effective = currentLayout;
    if (effective == CharMapping::Layout::Unknown || effective == CharMapping::Layout::Mixed) {
        effective = CharMapping::dominantLayout(w32);
    }
    if (effective == CharMapping::Layout::Unknown || effective == CharMapping::Layout::Mixed)
        return false; // can't determine what to swap to

    // Prefer the other layout's xkb reading when the daemon has one. It matches
    // the keys that will actually be replayed (shifted digit row, №, quotes).
    std::u32string swapped = !swappedAsTyped.empty()
                                 ? utf8_to_utf32(swappedAsTyped)
                                 : CharMapping::invertLayoutWord(w32);
    // Technical ASCII token heuristic (URLs/repos/protocol-like strings).
    if (effective == CharMapping::Layout::Russian && looksLikeAsciiTechToken(swapped))
        return true;
    // If user already types a technical ASCII token in EN layout ("https", "www", ".com"),
    // never auto-switch to RU.
    if (effective == CharMapping::Layout::English && looksLikeAsciiTechToken(w32))
        return false;

    // "hello." stays English. The period key is "ю" on the other layout, so
    // converting would glue that letter on. A real separator is space or the
    // slash key (the Russian period), which never enters the word.
    {
        const std::u32string stem = stripTrailingLetterKeyPunct(w32);
        if (!stem.empty() && stem.size() < w32.size()) return false;
    }

    if (effective == CharMapping::Layout::English && isEnLayoutJcukenLeadPunctWord(w32)) {
        return true;
    }

    CharMapping::Layout otherLayout = (effective == CharMapping::Layout::Russian)
                                       ? CharMapping::Layout::English
                                       : CharMapping::Layout::Russian;

    // Dictionary decides when exactly one of the two readings is a real word.
    // Bigram margin does not override that: "ghbdtn" is not English, "привет" is Russian.
    {
        const WordDictionary& dict = WordDictionary::instance();
        if (dict.blocked(w32) || dict.blocked(swapped)) return false;
        const bool typedIn   = dict.contains(w32, effective);
        const bool swappedIn = dict.contains(swapped, otherLayout);
        if (typedIn) return false;
        // Below minWordLength only the curated short list may switch ("ab" → "фи" must not).
        if ((swappedIn || dict.forced(swapped))
            && static_cast<int>(letterCount) >= cfg_.minWordLength) {
            return true;
        }
    }

    // 1–2 character Russian pronouns / interjections (я, мы, ты, эй, …) typed on EN layout
    // when the dictionary does not contain them (builtin list still covers the tests).
    if (effective == CharMapping::Layout::English && letterCount >= 1 && letterCount <= 2
        && static_cast<int>(letterCount) < cfg_.minWordLength) {
        if (!swapped.empty() && swapped.size() <= 2 && allRussianLetters(swapped)
            && isKnownShortRussianWord(swapped)) {
            return true;
        }
        return false;
    }

    if (static_cast<int>(letterCount) < cfg_.minWordLength) return false;

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

    // Neither reading is in the dictionary: require a wider bigram margin than a known word.
    const double need = std::max(cfg_.confidenceThreshold, 0.55);
    const double diff = scoreSwapped - scoreOriginal;
    return diff > need;
}

} // namespace punto
