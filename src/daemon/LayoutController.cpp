#include "LayoutController.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace punto {

namespace {

/// Without DBUS_SESSION_BUS_ADDRESS (e.g. stripped by sg), busctl can hang on connect.
static bool sessionBusEnvPresent() {
    return std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr;
}

/// busctl prints "u 0"; gdbus may print "(uint32 0,)"; qdbus may print "0".
static int parseLayoutIndexLine(const char* s) {
    if (!s) return -1;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    // busctl: "u 0"
    if (s[0] == 'u' && (s[1] == ' ' || s[1] == '\t')) {
        s += 2;
        while (*s == ' ' || *s == '\t') ++s;
    } else {
        // gdbus / dbus-send style: "(uint32 0)" or "uint32 0"
        const char* p = std::strstr(s, "uint32");
        if (p) {
            s = p + 6;
            while (*s == ' ' || *s == '\t' || *s == '(') ++s;
        }
    }
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || v < 0 || v > 31) return -1;
    return static_cast<int>(v);
}

} // namespace

LayoutController::LayoutController(const DaemonConfig& cfg) : cfg_(cfg) {}

void LayoutController::invalidateLayoutCache() {
    cachedLayout_ = -1;
}

bool LayoutController::kdeSetLayout(int index) {
    if (!sessionBusEnvPresent()) return false;
    {
        std::ostringstream oss;
        // busctl prints the D-Bus reply (e.g. "b true") on stdout — hide it when logging is dup'd to a file.
        oss << "timeout 0.12s busctl --user call org.kde.keyboard /Layouts org.kde.KeyboardLayouts setLayout u "
            << index << " &>/dev/null";
        if (std::system(oss.str().c_str()) == 0) return true;
    }
    {
        std::ostringstream oss;
        oss << "timeout 0.12s qdbus6 org.kde.keyboard /Layouts org.kde.KeyboardLayouts.setLayout " << index
            << " &>/dev/null";
        if (std::system(oss.str().c_str()) == 0) return true;
    }
    {
        std::ostringstream oss;
        oss << "timeout 0.12s qdbus org.kde.keyboard /Layouts org.kde.KeyboardLayouts.setLayout " << index
            << " &>/dev/null";
        if (std::system(oss.str().c_str()) == 0) return true;
    }
    {
        std::ostringstream oss;
        oss << "timeout 0.12s qdbus6 org.kde.keyboardlayout /Layout org.kde.keyboardlayout.setLayout " << index
            << " &>/dev/null";
        if (std::system(oss.str().c_str()) == 0) return true;
    }
    {
        std::ostringstream oss;
        oss << "timeout 0.12s qdbus org.kde.keyboardlayout /Layout org.kde.keyboardlayout.setLayout " << index
            << " &>/dev/null";
        return std::system(oss.str().c_str()) == 0;
    }
}

bool LayoutController::runCmdOr(const std::string& cmd, bool tryKde, int kdeIndex) {
    if (!cmd.empty()) return std::system(cmd.c_str()) == 0;
    if (tryKde) return kdeSetLayout(kdeIndex);
    return false;
}

void LayoutController::setEnglish() {
    if (!cfg_.cmdLayoutEnglish.empty()) {
        (void)runCmdOr(cfg_.cmdLayoutEnglish, false, 0);
        invalidateLayoutCache();
        return;
    }
    if (runCmdOr("", true, cfg_.layoutIndexEnglish)) {
        cachedLayout_ = cfg_.layoutIndexEnglish;
        layoutPollTs_ = std::chrono::steady_clock::now();
    }
}

void LayoutController::setRussian() {
    if (!cfg_.cmdLayoutRussian.empty()) {
        (void)runCmdOr(cfg_.cmdLayoutRussian, false, 0);
        invalidateLayoutCache();
        return;
    }
    if (runCmdOr("", true, cfg_.layoutIndexRussian)) {
        cachedLayout_ = cfg_.layoutIndexRussian;
        layoutPollTs_ = std::chrono::steady_clock::now();
    }
}

void LayoutController::setForCharLayout(CharMapping::Layout layout) {
    if (layout == CharMapping::Layout::Russian) setRussian();
    else if (layout == CharMapping::Layout::English) setEnglish();
}

int LayoutController::currentLayoutIndex() const {
    if (!sessionBusEnvPresent()) return -1;

    const auto now = std::chrono::steady_clock::now();
    if (now < layoutBackoffUntil_) return -1;

    if (cachedLayout_ >= 0 && (now - layoutPollTs_) < std::chrono::milliseconds(250))
        return cachedLayout_;
    layoutPollTs_ = now;

    static const char* cmds[] = {
        "timeout 0.12s busctl --user call org.kde.keyboard /Layouts org.kde.KeyboardLayouts getLayout 2>/dev/null",
        "timeout 0.12s qdbus6 org.kde.keyboard /Layouts org.kde.KeyboardLayouts.getLayout 2>/dev/null",
        "timeout 0.12s qdbus org.kde.keyboard /Layouts org.kde.KeyboardLayouts.getLayout 2>/dev/null",
        "timeout 0.12s qdbus6 org.kde.keyboardlayout /Layout org.kde.keyboardlayout.layout 2>/dev/null",
        "timeout 0.12s qdbus org.kde.keyboardlayout /Layout org.kde.keyboardlayout.layout 2>/dev/null",
        nullptr,
    };

    for (int i = 0; cmds[i]; ++i) {
        FILE* p = popen(cmds[i], "r");
        if (!p) continue;
        char buf[256];
        if (fgets(buf, sizeof(buf), p)) {
            int v = parseLayoutIndexLine(buf);
            (void)pclose(p);
            if (v >= 0) {
                cachedLayout_ = v;
                layoutBackoffUntil_ = {};
                return cachedLayout_;
            }
        } else {
            (void)pclose(p);
        }
    }
    cachedLayout_ = -1;
    layoutBackoffUntil_ = now + std::chrono::seconds(4);
    return -1;
}

} // namespace punto
