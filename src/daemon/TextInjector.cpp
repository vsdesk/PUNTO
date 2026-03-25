#include "TextInjector.h"

#include "SessionEnv.h"
#include "../core/Utf8Utils.h"

#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace punto {

namespace {

static void logWtypeFailure(int st) {
    const char* wl = std::getenv("WAYLAND_DISPLAY");
    if (WIFEXITED(st)) {
        std::fprintf(stderr,
                     "punto-switcher-daemon: text inject failed (wtype exit=%d, WAYLAND_DISPLAY=%s)\n",
                     WEXITSTATUS(st), (wl && wl[0]) ? wl : "(unset)");
    } else {
        std::fprintf(stderr, "punto-switcher-daemon: text inject failed (wtype did not exit normally)\n");
    }
    std::fflush(stderr);
}

bool injectViaForkSingle(const std::string& utf8, bool silent) {
    if (utf8.empty()) return true;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execl("/usr/bin/wtype", "wtype", utf8.c_str(), static_cast<char*>(nullptr));
        execlp("wtype", "wtype", utf8.c_str(), static_cast<char*>(nullptr));
        execl("/usr/bin/ydotool", "ydotool", "type", utf8.c_str(), static_cast<char*>(nullptr));
        execlp("ydotool", "ydotool", "type", utf8.c_str(), static_cast<char*>(nullptr));
        execl("/usr/bin/xdotool", "xdotool", "type", "--", utf8.c_str(), static_cast<char*>(nullptr));
        execlp("xdotool", "xdotool", "type", "--", utf8.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return false;
    const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok && !silent) logWtypeFailure(st);
    return ok;
}

} // namespace

bool injectUtf8Text(const std::string& utf8) {
    if (utf8.empty()) return true;

    ensureSessionEnvFromParents();

    if (injectViaForkSingle(utf8, false)) return true;

    std::u32string u32 = utf8_to_utf32(utf8);
    for (size_t i = 0; i < u32.size(); ++i) {
        std::string one = utf32_to_utf8(std::u32string(1, u32[i]));
        if (!injectViaForkSingle(one, true)) return false;
        if (i + 1 < u32.size()) ::usleep(1500);
    }
    return true;
}

} // namespace punto
