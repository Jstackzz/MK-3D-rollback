#pragma once
// Small Windows helpers: atomic file replace, share-everything reads, kill-on-close job object,
// dark title bar.

#include <QByteArray>
#include <QString>

class QWidget;

namespace winutil {

// Write `data` to `path` atomically: <path>.tmp, then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH),
// retrying for up to `retryMs` while a reader holds the target open.
bool writeFileAtomic(const QString& path, const QByteArray& data, QString* error = nullptr, int retryMs = 1000);

// Read a small file opened with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, so the
// writer's atomic replace never fails because of us. Returns an empty array when missing.
QByteArray readShared(const QString& path, bool* exists = nullptr);

// Processes assigned to the job die when the launcher exits (handle closed by the OS).
class KillOnCloseJob {
public:
    KillOnCloseJob();
    ~KillOnCloseJob();
    KillOnCloseJob(const KillOnCloseJob&) = delete;
    KillOnCloseJob& operator=(const KillOnCloseJob&) = delete;
    bool assign(qint64 pid);

private:
    void* job_ = nullptr;
};

KillOnCloseJob& gameJob();

void useDarkTitleBar(QWidget* window);

// Smart App Control (Windows 11): 1 on, blocking programs without a known publisher's signature (the games, whose
// code each PC builds for itself, never have one), 2 evaluating (it may switch itself on), 0 off, -1 not on this
// Windows. MK3D_SMART_APP_CONTROL=<state> pretends a state, to see what the launcher says.
int smartAppControl();

}  // namespace winutil
