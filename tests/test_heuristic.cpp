#include <gtest/gtest.h>
#include "../src/core/AutoSwitchHeuristic.h"
#include "../src/core/CharMapping.h"
#include "../src/core/Utf8Utils.h"

using namespace punto;

// Helper: Russian word typed on wrong (English) layout.
// E.g. user meant "работа" but typed on EN layout — physical keys produce English chars.
// We map "работа" to English (swap) to get what was actually typed:
//   р→h, а→f, б→,, о→j, т→n, а→f  → "hf,jnf"
// This is what arrives to the heuristic: the system sees "hf,jnf" as English text.
//
// shouldSwitch("hf,jnf", English) should return true.

TEST(Heuristic, EnglishGarbageFromRussianWord) {
    AutoSwitchHeuristic h;
    // "ghbdtn" = "привет" typed on EN layout
    // (п→g, р→h, и→b, в→d, е→t, т→n)
    bool result = h.shouldSwitch("ghbdtn", CharMapping::Layout::English);
    EXPECT_TRUE(result) << "Expected auto-switch for 'ghbdtn' (= 'привет' on EN layout)";
}

TEST(Heuristic, RealEnglishWordNotSwitched) {
    AutoSwitchHeuristic h;
    // "hello" is a real English word — do NOT auto-switch
    bool result = h.shouldSwitch("hello", CharMapping::Layout::English);
    EXPECT_FALSE(result) << "'hello' should not trigger auto-switch";
}

TEST(Heuristic, RealEnglishWordThe) {
    AutoSwitchHeuristic h;
    // "the" — very common English word
    EXPECT_FALSE(h.shouldSwitch("the", CharMapping::Layout::English));
}

TEST(Heuristic, RealRussianWordNotSwitched) {
    AutoSwitchHeuristic h;
    // "привет" in Russian layout — don't switch
    std::string ru = utf32_to_utf8(U"привет");
    bool result = h.shouldSwitch(ru, CharMapping::Layout::Russian);
    EXPECT_FALSE(result) << "'привет' should not trigger auto-switch";
}

// Autoswitch on space uses shouldSwitch — must not turn correct Russian into Latin ('ne).
TEST(Heuristic, EtuNotAutoSwitchedWhenRussianDominant) {
    AutoSwitchHeuristic h;
    const std::string etu = utf32_to_utf8(U"эту");
    EXPECT_FALSE(h.shouldSwitch(etu, CharMapping::Layout::Russian))
        << "autoswitch after space must not replace «эту» with mistyped Latin";
}

TEST(Heuristic, RussianTitleCaseGreetingNotSwitched) {
    AutoSwitchHeuristic h;
    std::string ru = utf32_to_utf8(U"Привет");
    EXPECT_FALSE(h.shouldSwitch(ru, CharMapping::Layout::Russian))
        << "Capitalized Russian greeting should not trigger auto-switch";
}

TEST(Heuristic, ShortWordIgnored) {
    AutoSwitchHeuristic h;
    HeuristicConfig cfg;
    cfg.minWordLength = 4;
    h.setConfig(cfg);
    // "ab" is 2 chars — below threshold
    EXPECT_FALSE(h.shouldSwitch("ab", CharMapping::Layout::English));
}

TEST(Heuristic, DisabledNeverSwitches) {
    HeuristicConfig cfg;
    cfg.enabled = false;
    AutoSwitchHeuristic h(cfg);
    EXPECT_FALSE(h.shouldSwitch("ghbdtn", CharMapping::Layout::English));
}

TEST(Heuristic, NumberGuard) {
    AutoSwitchHeuristic h;
    EXPECT_FALSE(h.shouldSwitch("abc123", CharMapping::Layout::English));
}

TEST(Heuristic, EmailGuard) {
    AutoSwitchHeuristic h;
    EXPECT_FALSE(h.shouldSwitch("user@host", CharMapping::Layout::English));
}

TEST(Heuristic, UrlGuard) {
    AutoSwitchHeuristic h;
    EXPECT_FALSE(h.shouldSwitch("http://", CharMapping::Layout::English));
}

// ---------------------------------------------------------------------------
// Bigram score tests
// ---------------------------------------------------------------------------

TEST(BigramScore, EnglishCommonBigrams) {
    // "the" contains "th" and "he" — both top-EN bigrams
    double score = AutoSwitchHeuristic::bigramScore(U"the", CharMapping::Layout::English);
    EXPECT_GT(score, 0.5) << "Common English word should score high in EN bigram model";
}

TEST(BigramScore, RussianCommonBigrams) {
    // "стол" contains "ст" — top-RU bigram
    double score = AutoSwitchHeuristic::bigramScore(U"стол", CharMapping::Layout::Russian);
    EXPECT_GT(score, 0.0);
}

TEST(BigramScore, GarbageScoresLow) {
    // "xzqwv" — unusual consonant cluster, should score low in EN model
    double score = AutoSwitchHeuristic::bigramScore(U"xzqwv", CharMapping::Layout::English);
    EXPECT_LT(score, 0.3);
}

// ---------------------------------------------------------------------------
// isGuarded tests
// ---------------------------------------------------------------------------

TEST(Guard, PureLettersNotGuarded) {
    EXPECT_FALSE(AutoSwitchHeuristic::isGuarded(U"hello"));
    EXPECT_FALSE(AutoSwitchHeuristic::isGuarded(U"привет"));
}

TEST(Guard, DigitsGuarded) {
    EXPECT_TRUE(AutoSwitchHeuristic::isGuarded(U"word2"));
    EXPECT_TRUE(AutoSwitchHeuristic::isGuarded(U"12345"));
}

TEST(Guard, AtSignGuarded) {
    EXPECT_TRUE(AutoSwitchHeuristic::isGuarded(U"user@mail"));
}

TEST(Guard, UrlGuarded) {
    EXPECT_TRUE(AutoSwitchHeuristic::isGuarded(U"http://example"));
}

TEST(Guard, DoubleDotsNotGuardedByItself) {
    EXPECT_FALSE(AutoSwitchHeuristic::isGuarded(U"abc.."));
}

// ---------------------------------------------------------------------------
// Confidence threshold sensitivity test
// ---------------------------------------------------------------------------

TEST(Heuristic, HighThresholdPreventsSwitch) {
    HeuristicConfig cfg;
    cfg.confidenceThreshold = 1.5; // above any possible (scoreSwapped - scoreOriginal) ∈ [0,1]
    AutoSwitchHeuristic h(cfg);
    EXPECT_FALSE(h.shouldSwitch("ghbdtn", CharMapping::Layout::English));
}

TEST(Heuristic, LowThresholdAllowsSwitch) {
    HeuristicConfig cfg;
    cfg.confidenceThreshold = 0.0; // always switch if swapped scores >= original
    cfg.minWordLength = 2;
    AutoSwitchHeuristic h(cfg);
    // "ghbdtn" should still switch
    EXPECT_TRUE(h.shouldSwitch("ghbdtn", CharMapping::Layout::English));
}

TEST(Heuristic, MistypedHttpsOnRussianLayoutSwitches) {
    AutoSwitchHeuristic h;
    std::string mistyped = utf32_to_utf8(U"реезыЖ"); // physical keys for "https:"
    EXPECT_TRUE(h.shouldSwitch(mistyped, CharMapping::Layout::Russian));
}

TEST(Heuristic, EnglishHttpsNotSwitchedToRussian) {
    AutoSwitchHeuristic h;
    EXPECT_FALSE(h.shouldSwitch("https", CharMapping::Layout::English));
    EXPECT_FALSE(h.shouldSwitch("github.com", CharMapping::Layout::English));
}

// "буква" typed with EN layout active: , e r d f
TEST(Heuristic, RussianWordBookLetterSwitchesFromEnglishLayout) {
    AutoSwitchHeuristic h;
    EXPECT_TRUE(h.shouldSwitch(",erdf", CharMapping::Layout::English));
}

// "букву" (accidental EN bigrams er, de — must not be blocked as "plausible English")
TEST(Heuristic, RussianWordBookLetterAccusativeSwitchesFromEnglishLayout) {
    AutoSwitchHeuristic h;
    EXPECT_TRUE(h.shouldSwitch(",erde", CharMapping::Layout::English));
}

// "это" typed with EN layout active: ' n j
TEST(Heuristic, RussianEtoWithLeadingApostropheSwitchesFromEnglishLayout) {
    AutoSwitchHeuristic h;
    EXPECT_TRUE(h.shouldSwitch("'nj", CharMapping::Layout::English));
}

// "эту" on EN layout: ' n e — EN bigram "ne" alone looks like English; must still switch.
TEST(Heuristic, RussianEtuOnEnglishLayoutLeadingApostropheNe) {
    AutoSwitchHeuristic h;
    EXPECT_TRUE(h.shouldSwitch("'ne", CharMapping::Layout::English));
    EXPECT_TRUE(h.shouldSwitch(utf32_to_utf8(U"\u2019ne"), CharMapping::Layout::English));
}

// Helper still encodes "plausible RU on RU keyboard" (manual swap paths no longer call it).
TEST(Heuristic, ShouldBlockManualSwapHelper_PlausibleRussianOnRussianKeyboard) {
    const std::u32string etu = utf8_to_utf32(utf32_to_utf8(U"эту"));
    EXPECT_TRUE(AutoSwitchHeuristic::shouldBlockManualSwapPlausibleRussianOnRussianKeyboard(
        etu, true, 0.42, 0.18));
    EXPECT_FALSE(AutoSwitchHeuristic::shouldBlockManualSwapPlausibleRussianOnRussianKeyboard(
        etu, false, 0.42, 0.18));
    const std::u32string ghbdtn = utf8_to_utf32("ghbdtn");
    EXPECT_FALSE(AutoSwitchHeuristic::shouldBlockManualSwapPlausibleRussianOnRussianKeyboard(
        ghbdtn, true, 0.42, 0.18));
}

// JCUKEN physical keys for short Russian words on EN layout (min_word_length would block these).
TEST(Heuristic, ShortRussianPronounsAndInterjectionsFromEnglishLayout) {
    AutoSwitchHeuristic h;
    EXPECT_TRUE(h.shouldSwitch("z", CharMapping::Layout::English));   // я
    EXPECT_TRUE(h.shouldSwitch("vs", CharMapping::Layout::English));  // мы
    EXPECT_TRUE(h.shouldSwitch("ns", CharMapping::Layout::English));  // ты
    EXPECT_TRUE(h.shouldSwitch("jy", CharMapping::Layout::English));  // он
    EXPECT_TRUE(h.shouldSwitch("ds", CharMapping::Layout::English));  // вы
    EXPECT_TRUE(h.shouldSwitch("tt", CharMapping::Layout::English));  // ее (е+е)
    // эй: э=', й=q
    EXPECT_TRUE(h.shouldSwitch("'q", CharMapping::Layout::English));
}

TEST(Heuristic, RussianBookLetterNotSwitchedWhenLayoutRussian) {
    AutoSwitchHeuristic h;
    std::string ru = utf32_to_utf8(U"буква");
    EXPECT_FALSE(h.shouldSwitch(ru, CharMapping::Layout::Russian));
}

// ---------------------------------------------------------------------------
// Manual QA checklist documentation test (documents expected behaviour)
// ---------------------------------------------------------------------------

TEST(ManualQA, ChecklistDocumented) {
    // This test always passes; it documents the manual QA scenarios.
    // See README.md section "Manual QA Checklist" for step-by-step verification.
    SUCCEED() << R"(
Manual QA Checklist (run in KDE Wayland):

1. SWAP LAST WORD:
   - Open Kate/gedit/any Qt app.
   - Type "ghbdtn" (= 'привет' on EN layout) with EN active.
   - Press Alt+' (default swap_last hotkey).
   - Verify: "ghbdtn" is replaced by "привет".

2. SWAP SELECTION:
   - Type "hello world".
   - Select "hello".
   - Press Alt+Shift+' (default swap_selection hotkey).
   - Verify: "hello" becomes "руддщ".

3. AUTO-SWITCH (EN→RU):
   - Keep English (US) layout active in KDE.
   - Type the Latin keys for "привет" on a Russian JCUKEN muscle map: ghbdtn then Space (not "ghjdtn" — и is the B key).
   - Verify: "ghbdtn " becomes "привет " after the space (autoswitch runs on word boundaries only).

4. AUTO-SWITCH ANTI-FLICKER:
   - Quickly type 10 chars in EN layout.
   - Verify no spurious replacements mid-word.

5. TOGGLE AUTO-SWITCH:
   - Press Alt+Shift+A.
   - Type same word — no auto-switch should fire.
   - Press Alt+Shift+A again — auto-switch re-enabled.

6. GUI HOTKEY CHANGE:
   - Run punto-switcher-config.
   - Change swap_last hotkey to Ctrl+Space.
   - Click Save & Reload.
   - Verify new hotkey works, old hotkey no longer triggers.

Known limitations:
   - selection swap requires app to support zwp_text_input_v3 surrounding_text.
     Works in: Kate, gedit, KWrite, Konsole, Qt apps in general.
     May not work in: Electron apps, some GTK3 apps on pure Wayland.
   - auto-switch uses bigram heuristics (no dictionary); short uncommon words
     may occasionally false-positive or false-negative.
)";
}
