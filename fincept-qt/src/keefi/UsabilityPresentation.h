#pragma once
#include <QCoreApplication>
#include <QDateTime>
#include <QRegularExpression>
#include <QString>
#include <QTimeZone>
#include <QUrl>

namespace keefi {
// Temporary G8 copy lives here. Display only: never use these strings to
// identify an instrument, decide freshness, validate a source, or persist data.
enum class UsabilityText {
    PortfolioLoadTitle, PortfolioLoadDelayed, PortfolioLoadProblem, PortfolioLoadAccounts, PortfolioLoadPrices, PortfolioLoadPresentation, PortfolioLoadTasks, PortfolioLoadChecking, PortfolioLoadUnitInfo, PortfolioLoadAvailable,
    Delayed, NeedsCheck, SourceTime, InstrumentSearch, PositionQuantity,
    AveragePrice, PositionProfit, IndicatorPerformance, ChartScore,
    ChartPullbackInside, ChartPullbackOutside, ChartTrendUp, ChartTrendDown,
    ChartTrendFlat, HoldingsSummary, ChartInformation, SimilaritySummary,
    PositionEntryAndLiquidation, LiquidationDistance, ProviderUnknown, StockVi,
    ProviderAverage, PositionProfitAmount, PositionLiquidation, CurrentPrice,
    ChartVolumeMultiple, ChartLargeTradePercentile, ChartOpenInterestChange,
    ChartSimilarityCount, MoneyCurrencyLabel, MoneyCurrencyKrw, MoneyCurrencyUsd,
    CurrencyWait, FxInformation, FxUnavailableInformation, MoneyValuationColumn, FxCrossInformation,
    FlowMetricKoreaGap, FlowMetricFunding, FlowMetricCoinbaseGap, FlowStateCool, FlowStateNormal, FlowStateHot, FlowChecking, FlowTodayTitle, FlowKoreaGapTitle, FlowKoreaGapMeaning, FlowFundingTitle, FlowPublishedFundingTitle, FlowFundingMeaning, FlowCoinbaseGapTitle, FlowCoinbaseGapMeaning, FlowOiTitle, FlowOiMeaning, FlowDerivativesTitle, FlowAlertEnable, FlowDayChange, FlowSummaryKorea, FlowSummaryFunding, FlowSummaryCoinbase, FlowSummaryChecking, FlowHistoryInfo, FlowMetricAlertTitle, FlowMetricAlertBody, FlowNoticesTitle, FlowCalendarTitle, FlowCalendarToday, FlowCalendarDays, FlowCalendarContext, FlowCalendarCpi, FlowCalendarJobs, FlowCalendarGdp, FlowCalendarPce, FlowCalendarOther, FlowTreasuryTitle, FlowTreasuryInfo, FlowNoticesInfo, FlowCalendarInfo, FlowContractLabel, FlowDirectionUp, FlowDirectionDown, FlowDirectionFlat, FlowPublishedFundingMeaning,
    FlowAlertOn, FlowExpand, FlowCollapse, FlowDerivativesAutoNote, FlowSentenceHot, FlowSentenceCool, FlowSentenceNormal, FlowCalendarSentence, FlowHistoryStart, FlowHistoryEnd, FlowNoticesFailed, FlowNoticesNone, FlowReferenceTitle, FlowAssetLabel, FlowNoticeWithdrawal, FlowNoticeDelisting, FlowNoticeNetwork, FlowDayToday, FlowDayTomorrow, FlowDawn, FlowMorning, FlowAfternoon, FlowNight, FlowCalendarRelease, FlowTreasuryDate, FlowDayChangeShort, ExploreViewRank, ExploreViewMap, ExploreViewFlow, ExploreMarketAll, ExploreMarketCoin, ExploreMarketKorea, ExploreMarketUs, ExploreCoinSearch, AlertsPermissionOff, VaultReadyNotice, FlowCalendarFomc,
    FlowCalendarBok, FlowCalendarDateOnly, FlowCalendarDateSentence, FlowEarningsTitle, FlowEarningsInfo, FlowEarningsEmpty, FlowEarningsUnavailable, FlowEarningsMore,
    FlowEarningsPreMarket, FlowEarningsAfterHours, FlowEarningsTimeUnknown, FlowEarningsEps, FlowEarningsKorea, FlowEarningsUs,
    FlowGapUnavailable, FlowGapNotOnUpbit, FlowGapNotOnBinance, FlowGapNotOnCoinbase, FlowGapProgress, FlowGapLate, FlowLoadingProgress, FlowNoData
};
inline QString usabilityText(UsabilityText key) {
    const auto tr=[](const char* text){return QCoreApplication::translate("G8Usability",text);};
    switch(key) {
    case UsabilityText::PortfolioLoadTitle:return tr("자산을 불러오고 있어요");
    case UsabilityText::PortfolioLoadDelayed:return tr("자산 확인이 늦어지고 있어요.");
    case UsabilityText::PortfolioLoadProblem:return tr("일부 계좌를 불러오지 못했어요.");
    case UsabilityText::PortfolioLoadAccounts:return tr("잔고 불러오기");
    case UsabilityText::PortfolioLoadPrices:return tr("시세 확인");
    case UsabilityText::PortfolioLoadPresentation:return tr("화면 정리");
    case UsabilityText::PortfolioLoadTasks:return tr("%1 / %2단계 완료");
    case UsabilityText::PortfolioLoadChecking:return tr("불러오는 중");
    case UsabilityText::PortfolioLoadUnitInfo:return tr("계좌 조회, 시세 확인, 화면 준비 단계를 합친 진행률이에요. 실패하거나 지원하지 않는 항목도 끝난 단계로 계산해요. 옅은 구간은 지금 확인 중인 단계라서 숫자에는 넣지 않아요.");
    case UsabilityText::PortfolioLoadAvailable:return tr("불러온 자산 먼저 보기");

    case UsabilityText::Delayed:return tr("지연");
    case UsabilityText::NeedsCheck:return tr("확인 필요");
    case UsabilityText::SourceTime:return tr("%1 · %2 기준");
    case UsabilityText::InstrumentSearch:return tr("종목 검색");
    case UsabilityText::PositionQuantity:return tr("수량 %1");
    case UsabilityText::AveragePrice:return tr("내 평균단가 %1 %2");
    case UsabilityText::PositionProfit:return tr("평가손익 %1 %2 (%3%)");
    case UsabilityText::IndicatorPerformance:return tr("지표 성적");
    case UsabilityText::ChartScore:return tr("최근 %1번 중 %2번 올랐어요");
    case UsabilityText::ChartPullbackInside:return tr("눌림존 안 · %1/10점");
    case UsabilityText::ChartPullbackOutside:return tr("눌림존 밖 · %1/10점");
    case UsabilityText::ChartTrendUp:return tr("상승 추세");
    case UsabilityText::ChartTrendDown:return tr("하락 추세");
    case UsabilityText::ChartTrendFlat:return tr("횡보 추세");
    case UsabilityText::HoldingsSummary:return tr("시세가 나오는 보유 종목 %1개");
    case UsabilityText::ChartInformation:return tr("차트 계산 방식");
    case UsabilityText::SimilaritySummary:return tr("비슷한 과거 차트 %1개");
    case UsabilityText::PositionEntryAndLiquidation:return tr("진입가 %1 %2 · 청산가 %3 %2");
    case UsabilityText::LiquidationDistance:return tr("청산가까지 %1% (%2 %3, %4)");
    case UsabilityText::ProviderUnknown:return tr("거래소");
    case UsabilityText::StockVi:return tr("VI 발동 중");
    case UsabilityText::ProviderAverage:return tr("거래소 평균 매수가 %1 %2");
    case UsabilityText::PositionProfitAmount:return tr("평가손익 %1 %2");
    case UsabilityText::PositionLiquidation:return tr("청산가 %1 %2");
    case UsabilityText::CurrentPrice:return tr("현재가");
    case UsabilityText::ChartVolumeMultiple:return tr("평소 거래량의 %1배");
    case UsabilityText::ChartLargeTradePercentile:return tr("큰손 체결 %1백분위");
    case UsabilityText::ChartOpenInterestChange:return tr("미결제약정 %1%");
    case UsabilityText::ChartSimilarityCount:return tr("%1건");
    case UsabilityText::MoneyCurrencyLabel:return tr("표시 통화");
    case UsabilityText::MoneyCurrencyKrw:return tr("원화(KRW)");
    case UsabilityText::MoneyCurrencyUsd:return tr("달러(USD)");
    case UsabilityText::CurrencyWait:return tr("환율 불러오는 중");
    case UsabilityText::FxInformation:return tr("원화 평가액을 달러 환율(%1 기준)로 나눈 금액이에요. 김프 때문에 USDT 수량과는 조금 달라요.");
    case UsabilityText::FxCrossInformation:return tr("원화 평가액을 코인베이스 USDT/USD(%1 기준)와 업비트 USDT/KRW(%2 기준) 시세로 환산한 달러 금액이에요. USDT 수량과는 달라요.");
    case UsabilityText::FxUnavailableInformation:return tr("달러 환율 정보를 아직 불러오지 못해 원화로 보여드려요. 차트와 탐색 화면은 원래 거래 통화로 나와요.");
    case UsabilityText::MoneyValuationColumn:return tr("평가금액 %1");
    case UsabilityText::FlowMetricKoreaGap:return tr("테더 환산 가격차");
    case UsabilityText::FlowMetricFunding:return tr("펀딩비");
    case UsabilityText::FlowMetricCoinbaseGap:return tr("코인베이스 프리미엄");
    case UsabilityText::FlowStateCool:return tr("침체");
    case UsabilityText::FlowStateNormal:return tr("보통");
    case UsabilityText::FlowStateHot:return tr("과열");
    case UsabilityText::FlowChecking:return tr("불러오는 중");
    case UsabilityText::FlowTodayTitle:return tr("오늘 시장 한 줄");
    case UsabilityText::FlowKoreaGapTitle:return tr("테더 환산 가격차");
    case UsabilityText::FlowKoreaGapMeaning:return tr("업비트 USDT 원화 시세로 환산한 업비트·바이낸스 가격차예요. 오늘의 원/달러 기준 BTC 김프와 다른 값이에요.");
    case UsabilityText::FlowFundingTitle:return tr("연환산 펀딩비");
    case UsabilityText::FlowPublishedFundingTitle:return tr("이번 주기 펀딩비");
    case UsabilityText::FlowFundingMeaning:return tr("정산 주기마다 내거나 받는 펀딩비를 1년 치로 환산한 비율이에요.");
    case UsabilityText::FlowCoinbaseGapTitle:return tr("코인베이스 프리미엄");
    case UsabilityText::FlowCoinbaseGapMeaning:return tr("코인베이스 달러 가격과 바이낸스 USDT 가격을 비교한 값이에요.");
    case UsabilityText::FlowOiTitle:return tr("미결제약정 추이");
    case UsabilityText::FlowOiMeaning:return tr("가격과 선물 미결제약정이 함께 어떻게 움직였는지 보여줘요.");
    case UsabilityText::FlowDerivativesTitle:return tr("선물 시장 지표");
    case UsabilityText::FlowAlertEnable:return tr("알림 받기");
    case UsabilityText::FlowDayChange:return tr("24시간 전보다 %1%p");
    case UsabilityText::FlowSummaryKorea:return tr("테더 환산 가격차 %1");
    case UsabilityText::FlowSummaryFunding:return tr("펀딩비 %1");
    case UsabilityText::FlowSummaryCoinbase:return tr("코인베이스 %1");
    case UsabilityText::FlowSummaryChecking:return tr("시장 지표를 불러오고 있어요");
    case UsabilityText::FlowHistoryInfo:return tr("지금 가격과 24시간 전 1시간봉 마감 가격을 비교해요. 세 시세의 시간대가 모두 맞을 때만 변동 폭을 보여줘요.");
    case UsabilityText::FlowMetricAlertTitle:return tr("%1 · %2 흐름 변화");
    case UsabilityText::FlowMetricAlertBody:return tr("%1에서 %2 상태로 바뀌었어요");
    case UsabilityText::FlowNoticesTitle:return tr("거래소 공지");
    case UsabilityText::FlowCalendarTitle:return tr("이번 주 경제 일정");
    case UsabilityText::FlowCalendarToday:return tr("오늘");
    case UsabilityText::FlowCalendarDays:return tr("%1일 뒤");
    case UsabilityText::FlowCalendarContext:return tr("내 자산 비중: 코인 %1 · 미국 주식 %2");
    case UsabilityText::FlowCalendarCpi:return tr("소비자 물가가 얼마나 변했는지 발표해요.");
    case UsabilityText::FlowCalendarJobs:return tr("일자리 수와 실업률 같은 고용 지표를 발표해요.");
    case UsabilityText::FlowCalendarGdp:return tr("나라 경제가 얼마나 성장했는지 발표해요.");
    case UsabilityText::FlowCalendarPce:return tr("개인이 실제로 쓴 소비 지출과 물가를 발표해요.");
    case UsabilityText::FlowCalendarOther:return tr("주요 기관에서 발표하는 공식 경제 지표예요.");
    case UsabilityText::FlowTreasuryInfo:return tr("미국 재무부가 매일 발표하는 10년 만기 국채 금리예요.");
    case UsabilityText::FlowNoticesInfo:return tr("빗썸·OKX·업비트의 입출금 중단, 상장폐지·유의, 네트워크 업그레이드 공지를 최대 5개까지 모아 보여줘요. 업비트는 금고를 열고 업비트를 연결했을 때 앱을 켠 뒤 새로 올라온 공지만 받아요. 내 보유 종목과 자동 연동되지는 않으며, 시간대가 없는 공지는 날짜로만 정렬해요.");
    case UsabilityText::FlowTreasuryTitle:return tr("미국 10년물 국채 금리");
    case UsabilityText::FlowCalendarInfo:return tr("미국 노동통계국(BLS)·경제분석국(BEA)·연준(FOMC)과 한국은행 금융통화위원회의 향후 7일 내 발표 일정이에요. FOMC는 회의 둘째 날 미국 동부 오후 2시 결정문 발표 기준이에요. 한국은행은 회의 날짜만 공개해서 시각은 표시하지 않아요. 현지 사정에 따라 일정이 바뀌기도 해요.");
    case UsabilityText::FlowContractLabel:return tr("가격 %1 · 미결제약정 %2");
    case UsabilityText::FlowDirectionUp:return tr("상승");
    case UsabilityText::FlowDirectionDown:return tr("하락");
    case UsabilityText::FlowDirectionFlat:return tr("보합");
    case UsabilityText::FlowPublishedFundingMeaning:return tr("이번 정산 주기에 적용되는 펀딩비예요. 정산 주기가 정해지지 않은 종목은 연환산 값을 따로 계산하지 않아요.");
    case UsabilityText::FlowAlertOn:return tr("알림 받는 중");
    case UsabilityText::FlowExpand:return tr("더보기");
    case UsabilityText::FlowCollapse:return tr("접기");
    case UsabilityText::FlowDerivativesAutoNote:return tr("보유 중인 선물 포지션이 있어 바로 펼쳐 두었어요");
    case UsabilityText::FlowSentenceHot:return tr("%1 과열돼 있어요.");
    case UsabilityText::FlowSentenceCool:return tr("%1 가라앉아 있어요.");
    case UsabilityText::FlowSentenceNormal:return tr("%1 평소 수준이에요.");
    case UsabilityText::FlowCalendarSentence:return tr("%1 %2에 %3 발표돼요.");
    case UsabilityText::FlowHistoryStart:return tr("24시간 전");
    case UsabilityText::FlowHistoryEnd:return tr("지금");
    case UsabilityText::FlowNoticesFailed:return tr("공지를 불러오지 못했어요");
    case UsabilityText::FlowNoticesNone:return tr("새로 올라온 주요 공지가 없어요");
    case UsabilityText::FlowReferenceTitle:return tr("보조 지표");
    case UsabilityText::FlowAssetLabel:return tr("코인");
    case UsabilityText::FlowNoticeWithdrawal:return tr("입출금 중단");
    case UsabilityText::FlowNoticeDelisting:return tr("상장폐지·유의");
    case UsabilityText::FlowNoticeNetwork:return tr("네트워크 업그레이드");
    case UsabilityText::FlowDayToday:return tr("오늘");
    case UsabilityText::FlowDayTomorrow:return tr("내일");
    case UsabilityText::FlowDawn:return tr("새벽");
    case UsabilityText::FlowMorning:return tr("오전");
    case UsabilityText::FlowAfternoon:return tr("오후");
    case UsabilityText::FlowNight:return tr("밤");
    case UsabilityText::FlowCalendarRelease:return tr("%1 발표");
    case UsabilityText::FlowTreasuryDate:return tr("%1 기준");
    case UsabilityText::FlowDayChangeShort:return tr("24시간 전보다");
    case UsabilityText::ExploreViewRank:return tr("순위");
    case UsabilityText::ExploreViewMap:return tr("지도");
    case UsabilityText::ExploreViewFlow:return tr("오늘");
    case UsabilityText::ExploreMarketAll:return tr("전체");
    case UsabilityText::ExploreMarketCoin:return tr("코인");
    case UsabilityText::ExploreMarketKorea:return tr("국내 주식");
    case UsabilityText::ExploreMarketUs:return tr("미국 주식");
    case UsabilityText::ExploreCoinSearch:return tr("코인 검색");
    case UsabilityText::AlertsPermissionOff:return tr("알림이 꺼져 있어요 · 알림 설정에서 켤 수 있어요");
    case UsabilityText::VaultReadyNotice:return tr("금고를 열었어요");
    case UsabilityText::FlowCalendarFomc:return tr("미국 기준금리를 정하는 회의 결과를 발표해요.");
    case UsabilityText::FlowCalendarBok:return tr("한국 기준금리를 정하는 회의 결과를 발표해요.");
    case UsabilityText::FlowCalendarDateOnly:return tr("시각 미공개");
    case UsabilityText::FlowCalendarDateSentence:return tr("%1 %2 발표돼요.");
    case UsabilityText::FlowEarningsTitle:return tr("이번 주 실적 발표");
    case UsabilityText::FlowEarningsInfo:return tr("국내는 한국거래소 KIND에 올라온 코스피 기업설명회(IR) 일정 중 실적 관련 일정이에요. 코스피 시가총액 상위 50개와 관심 종목만 보여줘요. 설명회 없이 공시로만 실적을 내는 회사는 빠져요.\n미국은 Nasdaq 실적 일정이에요. 시가총액 2,000억 달러 이상 기업과 관심 종목만 보여줘요. 날짜는 미국 현지 날짜이고, 장 시작 전·장 마감 후로만 알려줘요.\n회사 사정에 따라 일정이 바뀌기도 해요.");
    case UsabilityText::FlowEarningsEmpty:return tr("이번 주 예정된 주요 실적 발표가 없어요.");
    case UsabilityText::FlowEarningsUnavailable:return tr("실적 일정을 확인할 수 없어요.");
    case UsabilityText::FlowEarningsMore:return tr("외 %1건");
    case UsabilityText::FlowEarningsPreMarket:return tr("미국 장 시작 전");
    case UsabilityText::FlowEarningsAfterHours:return tr("미국 장 마감 후");
    case UsabilityText::FlowEarningsTimeUnknown:return tr("시간 미정");
    case UsabilityText::FlowEarningsEps:return tr("예상 주당순이익 %1 · 애널리스트 %2명");
    case UsabilityText::FlowEarningsKorea:return tr("국내");
    case UsabilityText::FlowEarningsUs:return tr("미국");
    case UsabilityText::FlowGapUnavailable:return tr("계산할 수 없어요");
    case UsabilityText::FlowGapNotOnUpbit:return tr("업비트 원화 시장에 없는 코인이에요");
    case UsabilityText::FlowGapNotOnBinance:return tr("바이낸스 현물 시장에 없는 코인이에요");
    case UsabilityText::FlowGapNotOnCoinbase:return tr("코인베이스에 없는 코인이에요");
    case UsabilityText::FlowGapProgress:return tr("시세 %1/%2개 받음");
    case UsabilityText::FlowGapLate:return tr("응답이 늦어요 · 시세 %1/%2개 받음");
    case UsabilityText::FlowLoadingProgress:return tr("시장 정보 받는 중 · %1/%2");
    case UsabilityText::FlowNoData:return tr("자료 없음");
    }
    return {};
}
// Korean postposition for display copy (이/가, 은/는, 와/과). A trailing
// parenthetical or Latin word falls back to the preceding Hangul syllable.
inline QString usabilityParticle(const QString& word, const QString& withFinal, const QString& withoutFinal) {
    QString text=word.trimmed();
    if(text.endsWith(')')){const auto open=text.lastIndexOf('(');if(open>0)text=text.left(open).trimmed();}
    for(auto i=text.size()-1;i>=0;--i){const auto code=text.at(i).unicode();
        if(code>=0xAC00&&code<=0xD7A3)return word+((code-0xAC00)%28?withFinal:withoutFinal);
        if(text.at(i).isLetterOrNumber())break;}
    return word+withFinal+"("+withoutFinal+")";
}
// Display names for public BLS/BEA releases. Unknown titles keep the original.
inline QString usabilityCalendarTitle(const QString& original) {
    const auto lower=original.toLower();
    const auto has=[&](const char* text){return lower.contains(QLatin1String(text));};
    if(has("fomc"))return QStringLiteral("미국 FOMC 금리 결정");
    if(original.contains(QStringLiteral("한국은행")))return QStringLiteral("한국은행 기준금리 결정");
    if(has("gross domestic product")||has("gdp"))return has("advance")?QStringLiteral("미국 GDP 속보치"):QStringLiteral("미국 GDP");
    if(has("personal income and outlays"))return QStringLiteral("미국 개인소득·소비(PCE 물가)");
    if(has("consumer price index"))return QStringLiteral("미국 소비자물가(CPI)");
    if(has("producer price index"))return QStringLiteral("미국 생산자물가(PPI)");
    if(has("employment cost index"))return QStringLiteral("미국 고용비용지수");
    if(has("employment situation"))return QStringLiteral("미국 고용보고서");
    if(has("job openings"))return QStringLiteral("미국 구인·이직(JOLTS)");
    if(has("real earnings"))return QStringLiteral("미국 실질 임금");
    if(has("import and export price")||has("import/export price"))return QStringLiteral("미국 수출입 물가");
    if(has("international trade"))return QStringLiteral("미국 무역수지");
    return original;
}
inline QString usabilityProviderName(const QString& providerOrUrl) {
    const auto key=providerOrUrl.toLower();
    const auto host=QUrl(providerOrUrl,QUrl::StrictMode).host().toLower();
    if(key=="upbit"||host=="api.upbit.com")return QStringLiteral("업비트");
    if(key=="bithumb"||host=="api.bithumb.com")return QStringLiteral("빗썸");
    if(key=="binance"||host=="api.binance.com")return QStringLiteral("Binance Spot");
    if(key=="binance_usdm"||host=="fapi.binance.com")return QStringLiteral("Binance Futures");
    if(key=="okx"||host=="www.okx.com"||host=="okx.com")return QStringLiteral("OKX");
    if(key=="coinbase"||host=="api.coinbase.com"||host=="api.exchange.coinbase.com")return QStringLiteral("Coinbase");
    if(key=="toss")return QStringLiteral("토스증권");
    if(key=="kiwoom")return QStringLiteral("키움증권");
    if(key=="kis")return QStringLiteral("한국투자증권");
    return {};
}
inline QString usabilityPublicText(QString text) {
    static const QRegularExpression urls(QStringLiteral("(?:[a-z][a-z0-9+.-]*://|www\\.)[^\\s<>\"']+"),QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression keys(QStringLiteral("\\b(?:stock|coin):[^\\s<>\"']+"));
    static const QRegularExpression references(QStringLiteral("\\[[0-9]+\\]"));
    text.remove(urls);text.remove(keys);text.remove(references);
    return text.trimmed();
}
inline QString usabilitySourceTime(const QString& constrainedProvider,qint64 atMs) {
    const auto provider=usabilityPublicText(constrainedProvider);
    if(provider.isEmpty()||atMs<=0)return provider;
    return usabilityText(UsabilityText::SourceTime).arg(provider,
        QDateTime::fromMSecsSinceEpoch(atMs,QTimeZone("Asia/Seoul")).toString("HH:mm")+QStringLiteral(" KST · UTC+9"));
}
inline QString usabilityFreshness(const QString& existingState) {
    // LocalWindow writes the Delayed display text back as an evidence state.
    if(existingState.contains(QStringLiteral("지연"))||existingState.contains(usabilityText(UsabilityText::Delayed))||existingState.contains(QStringLiteral("이전 시세"))||existingState.contains(QStringLiteral("오래됨")))
        return usabilityText(UsabilityText::Delayed);
    if(existingState.contains(QStringLiteral("미확인"))||existingState.contains(QStringLiteral("불완전"))||existingState.contains(QStringLiteral("확인 필요")))
        return usabilityText(UsabilityText::NeedsCheck);
    return {};
}
}
