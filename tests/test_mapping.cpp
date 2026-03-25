#include <gtest/gtest.h>
#include "../src/core/CharMapping.h"
#include "../src/core/Utf8Utils.h"

using namespace punto;

// ---------------------------------------------------------------------------
// Single-character swap tests
// ---------------------------------------------------------------------------

TEST(CharMappingChar, LowercaseRuToEn) {
    EXPECT_EQ(CharMapping::swapChar(U'й'), U'q');
    EXPECT_EQ(CharMapping::swapChar(U'ц'), U'w');
    EXPECT_EQ(CharMapping::swapChar(U'у'), U'e');
    EXPECT_EQ(CharMapping::swapChar(U'к'), U'r');
    EXPECT_EQ(CharMapping::swapChar(U'е'), U't');
    EXPECT_EQ(CharMapping::swapChar(U'н'), U'y');
    EXPECT_EQ(CharMapping::swapChar(U'г'), U'u');
    EXPECT_EQ(CharMapping::swapChar(U'ш'), U'i');
    EXPECT_EQ(CharMapping::swapChar(U'щ'), U'o');
    EXPECT_EQ(CharMapping::swapChar(U'з'), U'p');
    EXPECT_EQ(CharMapping::swapChar(U'х'), U'[');
    EXPECT_EQ(CharMapping::swapChar(U'ъ'), U']');
    EXPECT_EQ(CharMapping::swapChar(U'ф'), U'a');
    EXPECT_EQ(CharMapping::swapChar(U'ы'), U's');
    EXPECT_EQ(CharMapping::swapChar(U'в'), U'd');
    EXPECT_EQ(CharMapping::swapChar(U'а'), U'f');
    EXPECT_EQ(CharMapping::swapChar(U'п'), U'g');
    EXPECT_EQ(CharMapping::swapChar(U'р'), U'h');
    EXPECT_EQ(CharMapping::swapChar(U'о'), U'j');
    EXPECT_EQ(CharMapping::swapChar(U'л'), U'k');
    EXPECT_EQ(CharMapping::swapChar(U'д'), U'l');
    EXPECT_EQ(CharMapping::swapChar(U'ж'), U';');
    EXPECT_EQ(CharMapping::swapChar(U'э'), U'\'');
    EXPECT_EQ(CharMapping::swapChar(U'я'), U'z');
    EXPECT_EQ(CharMapping::swapChar(U'ч'), U'x');
    EXPECT_EQ(CharMapping::swapChar(U'с'), U'c');
    EXPECT_EQ(CharMapping::swapChar(U'м'), U'v');
    EXPECT_EQ(CharMapping::swapChar(U'и'), U'b');
    EXPECT_EQ(CharMapping::swapChar(U'т'), U'n');
    EXPECT_EQ(CharMapping::swapChar(U'ь'), U'm');
    EXPECT_EQ(CharMapping::swapChar(U'б'), U',');
    EXPECT_EQ(CharMapping::swapChar(U'ю'), U'.');
    EXPECT_EQ(CharMapping::swapChar(U'ё'), U'`');
}

TEST(CharMappingChar, LowercaseEnToRu) {
    EXPECT_EQ(CharMapping::swapChar(U'q'), U'й');
    EXPECT_EQ(CharMapping::swapChar(U'w'), U'ц');
    EXPECT_EQ(CharMapping::swapChar(U'e'), U'у');
    EXPECT_EQ(CharMapping::swapChar(U'r'), U'к');
    EXPECT_EQ(CharMapping::swapChar(U't'), U'е');
    EXPECT_EQ(CharMapping::swapChar(U'y'), U'н');
    EXPECT_EQ(CharMapping::swapChar(U'u'), U'г');
    EXPECT_EQ(CharMapping::swapChar(U'i'), U'ш');
    EXPECT_EQ(CharMapping::swapChar(U'o'), U'щ');
    EXPECT_EQ(CharMapping::swapChar(U'p'), U'з');
    EXPECT_EQ(CharMapping::swapChar(U'a'), U'ф');
    EXPECT_EQ(CharMapping::swapChar(U's'), U'ы');
    EXPECT_EQ(CharMapping::swapChar(U'd'), U'в');
    EXPECT_EQ(CharMapping::swapChar(U'f'), U'а');
    EXPECT_EQ(CharMapping::swapChar(U'g'), U'п');
    EXPECT_EQ(CharMapping::swapChar(U'h'), U'р');
    EXPECT_EQ(CharMapping::swapChar(U'j'), U'о');
    EXPECT_EQ(CharMapping::swapChar(U'k'), U'л');
    EXPECT_EQ(CharMapping::swapChar(U'l'), U'д');
    EXPECT_EQ(CharMapping::swapChar(U'z'), U'я');
    EXPECT_EQ(CharMapping::swapChar(U'x'), U'ч');
    EXPECT_EQ(CharMapping::swapChar(U'c'), U'с');
    EXPECT_EQ(CharMapping::swapChar(U'v'), U'м');
    EXPECT_EQ(CharMapping::swapChar(U'b'), U'и');
    EXPECT_EQ(CharMapping::swapChar(U'n'), U'т');
    EXPECT_EQ(CharMapping::swapChar(U'm'), U'ь');
    EXPECT_EQ(CharMapping::swapChar(U'`'), U'ё');
}

TEST(CharMappingChar, UppercaseRuToEn) {
    EXPECT_EQ(CharMapping::swapChar(U'Й'), U'Q');
    EXPECT_EQ(CharMapping::swapChar(U'Ф'), U'A');
    EXPECT_EQ(CharMapping::swapChar(U'Я'), U'Z');
    // Ё → ~ (tilde = Shift+backtick), preserving case from the Ё key
    EXPECT_EQ(CharMapping::swapChar(U'Ё'), U'~');
}

TEST(CharMappingChar, UppercaseEnToRu) {
    EXPECT_EQ(CharMapping::swapChar(U'Q'), U'Й');
    EXPECT_EQ(CharMapping::swapChar(U'A'), U'Ф');
    EXPECT_EQ(CharMapping::swapChar(U'Z'), U'Я');
}

TEST(CharMappingChar, NoMappingPassthrough) {
    // Characters with no mapping return unchanged
    EXPECT_EQ(CharMapping::swapChar(U'1'), U'1');
    EXPECT_EQ(CharMapping::swapChar(U' '), U' ');
    EXPECT_EQ(CharMapping::swapChar(U'@'), U'@');
}

// ---------------------------------------------------------------------------
// Word swap tests (UTF-8 API)
// ---------------------------------------------------------------------------

TEST(CharMappingWord, SwapRussianWordToEnglish) {
    // "привет" typed on wrong layout → should produce the en equivalent
    // п→g, р→h, и→b, в→d, е→t, т→n  →  "ghbdtn"
    EXPECT_EQ(CharMapping::swapWord(std::string("привет")), "ghbdtn");
}

TEST(CharMappingWord, SwapEnglishWordToRussian) {
    // "hello" on wrong layout:
    // h→р, e→у, l→д, l→д, o→щ  → "рудд"... wait:
    // h→р, e→у, l→д, l→д, o→щ  → "руддщ"
    std::string result = CharMapping::swapWord(std::string("hello"));
    EXPECT_EQ(result, utf32_to_utf8(U"руддщ"));
}

TEST(CharMappingWord, SwapTitleCase) {
    // "Привет" → "Ghbdtn" (capital П→G)
    std::string result = CharMapping::swapWord(std::string("Привет"));
    EXPECT_EQ(result, "Ghbdtn");
}

TEST(CharMappingWord, SwapAllCaps) {
    // "ПРИВЕТ" → "GHBDTN"
    std::string result = CharMapping::swapWord(std::string("ПРИВЕТ"));
    EXPECT_EQ(result, "GHBDTN");
}

TEST(CharMappingWord, DoubleSwapIsIdentity) {
    // Swapping twice should return the original word
    std::string original = "Привет";
    std::string once  = CharMapping::swapWord(original);
    std::string twice = CharMapping::swapWord(once);
    EXPECT_EQ(twice, original);
}

TEST(CharMappingWord, DoubleSwapEnglish) {
    std::string original = "Hello";
    EXPECT_EQ(CharMapping::swapWord(CharMapping::swapWord(original)), original);
}

// Edge case: ё / Ё
// Physical key mapping: ё ↔ ` (backtick)  |  Ё ↔ ~ (tilde = Shift+backtick)
TEST(CharMappingWord, YoCharacter) {
    std::string yo    = utf32_to_utf8(U"ёж");  // ё + ж
    std::string swapped = CharMapping::swapWord(yo);
    // ё→`, ж→;  → "`;"
    EXPECT_EQ(swapped, "`;");
    EXPECT_EQ(CharMapping::swapWord(swapped), yo);
}

TEST(CharMappingChar, YoUppercase) {
    // Ё → ~ (tilde, shift+backtick)  — case-preserving round-trip
    EXPECT_EQ(CharMapping::swapChar(U'Ё'), U'~');
    EXPECT_EQ(CharMapping::swapChar(U'~'), U'Ё');
    // Round-trip
    EXPECT_EQ(CharMapping::swapChar(CharMapping::swapChar(U'Ё')), U'Ё');
    EXPECT_EQ(CharMapping::swapChar(CharMapping::swapChar(U'ё')), U'ё');
}

// Edge case: ъ / Ъ (hard sign)
TEST(CharMappingWord, HardSign) {
    std::string s = utf32_to_utf8(U"объект");
    std::string swapped = CharMapping::swapWord(s);
    // о→j, б→,, ъ→], е→t, к→r, т→n  →  "j,]trn"
    EXPECT_EQ(swapped, "j,]trn");
    EXPECT_EQ(CharMapping::swapWord(swapped), s);
}

// Mixed non-letter characters pass through unchanged
TEST(CharMappingWord, NonLettersPassThrough) {
    // Numbers and spaces have no mapping
    std::string s = "abc 123";
    // a→ф, b→и, c→с, space→space, digits→digits
    std::string result = CharMapping::swapWord(s);
    EXPECT_EQ(result, utf32_to_utf8(U"фис 123"));
}

// ---------------------------------------------------------------------------
// Layout detection
// ---------------------------------------------------------------------------

TEST(CharMappingLayout, DetectRussian) {
    auto layout = CharMapping::dominantLayout(U"привет");
    EXPECT_EQ(layout, CharMapping::Layout::Russian);
}

TEST(CharMappingLayout, DetectEnglish) {
    auto layout = CharMapping::dominantLayout(U"hello");
    EXPECT_EQ(layout, CharMapping::Layout::English);
}

TEST(CharMappingLayout, DetectMixed) {
    auto layout = CharMapping::dominantLayout(U"privet");
    EXPECT_EQ(layout, CharMapping::Layout::English); // all English letters
}
