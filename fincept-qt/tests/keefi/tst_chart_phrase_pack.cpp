#include "keefi/ChartPhrasePack.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTest>
#include <limits>

using namespace keefi;
namespace {
QJsonObject syntheticPack() {
    const QMap<QString, QString> texts{
        {"pullback", "{sym} {tf} 표시 점수 {score}점 · 계산에 쓴 완료 봉 {n}개."},
        {"result_up", "{sym} {tf} 공개 신호의 {horizon} 뒤 가격 변화 {ret}% · 평가 표본 {n}개 · 비용·슬리피지 제외."},
        {"result_down", "{sym} {tf} 공개 신호의 {horizon} 뒤 가격 변화 {ret}% · 평가 표본 {n}개 · 비용·슬리피지 제외."},
        {"result_flat", "{sym} {tf} 공개 신호의 {horizon} 뒤 가격 변화 {ret}% · 평가 표본 {n}개 · 비용·슬리피지 제외."},
        {"notice", "{sym} 공식 공지 {n}건 · 원천 {source}."},
        {"line", "{sym} {tf} 기준선 {price} {currency} · 완료 표본 {n}개."},
        {"whale", "{sym} {tf} {signal} 관측 · 유효 표본 {n}개."}};
    QJsonObject templates;
    for (auto it = texts.cbegin(); it != texts.cend(); ++it)
        templates.insert(it.key(), QJsonArray{
            QJsonObject{{"id", "synthetic_" + it.key() + "_a"}, {"text", "SYNTHETIC_A: " + it.value()}},
            QJsonObject{{"id", "synthetic_" + it.key() + "_b"}, {"text", "SYNTHETIC_B: " + it.value()}}});
    return {{"schema", "keefi-chart-phrases/1"}, {"language", "ko-KR"},
            {"id", "synthetic_pack"}, {"name", "합성 문구 시험"}, {"templates", templates}};
}
QByteArray bytes(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
void editFirst(QJsonObject* pack, const QString& kind, const std::function<void(QJsonObject&)>& edit) {
    auto templates = pack->value("templates").toObject(); auto rows = templates.value(kind).toArray();
    auto phrase = rows.first().toObject(); edit(phrase); rows[0] = phrase; templates[kind] = rows; (*pack)["templates"] = templates;
}
ChartPhraseFacts facts() {
    ChartPhraseFacts result;
    result.symbol = "SYNUSDT"; result.source = "합성 공식 원천"; result.currency = "USDT"; result.positionSide = "LONG";
    result.intervalMs = 900000; result.horizonMs = 14400000; result.observedAtMs = 1792627200000;
    result.score = 8; result.samples = 12; result.noticeCount = 1;
    result.resultReturnPercent = 2.5; result.price = 100.5; result.distancePercent = 5.0;
    result.whaleSignal = ChartWhalePhraseSignal::VolumeBurst;
    return result;
}
}

class ChartPhrasePackTest final : public QObject {
    Q_OBJECT
  private slots:
    void bundledSampleCountsKeepTheirObservationMeaning_data();
    void bundledSampleCountsKeepTheirObservationMeaning();
    void plainFallbackHasRequiredFactsWithoutImportedPack();
    void validPackNeedsExactReviewedHashAndProducesPublicPreview();
    void everyKindRequiresItsExactPlaceholderSet_data();
    void everyKindRequiresItsExactPlaceholderSet();
    void malformedUnknownPrivateAndAsymmetricPacksRejectAtomically_data();
    void malformedUnknownPrivateAndAsymmetricPacksRejectAtomically();
    void knownProhibitedLiteralsAndLengthAreRejected_data();
    void knownProhibitedLiteralsAndLengthAreRejected();
    void reviewProhibitedPatterns_data();
    void reviewProhibitedPatterns();
    void nonImperativeBoundarySentencesRemainValid_data();
    void nonImperativeBoundarySentencesRemainValid();
    void invalidUtf8ControlsAndSizeNeverExposePreview_data();
    void invalidUtf8ControlsAndSizeNeverExposePreview();
    void previousSuccessfulPhraseIsExcludedBeforeRandomSelection();
    void dripOffImmediatelyUsesPlainAndLiquidationNeverUsesImportedText();
    void bundledDripPackRunsWhileDripIsOnUntilAReviewedPackIsImported();
    void bundledResultSentencesNeverSayFeesWereDeducted();
    void liquidationSourceDisplaysProviderWithoutChangingFactsOrPack();
    void missingOrContradictoryFactsNeverInventNumbers_data();
    void missingOrContradictoryFactsNeverInventNumbers();
    void verifiedOneWayPositionSideIsDisplayedWithoutGuessingDirection();
    void QObjectLifetimeCanBeObservedWithoutBorrowedDanglingPointers();
};

void ChartPhrasePackTest::bundledSampleCountsKeepTheirObservationMeaning_data(){
    QTest::addColumn<int>("kind");QTest::addColumn<int>("variant");QTest::addColumn<QString>("expectedMeaning");
    for(int variant=0;variant<2;++variant){
        QTest::newRow(qPrintable(QString("line-%1").arg(variant)))<<int(ChartPhraseKind::Line)<<variant<<QStringLiteral("인접한 완료 봉 2개");
        QTest::newRow(qPrintable(QString("whale-%1").arg(variant)))<<int(ChartPhraseKind::Whale)<<variant<<QStringLiteral("유효 관측 표본 7개");
    }
}
void ChartPhrasePackTest::bundledSampleCountsKeepTheirObservationMeaning(){
    QFETCH(int,kind);QFETCH(int,variant);QFETCH(QString,expectedMeaning);
    ChartPhrasePack pack;const auto original=ChartPhrasePack::plainJson();
    const auto validation=ChartPhrasePack::validate(original);QVERIFY(validation.valid);QCOMPARE(validation.preview.size(),14);
    auto input=facts();input.samples=kind==int(ChartPhraseKind::Line)?2:7;input.priceText=QStringLiteral("100.5000");
    const auto rendered=pack.render(ChartPhraseKind(kind),input,false,{},[variant](int){return variant;});
    QVERIFY(rendered.available);QVERIFY(!rendered.serious);QVERIFY2(rendered.body.contains(expectedMeaning),qPrintable(rendered.body));
    QVERIFY(rendered.body.contains(input.symbol));QVERIFY(rendered.body.contains(QStringLiteral("15분")));
    QVERIFY(!rendered.body.contains('{'));QVERIFY(!rendered.body.contains(QStringLiteral("과거 사례")));
    QVERIFY(!rendered.body.contains(QStringLiteral("연속 2번째")));QVERIFY(!rendered.body.contains(QStringLiteral("2번 연속")));
    if(kind==int(ChartPhraseKind::Line))QVERIFY(rendered.body.contains(QStringLiteral("100.5000 USDT")));
    else QVERIFY(rendered.body.contains(QStringLiteral("거래량 급증")));
    QCOMPARE(rendered.packSha256,validation.sha256);QVERIFY(pack.importedSha256().isEmpty());
    QCOMPARE(ChartPhrasePack::plainJson(),original);QCOMPARE(input.samples,kind==int(ChartPhraseKind::Line)?2:7);
}
void ChartPhrasePackTest::plainFallbackHasRequiredFactsWithoutImportedPack() {
    ChartPhrasePack pack;
    const auto plain = ChartPhrasePack::plainJson();
    QVERIFY2(!plain.isEmpty(), "Ordinary fallback templates are loaded from a bundled public JSON resource");
    const auto validation = ChartPhrasePack::validate(plain);
    QVERIFY(validation.valid); QCOMPARE(validation.counts.size(), 7);
    for (const auto count : validation.counts) QVERIFY(count >= 2);
    QVERIFY(pack.importedSha256().isEmpty());
    const auto rendered = pack.render(ChartPhraseKind::Pullback, facts(), true, {}, [](int) { return 0; });
    QVERIFY(rendered.available); QVERIFY(!rendered.serious); QVERIFY(!rendered.title.isEmpty());
    QVERIFY(rendered.body.contains("SYNUSDT")); QVERIFY(rendered.body.contains("8")); QVERIFY(rendered.body.contains("12"));
    QVERIFY(!rendered.body.contains('{')); QVERIFY(!rendered.phraseId.isEmpty());
}
void ChartPhrasePackTest::validPackNeedsExactReviewedHashAndProducesPublicPreview() {
    ChartPhrasePack pack; const auto raw = bytes(syntheticPack()); const auto validation = ChartPhrasePack::validate(raw);
    QVERIFY2(validation.valid, "All valid public variants can be inspected before human review");
    QCOMPARE(validation.sha256, QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
    QCOMPARE(validation.preview.size(), 14); QVERIFY(pack.importedSha256().isEmpty());
    QString error;
    QVERIFY(!pack.activateReviewed(raw, QByteArray(64, '0'), &error)); QVERIFY(!error.isEmpty());
    QVERIFY(pack.activateReviewed(raw, validation.sha256, &error)); QVERIFY(error.isEmpty());
    QCOMPARE(pack.importedSha256(), validation.sha256);
    auto changed = syntheticPack(); changed["name"] = "수정된 합성 팩";
    QVERIFY(!pack.activateReviewed(bytes(changed), validation.sha256, &error));
    QCOMPARE(pack.importedSha256(), validation.sha256);
    pack.clearImported(); QVERIFY(pack.importedSha256().isEmpty());
}
void ChartPhrasePackTest::everyKindRequiresItsExactPlaceholderSet_data() {
    QTest::addColumn<QString>("kind"); QTest::addColumn<QString>("token");
    const QMap<QString, QStringList> required{
        {"pullback", {"sym", "tf", "score", "n"}},
        {"result_up", {"sym", "tf", "ret", "horizon", "n"}}, {"result_down", {"sym", "tf", "ret", "horizon", "n"}},
        {"result_flat", {"sym", "tf", "ret", "horizon", "n"}}, {"notice", {"sym", "source", "n"}},
        {"line", {"sym", "tf", "price", "currency", "n"}}, {"whale", {"sym", "tf", "signal", "n"}}};
    for (auto it = required.cbegin(); it != required.cend(); ++it) for (const auto& token : it.value())
        QTest::newRow((it.key() + "-" + token).toUtf8().constData()) << it.key() << token;
}
void ChartPhrasePackTest::everyKindRequiresItsExactPlaceholderSet() {
    QFETCH(QString, kind); QFETCH(QString, token);
    auto pack = syntheticPack(); QVERIFY(ChartPhrasePack::validate(bytes(pack)).valid);
    editFirst(&pack, kind, [&](QJsonObject& phrase) { phrase["text"] = phrase["text"].toString().remove('{' + token + '}'); });
    const auto rejected = ChartPhrasePack::validate(bytes(pack));
    QVERIFY(!rejected.valid); QVERIFY(!rejected.issues.isEmpty()); QVERIFY(rejected.preview.isEmpty());
}
void ChartPhrasePackTest::malformedUnknownPrivateAndAsymmetricPacksRejectAtomically_data() {
    QTest::addColumn<QString>("change");
    for (const auto name : {"missing-kind", "unknown-kind", "liquidation-override", "result-parity", "duplicate-id", "duplicate-text", "unknown-placeholder", "nested-braces", "format-specifier", "private-root", "self-reviewed", "wrong-schema"})
        QTest::newRow(name) << QString(name);
}
void ChartPhrasePackTest::malformedUnknownPrivateAndAsymmetricPacksRejectAtomically() {
    QFETCH(QString, change); ChartPhrasePack active; const auto original = bytes(syntheticPack());
    const auto baseline = ChartPhrasePack::validate(original); QVERIFY(baseline.valid);
    QVERIFY(active.activateReviewed(original, baseline.sha256));
    auto object = syntheticPack(); auto templates = object["templates"].toObject();
    if (change == "missing-kind") templates.remove("line");
    if (change == "unknown-kind") templates["unknown"] = templates["pullback"];
    if (change == "liquidation-override") templates["liquidation"] = templates["pullback"];
    if (change == "result-parity") { auto rows = templates["result_down"].toArray(); rows.removeLast(); templates["result_down"] = rows; }
    if (change == "duplicate-id" || change == "duplicate-text") {
        auto rows = templates["pullback"].toArray(); auto second = rows[1].toObject();
        const auto field = change == "duplicate-id" ? "id" : "text";
        second[field] = rows[0].toObject()[field]; rows[1] = second; templates["pullback"] = rows;
    }
    object["templates"] = templates;
    if (change == "unknown-placeholder") editFirst(&object, "pullback", [](QJsonObject& p) { p["text"] = p["text"].toString() + " {hit}"; });
    if (change == "nested-braces") editFirst(&object, "pullback", [](QJsonObject& p) { p["text"] = p["text"].toString().replace("{sym}", "{{sym}}"); });
    if (change == "format-specifier") editFirst(&object, "pullback", [](QJsonObject& p) { p["text"] = p["text"].toString().replace("{score}", "{score:.2f}"); });
    if (change == "private-root") object["accountScope"] = "FAKE_PRIVATE_SCOPE";
    if (change == "self-reviewed") object["reviewed"] = true;
    if (change == "wrong-schema") object["schema"] = "unknown/9";
    const auto raw = bytes(object); const auto validation = ChartPhrasePack::validate(raw);
    QVERIFY(!validation.valid); QVERIFY(!validation.issues.isEmpty()); QVERIFY(validation.preview.isEmpty());
    QVERIFY(!active.activateReviewed(raw, QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex()));
    QCOMPARE(active.importedSha256(), baseline.sha256);
}
void ChartPhrasePackTest::knownProhibitedLiteralsAndLengthAreRejected_data() {
    QTest::addColumn<QString>("append");
    QTest::newRow("trade-instruction") << QString(" 매수해.");
    QTest::newRow("leverage-instruction") << QString(" 레버리지 올려.");
    QTest::newRow("loss-mockery") << QString(" 손실 축하.");
    QTest::newRow("harm-reference") << QString(" 자해.");
    QTest::newRow("group-mockery") << QString(" 특정 집단은 한심해.");
    QTest::newRow("person-mockery") << QString(" 실존 인물은 멍청해.");
    QTest::newRow("invented-numeric-claim") << QString(" 수익률 99%.");
    QTest::newRow("link") << QString(" https://example.invalid");
    QTest::newRow("markup") << QString(" <b>문장</b>");
    QTest::newRow("length") << QString(161, 'x');
}
void ChartPhrasePackTest::knownProhibitedLiteralsAndLengthAreRejected() {
    QFETCH(QString, append); auto pack = syntheticPack(); QVERIFY(ChartPhrasePack::validate(bytes(pack)).valid);
    editFirst(&pack, "pullback", [&](QJsonObject& phrase) { phrase["text"] = phrase["text"].toString() + append; });
    const auto result = ChartPhrasePack::validate(bytes(pack));
    QVERIFY(!result.valid); QVERIFY(!result.issues.isEmpty()); QVERIFY(result.preview.isEmpty());
}
void ChartPhrasePackTest::reviewProhibitedPatterns_data() {
    QTest::addColumn<QString>("append"); QTest::addColumn<QString>("category");
    for (const auto* text : {"한강", "한강물", "물온도", "한 강 물", "물 온 도"})
        QTest::newRow(text) << QString::fromUtf8(text) << QString("harm");
    for (const auto* text : {"사라", "팔아라", "들어가라", "숏쳐", "롱쳐", "탑승해", "사 라!", "팔 아 라.", "들어 가라?", "숏 쳐", "롱 쳐!", "탑 승 해…"})
        QTest::newRow(text) << QString::fromUtf8(text) << QString("instruction");
    for (const auto* text : {"개미새끼", "흑우새끼", "틀딱", "한남충", "한녀충"})
        QTest::newRow(text) << QString::fromUtf8(text) << QString("attack");
}
void ChartPhrasePackTest::reviewProhibitedPatterns() {
    QFETCH(QString, append); QFETCH(QString, category); auto pack = syntheticPack();
    editFirst(&pack, "pullback", [&](QJsonObject& phrase) { phrase["text"] = phrase["text"].toString() + " " + append; });
    const auto result = ChartPhrasePack::validate(bytes(pack));
    QVERIFY(!result.valid); QCOMPARE(result.issues.size(), 1); QCOMPARE(result.issues.first().code, category);
    QVERIFY(result.preview.isEmpty());
}
void ChartPhrasePackTest::nonImperativeBoundarySentencesRemainValid_data() {
    QTest::addColumn<QString>("append");
    for (const auto* text : {"사라졌다", "지금 사라졌다", "팔아라고 하지 않음", "들어가라고 말하지 않음", "숏쳐라는 명령은 제외", "롱쳐라는 문구 없음", "탑승해도 되는지는 모름", "개미 투자자의 관측", "노년층과 청년층의 관측"})
        QTest::newRow(text) << QString::fromUtf8(text);
}
void ChartPhrasePackTest::nonImperativeBoundarySentencesRemainValid() {
    QFETCH(QString, append); auto pack = syntheticPack();
    editFirst(&pack, "pullback", [&](QJsonObject& phrase) { phrase["text"] = phrase["text"].toString() + " " + append; });
    const auto result = ChartPhrasePack::validate(bytes(pack));
    QVERIFY(result.valid); QVERIFY(result.issues.isEmpty()); QCOMPARE(result.preview.size(), 14);
}
void ChartPhrasePackTest::invalidUtf8ControlsAndSizeNeverExposePreview_data() {
    QTest::addColumn<QString>("change");
    for (const auto name : {"utf8", "nul", "bidi", "zero-width", "astral-format", "surrogate", "size", "trailing-json"}) QTest::newRow(name) << QString(name);
}
void ChartPhrasePackTest::invalidUtf8ControlsAndSizeNeverExposePreview() {
    QFETCH(QString, change); auto pack = syntheticPack(); QVERIFY(ChartPhrasePack::validate(bytes(pack)).valid);
    if (change == "nul" || change == "bidi" || change == "zero-width") editFirst(&pack, "pullback", [&](QJsonObject& p) {
        p["text"] = p["text"].toString() + QChar(change == "nul" ? 0 : change == "bidi" ? 0x202e : 0x200b);
    });
    if (change == "astral-format") editFirst(&pack, "pullback", [](QJsonObject& p) {
        p["text"] = p["text"].toString() + QString::fromUtf8("\xf3\xa0\x80\x81"); // U+E0001 LANGUAGE TAG, Other_Format
    });
    auto raw = bytes(pack);
    if (change == "utf8") raw.replace("SYNTHETIC_A", QByteArray("\xc3\x28", 2));
    if (change == "surrogate") raw.replace("SYNTHETIC_A", "\\ud800");
    if (change == "size") raw = QByteArray(256 * 1024 + 1, ' ');
    if (change == "trailing-json") raw += "{}";
    const auto result = ChartPhrasePack::validate(raw);
    QVERIFY(!result.valid); QVERIFY(!result.issues.isEmpty()); QVERIFY(result.preview.isEmpty());
}
void ChartPhrasePackTest::previousSuccessfulPhraseIsExcludedBeforeRandomSelection() {
    ChartPhrasePack pack; const auto raw = bytes(syntheticPack()); const auto valid = ChartPhrasePack::validate(raw);
    QVERIFY(valid.valid); QVERIFY(pack.activateReviewed(raw, valid.sha256));
    QString previous, previousBody;
    for (int i = 0; i < 6; ++i) {
        const auto rendered = pack.render(ChartPhraseKind::Pullback, facts(), true, previous, [](int) { return 0; });
        QVERIFY(rendered.available); QVERIFY(rendered.phraseId != previous); QVERIFY(rendered.body != previousBody);
        QVERIFY(rendered.body.contains("SYNTHETIC")); QVERIFY(!rendered.body.contains('{'));
        previous = rendered.phraseId; previousBody = rendered.body;
    }
    QVERIFY(!pack.render(ChartPhraseKind::Pullback, facts(), true, {}, [](int n) { return n + 10; }).available);
}
void ChartPhrasePackTest::dripOffImmediatelyUsesPlainAndLiquidationNeverUsesImportedText() {
    ChartPhrasePack pack; const auto raw = bytes(syntheticPack()); const auto valid = ChartPhrasePack::validate(raw);
    QVERIFY(valid.valid); QVERIFY(pack.activateReviewed(raw, valid.sha256));
    const auto custom = pack.render(ChartPhraseKind::Pullback, facts(), true, {}, [](int) { return 0; });
    const auto plain = pack.render(ChartPhraseKind::Pullback, facts(), false, {}, [](int) { return 0; });
    QVERIFY(custom.available); QVERIFY(plain.available); QVERIFY(custom.body.contains("SYNTHETIC")); QVERIFY(!plain.body.contains("SYNTHETIC"));
    const auto serious = pack.render(ChartPhraseKind::Liquidation, facts(), true, {}, [](int) { return 0; });
    const auto seriousOff = pack.render(ChartPhraseKind::Liquidation, facts(), false, {}, [](int) { return 1; });
    QVERIFY(serious.available); QVERIFY(serious.serious); QCOMPARE(serious.body, seriousOff.body);
    QVERIFY(serious.body.contains(QStringLiteral("청산 위험"))); QVERIFY(!serious.body.contains("SYNTHETIC"));
    pack.clearImported(); QCOMPARE(pack.render(ChartPhraseKind::Liquidation, facts(), true).body, serious.body);
}
void ChartPhrasePackTest::bundledDripPackRunsWhileDripIsOnUntilAReviewedPackIsImported() {
    const auto drip = ChartPhrasePack::dripJson(); const auto dripValidation = ChartPhrasePack::validate(drip);
    QVERIFY(dripValidation.valid); QCOMPARE(dripValidation.preview.size(), 14); QVERIFY(drip != ChartPhrasePack::plainJson());
    ChartPhrasePack pack;
    const auto on = pack.render(ChartPhraseKind::Pullback, facts(), true, {}, [](int) { return 0; });
    const auto off = pack.render(ChartPhraseKind::Pullback, facts(), false, {}, [](int) { return 0; });
    QVERIFY(on.available); QVERIFY(off.available);
    QVERIFY2(on.phraseId.startsWith("drip_"), qPrintable(on.phraseId)); QCOMPARE(on.packSha256, dripValidation.sha256);
    QVERIFY2(off.phraseId.startsWith("plain_"), qPrintable(off.phraseId));
    const auto raw = bytes(syntheticPack()); const auto valid = ChartPhrasePack::validate(raw); QVERIFY(pack.activateReviewed(raw, valid.sha256));
    QVERIFY(pack.render(ChartPhraseKind::Pullback, facts(), true, {}, [](int) { return 0; }).body.contains("SYNTHETIC"));
    pack.clearImported(); QCOMPARE(pack.render(ChartPhraseKind::Pullback, facts(), true, {}, [](int) { return 0; }).phraseId, on.phraseId);
}
void ChartPhrasePackTest::bundledResultSentencesNeverSayFeesWereDeducted() {
    // Signal results exclude costs and slippage; copy must not read as a net-of-fee return.
    for (const auto& raw : {ChartPhrasePack::plainJson(), ChartPhrasePack::dripJson()}) {
        const auto validation = ChartPhrasePack::validate(raw); QVERIFY(validation.valid);
        for (const auto& phrase : validation.preview) {
            if (!phrase.kind.startsWith("result_")) continue;
            QVERIFY2(phrase.text.contains(QStringLiteral("수수료")), qPrintable(phrase.text));
            for (const auto* wrong : {"수수료는 뺐", "수수료 빼고", "수수료 떼고", "수수료 제외"})
                QVERIFY2(!phrase.text.contains(QString::fromUtf8(wrong)), qPrintable(phrase.text));
        }
    }
}
void ChartPhrasePackTest::liquidationSourceDisplaysProviderWithoutChangingFactsOrPack() {
    ChartPhrasePack pack;auto input=facts();input.source="https://fapi.binance.com/fapi/v3/positionRisk";
    const auto original=input;const auto plain=ChartPhrasePack::plainJson();
    const auto rendered=pack.render(ChartPhraseKind::Liquidation,input,false);
    QVERIFY(rendered.available);QVERIFY(rendered.serious);
    QVERIFY2(!rendered.body.contains("://"),qPrintable(rendered.body));QVERIFY(rendered.body.contains("Binance Futures"));
    QVERIFY(rendered.body.contains(input.symbol));QVERIFY(rendered.body.contains(input.positionSide));QVERIFY(rendered.body.contains("5%"));
    QCOMPARE(input.source,original.source);QCOMPARE(input.observedAtMs,original.observedAtMs);QCOMPARE(input.distancePercent,original.distancePercent);
    const auto drip=pack.render(ChartPhraseKind::Liquidation,input,true);QCOMPARE(drip.body,rendered.body);QCOMPARE(drip.packSha256,rendered.packSha256);
    QCOMPARE(ChartPhrasePack::plainJson(),plain);QVERIFY(pack.importedSha256().isEmpty());
}
void ChartPhrasePackTest::missingOrContradictoryFactsNeverInventNumbers_data() {
    QTest::addColumn<QString>("change");
    for (const auto name : {"symbol", "interval", "score", "sample", "result-sign", "missing-result", "missing-price", "missing-distance", "nan-distance", "missing-source-time", "unknown-side"})
        QTest::newRow(name) << QString(name);
}
void ChartPhrasePackTest::missingOrContradictoryFactsNeverInventNumbers() {
    QFETCH(QString, change); ChartPhrasePack pack;
    QVERIFY(ChartPhrasePack::validate(ChartPhrasePack::plainJson()).valid);
    auto values = facts(); auto kind = ChartPhraseKind::Pullback;
    if (change == "symbol") values.symbol.clear();
    if (change == "interval") values.intervalMs = 7;
    if (change == "score") values.score = 11;
    if (change == "sample") values.samples = -1;
    if (change == "result-sign") { kind = ChartPhraseKind::ResultUp; values.resultReturnPercent = -1; }
    if (change == "missing-result") { kind = ChartPhraseKind::ResultDown; values.resultReturnPercent.reset(); }
    if (change == "missing-price") { kind = ChartPhraseKind::Line; values.price.reset(); }
    if (change == "missing-distance") { kind = ChartPhraseKind::Liquidation; values.distancePercent.reset(); }
    if (change == "nan-distance") { kind = ChartPhraseKind::Liquidation; values.distancePercent = std::numeric_limits<double>::quiet_NaN(); }
    if (change == "missing-source-time") { kind = ChartPhraseKind::Liquidation; values.observedAtMs = 0; }
    if (change == "unknown-side") { kind = ChartPhraseKind::Liquidation; values.positionSide = "UNKNOWN"; }
    const auto result = pack.render(kind, values, true);
    QVERIFY(!result.available); QVERIFY(!result.reason.isEmpty()); QVERIFY(result.body.isEmpty());
}
void ChartPhrasePackTest::QObjectLifetimeCanBeObservedWithoutBorrowedDanglingPointers() {
    QPointer<ChartPhrasePack> guard = new ChartPhrasePack;
    delete guard.data(); QVERIFY(guard.isNull());
}
void ChartPhrasePackTest::verifiedOneWayPositionSideIsDisplayedWithoutGuessingDirection() {
    ChartPhrasePack pack;
    QVERIFY(ChartPhrasePack::validate(ChartPhrasePack::plainJson()).valid);
    auto values = facts(); values.positionSide = "BOTH";
    const auto rendered = pack.render(ChartPhraseKind::Liquidation, values, true);
    QVERIFY(rendered.available); QVERIFY(rendered.serious);
    QVERIFY(rendered.body.contains("BOTH"));
    QVERIFY(!rendered.body.contains("LONG")); QVERIFY(!rendered.body.contains("SHORT"));
    QCOMPARE(pack.render(ChartPhraseKind::Liquidation, values, false).body, rendered.body);
}

QTEST_GUILESS_MAIN(ChartPhrasePackTest)
#include "tst_chart_phrase_pack.moc"
