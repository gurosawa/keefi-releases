#include "keefi/KiwoomRequestBudget.h"
#include "keefi/TradingRequestLimits.h"
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <limits>

namespace keefi {
namespace {
bool valid(KiwoomBudgetRequest request) {
    return request==KiwoomBudgetRequest::General||request==KiwoomBudgetRequest::UsCatalogue;
}
qint64 add(qint64 now,qint64 duration) {
    return duration>std::numeric_limits<qint64>::max()-now?std::numeric_limits<qint64>::max():now+duration;
}
}
struct KiwoomRequestBudget::Private {
    Clock clock;
    std::shared_ptr<TradingRequestLimits> limits;
    QMutex mutex;
    QList<QPointer<QObject>> pending;
    QPointer<QObject> owner;
    quint64 active=0,next=1;
    qint64 generalUntil=0,catalogueUntil=0,cooldownUntil=0,lastNow=0;
    bool observed=false;
    void prune() {
        pending.erase(std::remove_if(pending.begin(),pending.end(),[](const auto& item){return item.isNull();}),pending.end());
        if(active&&owner.isNull())active=0;
    }
    bool time(qint64& now) {
        now=clock();
        if(now<0)return false;
        const bool forward=!observed||now>=lastNow;
        lastNow=observed?qMax(lastNow,now):now;observed=true;
        return forward;
    }
    qint64 remaining(qint64 now,KiwoomBudgetRequest request) const {
        qint64 wait=qMax<qint64>(0,qMax(generalUntil,cooldownUntil)-now);
        if(request==KiwoomBudgetRequest::UsCatalogue)wait=qMax(wait,qMax<qint64>(0,catalogueUntil-now));
        return qMax(wait,limits->remainingMs(QUrl("https://api.kiwoom.com")));
    }
};
KiwoomRequestBudget::KiwoomRequestBudget(Clock clock, std::shared_ptr<TradingRequestLimits> limits)
    : d_(std::make_unique<Private>()) {
    d_->clock=std::move(clock);d_->limits=limits?std::move(limits):TradingRequestLimits::shared();
    if(!d_->clock)d_->clock=[] {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();};
}
KiwoomRequestBudget::~KiwoomRequestBudget() = default;
std::shared_ptr<KiwoomRequestBudget> KiwoomRequestBudget::shared() {
    static const auto value = std::make_shared<KiwoomRequestBudget>();
    return value;
}
void KiwoomRequestBudget::setBrokerPending(QObject* owner,bool pending) {
    QMutexLocker lock(&d_->mutex);d_->prune();if(!owner)return;
    const auto found=std::find_if(d_->pending.begin(),d_->pending.end(),[owner](const auto& item){return item.data()==owner;});
    if(pending&&found==d_->pending.end())d_->pending.append(owner);
    else if(!pending&&found!=d_->pending.end())d_->pending.erase(found);
}
KiwoomBudgetDecision KiwoomRequestBudget::tryAcquire(QObject* owner,KiwoomBudgetLane lane,KiwoomBudgetRequest request) {
    QMutexLocker lock(&d_->mutex);d_->prune();
    if(!owner||!valid(request)||(lane!=KiwoomBudgetLane::ExistingBroker&&lane!=KiwoomBudgetLane::Display))return {};
    qint64 now=0;if(!d_->time(now))return {0,GeneralSpacingMs};
    const auto wait=d_->remaining(now,request);
    if(d_->active||(lane==KiwoomBudgetLane::Display&&!d_->pending.isEmpty()))return {0,qMax(GeneralSpacingMs,wait)};
    if(wait>0)return {0,wait};
    const auto charge=request==KiwoomBudgetRequest::UsCatalogue?UsCatalogueSpacingMs:GeneralSpacingMs;
    if(now>std::numeric_limits<qint64>::max()-charge||d_->next==std::numeric_limits<quint64>::max())return {0,GeneralSpacingMs};
    const auto lease=d_->next++;d_->active=lease;d_->owner=owner;
    d_->generalUntil=now+GeneralSpacingMs;
    if(request==KiwoomBudgetRequest::UsCatalogue)d_->catalogueUntil=now+UsCatalogueSpacingMs;
    return {lease,0};
}
bool KiwoomRequestBudget::finish(quint64 lease,qint64 additionalCooldownMs) {
    QMutexLocker lock(&d_->mutex);d_->prune();
    if(!lease||lease!=d_->active||additionalCooldownMs<0)return false;
    qint64 now=0;d_->time(now);now=d_->observed?d_->lastNow:0;
    d_->active=0;d_->owner.clear();
    if(additionalCooldownMs)d_->cooldownUntil=qMax(d_->cooldownUntil,add(now,additionalCooldownMs));
    return true;
}
qint64 KiwoomRequestBudget::remainingMs(KiwoomBudgetRequest request) const {
    QMutexLocker lock(&d_->mutex);d_->prune();
    if(!valid(request))return GeneralSpacingMs;
    qint64 now=0;if(!d_->time(now))return GeneralSpacingMs;
    const auto wait=d_->remaining(now,request);
    return d_->active?qMax(GeneralSpacingMs,wait):wait;
}
}
