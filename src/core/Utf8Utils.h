#pragma once
#include <string>
#include <vector>

// Minimal UTF-8 <-> UTF-32 conversion utilities.
// We avoid depending on ICU or Boost to keep the core dependency-free.

namespace punto {

// Convert UTF-8 string to UTF-32 codepoint vector
inline std::u32string utf8_to_utf32(const std::string& s) {
    std::u32string result;
    result.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        if (c < 0x80) {
            cp = c; i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= s.size()) break;
            cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i+1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= s.size()) break;
            cp = ((c & 0x0F) << 12)
               | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 6)
               | (static_cast<unsigned char>(s[i+2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= s.size()) break;
            cp = ((c & 0x07) << 18)
               | ((static_cast<unsigned char>(s[i+1]) & 0x3F) << 12)
               | ((static_cast<unsigned char>(s[i+2]) & 0x3F) << 6)
               | (static_cast<unsigned char>(s[i+3]) & 0x3F);
            i += 4;
        } else {
            ++i; // skip invalid byte
        }
        result.push_back(cp);
    }
    return result;
}

// Convert UTF-32 to UTF-8
inline std::string utf32_to_utf8(const std::u32string& s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (char32_t cp : s) {
        if (cp < 0x80) {
            result.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return result;
}

// Count Unicode codepoints in a UTF-8 string
inline size_t utf8_length(const std::string& s) {
    return utf8_to_utf32(s).size();
}

} // namespace punto
