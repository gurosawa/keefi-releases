#include "keefi/PublicFormatMigration.h"
#include "keefi/LegacyPublicFormatPrivate.h"
#include "keefi/ProfileMigrationPrivate.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace keefi {
#ifdef Q_OS_UNIX
namespace {
struct Rule { const char* key; const char* field; QStringList formats; };
const QList<Rule> Rules{
    {"chart/studies/v1", "format", {"keefi-chart-studies/1", "keefi-chart-studies/2", "keefi-chart-studies/3", "keefi-chart-studies/4", "keefi-chart-studies/5"}},
    {"chart/drawings/v1", "schema", {"keefi-chart-drawings/1"}},
    {"chart/alerts/ui/v1", "format", {"keefi-chart-alert-ui/1"}},
    {"chart/observations/v1", "format", {"keefi-chart-observations/1"}},
    {"chart/watchlists/v2", "schema", {"keefi-watchlist-workspace/2"}},
    {"display/movementColors/v1", "schema", {"keefi-movement-colors/1"}},
    {"chart/stock/sessions/v1", "format", {"keefi-stock-sessions/1"}}
};
bool fail(QString* error) {
    if (error) *error = QStringLiteral("복사한 공개 설정의 형식·파일 경계·변환 결과를 확인하지 못했습니다. 원본은 보존됩니다.");
    return false;
}
QByteArray read(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; }
QString sha(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()); }
bool sameMetadata(const struct stat& a, const struct stat& b) {
    return profile_detail::sameInode(a,b) && a.st_mode==b.st_mode && a.st_uid==b.st_uid
        && a.st_gid==b.st_gid && a.st_nlink==b.st_nlink && a.st_size==b.st_size;
}
bool safeFile(const QString& path, const QString& stage, struct stat* metadata = nullptr) {
    if (profile_detail::normalized(path)!=path || !path.startsWith(stage+'/')) return false;
    auto parent=profile_detail::openDirectory(QFileInfo(path).absolutePath(),nullptr,nullptr);
    struct stat directory{}, file{};
    if (!parent || ::fstat(parent.get(),&directory) || !profile_detail::safeNode(directory,true)
        || ::fstatat(parent.get(),QFile::encodeName(QFileInfo(path).fileName()).constData(),&file,AT_SYMLINK_NOFOLLOW)
        || !profile_detail::safeNode(file,false) || file.st_size<0 || file.st_size>8*1024*1024) return false;
    // These are ordinary public INI settings inside a private staging directory.
    // Preserve their original ordinary permissions; encrypted files have a separate policy.
    if (metadata) *metadata=file;
    return true;
}
// Locate a decoded top-level key without reserializing the JSON. All other bytes,
// including user text, nested reviewed packs and duplicate-sensitive fields, stay intact.
bool fieldSpan(const QByteArray& bytes, const QString& requested, qsizetype* begin, qsizetype* end) {
    int depth=0; bool found=false; QSet<QString> keys;
    const auto space=[&](qsizetype& i){while(i<bytes.size() && (bytes[i]==' '||bytes[i]=='\t'||bytes[i]=='\n'||bytes[i]=='\r'))++i;};
    const auto stringEnd=[&](qsizetype start){
        for(qsizetype i=start+1;i<bytes.size();++i){if(bytes[i]=='\\'){++i;continue;}if(bytes[i]=='"')return i+1;}return qsizetype(-1);
    };
    for(qsizetype i=0;i<bytes.size();++i){
        if(bytes[i]=='{'||bytes[i]=='['){++depth;continue;}
        if(bytes[i]=='}'||bytes[i]==']'){--depth;continue;}
        if(bytes[i]!='"')continue;
        const auto after=stringEnd(i); if(after<0)return false;
        qsizetype next=after;space(next);
        if(depth==1 && next<bytes.size() && bytes[next]==':'){
            const auto decoded=QJsonDocument::fromJson(QByteArray("[")+bytes.mid(i,after-i)+']').array();
            if(decoded.size()!=1||!decoded.first().isString())return false;
            const auto key=decoded.first().toString();if(keys.contains(key))return false;keys.insert(key);
            if(key==requested){++next;space(next);if(next>=bytes.size()||bytes[next]!='"')return false;
                const auto last=stringEnd(next);if(last<0||found)return false;*begin=next;*end=last;found=true;}
        }
        i=after-1;
    }
    return found && depth==0;
}
bool replaceFormat(const QByteArray& raw, const Rule& rule, QByteArray* result, bool* changed) {
    if(raw.size()>1024*1024)return false;
    QJsonParseError error;const auto document=QJsonDocument::fromJson(raw,&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())return false;
    const auto object=document.object();const auto value=object.value(QLatin1String(rule.field));
    if(!value.isString())return false;const auto current=canonicalPublicFormat(value.toString());
    if(!rule.formats.contains(current))return false;
    qsizetype begin=0,end=0;if(!fieldSpan(raw,QLatin1String(rule.field),&begin,&end))return false;
    *changed=current!=value.toString();*result=raw;
    if(*changed){const auto quoted=QJsonDocument(QJsonArray{current}).toJson(QJsonDocument::Compact);
        result->replace(begin,end-begin,quoted.mid(1,quoted.size()-2));}
    return true;
}
// QSettings writes these app-owned ASCII keys as [first-component] and backslash
// separated remaining components. Noncanonical or duplicate affected lines fail
// closed instead of rewriting unrelated comments, ordering, whitespace or values.
QMap<QString,QByteArray> iniValues(const QByteArray& bytes, const QSet<QString>& wanted, bool* ok) {
    QMap<QString,QByteArray> result;QString section;*ok=true;
    for(auto line:bytes.split('\n')){
        if(line.endsWith('\r'))line.chop(1);const auto trimmed=line.trimmed();
        if(trimmed.startsWith(';')||trimmed.startsWith('#')||trimmed.isEmpty())continue;
        if(trimmed.startsWith('[')&&trimmed.endsWith(']')){section=QString::fromUtf8(trimmed.mid(1,trimmed.size()-2));continue;}
        const auto equal=line.indexOf('=');if(equal<0)continue;
        auto key=QString::fromUtf8(line.left(equal).trimmed());key.replace('\\','/');
        if(!section.isEmpty()&&section!="General")key=section+'/'+key;
        if(!wanted.contains(key))continue;
        if(result.contains(key)){*ok=false;return {};}
        result.insert(key,line.mid(equal+1));
    }
    return result;
}
bool spliceValues(const QByteArray& before, const QMap<QString,QByteArray>& values, QByteArray* after) {
    QString section;QSet<QString> changed;QList<QByteArray> lines=before.split('\n');
    for(auto& raw:lines){const bool cr=raw.endsWith('\r');auto line=raw;if(cr)line.chop(1);const auto trimmed=line.trimmed();
        if(trimmed.startsWith(';')||trimmed.startsWith('#')||trimmed.isEmpty())continue;
        if(trimmed.startsWith('[')&&trimmed.endsWith(']')){section=QString::fromUtf8(trimmed.mid(1,trimmed.size()-2));continue;}
        const auto equal=line.indexOf('=');if(equal<0)continue;
        auto key=QString::fromUtf8(line.left(equal).trimmed());key.replace('\\','/');if(!section.isEmpty()&&section!="General")key=section+'/'+key;
        if(!values.contains(key))continue;if(changed.contains(key))return false;
        changed.insert(key);raw=line.left(equal+1)+values.value(key)+(cr?QByteArray("\r"):QByteArray{});
    }
    if(changed.size()!=values.size())return false;*after=lines.join('\n');return true;
}
struct Plan {QString path,relative;QByteArray before,after;int changedKeys=0;struct stat metadata{};};
}
#endif

QString canonicalPublicFormat(const QString& format) {
    for(const auto& pair:legacy::PublicFormats)if(format==QLatin1String(pair.first))return QLatin1String(pair.second);
    return format;
}

bool convertStagedPublicSettings(const QString& stageDirectory, QJsonArray* evidence, QString* error) {
    if (evidence) *evidence = {};
#ifndef Q_OS_UNIX
    Q_UNUSED(stageDirectory);
    return profile_detail::fail(error, QStringLiteral("이 시스템에서는 이전 프로필의 공개 설정 변환을 지원하지 않습니다."));
#else
    if(error)error->clear();const auto stage=profile_detail::normalized(stageDirectory);
    if(!profile_detail::safeDirectory(stage,false,true,nullptr,error))return fail(error);
    QTemporaryDir work(QDir(stage).filePath(".keefi-public-settings-XXXXXX"));if(!work.isValid())return fail(error);
    QList<Plan> plans;
    for(const auto& relative:{QString("KEEFI/KEEFI Dev.ini"),QString("KEEFI/KEEFI.ini"),QString("KEEFI.ini")}){
        const auto path=QDir(stage).filePath(relative);const QFileInfo info(path);
        bool parentExists=false;
        if(!profile_detail::safeDirectory(info.absolutePath(),true,false,&parentExists,error))return fail(error);
        if(!parentExists||(!info.exists()&&!info.isSymLink()))continue;
        struct stat original{};if(!safeFile(path,stage,&original))return fail(error);
        const auto before=read(path);QMap<QString,QVariant> originals;QMap<QString,QByteArray> replacements;
        {
            QSettings settings(path,QSettings::IniFormat);settings.setFallbacksEnabled(false);
            for(const auto& key:settings.allKeys())originals.insert(key,settings.value(key));
            if(settings.status()!=QSettings::NoError)return fail(error);
            for(const auto& rule:Rules){const auto key=QLatin1String(rule.key);if(!originals.contains(key))continue;
                const auto raw=originals.value(key).toByteArray();if(raw.isEmpty())continue;
                QByteArray converted;bool changed=false;if(!replaceFormat(raw,rule,&converted,&changed))return fail(error);
                if(changed)replacements.insert(key,converted);}
        }
        if(replacements.isEmpty())continue;
        const auto encodedPath=work.filePath(QString::number(plans.size())+"-encoded.ini");
        {QSettings encoded(encodedPath,QSettings::IniFormat);encoded.setFallbacksEnabled(false);
            for(auto it=replacements.cbegin();it!=replacements.cend();++it)encoded.setValue(it.key(),it.value());
            encoded.sync();if(encoded.status()!=QSettings::NoError)return fail(error);}
        QSet<QString> keys;for(auto it=replacements.cbegin();it!=replacements.cend();++it)keys.insert(it.key());
        bool parsed=false;const auto encoded=iniValues(read(encodedPath),keys,&parsed);QByteArray after;
        if(!parsed||encoded.size()!=replacements.size()||!spliceValues(before,encoded,&after))return fail(error);
        const auto checkPath=work.filePath(QString::number(plans.size())+"-verified.ini");
        {QFile check(checkPath);if(!check.open(QIODevice::WriteOnly|QIODevice::NewOnly)||!check.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||check.write(after)!=after.size()||!check.flush())return fail(error);}
        {QSettings check(checkPath,QSettings::IniFormat);check.setFallbacksEnabled(false);
            if(check.allKeys()!=originals.keys())return fail(error);
            for(auto it=originals.cbegin();it!=originals.cend();++it){const QVariant expected=replacements.contains(it.key())?QVariant(replacements.value(it.key())):it.value();
                if(check.value(it.key())!=expected)return fail(error);}
            if(check.status()!=QSettings::NoError)return fail(error);}
        plans.append({path,relative,before,after,int(replacements.size()),original});
    }
    QJsonArray result;
    for(const auto& plan:plans){
        struct stat before{},after{};
        if(!safeFile(plan.path,stage,&before)||!sameMetadata(before,plan.metadata)||read(plan.path)!=plan.before)return fail(error);
        QSaveFile file(plan.path);file.setDirectWriteFallback(false);
        if(!file.open(QIODevice::WriteOnly)||::fchown(file.handle(),uid_t(-1),plan.metadata.st_gid)
            ||::fchmod(file.handle(),plan.metadata.st_mode&0777)
            ||file.write(plan.after)!=plan.after.size()||!file.flush()||::fsync(file.handle())
            ||!file.commit()||!safeFile(plan.path,stage,&after)||read(plan.path)!=plan.after
            ||after.st_mode!=plan.metadata.st_mode||after.st_uid!=plan.metadata.st_uid
            ||after.st_gid!=plan.metadata.st_gid)return fail(error);
        result.append(QJsonObject{{"file",plan.relative},{"changedKeys",plan.changedKeys},
                                  {"sourceDigest",sha(plan.before)},{"currentDigest",sha(plan.after)}});
    }
    if(evidence)*evidence=result;return true;
#endif
}
} // namespace keefi
