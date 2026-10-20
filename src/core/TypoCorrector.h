#pragma once

#include "CharMapping.h"

#include <string>
#include <unordered_map>

namespace punto {

// Fixes a finished word after the layout decision. User rules in typos.txt
// replace a whole token. The rest is conservative: two capitals, a letter that
// is punctuation on the other layout, one adjacent-key typo or transposition.
class TypoCorrector {
public:
    static TypoCorrector& instance();
    static void reload();

    // Shown in settings when the user has not saved typos.txt yet.
    static std::string defaultRulesText();

    void setRulesFromText(const std::string& text);

    // Empty when the word should stay as typed.
    std::string correct(const std::u32string& word, CharMapping::Layout layout) const;

private:
    void loadFromDisk();

    std::unordered_map<std::string, std::u32string> rules_;
};

} // namespace punto
