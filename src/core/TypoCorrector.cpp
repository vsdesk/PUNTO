#include "TypoCorrector.h"

#include "Utf8Utils.h"
#include "WordDictionary.h"
#include "WordSwapper.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace punto {

namespace {

std::string foldKey(const std::u32string& word) {
    std::u32string out;
    out.reserve(word.size());
    for (char32_t c : word) {
        if (c >= U'A' && c <= U'Z') c = c + 32;
        else if (c >= U'А' && c <= U'Я') c = c + 0x20;
        else if (c == U'Ё' || c == U'ё') c = U'е';
        out.push_back(c);
    }
    return utf32_to_utf8(out);
}

char32_t lowerChar(char32_t c) {
    if (c >= U'A' && c <= U'Z') return c + 32;
    if (c >= U'А' && c <= U'Я') return c + 0x20;
    if (c == U'Ё') return U'ё';
    return c;
}

char32_t upperChar(char32_t c) {
    if (c >= U'a' && c <= U'z') return c - 32;
    if (c >= U'а' && c <= U'я') return c - 0x20;
    if (c == U'ё') return U'Ё';
    return c;
}

bool isLowerLetter(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'а' && c <= U'я') || c == U'ё';
}

bool isUpperLetter(char32_t c) {
    return (c >= U'A' && c <= U'Z') || (c >= U'А' && c <= U'Я') || c == U'Ё';
}

bool isLetter(char32_t c) { return isLowerLetter(c) || isUpperLetter(c); }

enum class CaseStyle { Lower, Capital, Upper, Mixed };

CaseStyle detectCase(const std::u32string& word) {
    bool seen = false;
    bool allUpper = true;
    bool allLower = true;
    bool capital = true;
    bool first = true;
    for (char32_t c : word) {
        if (!isLetter(c)) continue;
        seen = true;
        if (isUpperLetter(c)) allLower = false;
        else allUpper = false;
        if (first) {
            if (!isUpperLetter(c)) capital = false;
            first = false;
        } else if (!isLowerLetter(c)) {
            capital = false;
        }
    }
    if (!seen || allLower) return CaseStyle::Lower;
    if (allUpper) return CaseStyle::Upper;
    if (capital) return CaseStyle::Capital;
    return CaseStyle::Mixed;
}

std::u32string applyCase(std::u32string text, CaseStyle style) {
    if (style == CaseStyle::Mixed) return text;
    bool firstLetter = true;
    for (char32_t& c : text) {
        if (!isLetter(c)) continue;
        if (style == CaseStyle::Upper) c = upperChar(c);
        else if (style == CaseStyle::Capital) {
            c = firstLetter ? upperChar(c) : lowerChar(c);
        } else {
            c = lowerChar(c);
        }
        firstLetter = false;
    }
    return text;
}

std::u32string lowered(const std::u32string& word) {
    std::u32string out = word;
    for (char32_t& c : out) c = lowerChar(c);
    return out;
}

void trim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i > 0) s = s.substr(i);
}

CharMapping::Layout scriptLayout(const std::u32string& word) {
    bool ru = false;
    bool en = false;
    for (char32_t c : word) {
        if (CharMapping::isRussian(c)) ru = true;
        else if (CharMapping::isEnglish(c)) en = true;
    }
    if (ru && en) return CharMapping::Layout::Mixed;
    if (ru) return CharMapping::Layout::Russian;
    if (en) return CharMapping::Layout::English;
    return CharMapping::Layout::Unknown;
}

bool inDict(const std::u32string& word, CharMapping::Layout layout) {
    return WordDictionary::instance().contains(word, layout);
}

const std::unordered_map<char32_t, std::u32string>& neighbors(CharMapping::Layout layout) {
    static const std::unordered_map<char32_t, std::u32string> en = {
        {U'q', U"wa"},    {U'w', U"qesa"},  {U'e', U"wrds"},  {U'r', U"etdf"},
        {U't', U"ryfg"},  {U'y', U"tugh"},  {U'u', U"yihj"},  {U'i', U"uojk"},
        {U'o', U"ipkl"},  {U'p', U"ol"},    {U'a', U"qwsz"},  {U's', U"awedzx"},
        {U'd', U"serfxc"},{U'f', U"drtgcv"},{U'g', U"ftyhvb"},{U'h', U"gyujbn"},
        {U'j', U"huiknm"},{U'k', U"jiolm"}, {U'l', U"kop"},   {U'z', U"asx"},
        {U'x', U"zsdc"},  {U'c', U"xdfv"},  {U'v', U"cfgb"},  {U'b', U"vghn"},
        {U'n', U"bhjm"},  {U'm', U"njk"},
    };
    static const std::unordered_map<char32_t, std::u32string> ru = {
        {U'й', U"цфыё"}, {U'ц', U"йуывф"}, {U'у', U"цкваы"}, {U'к', U"уепавс"},
        {U'е', U"кнрпа"}, {U'н', U"егорп"}, {U'г', U"ншлор"}, {U'ш', U"гщдло"},
        {U'щ', U"шзждл"}, {U'з', U"щхэжд"}, {U'х', U"зъэж"},  {U'ъ', U"хэ"},
        {U'ф', U"йцыяч"}, {U'ы', U"йцувячсф"}, {U'в', U"цукаычсм"}, {U'а', U"укепвсми"},
        {U'п', U"кенрмит"}, {U'р', U"енгоитьп"}, {U'о', U"нгшлтьбр"}, {U'л', U"гшщдбюьо"},
        {U'д', U"шщзжюбл"}, {U'ж', U"щзхэюд"}, {U'э', U"зхъж"}, {U'я', U"фыч"},
        {U'ч', U"фывся"}, {U'с', U"ывапмч"}, {U'м', U"вапитс"}, {U'и', U"апртьм"},
        {U'т', U"проьи"}, {U'ь', U"ролбт"}, {U'б', U"олдюь"}, {U'ю', U"лджб"},
        {U'ё', U"й"},
    };
    return layout == CharMapping::Layout::Russian ? ru : en;
}

std::u32string fixDoubleCapital(const std::u32string& word) {
    if (word.size() < 3) return {};
    if (!isUpperLetter(word[0]) || !isUpperLetter(word[1]) || !isLowerLetter(word[2]))
        return {};
    for (size_t i = 3; i < word.size(); ++i) {
        if (!isLowerLetter(word[i])) return {};
    }
    std::u32string out = word;
    out[1] = lowerChar(out[1]);
    return out;
}

std::u32string fixEdge(const std::u32string& word, CharMapping::Layout layout) {
    if (word.size() < 3) return {};
    const char32_t last = word.back();
    const std::u32string stem = word.substr(0, word.size() - 1);
    auto stemOk = [&]() {
        return stem.size() >= 2 && scriptLayout(stem) == layout && inDict(stem, layout);
    };

    if (last == U'ё' || last == U'Ё') {
        if (!stemOk()) return {};
        std::u32string out = stem;
        out.push_back(U'!');
        return out;
    }
    if (isLowerLetter(last)) {
        const char32_t alt = CharMapping::swapChar(last);
        if (alt != last && WordSwapper::isWordBoundary(alt) && stemOk()) {
            std::u32string out = stem;
            out.push_back(alt);
            return out;
        }
    }
    if (last == U'1' && stem.size() >= 4 && stemOk()) {
        bool digits = false;
        for (char32_t c : stem)
            if (c >= U'0' && c <= U'9') digits = true;
        if (!digits) {
            std::u32string out = stem;
            out.push_back(U'!');
            return out;
        }
    }
    if (word[0] == U'9') {
        const std::u32string rest = word.substr(1);
        if (rest.size() >= 4 && scriptLayout(rest) == layout && inDict(rest, layout)) {
            std::u32string out;
            out.push_back(U'(');
            out += rest;
            return out;
        }
    }
    return {};
}

std::u32string fixCamel(const std::u32string& word, CharMapping::Layout layout) {
    if (word.size() < 6 || scriptLayout(word) != layout) return {};
    int transitions = 0;
    size_t at = 0;
    for (size_t i = 1; i < word.size(); ++i) {
        if (!isLetter(word[i]) || !isLetter(word[i - 1])) return {};
        if (isLowerLetter(word[i - 1]) && isUpperLetter(word[i])) {
            ++transitions;
            at = i;
        } else if (isUpperLetter(word[i]) && i != at) {
            return {};
        }
    }
    if (transitions != 1) return {};
    const std::u32string left = word.substr(0, at);
    const std::u32string right = word.substr(at);
    if (left.size() < 3 || right.size() < 3) return {};
    if (!inDict(left, layout) || !inDict(right, layout)) return {};
    std::u32string out = left;
    out.push_back(U' ');
    out += right;
    return out;
}

std::u32string fixEdit(const std::u32string& word, CharMapping::Layout layout) {
    if (word.size() < 3 || word.size() > 32) return {};
    for (char32_t c : word)
        if (!isLetter(c)) return {};
    if (scriptLayout(word) != layout) return {};

    const std::u32string swapped = CharMapping::swapWord(word);
    const CharMapping::Layout other = layout == CharMapping::Layout::Russian
                                          ? CharMapping::Layout::English
                                          : CharMapping::Layout::Russian;
    if (swapped != word && inDict(swapped, other)) return {};

    const CaseStyle style = detectCase(word);
    if (style == CaseStyle::Mixed) return {};
    const std::u32string low = lowered(word);
    const auto& near = neighbors(layout);

    std::unordered_set<std::string> hits;
    std::u32string chosen;
    auto consider = [&](std::u32string candidate) {
        if (candidate == low) return;
        if (!inDict(candidate, layout)) return;
        const std::string key = foldKey(candidate);
        if (hits.insert(key).second && hits.size() == 1) chosen = std::move(candidate);
    };

    if (low.size() >= 3) {
        for (size_t i = 0; i + 1 < low.size(); ++i) {
            if (low[i] == low[i + 1]) continue;
            std::u32string c = low;
            std::swap(c[i], c[i + 1]);
            consider(std::move(c));
        }
    }
    if (low.size() >= 4) {
        for (size_t i = 0; i < low.size(); ++i) {
            const auto it = near.find(low[i]);
            if (it == near.end()) continue;
            for (char32_t n : it->second) {
                std::u32string c = low;
                c[i] = n;
                consider(std::move(c));
            }
        }
    }
    if (hits.size() != 1) return {};
    return applyCase(std::move(chosen), style);
}

std::string homeDir() {
    const char* h = std::getenv("HOME");
    return (h && h[0]) ? std::string(h) : std::string();
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

std::string TypoCorrector::defaultRulesText() {
    return
        "# Слева — как набрали, справа — как должно быть.\n"
        "# Строка с # — комментарий. Разделитель «=» или «->».\n"
        "почемуто = почему-то\n"
        "отчегото = отчего-то\n"
        "зачемто = зачем-то\n"
        "чтото = что-то\n"
        "ктото = кто-то\n"
        "чегото = чего-то\n"
        "какойто = какой-то\n"
        "какаято = какая-то\n"
        "какоето = какое-то\n"
        "какието = какие-то\n"
        "гдето = где-то\n"
        "когдато = когда-то\n"
        "кудато = куда-то\n"
        "откудато = откуда-то\n"
        "какбы = как бы\n"
        "кембы = кем бы\n"
        "чембы = чем бы\n"
        "комубы = кому бы\n"
        "хорошобы = хорошо бы\n"
        "всетаки = все-таки\n"
        "какбудто = как будто\n";
}

TypoCorrector& TypoCorrector::instance() {
    static TypoCorrector self;
    return self;
}

void TypoCorrector::reload() {
    instance().loadFromDisk();
}

void TypoCorrector::setRulesFromText(const std::string& text) {
    rules_.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t sep = std::string::npos;
        size_t seplen = 0;
        const auto arrow = line.find("->");
        const auto eq = line.find('=');
        if (arrow != std::string::npos && (eq == std::string::npos || arrow < eq)) {
            sep = arrow;
            seplen = 2;
        } else if (eq != std::string::npos) {
            sep = eq;
            seplen = 1;
        } else {
            continue;
        }
        std::string left = line.substr(0, sep);
        std::string right = line.substr(sep + seplen);
        trim(left);
        trim(right);
        if (left.empty() || right.empty()) continue;
        rules_[foldKey(utf8_to_utf32(left))] = utf8_to_utf32(right);
    }
}

void TypoCorrector::loadFromDisk() {
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("PUNTO_DICT_DIR")) {
        if (env[0]) {
            const fs::path p = fs::path(env) / "typos.txt";
            if (fs::is_regular_file(p)) {
                setRulesFromText(readFile(p));
                return;
            }
        }
    }
    const std::string home = homeDir();
    if (!home.empty()) {
        const fs::path neu = fs::path(home) / ".config/rocket-switcher/dict/typos.txt";
        const fs::path old = fs::path(home) / ".config/punto-switcher/dict/typos.txt";
        if (fs::is_regular_file(neu)) {
            setRulesFromText(readFile(neu));
            return;
        }
        if (fs::is_regular_file(old)) {
            setRulesFromText(readFile(old));
            return;
        }
    }
    setRulesFromText(defaultRulesText());
}

std::string TypoCorrector::correct(const std::u32string& word, CharMapping::Layout layout) const {
    if (word.empty() || word.size() > 40) return {};

    const std::string typed = utf32_to_utf8(word);
    const auto rule = rules_.find(foldKey(word));
    if (rule != rules_.end()) {
        const std::string out = utf32_to_utf8(applyCase(rule->second, detectCase(word)));
        return out == typed ? std::string() : out;
    }

    std::u32string current = word;
    if (std::u32string doubled = fixDoubleCapital(current); !doubled.empty())
        current = std::move(doubled);

    CharMapping::Layout lay = scriptLayout(current);
    if (lay == CharMapping::Layout::Unknown) lay = layout;
    if (lay != CharMapping::Layout::Russian && lay != CharMapping::Layout::English)
        return {};

    if (inDict(current, lay) || WordDictionary::instance().blocked(current)) {
        const std::string out = utf32_to_utf8(current);
        return out == typed ? std::string() : out;
    }

    if (std::u32string edge = fixEdge(current, lay); !edge.empty())
        current = std::move(edge);
    else if (std::u32string camel = fixCamel(current, lay); !camel.empty())
        current = std::move(camel);
    else if (std::u32string edited = fixEdit(current, lay); !edited.empty())
        current = std::move(edited);

    const std::string out = utf32_to_utf8(current);
    return out == typed ? std::string() : out;
}

} // namespace punto
