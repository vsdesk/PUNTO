#include "WordDictionary.h"

#include "Utf8Utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

void insertFolded(std::unordered_set<std::string>& bag, const char32_t* lit) {
    bag.insert(foldKey(std::u32string(lit)));
}

struct Bags {
    std::unordered_set<std::string> en;
    std::unordered_set<std::string> ru;
    std::unordered_set<std::string> never;
    std::unordered_set<std::string> always;
};

void addLine(std::unordered_set<std::string>& bag, const std::string& line) {
    std::string w = line;
    while (!w.empty() && (w.back() == '\r' || w.back() == ' ' || w.back() == '\t')) w.pop_back();
    size_t i = 0;
    while (i < w.size() && (w[i] == ' ' || w[i] == '\t')) ++i;
    if (i > 0) w = w.substr(i);
    if (w.empty() || w[0] == '#') return;
    // "word count" frequency rows: keep the token.
    const auto sp = w.find_first_of(" \t");
    if (sp != std::string::npos) w.resize(sp);
    if (w.empty()) return;
    bag.insert(foldKey(utf8_to_utf32(w)));
}

void loadFile(const std::filesystem::path& path, std::unordered_set<std::string>& bag) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) addLine(bag, line);
}

void loadDir(const std::filesystem::path& dir, Bags& bags) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    loadFile(dir / "en.txt", bags.en);
    loadFile(dir / "ru.txt", bags.ru);
    loadFile(dir / "never.txt", bags.never);
    loadFile(dir / "always.txt", bags.always);
}

std::string homeDir() {
    const char* h = std::getenv("HOME");
    return (h && h[0]) ? std::string(h) : std::string();
}

Bags loadBags() {
    Bags bags;
    // Enough for unit tests and for running before the data files are installed.
    const char32_t* enBuiltin[] = {
        U"hello", U"the", U"and", U"that", U"this", U"with", U"have", U"from",
        U"you",   U"not", U"are", U"was",  U"for",  U"your", U"just", U"https",
        U"http",  U"ftp", U"www", U"github", U"com", U"org", U"net",
    };
    const char32_t* ruBuiltin[] = {
        U"привет", U"буква", U"букву", U"это", U"эту", U"стол", U"работа",
        U"я", U"а", U"у", U"о", U"к", U"в", U"с", U"и", U"ы",
        U"мы", U"ты", U"он", U"вы", U"ее", U"эй", U"ну", U"да", U"не", U"ни",
        U"ли", U"бы", U"же", U"то", U"но", U"по", U"на", U"из", U"за", U"что", U"как",
    };
    for (const char32_t* w : enBuiltin) insertFolded(bags.en, w);
    for (const char32_t* w : ruBuiltin) insertFolded(bags.ru, w);

    if (const char* env = std::getenv("PUNTO_DICT_DIR")) {
        if (env[0]) loadDir(env, bags);
    }
    loadDir("/usr/share/rocket-switcher/dict", bags);
    loadDir("/usr/share/punto-switcher/dict", bags);
    loadDir("data/dict", bags);
    loadDir("../data/dict", bags);
    loadDir("../../data/dict", bags);
    const std::string home = homeDir();
    if (!home.empty()) {
        loadDir(std::filesystem::path(home) / ".config/rocket-switcher/dict", bags);
        loadDir(std::filesystem::path(home) / ".config/punto-switcher/dict", bags);
    }
    return bags;
}

const Bags& bags() {
    static const Bags loaded = loadBags();
    return loaded;
}

} // namespace

WordDictionary::WordDictionary() {
    en_ = bags().en.size();
    ru_ = bags().ru.size();
}

const WordDictionary& WordDictionary::instance() {
    static const WordDictionary dict;
    return dict;
}

bool WordDictionary::contains(const std::u32string& word, CharMapping::Layout layout) const {
    if (word.empty()) return false;
    const std::string key = foldKey(word);
    if (layout == CharMapping::Layout::English) return bags().en.count(key) != 0;
    if (layout == CharMapping::Layout::Russian) return bags().ru.count(key) != 0;
    return false;
}

bool WordDictionary::blocked(const std::u32string& word) const {
    if (word.empty()) return false;
    return bags().never.count(foldKey(word)) != 0;
}

bool WordDictionary::forced(const std::u32string& word) const {
    if (word.empty()) return false;
    return bags().always.count(foldKey(word)) != 0;
}

} // namespace punto
