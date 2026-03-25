#include "DaemonConfig.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace punto {

static std::string configPath() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::string(home) + "/.config/punto-switcher/daemon.ini";
}

static void trim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i > 0) s = s.substr(i);
}

static bool parseBool(const std::string& v) {
    if (v == "1" || v == "true" || v == "True" || v == "yes") return true;
    return false;
}

/// "us(intl), ru" → tokens; strip variant in parentheses (KDE LayoutList format).
static std::string layoutEntryBase(std::string s) {
    trim(s);
    const auto p = s.find('(');
    if (p != std::string::npos) s.resize(p);
    trim(s);
    return s;
}

static bool layoutIsEnglishToken(const std::string& base) {
    return base == "us" || base == "gb" || base == "uk" || base == "en";
}

static bool layoutIsRussianToken(const std::string& base) {
    return base == "ru" || (base.size() >= 2 && base.compare(0, 2, "ru") == 0);
}

/// Match KDE Plasma layout order with org.kde.keyboard getLayout() indices (no manual daemon.ini).
static void applyLayoutOrderFromKxkbrc(DaemonConfig& c) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return;

    const std::filesystem::path path =
        std::filesystem::path(home) / ".config" / "kxkbrc";
    std::ifstream in(path);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        trim(line);
        if (line.rfind("LayoutList=", 0) != 0) continue;

        std::string list = line.substr(11);
        trim(list);
        if (list.empty()) break;

        std::vector<std::string> parts;
        {
            std::stringstream ss(list);
            std::string item;
            while (std::getline(ss, item, ',')) {
                trim(item);
                if (!item.empty()) parts.push_back(item);
            }
        }

        int foundEn = -1;
        int foundRu = -1;

        // Build xkb layout+variant strings matching KDE's exact order so that
        // xkb group N == KDE layout index N (avoids the EN/RU inversion on "ru,us" systems).
        std::string xkbLayouts, xkbVariants;
        for (size_t i = 0; i < parts.size(); ++i) {
            const std::string& entry = parts[i];
            const std::string  base  = layoutEntryBase(entry);

            if (foundEn < 0 && layoutIsEnglishToken(base)) foundEn = static_cast<int>(i);
            if (foundRu < 0 && layoutIsRussianToken(base)) foundRu = static_cast<int>(i);

            if (i > 0) { xkbLayouts += ','; xkbVariants += ','; }
            xkbLayouts += base;

            // Extract variant from "us(intl)" → "intl"
            const auto lp = entry.find('(');
            const auto rp = entry.rfind(')');
            if (lp != std::string::npos && rp != std::string::npos && rp > lp)
                xkbVariants += entry.substr(lp + 1, rp - lp - 1);
        }
        if (foundEn >= 0) c.layoutIndexEnglish = foundEn;
        if (foundRu >= 0) c.layoutIndexRussian = foundRu;
        if (!xkbLayouts.empty()) {
            c.xkbLayoutString  = xkbLayouts;
            c.xkbVariantString = xkbVariants;
        }
        break;
    }
}

DaemonConfig DaemonConfig::loadDefault() {
    DaemonConfig c;
    const std::string path = configPath();
    if (!path.empty()) {
        std::ifstream in(path);
        if (in) {
            std::string line;
            std::string section;
            while (std::getline(in, line)) {
                trim(line);
                if (line.empty() || line[0] == '#' || line[0] == ';') continue;
                if (line.front() == '[' && line.back() == ']') {
                    section = line.substr(1, line.size() - 2);
                    continue;
                }
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                trim(key);
                trim(val);

                if (section == "daemon") {
                    if (key == "device") c.devicePath = val;
                } else if (section == "hotkeys") {
                    if (key == "swap_last") c.swapLastSpec = val;
                    else if (key == "swap_selection") c.swapSelectionSpec = val;
                    else if (key == "toggle_auto") c.toggleAutoSpec = val;
                    else if (key == "undo") c.undoSpec = val;
                } else if (section == "autoswitch") {
                    if (key == "enabled") c.autoSwitchEnabled = parseBool(val);
                    else if (key == "min_word_length") {
                        try { c.minWordLength = std::stoi(val); } catch (...) {}
                    }                     else if (key == "confidence_threshold") {
                        try { c.confidenceThreshold = std::stod(val); } catch (...) {}
                    } else if (key == "min_plausible_dominant_bigram_score") {
                        try { c.minPlausibleDominantBigramScore = std::stod(val); } catch (...) {}
                    }
                } else if (section == "layout") {
                    if (key == "en_index") {
                        try { c.layoutIndexEnglish = std::stoi(val); } catch (...) {}
                    } else if (key == "ru_index") {
                        try { c.layoutIndexRussian = std::stoi(val); } catch (...) {}
                    } else if (key == "cmd_en") c.cmdLayoutEnglish = val;
                    else if (key == "cmd_ru") c.cmdLayoutRussian = val;
                }
            }
        }
    }
    applyLayoutOrderFromKxkbrc(c);
    return c;
}

bool DaemonConfig::save() const {
    const std::string path = configPath();
    if (path.empty()) return false;

    try {
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path());
    } catch (...) {
        return false;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "# punto-switcher-daemon — see README\n";
    out << "[daemon]\n";
    out << "device=" << devicePath << "\n\n";
    out << "[hotkeys]\n";
    out << "swap_last=" << swapLastSpec << "\n";
    out << "swap_selection=" << swapSelectionSpec << "\n";
    out << "toggle_auto=" << toggleAutoSpec << "\n";
    out << "undo=" << undoSpec << "\n\n";
    out << "[autoswitch]\n";
    out << "enabled=" << (autoSwitchEnabled ? "true" : "false") << "\n";
    out << "min_word_length=" << minWordLength << "\n";
    out << "confidence_threshold=" << confidenceThreshold << "\n";
    out << "min_plausible_dominant_bigram_score=" << minPlausibleDominantBigramScore << "\n\n";
    out << "[layout]\n";
    out << "en_index=" << layoutIndexEnglish << "\n";
    out << "ru_index=" << layoutIndexRussian << "\n";
    if (!cmdLayoutEnglish.empty()) out << "cmd_en=" << cmdLayoutEnglish << "\n";
    if (!cmdLayoutRussian.empty()) out << "cmd_ru=" << cmdLayoutRussian << "\n";
    return true;
}

} // namespace punto
