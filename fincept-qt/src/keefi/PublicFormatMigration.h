#pragma once
#include <QJsonArray>
#include <QString>

namespace keefi {
// Maps only registered machine format identifiers. It never rewrites user text.
QString canonicalPublicFormat(const QString& format);
// Called only on the owned, inactive staging copy before its final manifest.
// Unknown INI keys and every byte outside changed known-value lines are preserved.
bool convertStagedPublicSettings(const QString& stageDirectory, QJsonArray* evidence,
                                 QString* error = nullptr);
} // namespace keefi
