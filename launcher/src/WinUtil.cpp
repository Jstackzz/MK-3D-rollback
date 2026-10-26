#include "WinUtil.h"

#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QWidget>

#include <windows.h>
#include <dwmapi.h>

namespace winutil {

static std::wstring wide(const QString& s) {
    return QDir::toNativeSeparators(s).toStdWString();
}

bool writeFileAtomic(const QString& path, const QByteArray& data, QString* error, int retryMs) {
    const QString tmp = path + QStringLiteral(".tmp");
    HANDLE h = CreateFileW(wide(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = QStringLiteral("cannot create %1 (error %2)").arg(tmp).arg(GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data.constData(), static_cast<DWORD>(data.size()), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != static_cast<DWORD>(data.size())) {
        if (error) *error = QStringLiteral("cannot write %1").arg(tmp);
        DeleteFileW(wide(tmp).c_str());
        return false;
    }
    QElapsedTimer t;
    t.start();
    for (;;) {
        if (MoveFileExW(wide(tmp).c_str(), wide(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD err = GetLastError();
        if ((err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) || t.elapsed() > retryMs) {
            if (error) *error = QStringLiteral("cannot replace %1 (error %2)").arg(path).arg(err);
            DeleteFileW(wide(tmp).c_str());
            return false;
        }
        QThread::msleep(5);
    }
}

QByteArray readShared(const QString& path, bool* exists) {
    if (exists) *exists = false;
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    if (exists) *exists = true;
    QByteArray out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0 && out.size() < (1 << 20))
        out.append(buf, static_cast<qsizetype>(got));
    CloseHandle(h);
    return out;
}

KillOnCloseJob::KillOnCloseJob() {
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof info);
    }
}

KillOnCloseJob::~KillOnCloseJob() {
    if (job_) CloseHandle(job_);
}

bool KillOnCloseJob::assign(qint64 pid) {
    if (!job_ || pid <= 0) return false;
    HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!proc) return false;
    const BOOL ok = AssignProcessToJobObject(job_, proc);
    CloseHandle(proc);
    return ok != FALSE;
}

KillOnCloseJob& gameJob() {
    static KillOnCloseJob job;
    return job;
}

void useDarkTitleBar(QWidget* window) {
    if (!window) return;
    const BOOL dark = TRUE;
    constexpr DWORD kUseImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(reinterpret_cast<HWND>(window->winId()), kUseImmersiveDarkMode, &dark, sizeof dark);
}

}  // namespace winutil
