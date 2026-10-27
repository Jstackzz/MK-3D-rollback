#pragma once
// Small Windows helpers: atomic file replace, share-everything reads, kill-on-close job object,
// dark title bar.

#include <QByteArray>
#include <QList>
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

// A window of the launcher in front of whatever has the foreground, the game in its borderless window above all:
// Windows lets only the foreground process hand the foreground over and otherwise just flashes the taskbar, so the
// foreground window's thread input is attached to ours for the switch. True: it has the foreground now.
bool bringToFront(QWidget* window);
// The game's window in front again (the largest visible window of that process or of one it started), so the player
// is back in it without switching to it; false when it has none or did not get the foreground.
bool bringProcessToFront(qint64 pid);

// A game set aside for another one: its windows hidden (kept in `hidden` for later), then the process and any it
// started frozen, every thread stopped where it is; thawing runs them on and shows those windows again. A frozen game
// takes no CPU and plays no sound, and comes back exactly where it was.
bool freezeProcess(qint64 pid, QList<quintptr>* hidden);
bool thawProcess(qint64 pid, const QList<quintptr>& hidden);

// Smart App Control (Windows 11): 1 on, blocking programs without a known publisher's signature (the games, whose
// code each PC builds for itself, never have one), 2 evaluating (it may switch itself on), 0 off, -1 not on this
// Windows. MK3D_SMART_APP_CONTROL=<state> pretends a state, to see what the launcher says.
int smartAppControl();

}  // namespace winutil
