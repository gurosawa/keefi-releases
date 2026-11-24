#pragma once

#include <QString>

namespace keefi {
bool privateWindowsPath(const QString& path, bool newlyCreated, QString* error = nullptr);
bool createPrivateWindowsDirectory(const QString& path, QString* error);
}
