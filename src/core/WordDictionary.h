#pragma once

#include "CharMapping.h"

#include <cstddef>
#include <string>

namespace punto {

// Frequency dictionaries for ru/en. A typed word is "real" when its
// case-folded form is in the dictionary of the layout it was displayed in.
// Files under data/dict and /usr/share/rocket-switcher/dict extend the builtin set.
class WordDictionary {
public:
    static const WordDictionary& instance();
    // Re-read dictionary files, including never.txt. The daemon calls this
    // when the settings window saves the exception list.
    static void reload();

    bool contains(const std::u32string& word, CharMapping::Layout layout) const;
    // never.txt: do not auto-switch if either form is listed.
    bool blocked(const std::u32string& word) const;
    // always.txt: prefer the swapped form when the typed form is not a real word.
    bool forced(const std::u32string& word) const;

    std::size_t englishSize() const;
    std::size_t russianSize() const;

private:
    WordDictionary() = default;
};

} // namespace punto
