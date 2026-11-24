#pragma once

#include <QTemporaryFile>
#include <optional>
#ifdef Q_OS_WIN
#include <QDir>
#include <qt_windows.h>
#endif

namespace keefi {
class StagedStorageFile {
public:
    explicit StagedStorageFile(const QString& pathTemplate)
        : file_(std::in_place, pathTemplate) {}
    QTemporaryFile& file() { return *file_; }

#ifdef Q_OS_WIN
    bool publish(const QString& destination, bool replaceExisting) {
        const auto staged = file_->fileName();
        const auto source = QDir::toNativeSeparators(staged).toStdWString();
        const auto target = QDir::toNativeSeparators(destination).toStdWString();
        // Qt 6.8 keeps QTemporaryFile's native handle after close(), without
        // FILE_SHARE_DELETE. Destroy it before asking Windows to move the file.
        file_->setAutoRemove(false);
        file_.reset();
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
        if (MoveFileExW(source.c_str(), target.c_str(), flags)) return true;
        QFile::remove(staged);
        return false;
    }
#endif

private:
    std::optional<QTemporaryFile> file_;
};
} // namespace keefi
