#include "DaemonConfig.h"
#include "PuntoDaemon.h"
#include "SessionEnv.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

/// Prevent two copies (e.g. autostart + manual) from grabbing the same keyboard.
static bool acquireSingleInstanceLock() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    std::string path;
    if (rt && *rt)
        path = std::string(rt) + "/punto-switcher-daemon.lock";
    else
        path = std::string("/tmp/punto-switcher-daemon.") + std::to_string(getuid()) + ".lock";

    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return false;
    }
    // Keep fd open for process lifetime so the lock remains held.
    static int lockFd = -1;
    lockFd = fd;
    (void)lockFd;
    return true;
}

int main(int argc, char** argv) {
    /* Before LayoutController / wtype: sg(1) drops DBUS + Wayland; pull from parent /proc environ. */
    punto::ensureSessionEnvFromParents();

    const char* logPath = nullptr;
    /* sg input -c often drops the parent shell's environment; --debug does not rely on export. */
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--debug") == 0 || std::strcmp(argv[i], "-d") == 0) {
            (void)setenv("PUNTO_DEBUG", "1", 1);
        } else if (std::strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            logPath = argv[++i];
        }
    }

    if (logPath) {
        FILE* f = std::fopen(logPath, "a");
        if (f) {
            int fd = fileno(f);
            (void)dup2(fd, STDERR_FILENO);
            (void)dup2(fd, STDOUT_FILENO);
        } else {
            std::perror("punto-switcher-daemon: --log-file");
        }
    }
    /* Line-buffered stderr can hide PUNTO_DEBUG until flush when not a TTY. */
    (void)setvbuf(stderr, nullptr, _IONBF, 0);

    if (!acquireSingleInstanceLock()) {
        std::cerr << "punto-switcher-daemon: another instance is already running (lock held)\n";
        return 1;
    }

    const punto::DaemonConfig cfg = punto::DaemonConfig::loadDefault();
    punto::PuntoDaemon            daemon(cfg);
    return daemon.run();
}
