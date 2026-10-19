#include "DaemonClient.h"

#include "../core/DaemonEndpoint.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool waitFd(int fd, short events, int timeoutMs) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;
    for (;;) {
        const int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0 && (pfd.revents & events);
    }
}

} // namespace

QString daemonCommand(const QString& command) {
    const std::string path = punto::daemonControlSocketPath();
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {};

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return {};
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

    const int conn = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (conn < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return {};
    }
    if (conn < 0 && !waitFd(fd, POLLOUT, 400)) {
        ::close(fd);
        return {};
    }
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
        ::close(fd);
        return {};
    }

    const QByteArray line = command.toUtf8() + '\n';
    if (!waitFd(fd, POLLOUT, 400)) {
        ::close(fd);
        return {};
    }
    const ssize_t wr = ::write(fd, line.constData(), static_cast<size_t>(line.size()));
    if (wr != line.size()) {
        ::close(fd);
        return {};
    }

    if (!waitFd(fd, POLLIN, 400)) {
        ::close(fd);
        return {};
    }
    char buf[128];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return {};
    buf[n] = '\0';
    QString reply = QString::fromUtf8(buf).trimmed();
    return reply;
}
