#include "keefi/TradingTypes.h"
#include <QRegularExpression>

namespace keefi {
namespace {
QString strip(QString value) {
    while (value.size() > 1 && value.front() == '0' && value[1] != '.') value.remove(0, 1);
    return value;
}
struct Parts { QString digits; int scale; };
Parts parts(const QString& value) {
    auto n = decimal::normalize(value);
    const int point = n.indexOf('.');
    const int scale = point < 0 ? 0 : n.size() - point - 1;
    n.remove('.');
    return {strip(n), scale};
}
QString formatted(QString digits, int scale) {
    while (digits.size() <= scale) digits.prepend('0');
    if (scale) digits.insert(digits.size() - scale, '.');
    while (digits.contains('.') && digits.endsWith('0')) digits.chop(1);
    if (digits.endsWith('.')) digits.chop(1);
    return strip(digits);
}
int integerCompare(const QString& a, const QString& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return QString::compare(a, b);
}
QString integerSubtract(QString a, const QString& b) {
    int borrow = 0;
    for (int i = a.size() - 1, j = b.size() - 1; i >= 0; --i, --j) {
        int n = a[i].digitValue() - borrow - (j >= 0 ? b[j].digitValue() : 0);
        borrow = n < 0;
        if (borrow) n += 10;
        a[i] = QChar('0' + n);
    }
    return strip(a);
}
QString expandNumber(const QByteArray& token) {
    QString text = QString::fromLatin1(token);
    const int e = text.indexOf(QRegularExpression("[eE]"));
    if (e < 0) return text;
    bool ok = false;
    const int exponent = text.mid(e + 1).toInt(&ok);
    if (!ok || exponent < -256 || exponent > 256) return {};
    QString mantissa = text.left(e);
    const bool negative = mantissa.startsWith('-');
    if (negative) mantissa.remove(0, 1);
    int point = mantissa.indexOf('.');
    if (point < 0) point = mantissa.size();
    mantissa.remove('.');
    const int nextPoint = point + exponent;
    if (nextPoint <= 0) mantissa = "0." + QString(-nextPoint, '0') + mantissa;
    else if (nextPoint >= mantissa.size()) mantissa += QString(nextPoint - mantissa.size(), '0');
    else mantissa.insert(nextPoint, '.');
    return (negative ? "-" : "") + mantissa;
}
}
namespace decimal {
bool valid(const QString& value) {
    static const QRegularExpression pattern("^[0-9]{1,80}(?:\\.[0-9]{1,80})?$");
    return value.size() <= 161 && pattern.match(value).hasMatch();
}
QString normalize(const QString& value) {
    if (!valid(value)) return {};
    QString result = value;
    while (result.contains('.') && result.endsWith('0')) result.chop(1);
    if (result.endsWith('.')) result.chop(1);
    return strip(result);
}
int compare(const QString& a, const QString& b) {
    auto x = parts(a), y = parts(b);
    const int scale = qMax(x.scale, y.scale);
    x.digits += QString(scale - x.scale, '0');
    y.digits += QString(scale - y.scale, '0');
    return integerCompare(strip(x.digits), strip(y.digits));
}
QString add(const QString& a, const QString& b) {
    if (!valid(a) || !valid(b)) return {};
    auto x = parts(a), y = parts(b);
    const int scale = qMax(x.scale, y.scale);
    x.digits += QString(scale - x.scale, '0'); y.digits += QString(scale - y.scale, '0');
    const int size = qMax(x.digits.size(), y.digits.size());
    x.digits = x.digits.rightJustified(size, '0'); y.digits = y.digits.rightJustified(size, '0');
    QString result(size, '0'); int carry = 0;
    for (int i = size - 1; i >= 0; --i) {
        const int n = x.digits[i].digitValue() + y.digits[i].digitValue() + carry;
        result[i] = QChar('0' + n % 10); carry = n / 10;
    }
    if (carry) result.prepend('1');
    return formatted(result, scale);
}
QString subtract(const QString& a, const QString& b) {
    if (!valid(a) || !valid(b) || compare(a, b) < 0) return {};
    auto x = parts(a), y = parts(b);
    const int scale = qMax(x.scale, y.scale);
    x.digits += QString(scale - x.scale, '0'); y.digits += QString(scale - y.scale, '0');
    return formatted(integerSubtract(x.digits, y.digits), scale);
}
QString multiply(const QString& a, const QString& b) {
    if (!valid(a) || !valid(b)) return {};
    auto x = parts(a), y = parts(b);
    QList<int> out(x.digits.size() + y.digits.size(), 0);
    for (int i = x.digits.size() - 1; i >= 0; --i)
        for (int j = y.digits.size() - 1; j >= 0; --j)
            out[i + j + 1] += x.digits[i].digitValue() * y.digits[j].digitValue();
    for (int i = out.size() - 1; i > 0; --i) { out[i - 1] += out[i] / 10; out[i] %= 10; }
    QString result; for (int n : out) result += QChar('0' + n);
    return formatted(result, x.scale + y.scale);
}
bool multipleOf(const QString& value, const QString& step) {
    if (!valid(value) || !positive(step)) return false;
    auto x = parts(value), y = parts(step);
    const int scale = qMax(x.scale, y.scale);
    x.digits += QString(scale - x.scale, '0'); y.digits += QString(scale - y.scale, '0');
    y.digits = strip(y.digits);
    QString remainder = "0";
    for (QChar digit : x.digits) {
        remainder = strip(remainder + digit);
        while (integerCompare(remainder, y.digits) >= 0)
            remainder = integerSubtract(remainder, y.digits);
    }
    return remainder == "0";
}
bool positive(const QString& value) { return valid(value) && normalize(value) != "0"; }
}
QJsonDocument parseExactJson(const QByteArray& bytes, QString* error, JsonResponseScope scope) {
    auto fail = [&] { if (error) *error = "응답 JSON 형식 또는 크기가 올바르지 않습니다."; return QJsonDocument(); };
    const qsizetype maximum = scope == JsonResponseScope::BinanceMarketCatalog
        ? MaxBinanceMarketCatalogResponseBytes : MaxStandardResponseBytes;
    if (bytes.size() > maximum) return fail();
    // Validate the original JSON before rewriting number tokens (no permissive grammar).
    QJsonParseError originalError;
    if (QJsonDocument::fromJson(bytes, &originalError).isNull()) return fail();
    QByteArray exact; exact.reserve(bytes.size());
    bool quoted = false, escaped = false;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (quoted) {
            exact += c;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') { quoted = true; exact += c; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const qsizetype start = i;
            while (i < bytes.size() && ((bytes[i] >= '0' && bytes[i] <= '9') ||
                bytes[i] == '-' || bytes[i] == '+' || bytes[i] == '.' || bytes[i] == 'e' || bytes[i] == 'E')) ++i;
            const QString number = expandNumber(bytes.mid(start, i - start));
            if (number.isEmpty() || number.size() > 512) return fail();
            exact += '"' + number.toLatin1() + '"'; --i;
        } else exact += c;
    }
    QJsonParseError parseError;
    auto result = QJsonDocument::fromJson(exact, &parseError);
    if (parseError.error != QJsonParseError::NoError) return fail();
    return result;
}
bool isMutation(Operation op) {
    return op == Operation::PlaceOrder || op == Operation::CancelOrder || op == Operation::AmendOrder || op == Operation::AlgoCancelOrder;
}
QString operationName(Operation op) {
    switch (op) {
    case Operation::OrderList:return "주문 묶음 상태 조회";
    case Operation::EarnRead:return "Earn 원금·회수·보상 조회";
    case Operation::FundFlowRead:return "입출금·지갑 이동 기록 조회";
    case Operation::Positions: return "선물 포지션"; case Operation::SymbolConfig: return "종목 증거금·레버리지 설정";
    case Operation::LeverageBrackets: return "증거금 구간"; case Operation::MarkPrice: return "표시가격·펀딩";
    case Operation::ApiRestrictions: return "API 시장별 권한"; case Operation::TradingStatus: return "거래 제한 상태";
    case Operation::AlgoOpenOrders: return "조건부 미체결"; case Operation::AlgoHistory: return "조건부 주문 이력";
    case Operation::AlgoQueryOrder: return "조건부 주문 대사"; case Operation::AlgoCancelOrder: return "조건부 취소";
    case Operation::AmendHistory: return "정정 이력 대사"; case Operation::Income: return "선물 수입·비용";
    case Operation::AccountConfig: return "계정 설정·권한"; case Operation::FundingBalance: return "자금 계좌 잔고";
    case Operation::FxUsd: return "USD 환산 지수"; case Operation::FxUsdt: return "USDT 환산 지수";
    case Operation::Authenticate: return "인증"; case Operation::Accounts: return "증권 계좌 목록";
    case Operation::Holdings: return "보유 주식"; case Operation::BuyingPower: return "주문 가능 금액";
    case Operation::SellableQuantity: return "매도 가능 수량"; case Operation::Commissions: return "수수료";
    case Operation::PriceLimits: return "가격 제한"; case Operation::TradingSchedule: return "시장 운영 시간";
    case Operation::Time: return "서버 시간"; case Operation::Markets: return "시장 목록";
    case Operation::Rules: return "주문 규칙"; case Operation::Quote: return "현재가";
    case Operation::Depth: return "호가 잔량";
    case Operation::Reference: return "기준가/호가 단위"; case Operation::AveragePrice: return "평균 가격";
    case Operation::AssetFilters: return "자산 제한"; case Operation::Account: return "잔고";
    case Operation::OpenOrders: return "계좌 미체결"; case Operation::History: return "주문 이력";
    case Operation::QueryOrder: return "주문 대사"; case Operation::TestOrder: return "공식 주문 테스트";
    case Operation::PlaceOrder: return "신규 주문"; case Operation::CancelOrder: return "취소";
    case Operation::AmendOrder: return "정정";
    }
    return {};
}
} // namespace keefi
