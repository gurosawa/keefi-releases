#include "keefi/ChartStudySearch.h"
#include "keefi/ChartStudyHelp.h"
#include <QHash>
#include <QObject>
#include <QStringList>

namespace keefi {
namespace {
struct StudyVocabulary {
    QStringList aliases;
    QStringList concepts;
    QString conceptName;
};

// Previous displayed names keep their name-match priority after the UI adopts
// English names. A broad alias such as Low Volume Zones' "거래량" must not take
// the selection away from the former exact-name match, Volume.
const QHash<QString,QString>& legacyNames(){
    static const QHash<QString,QString> values{
        {"pullback","눌림존"},{"whales","큰손·반전"},{"similarity","닮은꼴 · 과거 비교"},{"cloud","추세 구름"},
        {"volume","거래량"},{"stock_flow","수급·거래량"},{"ma","이동평균"},{"bb","볼린저"},{"rsi","RSI"},{"macd","MACD"},
        {"levels","레벨선"},{"cvd","체결차"},{"oi","미결제약정"},{"contract","계약 흐름"},{"sentiment","선물 정보"},
        {"absorption","흡수 근사"},{"sweep","쓸고 복귀"},{"squeeze","변동성 압축"},{"adx","ADX"},{"breakout","돌파"},
        {"low_volume","빈 가격대"},{"strong","강한 반전"},{"trend","여러 시간봉 추세"},{"relative_strength","BTC 대비 흐름"}};
    return values;
}

const QHash<QString, StudyVocabulary>& vocabulary() {
    static const QHash<QString, StudyVocabulary> values{
        {"rsi", {{"Relative Strength Index", "상대강도", "과매수", "과매도", "모멘텀"},
                 {"많이 올랐", "많이 내렸", "과열", "오른 힘", "내린 힘"}, "최근 오름·내림 힘"}},
        {"bb", {{"Bollinger Bands", "볼린저", "볼린저 밴드", "변동성"},
                {"흔들림", "평소 범위", "밴드"}, "평소 가격 범위"}},
        {"squeeze", {{"Squeeze", "변동성 압축"},
                     {"흔들림", "잠잠", "조용"}, "흔들림이 줄어든 상태"}},
        {"adx", {{"Average Directional Index", "추세 강도"},
                 {"추세 세기", "방향성"}, "방향성 세기"}},
        {"ma", {{"Moving Average", "SMA", "EMA", "WMA", "이동평균", "추세"},
                {"평균선", "이평"}, "평균 가격선"}},
        {"macd", {{"Moving Average Convergence Divergence", "추세", "모멘텀"},
                  {"평균 차이"}, "짧은·긴 평균의 차이"}},
        {"cloud", {{"Trend Cloud", "추세 구름"},
                   {"빠른선", "느린선"}, "짧은·긴 흐름 비교"}},
        {"trend", {{"Trend", "여러 시간봉 추세", "추세", "고시간봉"},
                   {"큰 흐름", "상위 시간", "여러 시간", "일봉"}, "여러 시간봉 방향"}},
        {"volume", {{"Volume", "거래량", "수량"},
                    {"거래 많", "거래한 양"}, "거래한 양"}},
        {"low_volume", {{"Low Volume Profile", "빈 가격대", "거래 드문 가격대", "거래량"},
                        {"매물대", "빈 가격", "거래 드문"}, "거래가 드문 가격대(매물대의 반대쪽)"}},
        {"levels", {{"Support Resistance", "레벨선", "지지", "저항", "가격대"},
                    {"전일 고가", "전주", "전월", "기준 가격"}, "지난 기간 고가·저가"}},
        {"breakout", {{"Breakout", "돌파"},
                      {"신고가", "뚫었"}, "앞 20봉 범위 넘기"}},
        {"sweep", {{"Liquidity Sweep", "쓸고 복귀", "유동성 스윕"},
                   {"꼬리", "찍고 돌아", "속임"}, "범위 밖을 찍고 돌아옴"}},
        {"absorption", {{"Absorption", "흡수 근사", "흡수", "매수", "매도"},
                        {"거래 많은데", "버티"}, "거래량에 비해 좁은 움직임"}},
        {"pullback", {{"Pullback", "눌림존", "눌림목", "되돌림"},
                      {"눌림", "쉬어"}, "참고 가격대가 모인 범위"}},
        {"whales", {{"Whales", "큰손·반전", "큰손", "수급", "반전"},
                    {"고래", "큰 체결", "대량 체결"}, "큰 금액 체결"}},
        {"strong", {{"Strong Reversal", "강한 반전"},
                    {"반전 보강"}, "반전 보강 정도"}},
        {"cvd", {{"Cumulative Volume Delta", "체결차", "금액별 체결차", "누적 델타", "수급"},
                 {"누가 더 샀", "사는 쪽", "파는 쪽", "체결 방향"}, "금액대별 매수·매도 체결 차이"}},
        {"oi", {{"Open Interest", "미결제약정", "선물"},
                {"미결제", "계약 수"}, "선물 계약 수량 변화"}},
        {"contract", {{"Contract", "계약 흐름", "선물"},
                      {"가격과 계약"}, "가격과 계약 수량 함께 보기"}},
        {"sentiment", {{"Sentiment", "선물 정보", "Long Short", "롱숏", "심리", "Funding", "펀딩"},
                       {"선물 정보", "청산"}, "선물 시장 공개 정보"}},
        {"relative_strength", {{"Relative Strength", "BTC 대비 흐름", "상대강도", "BTC", "비트코인", "비교"},
                               {"비트코인보다", "BTC보다", "더 오른", "덜 오른"}, "BTC 대비 움직임"}},
        {"similarity", {{"Similarity Pattern", "닮은꼴 · 과거 비교", "닮은꼴", "유사 패턴", "과거 비교"},
                        {"비슷한 과거", "닮은"}, "같은 종목의 과거 비슷한 구간"}},
        {"stock_flow", {{"Stock Flow", "수급·거래량", "주식 수급"},
                        {"외국인", "기관", "순매수"}, "국내 일별 투자자 순매수"}}
    };
    return values;
}

bool isSearchSeparator(QChar c) {
    return c.isSpace() || c == QChar(0x00b7) || c == '-' || c == '/' || c == '?';
}

QString withoutSearchSeparators(const QString& text) {
    QString result;
    result.reserve(text.size());
    for (const auto c : text)
        if (!isSearchSeparator(c)) result += c;
    return result;
}

QString initialsQueryText(const QString& text) {
    QString result;
    result.reserve(text.size());
    for (const auto c : text) {
        // Normalize separator width without changing compatibility consonants.
        const auto normalized = QString(c).normalized(QString::NormalizationForm_KC);
        if (normalized.size() != 1 || !isSearchSeparator(normalized.front())) result += c;
    }
    return result;
}

bool isInitialsQuery(const QString& text) {
    if (text.size() < 2) return false;
    for (const auto c : text)
        if (c.unicode() < 0x3131 || c.unicode() > 0x314e) return false;
    return true;
}
}

QString chartStudySearchKey(const QString& text) {
    return withoutSearchSeparators(text.normalized(QString::NormalizationForm_KC).toLower());
}

QString chartStudyChoseong(const QString& text) {
    static const QString initials = QStringLiteral("ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ");
    QString result;
    result.reserve(text.size());
    for (const auto c : text) {
        const auto code = c.unicode();
        result += code >= 0xac00 && code <= 0xd7a3 ? initials.at((code - 0xac00) / 588) : c;
    }
    return result;
}

ChartStudySearchMatch matchChartStudy(const QString& id, const QString& query) {
    const auto key = chartStudySearchKey(query);
    if (key.isEmpty()) return {true, 0, {}};
    const auto name = chartStudyName(id);
    if (name.isEmpty()) return {};
    if (chartStudySearchKey(name).contains(key)) return {true, 3, {}};
    const auto legacy=legacyNames().value(id);
    if(!legacy.isEmpty()&&chartStudySearchKey(legacy).contains(key))
        return {true,3,QObject::tr("다른 이름 · %1").arg(legacy)};

    const auto found = vocabulary().constFind(id);
    if (found == vocabulary().cend()) return {};
    for (const auto& alias : found->aliases)
        if (chartStudySearchKey(alias).contains(key))
            return {true, 2, QObject::tr("다른 이름 · %1").arg(alias)};
    // Keep the old picker's public id searches (for example, low_volume).
    if (chartStudySearchKey(id).contains(key))
        return {true, 2, QObject::tr("다른 이름 · %1").arg(id)};

    // NFKC turns compatibility consonants into canonical jamo. Detect the
    // input before normalization, then normalize both sides for comparison.
    const auto initialsQuery = initialsQueryText(query);
    if (isInitialsQuery(initialsQuery)) {
        const auto matchesInitials = [&](const QString& value) {
            return chartStudySearchKey(chartStudyChoseong(value)).contains(key);
        };
        if (matchesInitials(name)) return {true, 2, QObject::tr("초성 · %1").arg(initialsQuery)};
        for (const auto& alias : found->aliases)
            if (matchesInitials(alias)) return {true, 2, QObject::tr("초성 · %1").arg(initialsQuery)};
    }

    if (key.size() >= 2)
        for (const auto& term : found->concepts) {
            const auto conceptKey = chartStudySearchKey(term);
            if (key.contains(conceptKey) || conceptKey.contains(key))
                return {true, 1, QObject::tr("‘%1’ → %2").arg(term, found->conceptName)};
        }
    return {};
}
}
