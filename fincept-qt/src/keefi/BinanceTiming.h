#pragma once
#include <QtGlobal>
#include "keefi/TradingTypes.h"
namespace keefi {
inline constexpr qint64 BinanceRecvWindowMs = 5000;
inline constexpr qint64 BinanceReadTimeReserveMs = 1000;
inline constexpr qint64 BinanceOrderMaxRttMs = 2000;
// Signed requests also require timestamp < serverTime + 1000. For asset reads
// anchor the response's serverTime at receipt without adding half the RTT.
// The sample can then be up to one full RTT old; reserve 1s for the next hop.
// https://developers.binance.com/en/docs/products/spot/rest-api#timing-security
inline bool binanceReadTimeUsable(qint64 rtt) {
    return rtt >= 0 && rtt < BinanceRecvWindowMs - BinanceReadTimeReserveMs;
}
inline QString binanceAssetReadFailure(const HttpResult& response, const AdapterResult& result, qint64 rtt) {
    QString reason, action = QStringLiteral("잠시 후 다시 조회해 주세요.");
    if (result.rateLimited || response.status == 429 || response.status == 418) {
        reason = QStringLiteral("요청 제한"); action = QStringLiteral("제한 대기 후 다시 조회해 주세요.");
    } else if (result.authenticationFailed) {
        reason = QStringLiteral("인증 확인 실패"); action = QStringLiteral("API 인증·허용 IP를 확인하고 다시 조회해 주세요.");
    } else if (response.timedOut) reason = QStringLiteral("응답 시간 초과");
    else if (response.networkError) reason = QStringLiteral("네트워크 오류");
    else if (response.status != 200) reason = QString("HTTP %1 응답 실패").arg(response.status);
    else if (!result.ok) reason = QStringLiteral("응답 형식·내용 확인 실패");
    else reason = QStringLiteral("서버 응답 지연으로 서명 시각 여유 부족");
    return QString("%1 · 왕복 %2 ms · %3").arg(reason).arg(rtt).arg(action);
}
}
