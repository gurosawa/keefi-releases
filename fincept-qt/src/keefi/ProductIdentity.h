#pragma once

#include <QString>

namespace keefi {

inline QString productName() {
    return QStringLiteral("KEEFI");
}

inline QString devApplicationName() {
    return productName() + QStringLiteral(" Dev");
}

inline bool isDevelopmentApplicationName(const QString& applicationName) {
    return applicationName == devApplicationName();
}

} // namespace keefi
