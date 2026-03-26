#pragma once

#include <linux/input.h>

#include <string>
#include <vector>

struct libevdev;

namespace punto {

class EvdevUinput {
public:
    EvdevUinput() = default;
    EvdevUinput(const EvdevUinput&) = delete;
    EvdevUinput& operator=(const EvdevUinput&) = delete;

    ~EvdevUinput();

    /// Open one or more keyboard devices and create one uinput sink. Returns false on error.
    bool init(const std::string& devicePath);
    /// Release grabs/devices/uinput; object can be re-initialized with init().
    void shutdown();
    /// Detect keyboard device set change (e.g. BT reconnect with new event node).
    bool hasDeviceSetChanged(const std::string& devicePath) const;

    void forward(const input_event& ev);
    void emitKey(unsigned int code, int value); // 0 release, 1 press, 2 repeat
    void sync();

    int pollFd() const;

    /// First opened device (compat); may be null if init failed.
    libevdev* dev() const { return devs_.empty() ? nullptr : devs_.front(); }

    const std::vector<libevdev*>& devices() const { return devs_; }

private:
    int                       uinputFd_ = -1;
    std::vector<libevdev*> devs_;
};

} // namespace punto
