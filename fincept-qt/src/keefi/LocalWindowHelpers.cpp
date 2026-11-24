#include "keefi/LocalWindowHelpers_p.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/PortfolioPresentation.h"

#include <QLabel>
#include <QPushButton>
#include <QTimer>

namespace keefi::local_window_detail {
bool hasPortfolioFigure(const PortfolioPresentation& view){
    if(view.summary.complete)return true;
    for(const auto& group:view.groups)for(const auto& holding:group.holdings)
        if(holding.state==PortfolioValueState::Fresh)return true;
    return false;
}

QLabel* textLabel(const QString& text, QWidget* parent, const char* name) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    if (name) label->setObjectName(name);
    return label;
}

QLabel* ledgerTextLabel(const QString& text,FontRole role,QWidget* parent,const char* name){
    auto* label=new LedgerTextLabel(text,role,parent);label->setWordWrap(true);
    if(name)label->setObjectName(name);return label;
}

QPushButton* button(const QString& text, const char* name, QWidget* parent) {
    auto* result = new QPushButton(text, parent);
    result->setObjectName(name);
    result->setMinimumHeight(36);
    return result;
}

QString displayChoice(const QString& value) {
    if (value == "okx_unified" || value == "okx_futures" || value == "hyperliquid_perps") return connectionMarketName(value);
    if (value == "live" || value == "real" || value == "production") return QObject::tr("실전");
    if (value == "paper" || value == "demo" || value == "mock") return QObject::tr("모의");
    if (value == "testnet") return QObject::tr("테스트넷");
    if (value == "mainnet") return QObject::tr("메인넷");
    if (value == "domestic_equity" || value == "domestic" || value == "domestic_stock" || value == "kr_stock" || value == "krx") return QObject::tr("국내주식");
    if (value == "spot") return QObject::tr("현물");
    if (value == "usdm_futures" || value == "usd_m_futures" || value == "usdm" || value == "usd-m") return QObject::tr("USDⓈ-M 선물");
    if (value == "swap" || value == "solana") return QObject::tr("Solana 지갑(현물)");
    return value;
}

const QStringList& problemConnectionStates() {
    static const QStringList states{"failed", "authentication_required", "account_required", "stopped"};
    return states;
}

void showTransientFeedback(QLabel* label, const QString& text) {
    if (!label) return;
    label->setText(text);
    QTimer::singleShot(4000, label, [label, text] { if (label->text() == text) label->clear(); });
}

} // namespace keefi::local_window_detail
