#include "InputTracker.h"
#include "../core/WordSwapper.h"
#include "../core/Utf8Utils.h"

namespace punto {

void InputTracker::addChar(const std::string& utf8char) {
    wordBuffer += utf8char;
}

std::optional<std::string> InputTracker::onBoundary() {
    if (wordBuffer.empty()) return std::nullopt;

    lastWord         = wordBuffer;
    lastWordByteSize = static_cast<int>(wordBuffer.size());
    wordBuffer.clear();
    ++tokenId;

    return lastWord;
}

std::optional<std::string> InputTracker::flush() {
    return onBoundary();
}

void InputTracker::reset() {
    wordBuffer.clear();
    lastWord.clear();
    lastWordByteSize = 0;
    ++tokenId;
    frozenTokenId = UINT64_MAX;

    // Cancel any pending undo information when we lose synchronization.
    undoPrevLayout.reset();
    undoOriginalTextUtf8.reset();
    undoSwappedTextUtf8.reset();
    undoSwappedCpCount = 0;
}

} // namespace punto
