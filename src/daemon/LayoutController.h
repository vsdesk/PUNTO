#pragma once

#include "../core/CharMapping.h"
#include "DaemonConfig.h"

#include <chrono>

namespace punto {

// Switches system keyboard layout (KDE dbus, optional shell commands).
class LayoutController {
public:
    explicit LayoutController(const DaemonConfig& cfg);

    void setEnglish();
    void setRussian();
    void setForCharLayout(CharMapping::Layout layout);

    /// Current KDE layout index for syncing xkb_state (-1 if unknown).
    int currentLayoutIndex() const;

private:
    const DaemonConfig& cfg_;
    bool runCmdOr(const std::string& cmd, bool fallbackKde, int kdeIndex);
    bool kdeSetLayout(int index);

    void invalidateLayoutCache();

    mutable std::chrono::steady_clock::time_point layoutPollTs_{};
    mutable int                               cachedLayout_{-1};
    /// After getLayout fails, avoid spawning busctl/qdbus on every key (was freezing the daemon).
    mutable std::chrono::steady_clock::time_point layoutBackoffUntil_{};
};

} // namespace punto
