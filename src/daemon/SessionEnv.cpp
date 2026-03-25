#include "SessionEnv.h"

#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <vector>

namespace punto {

namespace {

pid_t readParentPid(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    char  line[256];
    pid_t ppid = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "PPid:", 5) == 0) {
            ppid = static_cast<pid_t>(std::strtol(line + 5, nullptr, 10));
            break;
        }
    }
    std::fclose(f);
    return ppid;
}

bool shouldImportKey(const char* key) {
    return std::strcmp(key, "WAYLAND_DISPLAY") == 0 || std::strcmp(key, "XDG_RUNTIME_DIR") == 0
           || std::strcmp(key, "DBUS_SESSION_BUS_ADDRESS") == 0 || std::strcmp(key, "DISPLAY") == 0;
}

void applyEnvironFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return;
    }
    long sz = std::ftell(f);
    if (sz <= 0 || sz > 2 * 1024 * 1024) {
        std::fclose(f);
        return;
    }
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(static_cast<size_t>(sz) + 1);
    size_t            n = std::fread(buf.data(), 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    buf[n] = '\0';

    for (char* p = buf.data(); p < buf.data() + static_cast<ptrdiff_t>(n) && *p;) {
        char* eq = std::strchr(p, '=');
        if (!eq) break;
        *eq   = '\0';
        char* key = p;
        char* val = eq + 1;
        if (shouldImportKey(key)) {
            if (!std::getenv(key) || !std::getenv(key)[0]) (void)::setenv(key, val, 1);
        }
        p = val + std::strlen(val) + 1;
    }
}

} // namespace

void ensureSessionEnvFromParents() {
    pid_t p = ::getppid();
    for (int depth = 0; depth < 12 && p > 1; ++depth) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/environ", static_cast<int>(p));
        applyEnvironFile(path);
        p = readParentPid(p);
    }

    if (std::getenv("WAYLAND_DISPLAY") && std::getenv("WAYLAND_DISPLAY")[0]) return;

    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return;
    DIR* d = ::opendir(rt);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (std::strncmp(ent->d_name, "wayland-", 8) != 0) continue;
        (void)::setenv("WAYLAND_DISPLAY", ent->d_name, 1);
        break;
    }
    closedir(d);
}

} // namespace punto
