#pragma once
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QTimeZone>
#include <QJsonArray>
namespace keefi::test {
inline QDateTime lsNow(){return QDateTime(QDate(2026,10,19),QTime(1,10),QTimeZone::UTC);}
inline HttpResult lsJson(QJsonObject object,int status=200){HttpResult h;h.status=status;h.headers={{"tr_cont","N"},{"tr_cont_key",""}};h.body=QJsonDocument(object).toJson(QJsonDocument::Compact);return h;}
// exchname=KRX is a schema-field scenario; the official example omits it.
inline QJsonObject lsOpenOrder(QString number="100",QString qty="10",QString executed="0",QString remaining="10"){
 return {{"ordno",number},{"expcode","005930"},{"medosu","매수"},{"qty",qty},{"price","70000"},{"cheqty",executed},{"ordrem",remaining},{"cfmqty","0"},{"status","접수"},{"orgordno","0"},{"ordgb","보통"},{"hogagb","00"},{"orggb","02"},{"singb","00"},{"loandt",""},{"exchname","KRX"},{"ordtime","100900"}};
}
// Schema-shaped synthetic row, NOT an observed successful nonempty history response.
inline QJsonObject lsDetailOrder(QString number="100",QString qty="10",QString executed="0",QString available="10"){
 return {{"OrdDt","20261019"},{"OrdNo",number},{"OrgOrdNo","0"},{"IsuNo","A005930"},{"IsuNm","MOCK STOCK"},{"BnsTpCode","2"},{"BnsTpNm","매수"},{"OrdPtnCode","02"},{"OrdQty",qty},{"OrdPrc","70000"},{"ExecQty",executed},{"AllExecQty",executed},{"MrcAbleQty",available},{"MrcQty","0"},{"OrdprcPtnCode","00"},{"OrdCndiTpCode","0"},{"MbrNo","KRX"},{"LoanDt",""},{"OrdTrxPtnNm",""},{"MrcTpNm",""},{"MrcTpCode",""}};
}
inline HttpResult lsResponse(const RequestSpec& r,const QDateTime& now=lsNow()){
 const auto local=now.toTimeZone(QTimeZone("Asia/Seoul"));const QString tr=QString::fromLatin1(r.headers.value("tr_cd"));const auto body=QJsonDocument::fromJson(r.body).object();
 if(r.url.path()=="/oauth2/token")return lsJson({{"access_token","LS_FAKE_TOKEN.only-for-tests"},{"expire_in","3600"},{"token_type","Bearer"},{"scope","oob"}});
 if(tr=="CSPAQ12200")return lsJson({{"rsp_cd","00136"},{"CSPAQ12200OutBlock1",QJsonObject{{"AcntNo","01234567890"},{"Pwd","FAKE_PASSWORD_MUST_NOT_LEAK"},{"BalCreTp","0"}}},{"CSPAQ12200OutBlock2",QJsonObject{{"AcntNm","FAKE PRIVATE NAME"},{"Dps","7000000"},{"D1Dps","6900000"},{"D2Dps","6800000"},{"MnyOrdAbleAmt","6500000"},{"MnyoutAbleAmt","6400000"},{"RcvblUablOrdAbleAmt","6000000"}}}});
 if(tr=="t8436"){const auto market=body.value("t8436InBlock").toObject().value("gubun").toString();return lsJson({{"rsp_cd","00000"},{"t8436OutBlock",QJsonArray{QJsonObject{{"shcode",market=="1"?"005930":"035720"},{"hname","MOCK STOCK"},{"gubun",market},{"bu12gubun","01"},{"etfgubun",""},{"memedan","1"},{"spac_gubun","N"},{"uplmtprice","91000"},{"dnlmtprice","49000"}}}}});}
 if(tr=="t1404"||tr=="t1405")return lsJson({{"rsp_cd","00000"},{tr+"OutBlock",QJsonObject{{"cts_shcode",""}}},{tr+"OutBlock1",QJsonArray{}}});
 if(tr=="t1102")return lsJson({{"rsp_cd","00000"},{"t1102OutBlock",QJsonObject{{"shcode",body.value("t1102InBlock").toObject().value("shcode")},{"hname","MOCK STOCK"},{"price","70000"},{"uplmtprice","91000"},{"dnlmtprice","49000"},{"memedan","1"},{"info1",""},{"info2",""},{"info3",""},{"info4",""},{"info5",""},{"janginfo","KOSPI"},{"spac_gubun","N"},{"shterm_text",""}}}});
 if(tr=="t8450")return lsJson({{"rsp_cd","00000"},{"t8450OutBlock",QJsonObject{{"shcode",body.value("t8450InBlock").toObject().value("shcode")},{"offerho1","70100"},{"offerrem1","1000"},{"bidho1","69900"},{"bidrem1","1000"},{"hotime",local.toString("HHmmss")+"00"},{"ho_status",""}}}});
 if(tr=="t0167")return lsJson({{"rsp_cd","00000"},{"t0167OutBlock",QJsonObject{{"dt",local.toString("yyyyMMdd")},{"time",local.toString("HHmmss")+"000000"}}}});
 if(tr=="t8412")return lsJson({{"rsp_cd","00000"},{"t8412OutBlock",QJsonObject{{"shcode",body.value("t8412InBlock").toObject().value("shcode")},{"s_time","090000"},{"e_time","153000"},{"dshmin","10"},{"cts_date",""},{"cts_time",""}}},{"t8412OutBlock1",QJsonArray{QJsonObject{{"date",local.toString("yyyyMMdd")},{"time",local.addSecs(-20).toString("HHmmss")},{"close","70000"}}}}});
 if(tr=="t0424")return lsJson({{"rsp_cd","00000"},{"t0424OutBlock",QJsonObject{{"cts_expcode",""}}},{"t0424OutBlock1",QJsonArray{QJsonObject{{"expcode","005930"},{"hname","MOCK STOCK"},{"jangb",""},{"janqty","100"},{"mdposqt","90"},{"appamt","7000000"},{"sinamt","0"},{"loandt",""},{"lastdt",""},{"jonggb","3"},{"marketgb","1"}}}}});
 if(tr=="CSPBQ00200"){auto echo=body.value("CSPBQ00200InBlock1").toObject();echo["AcntNo"]="01234567890";echo["InptPwd"]="FAKE_PASSWORD_MUST_NOT_LEAK";return lsJson({{"rsp_cd","00136"},{"CSPBQ00200OutBlock1",echo},{"CSPBQ00200OutBlock2",QJsonObject{{"MgnRat100MnyOrdAbleAmt","6000000"},{"MgnRat100MnyOrdAbleQty","80"},{"MnyOrdAbleAmt","6500000"}}}});}
 if(tr=="t0425")return lsJson({{"rsp_cd","00000"},{"t0425OutBlock",QJsonObject{{"cts_ordno",""}}},{"t0425OutBlock1",QJsonArray{lsOpenOrder()}}});
 if(tr=="CSPAQ13700"){auto echo=body.value("CSPAQ13700InBlock1").toObject();echo["AcntNo"]="01234567890";return lsJson({{"rsp_cd","00200"},{"CSPAQ13700OutBlock1",echo},{"CSPAQ13700OutBlock2",QJsonObject{}},{"CSPAQ13700OutBlock3",QJsonArray{}}});}
 if(tr.startsWith("CSPAT")){auto echo=body.value(tr+"InBlock1").toObject();echo["AcntNo"]="01234567890";echo["InptPwd"]="FAKE_PASSWORD_MUST_NOT_LEAK";QJsonObject receipt{{"OrdNo","200"},{"OrdTime","101000"},{"ShtnIsuNo","A005930"},{"BnsTpCode","2"},{"MgntrnCode","000"},{"LoanDt",""}};QString code="00040";if(tr=="CSPAT00601"){receipt.remove("BnsTpCode");receipt.remove("MgntrnCode");receipt.remove("LoanDt");}if(tr=="CSPAT00801"){code="00156";receipt["PrntOrdNo"]="100";}if(tr=="CSPAT00701"){code="03181";receipt["OrdNo"]="0";receipt["PrntOrdNo"]="0";}return lsJson({{"rsp_cd",code},{tr+"OutBlock1",echo},{tr+"OutBlock2",receipt}});}
 return lsJson({{"rsp_cd","UNSUPPORTED_FIXTURE"}},400);
}
}
