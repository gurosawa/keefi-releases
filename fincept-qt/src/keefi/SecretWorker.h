#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QString>

class QProcess;
class QTimer;

namespace keefi {
class PlatformSecurity;

// One request per process. This G1 foundation has no financial adapter.
// Caller must separately hold an unlocked vault. No shell, arbitrary args, env
// injection, plaintext files, response echo, retry, or persistent worker queue.
class SecretWorker final : public QObject {
    Q_OBJECT
public:
    SecretWorker(PlatformSecurity* security, QString pythonExecutable,
                 QString scriptPath, QByteArray expectedScriptSha256,
                 QObject* parent = nullptr);
    ~SecretWorker() override;

    bool start(QByteArray payload, int timeoutMs = 5000);
    bool running() const { return process_ != nullptr; }
    qint64 processId() const;
    QString requestId() const { return request_id_; }
    static QProcessEnvironment isolatedEnvironment();

public slots:
    void stop();

signals:
    // code belongs to a fixed app-defined allowlist. Raw output never escapes.
    void finished(const QString& requestId, bool success, const QString& code);

private:
    void readOutput(quint64 generation);
    void consumeFrames(quint64 generation);
    void complete(bool success, const QString& code);
    static void wipe(QByteArray& bytes);

    QPointer<PlatformSecurity> security_;
    QString python_;
    QString script_;
    QByteArray script_hash_;
    QProcess* process_ = nullptr;
    QTimer* timer_;
    QByteArray pending_;
    QByteArray output_;
    qsizetype stderr_bytes_ = 0;
    bool ready_seen_ = false;
    bool response_seen_ = false;
    bool response_ok_ = false;
    quint64 generation_ = 0;
    QString request_id_;
};
} // namespace keefi
