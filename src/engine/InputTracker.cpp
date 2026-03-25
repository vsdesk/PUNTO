#include "InputTracker.h"
#include "../core/WordSwapper.h"
#include "../core/Utf8Utils.h"

namespace punto {

void InputTracker::addChar(const std::string& utf8char) {
    wordBuffer += utf8char;
}

std::optional<std::string> InputTracker::onBoundary(const std::string& boundaryUtf8) {
    if (wordBuffer.empty()) return std::nullopt;

    lastWord           = wordBuffer;
    lastWordByteSize   = static_cast<int>(wordBuffer.size());
    lastBoundaryUtf8   = boundaryUtf8;
    wordBuffer.clear();
    ++tokenId;

    return lastWord;
}

std::optional<std::string> InputTracker::flush() {
    if (wordBuffer.empty()) return std::nullopt;

    lastWord         = wordBuffer;
    lastWordByteSize = static_cast<int>(wordBuffer.size());
    lastBoundaryUtf8.clear();
    wordBuffer.clear();
    ++tokenId;

    return lastWord;
}

void InputTracker::reset(bool clearUndo) {
    wordBuffer.clear();
    lastWord.clear();
    lastBoundaryUtf8.clear();
    lastWordByteSize = 0;
    ++tokenId;
    frozenTokenId = UINT64_MAX;

    if (clearUndo) {
        // Cancel any pending undo information when we lose synchronization.
        undoPrevLayout.reset();
        undoOriginalTextUtf8.reset();
        undoSwappedTextUtf8.reset();
        undoSwappedCpCount = 0;
    }
}

} // namespace punto
