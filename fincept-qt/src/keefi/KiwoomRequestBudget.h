#pragma once

#include <QObject>
#include <QtGlobal>
#include <functional>
#include <memory>

namespace keefi {
class TradingRequestLimits;
enum class KiwoomBudgetLane { ExistingBroker, Display };
enum class KiwoomBudgetRequest { General, UsCatalogue }; // usa10099 only.
struct KiwoomBudgetDecision {
    quint64 lease = 0; // A nonzero opaque grant is the sole permission to dispatch.
    qint64 waitMs = 0; // Zero with no lease means invalid input; never dispatch.
};

// Local, origin-wide scheduling only. Exact request authorization stays at callers.
// No token, symbol, account, request body, cursor or response is retained here.
class KiwoomRequestBudget final {
  public:
    using Clock = std::function<qint64()>;
    static constexpr qint64 GeneralSpacingMs = 400;
    static constexpr qint64 UsCatalogueSpacingMs = 12000;
    explicit KiwoomRequestBudget(Clock monotonic = {},
                                std::shared_ptr<TradingRequestLimits> limits = {});
    ~KiwoomRequestBudget();
    KiwoomRequestBudget(const KiwoomRequestBudget&) = delete;
    KiwoomRequestBudget& operator=(const KiwoomRequestBudget&) = delete;
    static std::shared_ptr<KiwoomRequestBudget> shared();

    void setBrokerPending(QObject* existingOwner, bool pending);
    KiwoomBudgetDecision tryAcquire(QObject* owner, KiwoomBudgetLane,
                                    KiwoomBudgetRequest = KiwoomBudgetRequest::General);
    // Only a currently active matching lease may release or publish business pause.
    // A second finish/late generation cannot release a replacement request.
    bool finish(quint64 lease, qint64 additionalCooldownMs = 0);
    // Temporal deadlines + in-flight status; pending priority is lane-specific and
    // tested by tryAcquire. No authorization borrow is needed to poll this value.
    qint64 remainingMs(KiwoomBudgetRequest = KiwoomBudgetRequest::General) const;

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
