#pragma once
#include <string>
#include "CharMapping.h"

namespace punto {

// AutoSwitchHeuristic implements Punto-style "wrong layout" detection.
//
// Algorithm (MVP — no ML, no dictionary):
//
//  1. Require word length >= minWordLength (guard against short false positives).
//  2. Reject strings containing digits, '@', '/', '..' (URL/email/number guard).
//  3. Map the entire word to the OTHER layout via CharMapping::swapWord().
//  4. Score BOTH versions (original and swapped) with a bigram language model
//     built from hardcoded top-50 bigrams per language.
//  5. If   score(swapped) > score(original) + confidenceThreshold   →  switch.
//
// The bigram score of a string W is:
//   (#bigrams_of_W found in top-50 set) / max(1, total_bigrams_in_W)
//
// Rationale for thresholds:
//  - confidenceThreshold = 0.15 balances recall vs. false-positive rate for
//    typical 4-8 character words.  Configurable so users can tune.
//  - minWordLength = 3  prevents single-character or two-character false fires.

struct HeuristicConfig {
    int    minWordLength       = 3;
    double confidenceThreshold = 0.15; // how much better swapped must score
    bool   enabled             = true;
};

class AutoSwitchHeuristic {
public:
    explicit AutoSwitchHeuristic(HeuristicConfig cfg = {}) : cfg_(cfg) {}

    void setConfig(const HeuristicConfig& cfg) { cfg_ = cfg; }
    const HeuristicConfig& config() const { return cfg_; }

    // Returns true if `word` (UTF-8) appears to have been typed in the wrong
    // layout and should be swapped to the other layout.
    //
    // `currentLayout` is what the OS/keyboard currently reports.
    // If Unknown, heuristics are applied in both directions.
    bool shouldSwitch(const std::string& word,
                      CharMapping::Layout currentLayout = CharMapping::Layout::Unknown) const;

    // Score how well `text` fits a given layout's language model (bigrams).
    // Returns value in [0, 1].
    static double bigramScore(const std::u32string& text, CharMapping::Layout layout);

    // Guard: returns true if `word` looks like a URL, email, number — skip auto-switch.
    static bool isGuarded(const std::u32string& word);

private:
    HeuristicConfig cfg_;
};

} // namespace punto
