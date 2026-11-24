#include "SecretWorker.h"
#include "PlatformSecurity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <cerrno>
#include <memory>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace keefi {
namespace {
constexpr qsizetype maxPayload = 64 * 1024;
constexpr qsizetype maxOutput = 16 * 1024;
constexpr qsizetype maxFrame = 4096;
#ifdef Q_OS_WIN
struct WindowsHandleList {
    STARTUPINFOEXW startup{};
    HANDLE handles[3]{};
    std::vector<unsigned char> storage;
    bool initialized = false;
    bool prepare() {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) return false;
        storage.resize(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes);
        return initialized;
    }
    ~WindowsHandleList() {
        if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
};
#endif
}

SecretWorker::SecretWorker(PlatformSecurity* security, QString pythonExecutable,
                           QString scriptPath, QByteArray expectedScriptSha256, QObject* parent)
    : QObject(parent), security_(security), python_(std::move(pythonExecutable)),
      script_(std::move(scriptPath)), script_hash_(std::move(expectedScriptSha256)), timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { complete(false, QStringLiteral("timeout")); });
    if (security_) {
        connect(security_, &PlatformSecurity::lockRequested, this, &SecretWorker::stop);
        connect(security_, &QObject::destroyed, this, &SecretWorker::stop);
    }
}

SecretWorker::~SecretWorker() {
    // No destructor completion callbacks into UI that may already be gone.
    disconnect(this, nullptr, nullptr, nullptr);
    stop();
}

void SecretWorker::wipe(QByteArray& bytes) {
    if (!bytes.isEmpty()) {
        volatile char* data = bytes.data();
        for (qsizetype i = 0; i < bytes.size(); ++i) data[i] = 0;
    }
    bytes.clear();
}

QProcessEnvironment SecretWorker::isolatedEnvironment() {
    QProcessEnvironment environment;
    // Fixed runtime values rather than the parent's potentially secret values.
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
#ifdef Q_OS_WIN
    // Qt otherwise copies parent PATH and SystemRoot even for an empty env.
    // Derive both from the OS, never from arbitrary parent environment values.
    wchar_t systemRoot[32768]{};
    const UINT size = GetWindowsDirectoryW(systemRoot, 32768);
    const QString root = size > 0 && size < 32768 ? QString::fromWCharArray(systemRoot, int(size)) : QString();
    environment.insert(QStringLiteral("SystemRoot"), root);
    environment.insert(QStringLiteral("PATH"), root.isEmpty() ? QString() : root + QStringLiteral("\\System32"));
#endif
    return environment;
}

qint64 SecretWorker::processId() const { return process_ ? process_->processId() : 0; }

bool SecretWorker::start(QByteArray payload, int timeoutMs) {
    if (process_ || !security_ || !security_->ready() || payload.isEmpty() ||
        payload.size() > maxPayload || timeoutMs < 100 || timeoutMs > 30000) {
        wipe(payload);
        return false;
    }
    QString protectionError;
    if (!PlatformSecurity::hardenCurrentProcess(&protectionError)) { wipe(payload); return false; }
    const QFileInfo executable(python_), script(script_);
    if (!executable.isAbsolute() || !script.isAbsolute() || !executable.isFile() ||
        !executable.isExecutable() || !script.isFile() || script_hash_.size() != 32) {
        wipe(payload);
        return false;
    }
    QFile source(script.canonicalFilePath());
    if (!source.open(QIODevice::ReadOnly) || source.size() > 1024 * 1024 ||
        QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256) != script_hash_) {
        wipe(payload);
        return false;
    }
#ifdef Q_OS_WIN
    // Qt 6.8.3 otherwise uses CreateProcess(inheritHandles=true) without a
    // HANDLE_LIST. Limit inheritance to this worker's three pipe handles.
    auto inheritedHandles = std::make_shared<WindowsHandleList>();
    if (!inheritedHandles->prepare()) { wipe(payload); return false; }
#endif
    // Paths and a pinned hash are a deployment integrity check, not a defense
    // against another process able to rewrite this executable at exec time.
    pending_ = std::move(payload);
    request_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ready_seen_ = response_seen_ = response_ok_ = false;
    stderr_bytes_ = 0;
    wipe(output_);
    const quint64 current = ++generation_;
    auto* process = new QProcess(this);
    process_ = process;
    process->setProgram(executable.canonicalFilePath());
    process->setArguments({QStringLiteral("-I"), QStringLiteral("-B"), script.canonicalFilePath()});
    process->setWorkingDirectory(script.absolutePath());
    process->setProcessEnvironment(isolatedEnvironment());
    process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_UNIX
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
    process->setChildProcessModifier([process] {
        const rlimit disabled{0, 0};
        if (setrlimit(RLIMIT_CORE, &disabled) != 0)
            process->failChildProcessModifier("core limit", errno);
    });
#elif defined(Q_OS_WIN)
    process->setCreateProcessArgumentsModifier([inheritedHandles](QProcess::CreateProcessArguments* arguments) {
        inheritedHandles->startup.StartupInfo = *arguments->startupInfo;
        inheritedHandles->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        inheritedHandles->handles[0] = arguments->startupInfo->hStdInput;
        inheritedHandles->handles[1] = arguments->startupInfo->hStdOutput;
        inheritedHandles->handles[2] = arguments->startupInfo->hStdError;
        if (!UpdateProcThreadAttribute(inheritedHandles->startup.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles->handles,
                                       sizeof(inheritedHandles->handles), nullptr, nullptr)) {
            // An empty explicit executable name makes CreateProcess fail;
            // never fall back to unrestricted handle inheritance.
            arguments->applicationName = L"";
            arguments->inheritHandles = FALSE;
            return;
        }
        arguments->flags |= EXTENDED_STARTUPINFO_PRESENT;
        arguments->startupInfo = &inheritedHandles->startup.StartupInfo;
        arguments->inheritHandles = TRUE;
    });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, [this, current] { readOutput(current); });
    connect(process, &QProcess::readyReadStandardError, this, [this, current] {
        if (current != generation_ || !process_) return;
        process_->setReadChannel(QProcess::StandardError);
        const qint64 available = process_->bytesAvailable();
        QByteArray discarded = process_->read(maxOutput + 1);
        stderr_bytes_ += discarded.size();
        wipe(discarded);
        if (available > maxOutput || stderr_bytes_ > maxOutput) complete(false, QStringLiteral("output_limit"));
    });
    connect(process, &QProcess::errorOccurred, this, [this, current](QProcess::ProcessError error) {
        if (current != generation_ || !process_) return;
        if (error == QProcess::FailedToStart) complete(false, QStringLiteral("start_failed"));
        else if (error == QProcess::Crashed) complete(false, QStringLiteral("worker_crashed"));
        else if (error == QProcess::WriteError || error == QProcess::ReadError)
            complete(false, QStringLiteral("pipe_error"));
    });
    connect(process, &QProcess::finished, this, [this, current](int code, QProcess::ExitStatus status) {
        if (current != generation_ || !process_) return;
        readOutput(current);
        if (current != generation_ || !process_) return;
        if (status != QProcess::NormalExit || code != 0) complete(false, QStringLiteral("worker_failed"));
        else if (!response_seen_ || !output_.isEmpty()) complete(false, QStringLiteral("protocol_error"));
        else complete(response_ok_, response_ok_ ? QStringLiteral("ok") : QStringLiteral("worker_rejected"));
    });
    timer_->start(timeoutMs);
    process->start(QIODevice::ReadWrite);
    return true;
}

void SecretWorker::readOutput(quint64 generation) {
    if (generation != generation_ || !process_) return;
    process_->setReadChannel(QProcess::StandardOutput);
    if (process_->bytesAvailable() > maxOutput) { complete(false, QStringLiteral("output_limit")); return; }
    QByteArray incoming = process_->read(maxOutput + 1);
    if (output_.size() + incoming.size() > maxOutput) {
        wipe(incoming);
        complete(false, QStringLiteral("output_limit"));
        return;
    }
    output_.append(incoming);
    wipe(incoming);
    consumeFrames(generation);
}

void SecretWorker::consumeFrames(quint64 generation) {
    while (generation == generation_ && process_ && output_.size() >= 4) {
        const quint32 size = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(output_.constData()));
        if (size == 0 || size > maxFrame) { complete(false, QStringLiteral("protocol_error")); return; }
        if (output_.size() < 4 + size) return;
        QByteArray frame = output_.mid(4, size);
        output_.remove(0, 4 + size);
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(frame, &error);
        wipe(frame);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            complete(false, QStringLiteral("protocol_error")); return;
        }
        const QJsonObject object = document.object();
        if (!ready_seen_) {
            if (object.keys() != QStringList{QStringLiteral("protected"), QStringLiteral("ready"), QStringLiteral("version")} ||
                object.value(QStringLiteral("ready")) != QJsonValue(true) ||
                object.value(QStringLiteral("protected")) != QJsonValue(true) ||
                object.value(QStringLiteral("version")) != QJsonValue(1)) {
                complete(false, QStringLiteral("protection_handshake_failed")); return;
            }
            ready_seen_ = true;
            QByteArray body = request_id_.toLatin1() + '\n' + pending_;
            QByteArray envelope(4, '\0');
            qToBigEndian<quint32>(static_cast<quint32>(body.size()), reinterpret_cast<uchar*>(envelope.data()));
            envelope.append(body);
            const qint64 sent = process_->write(envelope);
            const qsizetype expected = envelope.size();
            wipe(body); wipe(envelope); wipe(pending_);
            if (sent != expected) { complete(false, QStringLiteral("pipe_error")); return; }
            process_->closeWriteChannel();
        } else {
            if (response_seen_ || object.keys() != QStringList{QStringLiteral("requestId"), QStringLiteral("status")} ||
                object.value(QStringLiteral("requestId")).toString() != request_id_ ||
                (object.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("ok")) &&
                 object.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("rejected")))) {
                complete(false, QStringLiteral("protocol_error")); return;
            }
            response_seen_ = true;
            response_ok_ = object.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("ok"));
        }
    }
}

void SecretWorker::complete(bool success, const QString& code) {
    if (!process_) return;
    ++generation_; // Queued signals from this process can never affect a new run.
    timer_->stop();
    auto* process = process_;
    process_ = nullptr;
    disconnect(process, nullptr, this, nullptr);
    process->closeWriteChannel();
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(1000);
    }
    for (const auto channel : {QProcess::StandardOutput, QProcess::StandardError}) {
        process->setReadChannel(channel);
        while (process->bytesAvailable() > 0) {
            QByteArray discarded = process->read(maxOutput);
            if (discarded.isEmpty()) break;
            wipe(discarded);
        }
    }
    process->deleteLater();
    wipe(pending_); wipe(output_);
    emit finished(request_id_, success, code);
}

void SecretWorker::stop() { complete(false, QStringLiteral("locked_or_cancelled")); }
} // namespace keefi
