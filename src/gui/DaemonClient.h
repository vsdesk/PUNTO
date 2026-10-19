#pragma once

#include <QString>

// One short command to the running daemon. Empty when it is not listening.
// Commands: "STATUS", "AUTO 1", "AUTO 0", "RELOAD".
QString daemonCommand(const QString& command);
