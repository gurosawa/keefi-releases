#pragma once
#include <QString>

namespace keefi {
struct ChartStudySearchMatch {
    bool matched = false;
    int rank = 0; // 3: name, 2: alias or initials, 1: concept, 0: no ranked match.
    QString reason;
};

QString chartStudySearchKey(const QString& text);
QString chartStudyChoseong(const QString& text);
ChartStudySearchMatch matchChartStudy(const QString& id, const QString& query);
}
