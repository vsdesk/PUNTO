#pragma once

#include <string>

namespace punto {

struct DaemonConfig {
    std::string devicePath; // empty = auto-detect first keyboard

    std::string swapLastSpec      = "Alt+apostrophe";
    std::string swapSelectionSpec = "Alt+Shift+apostrophe";
    std::string toggleAutoSpec    = "Alt+Shift+a";
    std::string undoSpec          = "Alt+Shift+BackSpace";

    bool   autoSwitchEnabled     = true;
    int    minWordLength         = 3;
    /// Margin for swapped-vs-original bigram scores (daemon.ini: confidence_threshold).
    double confidenceThreshold = 0.18;
    /// If word is almost all one script and bigram score in that script is >= this, never autoswap.
    double minPlausibleDominantBigramScore = 0.18;

    /// KDE / compositor layout indices (us=0, ru=1 typical).
    int layoutIndexEnglish = 0;
    int layoutIndexRussian = 1;

    /// Optional custom shell commands (empty = use built-in KDE dbus / noop).
    std::string cmdLayoutEnglish;
    std::string cmdLayoutRussian;

    /// xkb layout/variant strings built from KDE's LayoutList (kxkbrc), matching KDE's index order.
    /// Used in setupXkb() so that xkb group N == KDE layout N (avoids EN/RU inversion on "ru,us" systems).
    std::string xkbLayoutString  = "us,ru";
    std::string xkbVariantString = ",";

    /// Load from ~/.config/punto-switcher/daemon.ini (merged with defaults).
    static DaemonConfig loadDefault();
    bool save() const;
};

} // namespace punto
