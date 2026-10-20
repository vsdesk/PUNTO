#include "TypoCorrector.h"
#include "Utf8Utils.h"

#include <gtest/gtest.h>

using namespace punto;

namespace {

std::u32string u(const char* utf8) { return utf8_to_utf32(utf8); }

TypoCorrector withDefaults() {
    TypoCorrector c;
    c.setRulesFromText(TypoCorrector::defaultRulesText());
    return c;
}

} // namespace

TEST(Typo, UserRuleReplacesToken) {
    const TypoCorrector c = withDefaults();
    EXPECT_EQ(c.correct(u("почемуто"), CharMapping::Layout::Russian), "почему-то");
    EXPECT_EQ(c.correct(u("Почемуто"), CharMapping::Layout::Russian), "Почему-то");
    EXPECT_EQ(c.correct(u("ПОЧЕМУТО"), CharMapping::Layout::Russian), "ПОЧЕМУ-ТО");
    EXPECT_EQ(c.correct(u("кембы"), CharMapping::Layout::Russian), "кем бы");
}

TEST(Typo, ArrowAndComment) {
    TypoCorrector c;
    c.setRulesFromText("# comment\nчтото -> что-то\n");
    EXPECT_EQ(c.correct(u("чтото"), CharMapping::Layout::Russian), "что-то");
    EXPECT_EQ(c.correct(u("почемуто"), CharMapping::Layout::Russian), "");
}

TEST(Typo, DoubleCapital) {
    TypoCorrector c;
    EXPECT_EQ(c.correct(u("ПРивет"), CharMapping::Layout::Russian), "Привет");
    EXPECT_EQ(c.correct(u("WOrld"), CharMapping::Layout::English), "World");
    EXPECT_EQ(c.correct(u("ПРИВЕТ"), CharMapping::Layout::Russian), "");
    EXPECT_EQ(c.correct(u("Привет"), CharMapping::Layout::Russian), "");
}

TEST(Typo, OtherLayoutPunctuation) {
    TypoCorrector c;
    EXPECT_EQ(c.correct(u("пирогб"), CharMapping::Layout::Russian), "пирог,");
    EXPECT_EQ(c.correct(u("Приветб"), CharMapping::Layout::Russian), "Привет,");
    EXPECT_EQ(c.correct(u("привет"), CharMapping::Layout::Russian), "");
}

TEST(Typo, MissedShift) {
    TypoCorrector c;
    EXPECT_EQ(c.correct(u("привет1"), CharMapping::Layout::Russian), "привет!");
    EXPECT_EQ(c.correct(u("9хорошо"), CharMapping::Layout::Russian), "(хорошо");
}

TEST(Typo, CamelCaseSplit) {
    TypoCorrector c;
    EXPECT_EQ(c.correct(u("helloThe"), CharMapping::Layout::English), "hello The");
    EXPECT_EQ(c.correct(u("приветЭто"), CharMapping::Layout::Russian), "привет Это");
    EXPECT_EQ(c.correct(u("helloTheWorld"), CharMapping::Layout::English), "");
}

TEST(Typo, OneEditWhenUnique) {
    TypoCorrector c;
    EXPECT_EQ(c.correct(u("hlelo"), CharMapping::Layout::English), "hello");
    EXPECT_EQ(c.correct(u("привте"), CharMapping::Layout::Russian), "привет");
    // "bokk" is one key from both "book" and "boko", so it stays.
    EXPECT_EQ(c.correct(u("bokk"), CharMapping::Layout::English), "");
}

TEST(Typo, LayoutWordIsNotASpellingFix) {
    TypoCorrector c;
    EXPECT_EQ(c.correct(u("ghbdtn"), CharMapping::Layout::English), "");
}

TEST(Typo, RealWordStays) {
    TypoCorrector c;
    EXPECT_EQ(c.correct(u("book"), CharMapping::Layout::English), "");
    EXPECT_EQ(c.correct(u("пирог"), CharMapping::Layout::Russian), "");
}
