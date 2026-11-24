#include "keefi/PlainToolTip.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiVisuals.h"
#include <QDateTime>
#include <QPainter>

namespace keefi {

QString connectionMarketName(const QString& market) {
    if (market == "spot") return QObject::tr("현물");
    if (market == "usdm_futures") return QObject::tr("USDⓈ-M 선물");
    if (market == "okx_unified") return QObject::tr("OKX 통합계정");
    if (market == "okx_futures") return QObject::tr("선물 · SWAP/FUTURES");
    if (market == "hyperliquid_perps") return QObject::tr("무기한 선물(Hyperliquid)");
    if (market == "domestic_equity") return QObject::tr("국내주식");
    if (market == "solana") return QStringLiteral("Solana");
    return market;
}

QString connectionEnvironmentName(const QString& environment) {
    if (environment == "live") return QObject::tr("실전");
    if (environment == "demo" || environment == "paper") return QObject::tr("모의");
    if (environment == "testnet") return QObject::tr("테스트넷");
    if (environment == "mainnet") return QObject::tr("메인넷");
    return environment;
}

bool connectionNeedsAttention(const PortfolioConnection& c) {
    return c.state == "failed" || c.state == "stopped" ||
           c.state == "authentication_required" || c.state == "account_required";
}

QString connectionStateName(const PortfolioConnection& c) {
    if (AccountModel::isProviderSuspended(c.account.provider)) return QObject::tr("연결 비활성");
    if (c.state == "complete") return QObject::tr("조회 완료");
    if (c.state == "authentication_required") return QObject::tr("재인증 필요");
    if (c.state == "account_required") return QObject::tr("계좌 확인 필요");
    if (c.state == "failed") return QObject::tr("조회 실패");
    if (c.state == "stopped") return QObject::tr("조회 중단");
    if (c.state == "unsupported") return QObject::tr("조회 미지원");
    if (c.receivedAtMs > 0) return QObject::tr("조회 완료");
    if (c.active) return QObject::tr("첫 조회 중");
    if (c.state == "manual") return QObject::tr("조회 사용 중");
    return QObject::tr("조회 대기");
}

std::optional<WalletReadResult> phantomWalletReadResult(const PortfolioConnection& c, const PortfolioController& portfolio) {
    if (c.account.provider != "phantom" ||
        (c.account.market != "solana" && c.account.market != "hyperliquid_perps")) return {};
    const auto network = c.account.market == "solana" ? QStringLiteral("Solana") : QStringLiteral("Hyperliquid");
    if (c.active) return WalletReadResult{QObject::tr("%1 조회 중").arg(network), c.stage};
    if (c.state != "complete") {
        QString reason = c.stage;
        const auto prefix = QObject::tr("조회 실패 · ");
        if (reason.startsWith(prefix)) reason.remove(0, prefix.size());
        const auto status = connectionNeedsAttention(c) || c.state == "unsupported" ? connectionStateName(c)
            : c.state == "manual" ? QObject::tr("조회 사용 중") : QObject::tr("조회 대기");
        return WalletReadResult{QObject::tr("%1 %2").arg(network, status), reason};
    }
    const auto summary = portfolio.completedWalletRead(c.account.id);
    const auto partial = QObject::tr("%1 부분 조회").arg(network);
    if (!summary) return WalletReadResult{partial, QObject::tr("이 연결의 최신 잔고를 확인하지 못했어요.")};
    const auto complete = QObject::tr("%1 조회 완료").arg(network);
    if (c.account.market == "solana") {
        const auto held = summary->heldAssets;
        if (!summary->quantitiesConfirmed || !summary->balanceComplete)
            return WalletReadResult{partial, QObject::tr("보유 수량을 모두 확인하지 못했어요."), held > 0};
        const auto detail = held ? QObject::tr("보유 자산 %1종").arg(held)
                                 : QObject::tr("조회 범위에 보유 자산 없음");
        return WalletReadResult{complete, detail + QObject::tr("\n조회 범위 · SOL, SPL, Token-2022"), held > 0};
    }
    const auto& balance = summary->usdcBalance;
    const auto positions = summary->openPositions;
    if (!summary->quantitiesConfirmed)
        return WalletReadResult{partial, QObject::tr("USDC 잔고와 열린 포지션을 모두 확인하지 못했어요."), positions > 0};
    const bool hasBalance = decimal::positive(balance.startsWith('-') ? balance.mid(1) : balance);
    const auto positionCount = QObject::tr("열린 포지션 %1개").arg(positions);
    if (!summary->balanceComplete)
        return WalletReadResult{partial, QObject::tr("공유 잔고 %1 USDC · %2\n통합 계좌의 전체 순자산은 미확정이에요.").arg(balance, positionCount),
                               hasBalance || positions > 0};
    if (!hasBalance && !positions)
        return WalletReadResult{complete, QObject::tr("기본 USDC 선물 계좌에 잔고와 열린 포지션 없음")};
    return WalletReadResult{complete, QObject::tr("기본 선물 계좌가치 %1 USDC · %2").arg(balance, positionCount), true};
}

QString connectionLastSuccess(qint64 receivedAtMs) {
    if (receivedAtMs <= 0) return QObject::tr("아직 조회 내역 없음");
    const auto time = QDateTime::fromMSecsSinceEpoch(receivedAtMs);
    return QObject::tr("최근 성공 %1").arg(time.toString(
        time.date() == QDate::currentDate() ? "HH:mm:ss" : "MM-dd HH:mm"));
}

int ConnectionListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : groups_.size();
}

ConnectionGroup ConnectionListModel::groupAt(int row) const {
    return row >= 0 && row < groups_.size() ? groups_[row] : ConnectionGroup{};
}

int ConnectionListModel::rowForAccount(const QString& id) const {
    for (int row = 0; row < groups_.size(); ++row)
        for (const auto& account : groups_[row].accounts)
            if (account.id == id) return row;
    return -1;
}

QVariant ConnectionListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= groups_.size()) return {};
    const auto& group = groups_[index.row()];
    const auto& first = group.accounts.first();
    const auto* provider = AccountModel::provider(first.provider);
    // One line under the name: the service, its markets when it has several,
    // then what needs attention or when it was last read. Live is the only
    // environment for new connections, so only an older one is named.
    QStringList parts{provider ? provider->label : first.provider};
    if (first.environment != "live" && first.environment != "mainnet") parts.append(connectionEnvironmentName(first.environment));
    if (group.accounts.size() > 1) {
        QStringList markets;
        for (const auto& account : group.accounts) markets.append(connectionMarketName(account.market));
        parts.append(markets.join(QStringLiteral("·")));
    }
    QString status;
    qint64 latest = 0;
    for (const auto& account : group.accounts) {
        if (AccountModel::isProviderSuspended(account.provider)) { status = QObject::tr("연결 비활성"); break; }
        const auto c = states_.value(account.id);
        if (connectionNeedsAttention(c)) { status = connectionStateName(c); break; }
        latest = qMax(latest, c.receivedAtMs);
    }
    if (status.isEmpty() && latest > 0) {
        const auto time = QDateTime::fromMSecsSinceEpoch(latest);
        status = QObject::tr("%1 조회").arg(time.toString(time.date() == QDate::currentDate() ? "HH:mm" : "MM-dd HH:mm"));
    }
    if (status.isEmpty()) status = connectionStateName(states_.value(first.id));
    parts.append(status);
    const auto context = parts.join(QStringLiteral(" · "));
    if (role == Qt::DisplayRole) return first.alias;
    if (role == ProviderRole) return first.provider;
    if (role == ContextRole) return context;
    QStringList lines;
    QList<bool> problems;
    for (const auto& account : group.accounts) {
        auto c = states_.value(account.id);
        c.account = account;
        lines.append(connectionMarketName(account.market) + " · " + connectionStateName(c));
        lines.append(connectionLastSuccess(c.receivedAtMs));
        problems.append(connectionNeedsAttention(c));
        problems.append(false);
    }
    if (role == StatusLinesRole) return lines;
    if (role == ProblemLinesRole) return QVariant::fromValue(problems);
    if (role == Qt::ToolTipRole)
        return plainToolTip(first.alias + "\n" + context + "\n" + lines.join('\n'));
    if (role == Qt::AccessibleTextRole)
        return first.alias + "\n" + context + "\n" + lines.join('\n');
    return {};
}

void ConnectionListModel::refresh(const QList<AccountRecord>& accounts, const QList<PortfolioConnection>& states) {
    QList<ConnectionGroup> next;
    QHash<QString, int> rows;
    for (const auto& account : accounts) {
        const bool sharedMarket = account.provider == "binance" && account.environment == "live" &&
            !account.credentialRef.isEmpty() && (account.market == "spot" || account.market == "usdm_futures");
        const auto key = sharedMarket ? "binance/live/" + account.credentialRef : "account/" + account.id;
        if (!rows.contains(key)) { rows.insert(key, next.size()); next.append({key, {}}); }
        next[rows.value(key)].accounts.append(account);
    }
    QStringList oldIds, newIds;
    for (const auto& group : groups_) for (const auto& a : group.accounts) oldIds.append(group.id + "/" + a.id);
    for (const auto& group : next) for (const auto& a : group.accounts) newIds.append(group.id + "/" + a.id);
    const bool reset = oldIds != newIds;
    if (reset) beginResetModel();
    groups_ = next;
    states_.clear();
    for (const auto& state : states) states_.insert(state.account.id, state);
    if (reset) endResetModel();
    else if (!groups_.isEmpty()) emit dataChanged(index(0), index(groups_.size() - 1));
}

QSize ConnectionRowDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return {240, 62}; // the name and one status line; markets and times live in the detail
}

void ConnectionRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    painter->save();
    const bool selected = option.state & QStyle::State_Selected;
    if (selected || (option.state & QStyle::State_MouseOver))
        painter->fillRect(option.rect, themeColors().surface);
    if (option.state & QStyle::State_HasFocus) {
        painter->setPen(themeColors().accent);
        painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
    }
    const auto rect = option.rect.adjusted(12, 10, -12, -10);
    auto draw = [&](const QString& text, int y, QColor color, bool strong) {
        painter->setPen(color);
        const QRect line(rect.left(), y, rect.width(), 21);
        drawLedgerText(*painter,line,text,strong?FontRole::SecondaryStrong14:FontRole::Caption12);
    };
    draw(index.data().toString(), rect.top(), themeColors().text, true);
    const auto problems = index.data(ConnectionListModel::ProblemLinesRole).value<QList<bool>>();
    draw(index.data(ConnectionListModel::ContextRole).toString(), rect.top() + 21,
         problems.contains(true) ? themeColors().accentText : themeColors().text3, false);
    painter->restore();
}

} // namespace keefi
