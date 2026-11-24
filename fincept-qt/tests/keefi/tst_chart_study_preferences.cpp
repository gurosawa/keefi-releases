#include "keefi/ChartStudyPreferences.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
using namespace keefi;
class ChartStudyPreferencesTest:public QObject{
    Q_OBJECT
  private slots:
    void settingsRoundTripAndRejectInvalidValuesAtomically() {
        ChartStudyPreferences p;
        const auto defaults=p.publicState();
        auto settings=p.studySettings();
        settings.rsiPeriod=2;settings.macdFast=2;settings.macdSlow=120;settings.macdSignal=81;
        settings.displayBandPeriod=31;settings.displayBandDeviations=2.75;
        settings.colors.insert("ma2",QColor("#c018ec"));settings.colors.insert("rsi",QColor("#123456"));
        auto ma=p.movingAverages();ma[1].kind=ExtendedMaKind::Exponential;ma[1].period=7;
        QSignalSpy changed(&p,&ChartStudyPreferences::changed);
        QVERIFY(p.setStudySettings(settings,ma));QCOMPARE(changed.size(),1);
        ChartStudyPreferences read;QVERIFY(read.restorePublicState(p.publicState()));
        QVERIFY(read.studySettings()==settings);QCOMPARE(read.movingAverages()[1].period,7);
        auto invalid=settings;invalid.macdSignal=82;
        QVERIFY(!p.setStudySettings(invalid,ma));QCOMPARE(changed.size(),1);
        auto object=QJsonDocument::fromJson(p.publicState()).object();auto fields=object["settings"].toObject();
        fields["rsi"]=2.5;object["settings"]=fields;
        QVERIFY(!read.restorePublicState(QJsonDocument(object).toJson()));QCOMPARE(read.publicState(),p.publicState());
        fields["rsi"]=14;fields["colors"]=QJsonObject{{"ma2","#12345g"}};object["settings"]=fields;
        QVERIFY(!read.restorePublicState(QJsonDocument(object).toJson()));QCOMPARE(read.publicState(),p.publicState());
        fields["colors"]=QJsonObject{{"unknown","#123456"}};object["settings"]=fields;
        QVERIFY(!read.restorePublicState(QJsonDocument(object).toJson()));
        QVERIFY(read.restorePublicState(defaults));QVERIFY(read.studySettings()==ChartStudySettings{});
    }
    void newStudiesAreOffAndLegacyChoicesMigrateWithoutReset(){
        ChartStudyPreferences p;
        for(const auto* id:{"ma","bb","rsi","macd","levels","cvd","oi","contract","sentiment"}){
            QVERIFY2(p.supportedIds("coin").contains(id),id);
            QVERIFY2(!p.enabled(id),id);
        }
        QVERIFY(!p.supportedIds("stock").contains("oi"));
        QVERIFY(!p.supportedIds("stock").contains("sentiment"));
        QJsonObject legacyStudies;
        for(const auto* id:{"pullback","whales","similarity","cloud","volume","stock_flow"})
            legacyStudies.insert(id,QJsonObject{{"enabled",QString(id)!="cloud"},{"visible",QString(id)!="similarity"}});
        const auto legacy=QJsonDocument(QJsonObject{{"format","keefi-chart-studies/1"},{"studies",legacyStudies},{"introSeen",true}}).toJson(QJsonDocument::Compact);
        QString reason;QVERIFY2(p.restorePublicState(legacy,&reason),qPrintable(reason));
        // Saved off and hidden bits are choices even in the oldest schema.
        QVERIFY(!p.enabled("cloud"));QVERIFY(p.visible("cloud"));QVERIFY(p.enabled("similarity"));QVERIFY(!p.visible("similarity"));QVERIFY(p.introSeen());
        QVERIFY(p.visible("volume"));QVERIFY(p.enabled("volume"));
        QVERIFY(!p.enabled("rsi"));QVERIFY(!p.enabled("levels"));
        ChartStudyPreferences roundTrip;QVERIFY(roundTrip.restorePublicState(p.publicState()));QCOMPARE(roundTrip.publicState(),p.publicState());
    }
    void threeCoreStudiesAreEnabledAndVisibleForNewProfiles(){
        ChartStudyPreferences p;
        QCOMPARE(p.supportedIds("coin").first(5),QStringList({"pullback","whales","similarity","cloud","volume"}));
        QCOMPARE(p.supportedIds("stock").first(5),QStringList({"pullback","stock_flow","similarity","cloud","volume"}));
        for(const auto& market:{QString("coin"),QString("stock")}){
            QVERIFY(p.enabledOrder(market).contains("similarity"));
            for(const auto& id:p.supportedIds(market).first(5)){QVERIFY(p.enabled(id));QVERIFY(p.visible(id));}
        }
        QVERIFY(p.supportedIds("unsupported").isEmpty());QVERIFY(!p.enabled("invented"));
        QVERIFY(!p.introSeen());
    }
    void hideAndDisableStayIndependentAndSurvivePublicRoundTrip(){
        ChartStudyPreferences p;QSignalSpy changed(&p,&ChartStudyPreferences::changed);
        QVERIFY(p.setEnabled("similarity",true));
        QVERIFY(p.setVisible("similarity",false));QVERIFY(p.enabled("similarity"));QVERIFY(!p.visible("similarity"));
        QVERIFY(p.setEnabled("cloud",false));QVERIFY(p.visible("cloud"));QVERIFY(!p.enabled("cloud"));
        p.markIntroSeen();QVERIFY(p.introSeen());QCOMPARE(changed.size(),3);
        const auto bytes=p.publicState();QVERIFY(!bytes.isEmpty());
        ChartStudyPreferences restored;QString reason;QVERIFY2(restored.restorePublicState(bytes,&reason),qPrintable(reason));
        QCOMPARE(restored.publicState(),bytes);QVERIFY(restored.enabled("similarity"));QVERIFY(!restored.visible("similarity"));
        QVERIFY(!restored.enabled("cloud"));QVERIFY(restored.visible("cloud"));QVERIFY(restored.introSeen());
        const auto count=changed.size();QVERIFY(p.setVisible("similarity",false));p.markIntroSeen();QCOMPARE(changed.size(),count);
    }
    void storedSimilarityChoicesSurviveEveryVersionAndMarketQueries(){
        for(int version:{1,2,3,4,5})for(bool enabled:{false,true})for(bool visible:{false,true}){
            ChartStudyPreferences source;auto object=QJsonDocument::fromJson(source.publicState()).object();auto studies=object["studies"].toObject();
            studies["similarity"]=QJsonObject{{"enabled",enabled},{"visible",visible}};
            if(version==1){QJsonObject old;for(const auto* id:{"pullback","whales","similarity","cloud","volume","stock_flow"})old[id]=studies[id];object=QJsonObject{{"format","keefi-chart-studies/1"},{"studies",old},{"introSeen",true}};}
            else{object["studies"]=studies;object["format"]="keefi-chart-studies/"+QString::number(version);if(version<=4)object["pane"]="";
                if(version<4)object["averages"]=QJsonArray{QJsonObject{{"period",20},{"kind","sma"}},QJsonObject{{"period",60},{"kind","sma"}}};}
            ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(QJsonDocument(object).toJson()));
            QCOMPARE(restored.enabled("similarity"),enabled);QCOMPARE(restored.visible("similarity"),visible);const auto before=restored.publicState();
            for(const auto& market:{QString("coin"),QString("stock"),QString("coin")})QCOMPARE(restored.enabledOrder(market).contains("similarity"),enabled);
            QCOMPARE(restored.publicState(),before);ChartStudyPreferences roundTrip;QVERIFY(roundTrip.restorePublicState(before));QCOMPARE(roundTrip.publicState(),before);
        }
    }
    void savedCoreChoicesKeepIndependentBitsOnRestart(){
        for(const auto* id:{"pullback","similarity","whales"})for(bool enabled:{false,true})for(bool visible:{false,true}){
            ChartStudyPreferences original;QVERIFY(original.setEnabled(id,enabled));QVERIFY(original.setVisible(id,visible));
            const auto saved=original.publicState();ChartStudyPreferences reopened;QVERIFY(reopened.restorePublicState(saved));
            QCOMPARE(reopened.enabled(id),enabled);QCOMPARE(reopened.visible(id),visible);
            ChartStudyPreferences secondRestart;QVERIFY(secondRestart.restorePublicState(reopened.publicState()));
            QCOMPARE(secondRestart.publicState(),saved);
        }
    }
    void legacyCoreBitsArePreservedInsteadOfForcingDefaultVisibility(){
        const QStringList core{"cloud","pullback","whales","stock_flow","similarity","volume"};
        for(int version:{1,2})for(bool enabled:{false,true})for(bool visible:{false,true}){
            ChartStudyPreferences source;auto object=QJsonDocument::fromJson(source.publicState()).object();
            auto studies=object["studies"].toObject();for(const auto& id:core)studies[id]=QJsonObject{{"enabled",enabled},{"visible",visible}};
            if(version==1){QJsonObject old;for(const auto& id:core)old[id]=studies[id];object=QJsonObject{{"format","keefi-chart-studies/1"},{"studies",old},{"introSeen",true}};}
            else{object["studies"]=studies;object["format"]="keefi-chart-studies/2";object["pane"]="";
                object["averages"]=QJsonArray{QJsonObject{{"period",20},{"kind","sma"}},QJsonObject{{"period",60},{"kind","sma"}}};}
            ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(QJsonDocument(object).toJson()));
            for(const auto& id:core){QCOMPARE(restored.enabled(id),enabled);QCOMPARE(restored.visible(id),visible);}
            QCOMPARE(restored.movingAverages().size(),2);QCOMPARE(restored.movingAverages()[0].period,20);QCOMPARE(restored.movingAverages()[1].period,60);
            QCOMPARE(restored.movingAverages()[0].kind,ExtendedMaKind::Simple);QCOMPARE(restored.movingAverages()[1].kind,ExtendedMaKind::Simple);
            ChartStudyPreferences restarted;QVERIFY(restarted.restorePublicState(restored.publicState()));QCOMPARE(restarted.publicState(),restored.publicState());
        }
    }
    void movingAveragesValidateFourChoicesAndPersist(){
        ChartStudyPreferences p;
        const QList<ExtendedMaSpec> wanted{{"ma1",2,ExtendedMaKind::Simple},{"ma2",60,ExtendedMaKind::Exponential},{"ma3",120,ExtendedMaKind::Weighted,false},{"ma4",200,ExtendedMaKind::Exponential}};
        QVERIFY(p.setMovingAverages(wanted));QCOMPARE(p.movingAverages().size(),4);
        const auto before=p.publicState();QVERIFY(!p.setMovingAverages({{"ma1",1,ExtendedMaKind::Simple}}));QCOMPARE(p.publicState(),before);
        auto excess=wanted;excess.append({"ma5",20,ExtendedMaKind::Simple});QVERIFY(!p.setMovingAverages(excess));QCOMPARE(p.publicState(),before);
        QVERIFY(!p.setMovingAverages({{"ma1",201,ExtendedMaKind::Simple}}));QCOMPARE(p.publicState(),before);
        ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(before));QCOMPARE(restored.publicState(),before);
        QCOMPARE(restored.movingAverages()[1].kind,ExtendedMaKind::Exponential);QCOMPARE(restored.movingAverages()[2].period,120);
        QVERIFY(!restored.movingAverages()[2].enabled);QCOMPARE(restored.movingAverages()[3].period,200);
    }
    void newPresetAndExplicitResetKeepOtherChoices(){
        ChartStudyPreferences p;const QList<int> periods{21,50,100,200};
        QCOMPARE(p.movingAverages().size(),4);
        for(int i=0;i<4;++i){QCOMPARE(p.movingAverages()[i].period,periods[i]);QCOMPARE(p.movingAverages()[i].kind,ExtendedMaKind::Exponential);QVERIFY(p.movingAverages()[i].enabled);}
        QVERIFY(p.setEnabled("ma",true));QVERIFY(p.setVisible("ma",false));QVERIFY(p.setFavorite("ma",true));
        QVERIFY(p.setMovingAverages({{"ma1",20,ExtendedMaKind::Simple},{"ma2",60,ExtendedMaKind::Weighted,false}}));
        QVERIFY(p.setMovingAverages(defaultChartMovingAverages()));
        QVERIFY(p.enabled("ma"));QVERIFY(!p.visible("ma"));QVERIFY(p.favorite("ma"));
        for(int i=0;i<4;++i){QCOMPARE(p.movingAverages()[i].period,periods[i]);QCOMPARE(p.movingAverages()[i].kind,ExtendedMaKind::Exponential);QVERIFY(p.movingAverages()[i].enabled);}
    }
    void savedSmaDefaultsAndThirdLineAreNeverGuessedToBeUnchosen(){
        for(int count:{2,3}){
            ChartStudyPreferences original;QVERIFY(original.setEnabled("ma",true));QVERIFY(original.setVisible("ma",false));QVERIFY(original.setFavorite("ma",true));QVERIFY(original.setEnabled("cloud",false));
            auto object=QJsonDocument::fromJson(original.publicState()).object();object["format"]="keefi-chart-studies/3";object["pane"]="";
            QJsonArray averages{QJsonObject{{"period",20},{"kind","sma"}},QJsonObject{{"period",60},{"kind","sma"}}};
            if(count==3)averages.append(QJsonObject{{"period",120},{"kind","wma"}});
            object["averages"]=averages;ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(QJsonDocument(object).toJson()));
            QCOMPARE(restored.movingAverages().size(),count);QCOMPARE(restored.movingAverages()[0].period,20);QCOMPARE(restored.movingAverages()[1].period,60);
            for(int i=0;i<2;++i)QCOMPARE(restored.movingAverages()[i].kind,ExtendedMaKind::Simple);
            for(const auto& spec:restored.movingAverages())QVERIFY(spec.enabled);
            if(count==3){QCOMPARE(restored.movingAverages()[2].period,120);QCOMPARE(restored.movingAverages()[2].kind,ExtendedMaKind::Weighted);}
            QVERIFY(restored.enabled("ma"));QVERIFY(!restored.visible("ma"));QVERIFY(restored.favorite("ma"));QVERIFY(!restored.enabled("cloud"));
            ChartStudyPreferences again;QVERIFY(again.restorePublicState(restored.publicState()));QCOMPARE(again.publicState(),restored.publicState());
        }
    }
    void allFourDisabledLinesKeepPeriodsKindsAndRejectMalformedState(){
        ChartStudyPreferences p;auto specs=defaultChartMovingAverages();
        for(auto& spec:specs)spec.enabled=false;specs[0].kind=ExtendedMaKind::Simple;specs[1].kind=ExtendedMaKind::Weighted;specs[2].period=121;
        QVERIFY(p.setMovingAverages(specs));const auto saved=p.publicState();ChartStudyPreferences reopened;QVERIFY(reopened.restorePublicState(saved));
        QCOMPARE(reopened.movingAverages().size(),4);for(const auto& spec:reopened.movingAverages())QVERIFY(!spec.enabled);
        QCOMPARE(reopened.movingAverages()[0].kind,ExtendedMaKind::Simple);QCOMPARE(reopened.movingAverages()[1].kind,ExtendedMaKind::Weighted);QCOMPARE(reopened.movingAverages()[2].period,121);
        QSignalSpy changes(&reopened,&ChartStudyPreferences::changed);
        for(const auto& bad:QList<QJsonValue>{QJsonValue(1),QJsonValue("false"),QJsonValue(QJsonValue::Null)}){
            auto object=QJsonDocument::fromJson(saved).object();auto averages=object["averages"].toArray();auto first=averages[0].toObject();first["enabled"]=bad;averages[0]=first;object["averages"]=averages;
            QVERIFY(!reopened.restorePublicState(QJsonDocument(object).toJson()));QCOMPARE(reopened.publicState(),saved);
        }
        auto object=QJsonDocument::fromJson(saved).object();object.remove("averages");QVERIFY(!reopened.restorePublicState(QJsonDocument(object).toJson()));QCOMPARE(reopened.publicState(),saved);QCOMPARE(changes.size(),0);
    }
    void multiplePanesKeepIndependentCalculationAndDisplay() {
        ChartStudyPreferences p;
        QVERIFY(p.setVisible("rsi",false));QVERIFY(!p.enabled("rsi"));
        QVERIFY(p.setEnabled("rsi",true));QVERIFY(p.visible("rsi"));
        QVERIFY(p.setEnabled("macd",true));QVERIFY(p.setEnabled("cvd",true));QVERIFY(p.setEnabled("oi",true));
        QCOMPARE(p.visiblePanes("coin"),QStringList({"rsi","macd","cvd","oi"}));
        QCOMPARE(p.visiblePanes("stock"),QStringList({"rsi","macd"}));
        QCOMPARE(p.enabledOrder("coin").first(),QString("oi"));
        QVERIFY(p.setVisible("macd",false));QVERIFY(p.enabled("macd"));
        QVERIFY(p.setEnabled("macd",true));QVERIFY(!p.visible("macd"));
        ChartStudyPreferences restored;
        QVERIFY(restored.restorePublicState(p.publicState()));QCOMPARE(restored.publicState(),p.publicState());
        QCOMPARE(restored.visiblePanes("coin"),QStringList({"rsi","cvd","oi"}));
        QVERIFY(restored.setEnabled("macd",false));QVERIFY(restored.setEnabled("macd",true));
        QCOMPARE(restored.visiblePanes("coin"),QStringList({"rsi","macd","cvd","oi"}));
    }
    void versionFourPreservesExplicitlyHiddenPanesWithoutExclusivity() {
        ChartStudyPreferences p;
        p.setEnabled("rsi",true);p.setVisible("rsi",false);p.setEnabled("macd",true);
        auto old=QJsonDocument::fromJson(p.publicState()).object();
        old["format"]="keefi-chart-studies/4";old["pane"]="macd";
        ChartStudyPreferences restored;
        QVERIFY(restored.restorePublicState(QJsonDocument(old).toJson()));
        QCOMPARE(restored.visiblePanes("coin"),QStringList({"macd"}));QVERIFY(restored.enabled("rsi"));
        QVERIFY(restored.setVisible("rsi",true));
        QCOMPARE(restored.visiblePanes("coin"),QStringList({"rsi","macd"}));
        const auto current=restored.publicState();
        old["pane"]="rsi";QVERIFY(!restored.restorePublicState(QJsonDocument(old).toJson()));
        QCOMPARE(restored.publicState(),current);
    }
    void favoritesArePublicIdsAndRejectMalformedStateAtomically(){
        ChartStudyPreferences preferences;const auto original=preferences.publicState();
        QVERIFY(preferences.setFavorite("bb",true));QVERIFY(preferences.favorite("bb"));QVERIFY(!preferences.enabled("bb"));
        QVERIFY(!preferences.setFavorite("private-account",true));const auto saved=preferences.publicState();
        ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(saved));QVERIFY(restored.favorite("bb"));QVERIFY(!restored.enabled("bb"));
        for(const auto& bad:QList<QJsonValue>{QJsonValue("bb"),QJsonArray{"bb","bb"},QJsonArray{"bb","accountScope"},QJsonArray{7}}){
            auto object=QJsonDocument::fromJson(saved).object();object["favorites"]=bad;QVERIFY(!restored.restorePublicState(QJsonDocument(object).toJson()));QCOMPARE(restored.publicState(),saved);
        }
        QVERIFY(restored.restorePublicState(original));QVERIFY(!restored.favorite("bb"));QCOMPARE(restored.publicState(),original);
    }
    void malformedOrPrivateStateIsRejectedAtomically(){
        ChartStudyPreferences p;const auto before=p.publicState();QVERIFY(!before.isEmpty());
        auto object=QJsonDocument::fromJson(before).object();object["accountScope"]="FAKE_PRIVATE_FIELD_MUST_NOT_LOAD";
        QString reason;QVERIFY(!p.restorePublicState(QJsonDocument(object).toJson(),&reason));QVERIFY(!reason.isEmpty());QCOMPARE(p.publicState(),before);
        object=QJsonDocument::fromJson(before).object();auto studies=object["studies"].toObject();auto state=studies["pullback"].toObject();state["enabled"]="true";studies["pullback"]=state;object["studies"]=studies;
        QVERIFY(!p.restorePublicState(QJsonDocument(object).toJson()));QCOMPARE(p.publicState(),before);
        QVERIFY(!p.restorePublicState(QByteArray(20000,'x')));QCOMPARE(p.publicState(),before);
        QSignalSpy changed(&p,&ChartStudyPreferences::changed);QVERIFY(!p.setEnabled("fake",true));QVERIFY(!p.setVisible("fake",false));QCOMPARE(changed.size(),0);
    }
    void chartStyleIsOptionalPublicStateAndValidated(){
        // Candles stay the default and add nothing to a saved profile; bars and line round-trip.
        ChartStudyPreferences p;const auto defaults=p.publicState();QCOMPARE(p.chartStyle(),QString("candles"));QVERIFY(!defaults.contains("chartStyle"));
        QSignalSpy changed(&p,&ChartStudyPreferences::changed);
        QVERIFY(!p.setChartStyle("area"));QCOMPARE(changed.size(),0);QVERIFY(p.setChartStyle("candles"));QCOMPARE(changed.size(),0);
        QVERIFY(p.setChartStyle("line"));QCOMPARE(changed.size(),1);QVERIFY(p.publicState().contains("\"chartStyle\":\"line\""));
        ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(p.publicState()));QCOMPARE(restored.chartStyle(),QString("line"));QCOMPARE(restored.publicState(),p.publicState());
        QVERIFY(p.setChartStyle("bars"));QVERIFY(restored.restorePublicState(p.publicState()));QCOMPARE(restored.chartStyle(),QString("bars"));
        // Restoring a profile without the key returns to candles.
        QVERIFY(restored.restorePublicState(defaults));QCOMPARE(restored.chartStyle(),QString("candles"));
        // An unknown or non-string style is rejected as a whole.
        auto object=QJsonDocument::fromJson(p.publicState()).object();object["chartStyle"]="area";
        QVERIFY(!restored.restorePublicState(QJsonDocument(object).toJson()));QCOMPARE(restored.chartStyle(),QString("candles"));
        object["chartStyle"]=3;QVERIFY(!restored.restorePublicState(QJsonDocument(object).toJson()));
        // Earlier formats never carry it.
        auto legacy=QJsonDocument::fromJson(defaults).object();legacy["format"]="keefi-chart-studies/2";legacy["chartStyle"]="line";
        QVERIFY(!restored.restorePublicState(QJsonDocument(legacy).toJson()));
    }
};
QTEST_GUILESS_MAIN(ChartStudyPreferencesTest)
#include "tst_chart_study_preferences.moc"
