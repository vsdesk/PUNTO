#include <gtest/gtest.h>
#include "../src/core/WordSwapper.h"
#include "../src/core/Utf8Utils.h"

using namespace punto;

// ---------------------------------------------------------------------------
// isWordBoundary
// ---------------------------------------------------------------------------

TEST(WordBoundary, SpaceIsBoundary) {
    EXPECT_TRUE(WordSwapper::isWordBoundary(U' '));
    EXPECT_TRUE(WordSwapper::isWordBoundary(U'\n'));
    EXPECT_TRUE(WordSwapper::isWordBoundary(U'\t'));
}

TEST(WordBoundary, PunctIsBoundary) {
    EXPECT_TRUE(WordSwapper::isWordBoundary(U'.'));
    EXPECT_TRUE(WordSwapper::isWordBoundary(U','));
    EXPECT_TRUE(WordSwapper::isWordBoundary(U'!'));
    EXPECT_TRUE(WordSwapper::isWordBoundary(U'?'));
}

TEST(WordBoundary, LetterIsNotBoundary) {
    EXPECT_FALSE(WordSwapper::isWordBoundary(U'a'));
    EXPECT_FALSE(WordSwapper::isWordBoundary(U'я'));
    EXPECT_FALSE(WordSwapper::isWordBoundary(U'-'));
    EXPECT_FALSE(WordSwapper::isWordBoundary(U'\''));
}

// ---------------------------------------------------------------------------
// swapLastWord — cursor right after the word (no trailing boundary)
// ---------------------------------------------------------------------------

TEST(SwapLastWord, SimpleEnglishWord) {
    // Text before cursor: "hello"
    auto result = WordSwapper::swapLastWord("hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->swapped, utf32_to_utf8(U"руддщ"));
    // deleteBack should be byte length of "hello" = 5
    EXPECT_EQ(result->deleteBack, 5);
}

TEST(SwapLastWord, SimpleRussianWord) {
    // Text before cursor: "привет"
    std::string ru = utf32_to_utf8(U"привет");
    auto result = WordSwapper::swapLastWord(ru);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->swapped, "ghbdtn");
}

TEST(SwapLastWord, WordAfterSpace) {
    // Text: "some text hello" — last word is "hello"
    auto result = WordSwapper::swapLastWord("some text hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->swapped, utf32_to_utf8(U"руддщ"));
    EXPECT_EQ(result->deleteBack, 5); // only "hello" length
}

TEST(SwapLastWord, WordAfterBoundaryChar) {
    // Text: "prefix,world" — last word is "world"
    auto result = WordSwapper::swapLastWord("prefix,world");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->swapped, utf32_to_utf8(U"цщкдв"));
}

TEST(SwapLastWord, TextEndsWithBoundary) {
    // Punto behaviour: cursor is right after a space, swap the word BEFORE space.
    // "hello " — last boundary is space, so the word "hello" is before it.
    auto result = WordSwapper::swapLastWord("hello ");
    ASSERT_TRUE(result.has_value());
    // swapped of "hello"
    EXPECT_EQ(result->swapped, utf32_to_utf8(U"руддщ"));
    EXPECT_EQ(result->deleteBack, 5);
}

TEST(SwapLastWord, CursorAtStart) {
    auto result = WordSwapper::swapLastWord("");
    EXPECT_FALSE(result.has_value());
}

TEST(SwapLastWord, OnlyBoundaryChars) {
    auto result = WordSwapper::swapLastWord("   ");
    EXPECT_FALSE(result.has_value());
}

TEST(SwapLastWord, NoMappableChars) {
    // A word consisting entirely of digits — no mapping, returns nullopt
    auto result = WordSwapper::swapLastWord("12345");
    EXPECT_FALSE(result.has_value());
}

// Hyphen stays inside word
TEST(SwapLastWord, HyphenInWord) {
    // "some-word" — treated as one word (hyphen is not a boundary)
    auto result = WordSwapper::swapLastWord("some-word");
    ASSERT_TRUE(result.has_value());
    // entire "some-word" is swapped (hyphen passes through)
    // s→ы, o→щ, m→ь, e→у, -→-, w→ц, o→щ, r→к, d→в
    std::string expected = utf32_to_utf8(U"ыщьу-цщкв");
    EXPECT_EQ(result->swapped, expected);
}

// ---------------------------------------------------------------------------
// swapSelection
// ---------------------------------------------------------------------------

TEST(SwapSelection, BasicSelection) {
    std::string sel = utf32_to_utf8(U"привет");
    std::string result = WordSwapper::swapSelection(sel);
    EXPECT_EQ(result, "ghbdtn");
}

TEST(SwapSelection, MultiWordSelection) {
    // Selection can span spaces — swaps each char individually
    std::string sel = "hello world";
    std::string result = WordSwapper::swapSelection(sel);
    // h→р, e→у, l→д, l→д, o→щ, space→space, w→ц, o→щ, r→к, l→д, d→в
    EXPECT_EQ(result, utf32_to_utf8(U"руддщ цщкдв"));
}

TEST(SwapSelection, EmptySelection) {
    EXPECT_EQ(WordSwapper::swapSelection(""), "");
}

TEST(SwapSelection, UppercasePreserved) {
    // "Привет" → "Ghbdtn"
    std::string sel = utf32_to_utf8(U"Привет");
    EXPECT_EQ(WordSwapper::swapSelection(sel), "Ghbdtn");
}

// ---------------------------------------------------------------------------
// Round-trip invariant: swap(swap(x)) == x  for pure letter strings
// ---------------------------------------------------------------------------

TEST(SwapRoundTrip, RussianWords) {
    std::vector<std::string> words = {
        utf32_to_utf8(U"привет"),
        utf32_to_utf8(U"работа"),
        utf32_to_utf8(U"клавиатура"),
        utf32_to_utf8(U"Ёжик"),
        utf32_to_utf8(U"объект"),
    };
    for (const auto& w : words) {
        EXPECT_EQ(WordSwapper::swapSelection(WordSwapper::swapSelection(w)), w)
            << "Round-trip failed for: " << w;
    }
}

TEST(SwapRoundTrip, EnglishWords) {
    std::vector<std::string> words = {"hello", "world", "keyboard", "Hello", "WORLD"};
    for (const auto& w : words) {
        EXPECT_EQ(WordSwapper::swapSelection(WordSwapper::swapSelection(w)), w)
            << "Round-trip failed for: " << w;
    }
}
