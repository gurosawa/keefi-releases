#pragma once
#include "keefi/PortfolioPresentation.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/MarketMetrics.h"
#include <QVector>
#include <limits>

namespace keefi {
enum class MoneyCurrency { Krw, Usd };

// Transient display projection. It owns no transport, storage or valuation data.
class PortfolioMoneyDisplay final {
  public:
    PortfolioMoneyDisplay() = default;
    static PortfolioMoneyDisplay fromQuotes(MoneyCurrency requested, const QList<AssetQuote>& quotes, qint64 asOfMs) {
        PortfolioMoneyDisplay display;
        if (requested == MoneyCurrency::Krw) return display;
        display.waiting_ = true;
        const auto direct = latest(quotes, asOfMs, "USD", "KRW", {}, {});
        // A conflicted direct observation must not be silently replaced by another route.
        if (direct.conflict) return display;
        qint64 oldest = 0;
        if (direct.quote) {
            display.rate_ = direct.quote->price;
            display.sourceTimeMs_ = direct.quote->evidence.sourceTimeMs;
            oldest = qMin(display.sourceTimeMs_, direct.quote->evidence.receivedAtMs);
        } else {
            const auto won = latest(quotes, asOfMs, "USDT", "KRW", "public:upbit:KRW-USDT",
                {"https://api.upbit.com/v1/ticker", "wss://api.upbit.com/websocket/v1"});
            const auto dollar = latest(quotes, asOfMs, "USDT", "USD", "public:coinbase:USDT-USD",
                {"https://api.exchange.coinbase.com/products/USDT-USD/ticker"});
            if (!won.quote || !dollar.quote || won.conflict || dollar.conflict ||
                qAbs(won.quote->evidence.sourceTimeMs - dollar.quote->evidence.sourceTimeMs) > MarketMetrics::MaximumLegSkewMs) return display;
            display.rate_ = won.quote->price;
            display.numeratorRate_ = dollar.quote->price;
            display.sourceTimeMs_ = won.quote->evidence.sourceTimeMs;
            display.numeratorSourceTimeMs_ = dollar.quote->evidence.sourceTimeMs;
            oldest = qMin(qMin(display.sourceTimeMs_, won.quote->evidence.receivedAtMs),
                qMin(display.numeratorSourceTimeMs_, dollar.quote->evidence.receivedAtMs));
        }
        const auto maximum = std::numeric_limits<qint64>::max();
        display.expiryMs_ = oldest > maximum - DisplayFreshnessMs - 1 ? maximum : oldest + DisplayFreshnessMs + 1;
        display.waiting_ = false;
        return display;
    }
    QString amount(const QString& exactSignedKrw) const {
        if (rate_.isEmpty()) return krwAmount(exactSignedKrw);
        const bool negative = exactSignedKrw.startsWith('-');
        const auto magnitude = negative ? exactSignedKrw.mid(1) : exactSignedKrw;
        // Keep missing/invalid display states; conversion must never turn them into 0.
        if (!decimal::valid(magnitude)) return exactSignedKrw;
        const auto amount = roundedRatio(magnitude,numeratorRate_,rate_);
        return negative && amount != QStringLiteral("0.00") ? '-' + amount : amount;
    }
    static QString changePercent(const QString& difference, const QString& opening) {
        const bool negative = difference.startsWith('-');
        const auto magnitude = negative ? difference.mid(1) : difference;
        if (!decimal::positive(opening) || !decimal::valid(magnitude)) return {};
        const auto percent = roundedRatio(magnitude,QStringLiteral("100"),opening);
        if (!decimal::positive(magnitude)) return QStringLiteral("0.00%");
        if (percent == "0.00") return negative ? QObject::tr("0.01% 미만 하락") : QObject::tr("0.01% 미만 상승");
        return (negative ? QStringLiteral("-") : QStringLiteral("+")) + percent + '%';
    }
    QString unit() const { return rate_.isEmpty() ? QStringLiteral("KRW") : QStringLiteral("USD"); }
    bool waitingForRate() const { return waiting_; }
    QString status() const { return waiting_ ? usabilityText(UsabilityText::CurrencyWait) : QString{}; }
    QString information() const {
        if (waiting_) return usabilityText(UsabilityText::FxUnavailableInformation);
        if (rate_.isEmpty()) return {};
        if (numeratorSourceTimeMs_ > 0) return usabilityText(UsabilityText::FxCrossInformation)
            .arg(QDateTime::fromMSecsSinceEpoch(numeratorSourceTimeMs_, QTimeZone("Asia/Seoul")).toString("HH:mm"),
                 QDateTime::fromMSecsSinceEpoch(sourceTimeMs_, QTimeZone("Asia/Seoul")).toString("HH:mm"));
        return usabilityText(UsabilityText::FxInformation).arg(QDateTime::fromMSecsSinceEpoch(sourceTimeMs_, QTimeZone("Asia/Seoul")).toString("HH:mm"));
    }
    qint64 expiresAtMs() const { return expiryMs_; }
    bool operator==(const PortfolioMoneyDisplay&) const = default;
  private:
    static QString roundedRatio(const QString& magnitude,const QString& multiplier,const QString& divisor) {
        auto numerator = multiply(digits(magnitude), digits(multiplier));
        auto denominator = digits(divisor);
        const int shift = scale(divisor) + 2 - scale(magnitude) - scale(multiplier);
        if (shift >= 0) numerator += QString(shift, '0');
        else denominator += QString(-shift, '0');
        numerator = strip(numerator);
        denominator = strip(denominator);
        QString cents, remainder = "0";
        for (const auto digit : numerator) {
            remainder = strip(remainder + digit);
            int result = 0;
            while (compare(remainder, denominator) >= 0) {
                remainder = subtract(remainder, denominator);
                ++result;
            }
            cents += QChar('0' + result);
        }
        cents = strip(cents);
        // Remainder >= (denominator - remainder) is exact 2*r >= d, including ties.
        if (compare(remainder, subtract(denominator, remainder)) >= 0) cents = increment(cents);
        cents = cents.rightJustified(3, '0');
        QString whole = cents.left(cents.size() - 2);
        for (qsizetype i = whole.size() - 3; i > 0; i -= 3) whole.insert(i, ',');
        return whole + "." + cents.right(2);
    }
    struct Selection { const AssetQuote* quote = nullptr; bool conflict = false; };
    static Selection latest(const QList<AssetQuote>& quotes, qint64 asOfMs, const QString& base, const QString& counter,
                            const QString& scope, const QStringList& urls) {
        Selection result;
        for (const auto& quote : quotes) {
            const auto& e = quote.evidence;
            if (quote.baseAsset != base || quote.quoteAsset != counter || !decimal::positive(quote.price) || !e.complete ||
                e.source.trimmed().isEmpty() || e.url.trimmed().isEmpty() || !e.scope.startsWith("public:") || e.scope.size() <= 7 ||
                (!scope.isEmpty() && e.scope != scope) || (!urls.isEmpty() && !urls.contains(e.url)) || asOfMs <= 0 ||
                e.sourceTimeMs <= 0 || e.receivedAtMs <= 0 || e.sourceTimeMs > asOfMs || e.receivedAtMs > asOfMs ||
                asOfMs - e.sourceTimeMs > DisplayFreshnessMs || asOfMs - e.receivedAtMs > DisplayFreshnessMs ||
                e.sourceTimeMs - e.receivedAtMs > 2000) continue;
            if (!result.quote || e.sourceTimeMs > result.quote->evidence.sourceTimeMs) result = {&quote, false};
            else if (e.sourceTimeMs == result.quote->evidence.sourceTimeMs) {
                if (decimal::normalize(quote.price) != decimal::normalize(result.quote->price)) result.conflict = true;
                else if (e.receivedAtMs < result.quote->evidence.receivedAtMs) result.quote = &quote;
            }
        }
        return result;
    }
    static QString multiply(const QString& a, const QString& b) {
        if (a == "0" || b == "0") return "0";
        if (b == "1") return a;
        QVector<int> product(a.size() + b.size(), 0);
        for (qsizetype i = a.size(); i-- > 0;)
            for (qsizetype j = b.size(); j-- > 0;) product[i + j + 1] += a[i].digitValue() * b[j].digitValue();
        for (qsizetype i = product.size(); --i > 0;) { product[i - 1] += product[i] / 10; product[i] %= 10; }
        QString result;result.reserve(product.size());
        for (const auto digit : product) result += QChar('0' + digit);
        return strip(result);
    }
    static int scale(const QString& value) { const auto dot = value.indexOf('.'); return dot < 0 ? 0 : int(value.size() - dot - 1); }
    static QString digits(QString value) { value.remove('.'); return strip(value); }
    static QString strip(QString value) {
        qsizetype first = 0;
        while (first + 1 < value.size() && value[first] == '0') ++first;
        return value.mid(first);
    }
    static int compare(const QString& a, const QString& b) {
        if (a.size() != b.size()) return a.size() > b.size() ? 1 : -1;
        return QString::compare(a, b);
    }
    static QString subtract(QString a, const QString& b) {
        int borrow = 0;
        qsizetype j = b.size() - 1;
        for (qsizetype i = a.size(); i-- > 0;) {
            int digit = a[i].digitValue() - borrow - (j >= 0 ? b[j--].digitValue() : 0);
            borrow = digit < 0 ? 1 : 0;
            if (borrow) digit += 10;
            a[i] = QChar('0' + digit);
        }
        return strip(a);
    }
    static QString increment(QString value) {
        for (qsizetype i = value.size(); i-- > 0;) {
            if (value[i] != '9') { value[i] = QChar(value[i].unicode() + 1); return value; }
            value[i] = '0';
        }
        return '1' + value;
    }
    QString rate_, numeratorRate_ = "1";
    qint64 sourceTimeMs_ = 0, numeratorSourceTimeMs_ = 0, expiryMs_ = 0;
    bool waiting_ = false;
};
}
