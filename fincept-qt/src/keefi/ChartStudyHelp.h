#pragma once
#include <QString>
#include <QStringList>
#include <optional>

namespace keefi {
// Public, static descriptions of the implemented display studies. Runtime
// source timestamps, values and missing-data reasons remain the caller's DTO.
struct ChartStudyHelp {
    QString id, title, brief, inputs, reading, limitations, calculation;
    QStringList markets;
    bool historicalOnly = false;
};
QString chartStudyName(const QString& id);
QStringList chartStudyHelpIds();
std::optional<ChartStudyHelp> chartStudyHelp(const QString& id);
}
