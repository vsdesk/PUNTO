#pragma once
#include <string>
#include <deque>
#include <optional>
#include <cstdint>
#include "../core/CharMapping.h"

namespace punto {

// Per-InputContext state maintained by PuntoModule.
//
// Tracks:
//  - `wordBuffer`       — characters committed since the last word boundary.
//  - `lastWord`         — the most-recently-completed word (before last boundary).
//  - `lastWordByteSize` — its UTF-8 byte length (used for deleteSurroundingText).
//  - `processedTokenId` — monotonically incrementing token ID; after a successful
//                         auto-switch the token is "frozen" until a new word starts,
//                         preventing repeated re-swaps on the same word.
//
// Thread-safety: NOT thread-safe.  Fcitx5 calls all input-context callbacks on
// the main thread, so no locking is needed.

struct InputTracker {
    std::string wordBuffer;      // current in-progress word (UTF-8)
    std::string lastWord;        // last completed word before boundary
    /// UTF-8 of the boundary character that completed `lastWord` (for daemon delete path).
    std::string lastBoundaryUtf8;
    int         lastWordByteSize = 0;
    uint64_t    tokenId          = 0;  // current token counter
    uint64_t    frozenTokenId    = UINT64_MAX; // token that was auto-switched

    // When Punto injects keys (BackSpace / inserted characters) as part of
    // a swap/auto-switch operation, those injected key events may also pass
    // through our PreInputMethod watcher. To keep the tracker consistent,
    // we suppress all state updates while internal injection is in flight.
    int internalDepth = 0;

    void beginInternal() { ++internalDepth; }
    void endInternal()   { if (internalDepth > 0) --internalDepth; }
    bool isInternal() const { return internalDepth > 0; }

    // Undo support (per last operation in this input context).
    // 1) Layout undo: switch keyboard back to previous layout.
    std::optional<CharMapping::Layout> undoPrevLayout;
    // 2) Text undo (best-effort): delete last swapped token and restore original.
    std::optional<std::string> undoOriginalTextUtf8;
    std::optional<std::string> undoSwappedTextUtf8;
    size_t undoSwappedCpCount = 0;

    // Call when a character is committed that belongs to a word (not a boundary).
    void addChar(const std::string& utf8char);

    // Call when a word-boundary character was just committed.
    // `boundaryUtf8` is the boundary character (space, punctuation, newline, …).
    // Returns the completed word (non-empty if there was one), clears buffer.
    std::optional<std::string> onBoundary(const std::string& boundaryUtf8 = {});

    // Forcibly finalize the word buffer (e.g. on focus-out).
    std::optional<std::string> flush();

    // Mark the current token as already auto-switched.
    void freezeCurrentToken() { frozenTokenId = tokenId; }

    // Is the current token already processed (do NOT auto-switch again)?
    bool isCurrentTokenFrozen() const { return frozenTokenId == tokenId; }

    // Reset all state (e.g. on focus change or delete key).
    void reset(bool clearUndo = true);
};

} // namespace punto
