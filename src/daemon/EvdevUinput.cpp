#include "EvdevUinput.h"

#include <libevdev/libevdev.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <set>

#include <glob.h>

namespace punto {

namespace {

static bool nameLooksBad(const char* name) {
    if (!name || !*name) return false;
    std::string s(name);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.find("punto-switcher") != std::string::npos) return true;
    if (s.find("rocket-switcher") != std::string::npos) return true;
    if (s.find("wtype") != std::string::npos) return true;
    if (s.find("ydotool") != std::string::npos) return true;
    if (s.find("touchpad") != std::string::npos) return true;
    if (s.find("trackpad") != std::string::npos) return true;
    if (s.find("mouse") != std::string::npos && s.find("keyboard") == std::string::npos) return true;
    return false;
}

static bool looksLikePcKeyboard(libevdev* d) {
    if (!libevdev_has_event_type(d, EV_KEY)) return false;
    if (!libevdev_has_event_code(d, EV_KEY, KEY_A)) return false;
    if (!libevdev_has_event_code(d, EV_KEY, KEY_SPACE)) return false;
    if (!libevdev_has_event_code(d, EV_KEY, KEY_LEFTSHIFT)) return false;
    if (!libevdev_has_event_code(d, EV_KEY, KEY_ENTER)) return false;
    const char* n = libevdev_get_name(d);
    if (nameLooksBad(n)) return false;
    return true;
}

static bool probePath(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) != 0) {
        ::close(fd);
        return false;
    }
    const bool ok = looksLikePcKeyboard(dev);
    libevdev_free(dev);
    ::close(fd);
    return ok;
}

/// Same physical USB device often appears as both ...-usb-... and ...-usbv2-...; merge to one path (prefer usbv2).
static std::string usbIdentityKey(const std::string& p) {
    if (p.find("platform-") != std::string::npos) return std::string("platform:") + p;
    const auto u2 = p.find("-usbv2-");
    if (u2 != std::string::npos) return p.substr(0, u2) + p.substr(u2 + 7);
    const auto u = p.find("-usb-");
    if (u != std::string::npos) return p.substr(0, u) + p.substr(u + 5);
    return p;
}

static std::vector<std::string> dedupeUsbAliases(std::vector<std::string> paths) {
    std::unordered_map<std::string, std::string> best;
    for (const auto& p : paths) {
        const std::string k = usbIdentityKey(p);
        auto               it = best.find(k);
        if (it == best.end()) {
            best[k] = p;
            continue;
        }
        const bool haveV2 = it->second.find("usbv2") != std::string::npos;
        const bool pIsV2  = p.find("usbv2") != std::string::npos;
        if (pIsV2 && !haveV2) it->second = p;
    }
    std::vector<std::string> out;
    out.reserve(best.size());
    for (auto& e : best) out.push_back(e.second);
    std::sort(out.begin(), out.end());
    return out;
}

/// Same char device as another path (by-path symlink vs /dev/input/eventN).
static std::string inodeKey(const std::string& p) {
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) return p;
    return std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_ino));
}

static std::vector<std::string> findDefaultKeyboardPaths(bool verbose = true) {
    std::vector<std::string> fromByPath;
    glob_t                   g;
    ::memset(&g, 0, sizeof(g));
    if (::glob("/dev/input/by-path/*kbd*", GLOB_NOSORT, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) fromByPath.emplace_back(g.gl_pathv[i]);
        ::globfree(&g);
    }
    std::vector<std::string> okByPath;
    for (const auto& p : fromByPath) {
        if (probePath(p)) okByPath.push_back(p);
    }
    okByPath = dedupeUsbAliases(okByPath);

    std::unordered_set<std::string> seen;
    std::vector<std::string>        merged;
    auto                            add = [&](const std::string& p) {
        const std::string k = inodeKey(p);
        if (!seen.insert(k).second) return;
        merged.push_back(p);
    };

    for (const auto& p : okByPath) add(p);

    /* USB/BT dongle vs built-in: by-path can miss a second interface (e.g. Bluetooth K780). */
    for (int i = 0; i < 128; ++i) {
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (!probePath(path)) continue;
        add(path);
    }

    if (verbose && merged.size() > 1) {
        std::cerr << "rocket-switcher-daemon: opening ALL of these keyboards (grab each):";
        for (const auto& p : merged) std::cerr << ' ' << p;
        std::cerr << "\n";
    }
    if (!merged.empty()) return merged;

    if (verbose) {
        std::cerr << "rocket-switcher-daemon: no suitable keyboard under /dev/input/by-path/*kbd* or event0..127\n";
    }
    return {};
}

static std::set<std::string> inodeSetFromPaths(const std::vector<std::string>& paths) {
    std::set<std::string> out;
    for (const auto& p : paths) out.insert(inodeKey(p));
    return out;
}

} // namespace

EvdevUinput::~EvdevUinput() {
    shutdown();
}

void EvdevUinput::shutdown() {
    for (libevdev* d : devs_) {
        if (!d) continue;
        libevdev_grab(d, LIBEVDEV_UNGRAB);
        int fd = libevdev_get_fd(d);
        libevdev_free(d);
        if (fd >= 0) ::close(fd);
    }
    devs_.clear();
    if (uinputFd_ >= 0) {
        ioctl(uinputFd_, UI_DEV_DESTROY);
        ::close(uinputFd_);
        uinputFd_ = -1;
    }
}

bool EvdevUinput::init(const std::string& userPath) {
    std::vector<std::string> paths;
    if (userPath.empty()) {
        paths = findDefaultKeyboardPaths();
    } else {
        paths.push_back(userPath);
    }
    if (paths.empty()) return false;

    for (const auto& p : paths) {
        std::cerr << "rocket-switcher-daemon: keyboard device: " << p
                  << (userPath.empty() ? " (auto)\n" : "\n");
    }

    std::vector<libevdev*> opened;
    opened.reserve(paths.size());
    for (const auto& path : paths) {
        /* O_NONBLOCK required: poll() + drain loop; without it the second libevdev_next_event blocks forever. */
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            std::cerr << "rocket-switcher-daemon: open " << path << ": " << std::strerror(errno) << "\n";
            for (libevdev* d : opened) {
                int xfd = libevdev_get_fd(d);
                libevdev_free(d);
                if (xfd >= 0) ::close(xfd);
            }
            return false;
        }
        libevdev* d = nullptr;
        if (libevdev_new_from_fd(fd, &d) != 0) {
            std::cerr << "rocket-switcher-daemon: libevdev_new_from_fd(" << path << "): failed\n";
            ::close(fd);
            for (libevdev* od : opened) {
                int xfd = libevdev_get_fd(od);
                libevdev_free(od);
                if (xfd >= 0) ::close(xfd);
            }
            return false;
        }
        if (!looksLikePcKeyboard(d)) {
            int xfd = libevdev_get_fd(d);
            libevdev_free(d);
            if (xfd >= 0) ::close(xfd);
            continue;
        }
        opened.push_back(d);
    }
    if (opened.empty()) {
        std::cerr << "rocket-switcher-daemon: no valid keyboard devices opened\n";
        return false;
    }

    uinputFd_ = ::open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (uinputFd_ < 0) {
        std::cerr << "rocket-switcher-daemon: open /dev/uinput: " << std::strerror(errno) << "\n";
        for (libevdev* d : opened) {
            int xfd = libevdev_get_fd(d);
            libevdev_free(d);
            if (xfd >= 0) ::close(xfd);
        }
        return false;
    }

    ioctl(uinputFd_, UI_SET_EVBIT, EV_KEY);
    ioctl(uinputFd_, UI_SET_EVBIT, EV_SYN);
    ioctl(uinputFd_, UI_SET_EVBIT, EV_MSC);
    for (libevdev* d : opened) {
        for (int i = 0; i < KEY_MAX; ++i) {
            if (libevdev_has_event_code(d, EV_KEY, i)) ioctl(uinputFd_, UI_SET_KEYBIT, i);
        }
    }
    // Some nodes advertise an incomplete key mask; the kernel may drop EV_KEY writes for
    // bits that were never set. Always enable a full PC-104-ish set used for typing.
    static const int kEnsureKeyBits[] = {
        KEY_ESC, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS,
        KEY_EQUAL, KEY_BACKSPACE, KEY_TAB, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U,
        KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER, KEY_LEFTCTRL, KEY_A,
        KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE,
        KEY_GRAVE, KEY_LEFTSHIFT, KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
        KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RIGHTSHIFT, KEY_KPASTERISK, KEY_LEFTALT, KEY_SPACE,
        KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
        KEY_F10, KEY_F11, KEY_F12, KEY_NUMLOCK, KEY_SCROLLLOCK, KEY_KP7, KEY_KP8, KEY_KP9,
        KEY_KPMINUS, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KPPLUS, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP0,
        KEY_KPDOT, KEY_102ND, KEY_RIGHTALT, KEY_LINEFEED, KEY_HOME, KEY_UP, KEY_PAGEUP, KEY_LEFT,
        KEY_RIGHT, KEY_END, KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE, KEY_KPSLASH,
        KEY_RIGHTMETA, KEY_LEFTMETA,
    };
    for (unsigned int k : kEnsureKeyBits) {
        (void)ioctl(uinputFd_, UI_SET_KEYBIT, static_cast<int>(k));
    }

#ifdef UI_DEV_SETUP
    uinput_setup usetup{};
    std::memset(&usetup, 0, sizeof(usetup));
    std::strncpy(usetup.name, "rocket-switcher-daemon", sizeof(usetup.name) - 1);
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1d50;
    usetup.id.product = 0x600d;
    usetup.id.version = 1;
    if (ioctl(uinputFd_, UI_DEV_SETUP, &usetup) != 0) {
        std::cerr << "rocket-switcher-daemon: UI_DEV_SETUP: " << std::strerror(errno) << "\n";
        ::close(uinputFd_);
        uinputFd_ = -1;
        for (libevdev* d : opened) {
            int xfd = libevdev_get_fd(d);
            libevdev_free(d);
            if (xfd >= 0) ::close(xfd);
        }
        return false;
    }
#else
#error "rocket-switcher-daemon requires Linux uinput UI_DEV_SETUP (kernel 4.4+)"
#endif

    if (ioctl(uinputFd_, UI_DEV_CREATE) != 0) {
        std::cerr << "rocket-switcher-daemon: UI_DEV_CREATE: " << std::strerror(errno) << "\n";
        ::close(uinputFd_);
        uinputFd_ = -1;
        for (libevdev* d : opened) {
            int xfd = libevdev_get_fd(d);
            libevdev_free(d);
            if (xfd >= 0) ::close(xfd);
        }
        return false;
    }

    for (libevdev* d : opened) {
        if (libevdev_grab(d, LIBEVDEV_GRAB) != 0) {
            std::cerr << "rocket-switcher-daemon: libevdev_grab: " << std::strerror(errno) << "\n";
            ioctl(uinputFd_, UI_DEV_DESTROY);
            ::close(uinputFd_);
            uinputFd_ = -1;
            for (libevdev* od : opened) {
                int xfd = libevdev_get_fd(od);
                libevdev_free(od);
                if (xfd >= 0) ::close(xfd);
            }
            return false;
        }
    }

    devs_ = std::move(opened);
    for (libevdev* d : devs_) {
        const char* nm = libevdev_get_name(d);
        std::cerr << "rocket-switcher-daemon: keyboard name: \"" << (nm ? nm : "?") << "\"\n";
    }

    return true;
}

bool EvdevUinput::hasDeviceSetChanged(const std::string& userPath) const {
    std::vector<std::string> paths;
    if (userPath.empty()) paths = findDefaultKeyboardPaths(false);
    else paths.push_back(userPath);

    std::set<std::string> wanted = inodeSetFromPaths(paths);
    std::set<std::string> have;
    for (libevdev* d : devs_) {
        if (!d) continue;
        const int fd = libevdev_get_fd(d);
        if (fd < 0) continue;
        struct stat st {};
        if (::fstat(fd, &st) != 0) continue;
        have.insert(std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
                    std::to_string(static_cast<unsigned long long>(st.st_ino)));
    }
    // A Bluetooth keyboard drops its event node on a link blip and comes back
    // as a new node a few seconds later. Both a missing node and a new one
    // must reopen the set; the caller waits for two stable probes so a
    // device that is still enumerating does not cause a reopen loop.
    return have != wanted;
}

void EvdevUinput::forward(const input_event& ev) {
    if (uinputFd_ < 0) return;
    const ssize_t nw = ::write(uinputFd_, &ev, sizeof(ev));
    if (nw != static_cast<ssize_t>(sizeof(ev))) {
        std::cerr << "rocket-switcher-daemon: uinput write failed (" << std::strerror(errno) << ")\n";
    }
    // libinput often expects SYN_REPORT immediately after each key change; raw evdev may send
    // SYN in the next read(), but batching KEY+SYN avoids dropped keys on some compositors.
    if (ev.type == EV_KEY) {
        input_event syn{};
        syn.type  = EV_SYN;
        syn.code  = SYN_REPORT;
        syn.value = 0;
        const ssize_t ns = ::write(uinputFd_, &syn, sizeof(syn));
        if (ns != static_cast<ssize_t>(sizeof(syn))) {
            std::cerr << "rocket-switcher-daemon: uinput SYN write failed (" << std::strerror(errno)
                      << ")\n";
        }
    }
}

void EvdevUinput::emitKey(unsigned int code, int value) {
    input_event ev{};
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    forward(ev);
}

void EvdevUinput::sync() {
    input_event ev{};
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    forward(ev);
}

int EvdevUinput::pollFd() const {
    if (devs_.empty()) return -1;
    return libevdev_get_fd(devs_.front());
}

bool EvdevUinput::capsLockLedOn() const {
    for (libevdev* d : devs_) {
        if (!d) continue;
        if (!libevdev_has_event_type(d, EV_LED)) continue;
        if (!libevdev_has_event_code(d, EV_LED, LED_CAPSL)) continue;
        if (libevdev_get_event_value(d, EV_LED, LED_CAPSL) != 0) return true;
    }
    return false;
}

} // namespace punto
