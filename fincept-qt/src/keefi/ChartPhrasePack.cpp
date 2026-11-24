#include "keefi/ChartPhrasePack.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/ProductIdentity.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/UsabilityPresentation.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <cmath>

// Keep public JSON resources linked when the module is in the static library.
static void initKeefiPhraseResources() { Q_INIT_RESOURCE(keefi_phrases); }

namespace keefi {
namespace {
constexpr qsizetype MaxPackBytes = 256 * 1024;
struct Phrase { QString id, text; };
using Templates = QMap<QString, QList<Phrase>>;
const QMap<QString, QSet<QString>>& required() {
    static const QMap<QString, QSet<QString>> value{
        {"pullback", {"sym", "tf", "score", "n"}},
        {"result_up", {"sym", "tf", "ret", "horizon", "n"}},
        {"result_down", {"sym", "tf", "ret", "horizon", "n"}},
        {"result_flat", {"sym", "tf", "ret", "horizon", "n"}},
        {"notice", {"sym", "source", "n"}},
        {"line", {"sym", "tf", "price", "currency", "n"}},
        {"whale", {"sym", "tf", "signal", "n"}}};
    return value;
}
QString normalized(const QString& value) {
    return value.normalized(QString::NormalizationForm_KC).toCaseFolded().simplified();
}
bool plainCharacters(const QString& text, qsizetype maximum) {
    if (text.isEmpty() || text.size() > maximum || text.trimmed().isEmpty()) return false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c.isHighSurrogate()) {
            if (i + 1 >= text.size() || !text[i + 1].isLowSurrogate()) return false;
            const auto category = QChar::category(QChar::surrogateToUcs4(c, text[i + 1]));
            if (category == QChar::Other_Control || category == QChar::Other_Format ||
                category == QChar::Separator_Line || category == QChar::Separator_Paragraph) return false;
            ++i; continue;
        }
        if (c.isLowSurrogate() || c.category() == QChar::Other_Control || c.category() == QChar::Other_Format ||
            c.category() == QChar::Separator_Line || c.category() == QChar::Separator_Paragraph) return false;
    }
    return true;
}
bool publicFact(const QString& text, qsizetype maximum) {
    return plainCharacters(text, maximum) && !text.contains('{') && !text.contains('}') &&
           !text.contains('<') && !text.contains('>');
}
bool exactKeys(const QJsonObject& object, const QSet<QString>& expected) {
    const auto keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}
bool idValid(const QString& id) {
    static const QRegularExpression expression(QStringLiteral("\\A[a-z][a-z0-9_-]{0,31}\\z"));
    return expression.match(id).hasMatch();
}
QString bannedCategory(const QString& original) {
    const QString text = normalized(original);
    QString compact = text; compact.remove(QRegularExpression(QStringLiteral("\\s+")));
    // Fixed known patterns are a limited content filter, not a semantic safety guarantee
    // or a replacement for human review. Ending checks deliberately leave quoted or
    // negated boundary prose such as "사라졌다" / "팔아라고 하지 않음" for that review.
    for (const auto* token : {"매수해", "매도해", "풀매수", "몰빵", "물타기", "빚내", "레버리지올려",
                              "레버리지늘려", "레버리지높여"})
        if (compact.contains(QString::fromUtf8(token))) return QStringLiteral("instruction");
    QString ending = compact;
    ending.remove(QRegularExpression(QStringLiteral("[\\p{P}]+$")));
    for (const auto* token : {"사라", "팔아라", "들어가라", "숏쳐", "롱쳐", "탑승해", "지금팔아"})
        if (ending.endsWith(QString::fromUtf8(token))) return QStringLiteral("instruction");
    for (const auto* token : {"자해", "자살", "투신", "극단적선택", "죽어버", "죽어라", "한강", "물온도"})
        if (compact.contains(QString::fromUtf8(token))) return QStringLiteral("harm");
    for (const auto* token : {"한심", "멍청", "병신", "바보새끼", "꼴좋", "개미새끼", "흑우새끼", "틀딱", "한남충", "한녀충"})
        if (compact.contains(QString::fromUtf8(token))) return QStringLiteral("attack");
    if ((compact.contains(QStringLiteral("손실")) || compact.contains(QStringLiteral("청산"))) &&
        (compact.contains(QStringLiteral("축하")) || compact.contains(QStringLiteral("비웃")) ||
         compact.contains(QStringLiteral("ㅋㅋ")))) return QStringLiteral("mockery");
    if (text.contains(QStringLiteral("://")) || text.contains(QStringLiteral("www.")) ||
        text.contains('<') || text.contains('>') || text.contains(QStringLiteral("]("))) return QStringLiteral("link_markup");
    return {};
}
bool placeholders(const QString& text, const QSet<QString>& expected, QString* problem = nullptr) {
    QSet<QString> found;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == '}') { if (problem) *problem = QStringLiteral("unbalanced_placeholder"); return false; }
        if (text[i] != '{') continue;
        const qsizetype end = text.indexOf('}', i + 1);
        if (end < 0 || (i && text[i - 1] == '$')) { if (problem) *problem = QStringLiteral("invalid_placeholder"); return false; }
        const QString token = text.mid(i + 1, end - i - 1);
        if (!expected.contains(token)) { if (problem) *problem = QStringLiteral("unknown_placeholder"); return false; }
        found.insert(token); i = end;
    }
    if (found != expected) { if (problem) *problem = QStringLiteral("missing_placeholder"); return false; }
    return true;
}

// Qt discards duplicate JSON keys. Check the syntax-parsed input before using that
// representation, including escaped spellings of the same key.
class JsonKeys final {
  public:
    explicit JsonKeys(const QString& text) : text_(text) {}
    bool valid() { return value(0); }
  private:
    const QString& text_; qsizetype at_ = 0;
    void whitespace() { while (at_ < text_.size() && text_[at_].isSpace()) ++at_; }
    QString string() {
        const qsizetype start = at_++;
        while (at_ < text_.size()) {
            if (text_[at_] == '\\') { at_ += 2; continue; }
            if (text_[at_++] == '"') break;
        }
        const auto wrapper = QByteArray("[") + text_.mid(start, at_ - start).toUtf8() + ']';
        return QJsonDocument::fromJson(wrapper).array().first().toString();
    }
    bool value(int depth) {
        if (depth > 16) return false;
        whitespace(); if (at_ >= text_.size()) return false;
        if (text_[at_] == '"') { string(); return true; }
        if (text_[at_] == '{') {
            ++at_; whitespace(); QSet<QString> keys;
            if (text_[at_] == '}') { ++at_; return true; }
            while (at_ < text_.size()) {
                const auto key = string();
                if (keys.contains(key)) return false;
                keys.insert(key); whitespace(); ++at_;
                if (!value(depth + 1)) return false;
                whitespace(); if (text_[at_++] == '}') return true;
                whitespace();
            }
            return false;
        }
        if (text_[at_] == '[') {
            ++at_; whitespace(); if (text_[at_] == ']') { ++at_; return true; }
            while (at_ < text_.size()) {
                if (!value(depth + 1)) return false;
                whitespace(); if (text_[at_++] == ']') return true;
            }
            return false;
        }
        while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != ']' && text_[at_] != '}' && !text_[at_].isSpace()) ++at_;
        return true;
    }
};

ChartPhraseValidation inspect(const QByteArray& raw, Templates* parsed = nullptr) {
    ChartPhraseValidation result;
    auto reject = [&](const QString& code, const QString& kind = {}, const QString& id = {}, const QString& field = {}) {
        result.issues.append({code, kind, id, field, 0}); result.preview.clear(); return result;
    };
    if (raw.isEmpty() || raw.size() > MaxPackBytes) return reject(QStringLiteral("byte_limit"));
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(raw);
    if (decoder.hasError() || text.startsWith(QChar(0xfeff))) return reject(QStringLiteral("utf8"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return reject(QStringLiteral("json"));
    if (!JsonKeys(text).valid()) return reject(QStringLiteral("duplicate_key_or_depth"));
    const auto root = document.object();
    if (!exactKeys(root, {"schema", "language", "id", "name", "templates"})) return reject(QStringLiteral("root_keys"));
    if (canonicalPublicFormat(root.value("schema").toString()) != "keefi-chart-phrases/1" || root.value("language").toString() != "ko-KR")
        return reject(QStringLiteral("schema"));
    if (!root.value("id").isString() || !idValid(root.value("id").toString())) return reject(QStringLiteral("pack_id"));
    if (!root.value("name").isString() || !publicFact(root.value("name").toString(), 64) ||
        !bannedCategory(root.value("name").toString()).isEmpty()) return reject(QStringLiteral("pack_name"));
    if (!root.value("templates").isObject()) return reject(QStringLiteral("templates"));
    const auto groups = root.value("templates").toObject(); const auto kinds = required().keys();
    if (!exactKeys(groups, QSet<QString>(kinds.cbegin(), kinds.cend()))) return reject(QStringLiteral("kind_keys"));
    Templates templates; QSet<QString> ids; QList<ChartPhrasePreview> previews;
    for (auto kind = required().cbegin(); kind != required().cend(); ++kind) {
        const auto rows = groups.value(kind.key());
        if (!rows.isArray() || rows.toArray().size() < 2 || rows.toArray().size() > 32)
            return reject(QStringLiteral("variant_count"), kind.key());
        QSet<QString> uniqueText;
        for (const auto& row : rows.toArray()) {
            if (!row.isObject()) return reject(QStringLiteral("phrase_object"), kind.key());
            const auto phrase = row.toObject();
            if (!exactKeys(phrase, {"id", "text"})) return reject(QStringLiteral("phrase_keys"), kind.key());
            const auto id = phrase.value("id").toString();
            if (!phrase.value("id").isString() || !idValid(id) || ids.contains(id)) return reject(QStringLiteral("phrase_id"), kind.key());
            const auto body = phrase.value("text").toString();
            if (!phrase.value("text").isString() || !plainCharacters(body, 160))
                return reject(QStringLiteral("text_length_or_control"), kind.key(), id, QStringLiteral("text"));
            QString problem;
            if (!placeholders(body, kind.value(), &problem)) return reject(problem, kind.key(), id, QStringLiteral("text"));
            QString literal = normalized(body); literal.remove(QRegularExpression(QStringLiteral("\\{[a-z_]+\\}")));
            for (const auto c : literal) if (c.isDigit()) return reject(QStringLiteral("numeric_literal"), kind.key(), id, QStringLiteral("text"));
            const auto banned = bannedCategory(body);
            if (!banned.isEmpty()) return reject(banned, kind.key(), id, QStringLiteral("text"));
            const QString identity = normalized(body);
            if (uniqueText.contains(identity)) return reject(QStringLiteral("duplicate_text"), kind.key(), id, QStringLiteral("text"));
            uniqueText.insert(identity); ids.insert(id);
            templates[kind.key()].append({id, body}); previews.append({kind.key(), id, body});
        }
        result.counts.insert(kind.key(), templates.value(kind.key()).size());
    }
    if (result.counts.value("result_up") != result.counts.value("result_down") || result.counts.value("result_up") != result.counts.value("result_flat"))
        return reject(QStringLiteral("result_parity"));
    result.valid = true; result.sha256 = QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex();
    result.packId = root.value("id").toString(); result.name = root.value("name").toString(); result.preview = previews;
    if (parsed) *parsed = templates;
    return result;
}
QByteArray resource(const char* path) {
    static const bool initialized = [] { initKeefiPhraseResources(); return true; }(); Q_UNUSED(initialized);
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxPackBytes) return {};
    return file.readAll();
}
QString kindKey(ChartPhraseKind kind) {
    switch (kind) {
    case ChartPhraseKind::Pullback: return QStringLiteral("pullback");
    case ChartPhraseKind::ResultUp: return QStringLiteral("result_up");
    case ChartPhraseKind::ResultDown: return QStringLiteral("result_down");
    case ChartPhraseKind::ResultFlat: return QStringLiteral("result_flat");
    case ChartPhraseKind::Notice: return QStringLiteral("notice");
    case ChartPhraseKind::Line: return QStringLiteral("line");
    case ChartPhraseKind::Whale: return QStringLiteral("whale");
    case ChartPhraseKind::Liquidation: return {};
    }
    return {};
}
QString timeframe(qint64 interval) {
    switch (interval) {
    case 60000: return QStringLiteral("1분"); case 300000: return QStringLiteral("5분");
    case 900000: return QStringLiteral("15분"); case 3600000: return QStringLiteral("1시간");
    case 14400000: return QStringLiteral("4시간"); case 86400000: return QStringLiteral("1일");
    default: return {};
    }
}
QString number(double value, bool signedValue = false) {
    const QString result = QString::number(value == 0 ? 0.0 : value, 'g', 15);
    return signedValue && value > 0 ? '+' + result : result;
}
QString substitute(const QString& text, const QMap<QString, QString>& values) {
    QString result;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] != '{') { result += text[i]; continue; }
        const auto end = text.indexOf('}', i + 1);
        if (end < 0) return {};
        const auto key = text.mid(i + 1, end - i - 1);
        if (!values.contains(key)) return {};
        result += values.value(key); i = end;
    }
    return result;
}
int exactPriceCompare(const QString& a,const QString& b){
    const auto ap=a.indexOf('.'),bp=b.indexOf('.');const auto ai=ap<0?a:a.left(ap),bi=bp<0?b:b.left(bp);if(ai.size()!=bi.size())return ai.size()<bi.size()?-1:1;const auto whole=QString::compare(ai,bi);if(whole)return whole<0?-1:1;
    const auto af=ap<0?QString{}:a.mid(ap+1),bf=bp<0?QString{}:b.mid(bp+1);for(qsizetype i=0;i<qMax(af.size(),bf.size());++i){const auto ac=i<af.size()?af.at(i):QChar('0'),bc=i<bf.size()?bf.at(i):QChar('0');if(ac!=bc)return ac<bc?-1:1;}return 0;
}
bool exactLinePrice(const QString& value){
    static const QRegularExpression grammar("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z");static const QString minimum="0."+QString(99,'0')+"1",maximum="1"+QString(100,'0');return value.size()<=512&&grammar.match(value).hasMatch()&&exactPriceCompare(value,minimum)>=0&&exactPriceCompare(value,maximum)<=0;
}
bool factValues(ChartPhraseKind kind, const ChartPhraseFacts& facts, QMap<QString, QString>* values) {
    // A verified Binance name may use two UTF-16 units per scalar. Preserve
    // its exact bounded identifier without broadening arbitrary public text.
    const auto symbolLimit=(kind==ChartPhraseKind::Whale||kind==ChartPhraseKind::Line)&&binanceIdentifierValid(facts.symbol)?80:64;
    if (!publicFact(facts.symbol, symbolLimit)) return false;
    values->insert("sym", facts.symbol);
    if (kind == ChartPhraseKind::Liquidation) {
        if ((facts.positionSide != "LONG" && facts.positionSide != "SHORT" && facts.positionSide != "BOTH") || !publicFact(facts.source, 256) ||
            !facts.distancePercent || !std::isfinite(*facts.distancePercent) || *facts.distancePercent < 0 || facts.observedAtMs <= 0) return false;
        const auto at = QDateTime::fromMSecsSinceEpoch(facts.observedAtMs, Qt::UTC);
        if (!at.isValid()) return false;
        values->insert("side", facts.positionSide); values->insert("dist", number(*facts.distancePercent));
        values->insert("source", facts.source); values->insert("observedAt", at.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")));
        return true;
    }
    if (kind == ChartPhraseKind::Notice) {
        if (!publicFact(facts.source, 256) || facts.noticeCount < 1) return false;
        values->insert("source", facts.source); values->insert("n", QString::number(facts.noticeCount)); return true;
    }
    const auto tf = timeframe(facts.intervalMs);
    if (tf.isEmpty() || facts.samples < 0) return false;
    values->insert("tf", tf); values->insert("n", QString::number(facts.samples));
    if (kind == ChartPhraseKind::Pullback) {
        if (facts.score < 0 || facts.score > 10) return false;
        values->insert("score", QString::number(facts.score)); return true;
    }
    if (kind == ChartPhraseKind::ResultUp || kind == ChartPhraseKind::ResultDown || kind == ChartPhraseKind::ResultFlat) {
        const qint64 expected = facts.intervalMs <= 300000 ? 3600000 : facts.intervalMs == 900000 ? 14400000 : 86400000;
        if (facts.samples < 1 || !facts.resultReturnPercent || !std::isfinite(*facts.resultReturnPercent) || facts.horizonMs != expected) return false;
        const double change = *facts.resultReturnPercent;
        if ((kind == ChartPhraseKind::ResultUp && change <= 0) || (kind == ChartPhraseKind::ResultDown && change >= 0) ||
            (kind == ChartPhraseKind::ResultFlat && change != 0)) return false;
        values->insert("ret", number(change, true)); values->insert("horizon", timeframe(expected)); return true;
    }
    if (kind == ChartPhraseKind::Line) {
        if(facts.samples<1||!binanceIdentifierValid(facts.currency))return false;
        if(facts.priceText){if(!exactLinePrice(*facts.priceText))return false;values->insert("price",*facts.priceText);}
        else{if(!facts.price||!std::isfinite(*facts.price)||*facts.price<=0)return false;values->insert("price",number(*facts.price));}
        values->insert("currency",facts.currency);return true;
    }
    if (kind == ChartPhraseKind::Whale) {
        if (facts.samples < 1) return false;
        QString label;
        switch (facts.whaleSignal) {
        case ChartWhalePhraseSignal::LargeTrade: label = QStringLiteral("대량 체결"); break;
        case ChartWhalePhraseSignal::TakerSkew: label = QStringLiteral("시장가 체결 쏠림"); break;
        case ChartWhalePhraseSignal::OpenInterestJump: label = QStringLiteral("미결제약정 급변"); break;
        case ChartWhalePhraseSignal::VolumeBurst: label = QStringLiteral("거래량 급증"); break;
        case ChartWhalePhraseSignal::Rebound: label = QStringLiteral("반등 조건"); break;
        case ChartWhalePhraseSignal::TurnDown: label = QStringLiteral("꺾임 조건"); break;
        case ChartWhalePhraseSignal::None: return false;
        }
        if (label.isEmpty()) return false;
        values->insert("signal", label); return true;
    }
    return false;
}
QString title(ChartPhraseKind kind) {
    switch (kind) {
    case ChartPhraseKind::Pullback: return QStringLiteral("%1 눌림존 알림").arg(productName());
    case ChartPhraseKind::ResultUp: case ChartPhraseKind::ResultDown: case ChartPhraseKind::ResultFlat: return QStringLiteral("%1 공개 신호 결과").arg(productName());
    case ChartPhraseKind::Notice: return QStringLiteral("%1 공식 공지").arg(productName());
    case ChartPhraseKind::Line: return QStringLiteral("%1 기준선 알림").arg(productName());
    case ChartPhraseKind::Whale: return QStringLiteral("%1 큰손·반전 알림").arg(productName());
    case ChartPhraseKind::Liquidation: return QStringLiteral("%1 청산 경고").arg(productName());
    }
    return {};
}
}

struct ChartPhrasePack::Private { Templates imported; QByteArray importedHash; };
ChartPhrasePack::ChartPhrasePack(QObject* parent) : QObject(parent), d_(std::make_unique<Private>()) {}
ChartPhrasePack::~ChartPhrasePack() = default;
ChartPhraseValidation ChartPhrasePack::validate(const QByteArray& raw) { return inspect(raw); }
QByteArray ChartPhrasePack::plainJson() { return resource(":/keefi/phrases/plain.json"); }
QByteArray ChartPhrasePack::dripJson() { return resource(":/keefi/phrases/drip.json"); }
bool ChartPhrasePack::activateReviewed(const QByteArray& raw, const QByteArray& hash, QString* error) {
    Templates candidate; const auto validation = inspect(raw, &candidate);
    if (!validation.valid || validation.sha256 != hash) {
        if (error) *error = QStringLiteral("자동 검사 결과와 직접 검수한 문구 팩 지문을 확인하세요.");
        return false;
    }
    d_->imported = candidate; d_->importedHash = validation.sha256;
    if (error) error->clear();
    return true;
}
void ChartPhrasePack::clearImported() { d_->imported.clear(); d_->importedHash.clear(); }
QByteArray ChartPhrasePack::importedSha256() const { return d_->importedHash; }
ChartPhraseText ChartPhrasePack::render(ChartPhraseKind kind, const ChartPhraseFacts& facts, bool dripEnabled,
                                      const QString& previous, IndexSelector choose) const {
    ChartPhraseText result;
    auto fail = [&](const QString& reason) { result.reason = reason; return result; };
    QMap<QString, QString> values;
    if (!factValues(kind, facts, &values)) return fail(QStringLiteral("표시에 필요한 검증된 사실이 없거나 서로 맞지 않습니다."));
    if(values.contains("source")){
        auto shown=usabilityProviderName(values.value("source"));
        if(shown.isEmpty())shown=usabilityPublicText(values.value("source"));
        if(shown.contains("://"))shown.clear();
        values["source"]=shown.isEmpty()?usabilityText(UsabilityText::ProviderUnknown):shown;
    }
    QString body, id; QByteArray hash;
    if (kind == ChartPhraseKind::Liquidation) {
        const auto raw = resource(":/keefi/phrases/liquidation-serious.json");
        const auto document = QJsonDocument::fromJson(raw); const auto object = document.object();
        const QSet<QString> tokens{"sym", "side", "dist", "source", "observedAt"};
        // The reviewed resource bytes keep their original title; the displayed title is selected below.
        if (!document.isObject() || !exactKeys(object, {"schema", "id", "title", "text"}) ||
            object.value("schema").toString() != "keefi-liquidation-phrase/1" || object.value("id").toString() != "serious_liquidation" ||
            object.value("title").toString() != QStringLiteral("KEEFI 청산 경고") || !plainCharacters(object.value("text").toString(), 160) ||
            !placeholders(object.value("text").toString(), tokens)) return fail(QStringLiteral("기본 청산 경고 문장을 불러오지 못했습니다."));
        body = object.value("text").toString(); id = object.value("id").toString();
        hash = QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex(); result.serious = true;
    } else {
        // Drip uses an imported reviewed pack first, then the bundled drip pack; off uses plain.
        const bool imported = dripEnabled && !d_->importedHash.isEmpty();
        Templates bundled; const auto validation = inspect(dripEnabled && !imported ? dripJson() : plainJson(), &bundled);
        if (!validation.valid) return fail(QStringLiteral("기본 문구 팩을 불러오지 못했습니다."));
        const auto candidates = (imported ? d_->imported : bundled).value(kindKey(kind));
        QList<Phrase> eligible;
        for (const auto& phrase : candidates) if (phrase.id != previous) eligible.append(phrase);
        if (eligible.isEmpty()) return fail(QStringLiteral("사용 가능한 다음 문장이 없습니다."));
        const int index = choose ? choose(eligible.size()) : QRandomGenerator::global()->bounded(int(eligible.size()));
        if (index < 0 || index >= eligible.size()) return fail(QStringLiteral("문장 선택 결과가 범위를 벗어났습니다."));
        body = eligible[index].text; id = eligible[index].id; hash = imported ? d_->importedHash : validation.sha256;
    }
    const QString rendered = substitute(body, values);
    if (!publicFact(rendered, kind==ChartPhraseKind::Line?1024:320)) return fail(QStringLiteral("치환한 문장이 표시 한도를 벗어났습니다."));
    result.available = true; result.title = title(kind); result.body = rendered; result.phraseId = id; result.packSha256 = hash;
    return result;
}
} // namespace keefi
