#pragma once
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>
#include <QUrlQuery>

// Synthetic, non-network fixtures only. Public catalogue ZIP is supplied by the
// catalogue test helper; this helper never fetches data or credentials.
namespace keefi::test {
inline HttpResult kisJson(const QJsonObject& payload, int status = 200) {
    HttpResult r; r.status = status; r.headers["tr_cont"] = "D"; r.body = QJsonDocument(payload).toJson(QJsonDocument::Compact); return r;
}
inline QJsonObject kisEnvelope(const QJsonValue& output = QJsonObject{}) {
    return {{"rt_cd", "0"}, {"msg_cd", "MOCK0000"}, {"msg1", "synthetic response"}, {"output", output}};
}
inline QJsonObject kisOrder(const QDateTime& now, const QString& number = "0000000011", const QString& quantity = "10",
                           const QString& executed = "0", const QString& remaining = "10") {
    const auto korea = now.toTimeZone(QTimeZone("Asia/Seoul"));
    return {{"ord_dt", korea.date().toString("yyyyMMdd")}, {"ord_gno_brno", "00001"}, {"odno", number},
        {"orgn_odno", "0000000000"}, {"pdno", "005930"}, {"prdt_name", "가상 국내 종목"}, {"sll_buy_dvsn_cd", "02"},
        {"ord_dvsn_cd", "00"}, {"ord_qty", quantity}, {"ord_unpr", "70000"}, {"ord_tmd", korea.time().toString("HHmmss")},
        {"tot_ccld_qty", executed}, {"tot_ccld_amt", decimal::multiply(executed, "70000")}, {"rmn_qty", remaining},
        {"psbl_qty", remaining}, {"cncl_yn", "N"}, {"cncl_cfrm_qty", "0"}, {"rjct_qty", "0"},
        {"excg_id_dvsn_cd", "KRX"}, {"loan_dt", ""}};
}
inline HttpResult kisResponse(const RequestSpec& request, const QDateTime& now = QDateTime::currentDateTimeUtc()) {
    const auto korea = now.toTimeZone(QTimeZone("Asia/Seoul")); const auto path = request.url.path(); const QUrlQuery query(request.url);
    if (path == "/oauth2/tokenP") return kisJson({{"access_token", "synthetic-kis-token.header-safe"}, {"token_type", "Bearer"}, {"expires_in", "86400"}});
    if (path.endsWith("/inquire-balance")) {
        auto o = kisEnvelope(); o.remove("output");
        o["output1"] = QJsonArray{QJsonObject{{"pdno", "005930"}, {"prdt_name", "가상 국내 종목"}, {"hldg_qty", "100"},
            {"ord_psbl_qty", "90"}, {"evlu_amt", "7000000"}, {"trad_dvsn_name", "현금"}, {"loan_dt", ""}, {"expd_dt", ""}, {"loan_amt", "0"}}};
        o["output2"] = QJsonArray{QJsonObject{{"dnca_tot_amt", "10000000"}}};
        o["ctx_area_fk100"] = ""; o["ctx_area_nk100"] = ""; return kisJson(o);
    }
    if (path.endsWith("/inquire-psbl-order")) return kisJson(kisEnvelope(QJsonObject{
        {"ord_psbl_cash", "10000000"}, {"nrcvb_buy_amt", "7000000"}, {"nrcvb_buy_qty", "100"}, {"max_buy_amt", "50000000"}, {"max_buy_qty", "714"}}));
    if (path.endsWith("/inquire-psbl-sell")) { auto o = kisEnvelope(); o.remove("output"); o["output1"] = QJsonObject{{"pdno", query.queryItemValue("PDNO")}, {"ord_psbl_qty", "90"}}; return kisJson(o); }
    if (path.endsWith("/inquire-price")) return kisJson(kisEnvelope(QJsonObject{{"stck_shrn_iscd", query.queryItemValue("FID_INPUT_ISCD")},
        {"stck_prpr", "70000"}, {"stck_mxpr", "91000"}, {"stck_llam", "49000"}, {"aspr_unit", "100"}, {"hts_deal_qty_unit_val", "1"},
        {"temp_stop_yn", "N"}, {"sltr_yn", "N"}, {"short_over_yn", "N"}, {"iscd_stat_cls_code", "55"}, {"vi_cls_code", "0"}}));
    if (path.endsWith("/inquire-asking-price-exp-ccn")) {
        auto o = kisEnvelope(); o.remove("output"); QJsonObject book{{"aspr_acpt_hour", korea.time().toString("HHmmss")}, {"new_mkop_cls_code", "20"}};
        for (int i = 1; i <= 10; ++i) {
            book["askp" + QString::number(i)] = QString::number(70000 + i * 100);
            book["bidp" + QString::number(i)] = QString::number(70000 - i * 100);
            book["askp_rsqn" + QString::number(i)] = "1000"; book["bidp_rsqn" + QString::number(i)] = "1000";
        }
        o["output1"] = book; o["output2"] = QJsonObject{{"stck_shrn_iscd", query.queryItemValue("FID_INPUT_ISCD")}}; return kisJson(o);
    }
    if (path.endsWith("/chk-holiday")) return kisJson(kisEnvelope(QJsonArray{QJsonObject{{"bass_dt", korea.date().toString("yyyyMMdd")}, {"opnd_yn", "Y"}}}));
    if (path.endsWith("/inquire-time-itemchartprice")) {
        auto o = kisEnvelope(); o.remove("output"); o["output1"] = QJsonObject{};
        o["output2"] = QJsonArray{QJsonObject{{"stck_bsop_date", korea.date().toString("yyyyMMdd")}, {"stck_cntg_hour", korea.time().toString("HHmmss")}, {"stck_prpr", "70000"}}}; return kisJson(o);
    }
    if (path.endsWith("/inquire-psbl-rvsecncl")) return kisJson(kisEnvelope(QJsonArray{kisOrder(now)}));
    if (path.endsWith("/inquire-daily-ccld")) {
        auto o = kisEnvelope(); o.remove("output"); auto row = kisOrder(now, query.queryItemValue("ODNO").isEmpty() ? "0000000011" : query.queryItemValue("ODNO"));
        if (!query.queryItemValue("ORD_GNO_BRNO").isEmpty()) row["ord_gno_brno"] = query.queryItemValue("ORD_GNO_BRNO");
        if (!query.queryItemValue("ODNO").isEmpty()) row["ord_dt"] = query.queryItemValue("INQR_STRT_DT");
        o["output1"] = QJsonArray{row}; o["output2"] = QJsonObject{}; o["ctx_area_fk100"] = ""; o["ctx_area_nk100"] = ""; return kisJson(o);
    }
    if (path.endsWith("/order-cash")) return kisJson(kisEnvelope(QJsonObject{{"KRX_FWDG_ORD_ORGNO", "00001"}, {"ODNO", "0000000022"}, {"ORD_TMD", korea.time().toString("HHmmss")}}));
    if (path.endsWith("/order-rvsecncl")) return kisJson(kisEnvelope(QJsonObject{{"krx_fwdg_ord_orgno", "00001"}, {"odno", "0000000033"}, {"ord_tmd", korea.time().toString("HHmmss")}}));
    return kisJson({{"rt_cd", "1"}, {"msg_cd", "UNSUPPORTED"}, {"msg1", "synthetic unsupported"}}, 400);
}
} // namespace keefi::test
