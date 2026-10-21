#include "ActionSound.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <sys/wait.h>

namespace punto {

namespace {

bool readable(const std::string& path) {
    return !path.empty() && ::access(path.c_str(), R_OK) == 0;
}

std::string soundPath() {
    if (const char* env = std::getenv("ROCKET_SWITCHER_SOUND")) {
        if (env[0] && readable(env)) return env;
    }
    const char* fixed[] = {
        "/usr/share/rocket-switcher/sounds/switch.wav",
        "/usr/local/share/rocket-switcher/sounds/switch.wav",
    };
    for (const char* p : fixed) {
        if (readable(p)) return p;
    }

    char exe[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        std::string dir(exe);
        const auto slash = dir.find_last_of('/');
        if (slash != std::string::npos) dir.resize(slash);
        const std::string beside = dir + "/../share/rocket-switcher/sounds/switch.wav";
        if (readable(beside)) return beside;
    }

    if (readable("data/sounds/switch.wav")) return "data/sounds/switch.wav";
    if (readable("../data/sounds/switch.wav")) return "../data/sounds/switch.wav";
    return {};
}

void silenceAndCloseExtraFds() {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) ::close(devnull);
    }
    for (int fd = STDERR_FILENO + 1; fd < 1024; ++fd) ::close(fd);
}

} // namespace

void playActionSound(bool enabled) {
    if (!enabled) return;
    const std::string path = soundPath();
    if (path.empty()) return;

    const pid_t mid = ::fork();
    if (mid < 0) return;
    if (mid == 0) {
        ::setsid();
        const pid_t inner = ::fork();
        if (inner == 0) {
            silenceAndCloseExtraFds();
            ::execlp("paplay", "paplay", path.c_str(), static_cast<char*>(nullptr));
            ::execlp("pw-play", "pw-play", path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(inner < 0 ? 1 : 0);
    }
    // The intermediate child exits at once. The player is reparented and
    // does not stall the keyboard loop.
    int status = 0;
    while (::waitpid(mid, &status, 0) < 0 && errno == EINTR) {}
}

} // namespace punto
