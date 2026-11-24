#pragma once
#include "keefi/RankingStudies.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
struct RankingStudyCalculation {
    QString key;
    RankingStudy study=RankingStudy::None;
    ChartStudyObservation observation;
};
// Public immutable inputs only. One in-flight calculation, at most sixty
// active requests, and 128 completed results. clear() revokes the epoch.
class RankingStudyRunner final:public QObject {
    Q_OBJECT
public:
    using Calculator=std::function<RankingStudyValue(RankingStudy,const ChartStudyObservation&)>;
    explicit RankingStudyRunner(QObject* parent=nullptr,Calculator={});
    ~RankingStudyRunner() override;
    void setRequests(const QList<RankingStudyCalculation>&);
    RankingStudyValue value(const QString& key)const;
    void clear();
    int requestCount()const;
    int cacheSize()const;
    bool pending()const;
signals:
    void changed();
private:
    struct Private;std::unique_ptr<Private> d_;
};
}
