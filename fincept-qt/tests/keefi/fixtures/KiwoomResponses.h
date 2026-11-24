#pragma once
#include "keefi/TradingTypes.h"
#include "KisMasterFixture.h"
#include <QDateTime>
#include <QTimeZone>
#include <QJsonArray>
namespace keefi::test {
inline QDateTime kiwoomNow() { return QDateTime(QDate(2026,10,19),QTime(1,10),QTimeZone::UTC); }
inline HttpResult kiwoomJson(QJsonObject obj, int status=200) {
    if (!obj.contains("return_code")) obj.insert("return_code",0);
    HttpResult r; r.status=status; r.headers={{"cont-yn","N"}};
    r.body=QJsonDocument(obj).toJson(QJsonDocument::Compact); return r;
}
inline QJsonObject kiwoomDetailOrder(QString number="0000100", QString root="0000000", QString quantity="10", QString executed="0", QString remaining="10", QString price="70000", QString modification={}) {
    return {{"ord_no",number},{"stk_cd","A005930"},{"trde_tp","보통"},{"crd_tp","보통매매"},
        {"ord_qty",quantity},{"ord_uv",price},{"cnfm_qty",modification.isEmpty()?"0":quantity},
        {"acpt_tp","접수"},{"ord_tm","10:09:00"},{"ori_ord",root},{"stk_nm","MOCK STOCK"},
        {"io_tp_nm","현금매수"},{"loan_dt",""},{"cntr_qty",executed},{"cntr_uv","70000"},
        {"ord_remnq",remaining},{"mdfy_cncl",modification},{"cnfm_tm","10:09:00"},{"dmst_stex_tp","KRX"}};
}
inline QJsonObject kiwoomOpenOrder(QString number="0000100",QString quantity="10",QString executed="0",QString remaining="10") {
    return {{"acnt_no","1234567890"},{"ord_no",number},{"stk_cd","005930"},{"ord_stt","접수"},
        {"ord_qty",quantity},{"ord_pric","70000"},{"oso_qty",remaining},{"cntr_tot_amt","0"},
        {"orig_ord_no","0000000"},{"io_tp_nm","+매수"},{"trde_tp","보통"},{"tm","100900"},
        {"cntr_qty",executed},{"stex_tp","1"},{"sor_yn","N"}};
}
inline HttpResult kiwoomResponse(const RequestSpec& request,const QDateTime& now=kiwoomNow()) {
    const auto kst=now.toTimeZone(QTimeZone("Asia/Seoul"));
    if (request.url.host()=="new.real.download.dws.co.kr") {
        const QString market=request.url.path().contains("kosdaq")?"KOSDAQ":"KOSPI";
        auto row=keefi::test::row(market,market=="KOSPI"?"005930":"035720");
        const int tailSize=market=="KOSPI"?227:221;
        row[row.size()-1-tailSize+(market=="KOSPI"?158:153)]='0';
        auto second=keefi::test::row(market,market=="KOSPI"?"005935":"035725");
        second[second.size()-1-tailSize+(market=="KOSPI"?158:153)]='1';
        HttpResult r; r.status=200;r.body=zip(row+second,(market.toLower()+"_code.mst").toLatin1());
        r.headers={{"date",now.toUTC().toString(Qt::RFC2822Date).toLatin1()},{"last-modified",now.addSecs(-3600).toUTC().toString(Qt::RFC2822Date).toLatin1()}}; return r;
    }
    const auto body=QJsonDocument::fromJson(request.body).object();
    const auto api=request.headers.value("api-id");
    if(request.url.path()=="/oauth2/token") return kiwoomJson({{"token","fake-token.only_for_test"},{"token_type","Bearer"},{"expires_dt",kst.addSecs(3600).toString("yyyyMMddHHmmss")}});
    if(api=="ka00001") return kiwoomJson({{"acctNo","1234567890"}});
    if(api=="ka10099") {
        const QString market=body.value("mrkt_tp").toString(),symbol=market=="0"?"005930":"035720";
        const QString name=QString::fromUtf8("가짜한글종목");
        return kiwoomJson({{"list",QJsonArray{
            QJsonObject{{"code",symbol},{"name",name},{"state","증거금20%|담보대출|신용가능"},{"auditInfo","정상"},{"marketCode",market},{"orderWarning","0"},{"nxtEnable","Y"}},
            QJsonObject{{"code",market=="0"?"005935":"035725"},{"name",name},{"state","증거금20%|담보대출|신용가능"},{"auditInfo","정상"},{"marketCode",market},{"orderWarning","0"},{"nxtEnable","N"}}
        }}});
    }
    if(api=="ka10100") return kiwoomJson({{"code",body.value("stk_cd")},{"name",QString::fromUtf8("가짜한글종목")},{"marketCode",body.value("stk_cd").toString().startsWith("035")?"10":"0"},{"state","증거금20%|담보대출|신용가능"},{"auditInfo","정상"},{"orderWarning","0"}});
    if(api=="ka10001") return kiwoomJson({{"stk_cd",body.value("stk_cd")},{"stk_nm","MOCK STOCK"},{"cur_prc","+70000"},{"upl_pric","+91000"},{"lst_pric","-49000"},{"base_pric","70000"}});
    if(api=="ka10004") { QJsonObject out{{"bid_req_base_tm",kst.toString("HHmmss")},{"sel_fpr_bid","+70100"},{"sel_fpr_req","1000"},{"buy_fpr_bid","-69900"},{"buy_fpr_req","1000"}};return kiwoomJson(out); }
    if(api=="ka10080") return kiwoomJson({{"stk_cd",body.value("stk_cd")},{"stk_min_pole_chart_qry",QJsonArray{QJsonObject{{"cntr_tm",kst.addSecs(-20).toString("yyyyMMddHHmmss")},{"cur_prc","+70000"},{"trde_qty","10"}}}}});
    if(api=="kt00018") return kiwoomJson({{"acnt_evlt_remn_indv_tot",QJsonArray{QJsonObject{{"stk_cd","A005930"},{"stk_nm","MOCK STOCK"},{"rmnd_qty","100"},{"trde_able_qty","90"},{"pur_pric","65000"},{"cur_prc","70000"},{"evlt_amt","7000000"},{"evltv_prft","500000"},{"crd_tp","00"},{"crd_tp_nm","현금"},{"crd_loan_dt",""}}}}});
    if(api=="kt00001") return kiwoomJson({{"entr","7000000"},{"d1_entra","6900000"},{"d2_entra","6800000"},{"ord_alow_amt","6500000"},{"pymn_alow_amt","6400000"}});
    if(api=="kt00011") return kiwoomJson({{"min_ord_alow_amt","7000000"},{"min_ord_alowq","100"},{"ord_alowa","7000000"}});
    if(api=="kt00010") return kiwoomJson({{"cmsn","100"},{"pur_exct_amt","700100"},{"ord_alowa","7000000"}});
    if(api=="ka10075") return kiwoomJson({{"oso",QJsonArray{kiwoomOpenOrder()}}});
    if(api=="kt00007") return kiwoomJson({{"acnt_ord_cntr_prps_dtl",QJsonArray{kiwoomDetailOrder()}}});
    if(api=="kt10000"||api=="kt10001") return kiwoomJson({{"ord_no","0000200"},{"dmst_stex_tp","KRX"}});
    if(api=="kt10002") return kiwoomJson({{"ord_no","0000201"},{"base_orig_ord_no","0000100"},{"mdfy_qty","10"},{"dmst_stex_tp","KRX"}});
    if(api=="kt10003") return kiwoomJson({{"ord_no","0000202"},{"base_orig_ord_no","0000100"},{"cncl_qty","10"}});
    return kiwoomJson({{"return_code",9999}},400);
}
} // namespace keefi::test
