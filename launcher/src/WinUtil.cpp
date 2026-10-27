#include "WinUtil.h"

#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <vector>

#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>

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

static bool forceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    const DWORD me = GetCurrentThreadId();
    // never a hidden one's thread: a game set aside (freezeProcess) hides its window first and answers nothing after
    const DWORD fgThread = fg && IsWindowVisible(fg) ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    // the top of the z-order even if the switch is refused: above the game's window, without staying on top
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
    return GetForegroundWindow() == hwnd;
}

bool bringToFront(QWidget* window) {
    if (!window) return false;
    window->show();
    window->raise();
    const bool front = forceForeground(reinterpret_cast<HWND>(window->winId()));
    window->activateWindow();
    return front;
}

// the process and any it started (a game that relaunched itself into another image runs on in a child)
static std::vector<DWORD> processTree(qint64 pid) {
    std::vector<DWORD> pids{static_cast<DWORD>(pid)};
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W e{};
        e.dwSize = sizeof e;
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
            if (e.th32ParentProcessID == static_cast<DWORD>(pid)) pids.push_back(e.th32ProcessID);
        CloseHandle(snap);
    }
    return pids;
}

// their visible top-level windows, largest first
static std::vector<HWND> topWindows(const std::vector<DWORD>& pids) {
    struct Search {
        const std::vector<DWORD>* pids;
        std::vector<std::pair<long long, HWND>> found;
    } search{&pids, {}};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) ||
                std::find(s->pids->begin(), s->pids->end(), owner) == s->pids->end())
                return TRUE;
            RECT r{};
            GetWindowRect(hwnd, &r);
            s->found.emplace_back(static_cast<long long>(r.right - r.left) * (r.bottom - r.top), hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    std::sort(search.found.begin(), search.found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<HWND> out;
    for (const auto& f : search.found) out.push_back(f.second);
    return out;
}

bool bringProcessToFront(qint64 pid) {
    if (pid <= 0) return false;
    const std::vector<HWND> windows = topWindows(processTree(pid));
    return !windows.empty() && forceForeground(windows.front());
}

// ntdll's whole-process suspend and resume (every thread's suspend count up or down by one)
static bool suspendResume(DWORD pid, bool suspend) {
    using Fn = LONG(NTAPI*)(HANDLE);
    static const auto ntSuspend =
        reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess"));
    static const auto ntResume = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess"));
    const Fn fn = suspend ? ntSuspend : ntResume;
    HANDLE proc = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!proc || !fn) {
        if (proc) CloseHandle(proc);
        return false;
    }
    const bool ok = fn(proc) >= 0;
    CloseHandle(proc);
    return ok;
}

bool freezeProcess(qint64 pid, QList<quintptr>* hidden) {
    if (pid <= 0) return false;
    const std::vector<DWORD> pids = processTree(pid);
    // hidden while it still runs: a frozen window answers nothing, and a visible one would turn "not responding"
    for (HWND hwnd : topWindows(pids)) {
        ShowWindow(hwnd, SW_HIDE);
        if (hidden) hidden->append(reinterpret_cast<quintptr>(hwnd));
    }
    bool ok = true;
    for (DWORD p : pids) ok = suspendResume(p, true) && ok;
    return ok;
}

bool thawProcess(qint64 pid, const QList<quintptr>& hidden) {
    if (pid <= 0) return false;
    bool ok = true;
    for (DWORD p : processTree(pid)) ok = suspendResume(p, false) && ok;
    for (quintptr w : hidden) {
        HWND hwnd = reinterpret_cast<HWND>(w);
        if (IsWindow(hwnd)) ShowWindow(hwnd, SW_SHOW);
    }
    return ok;
}

int smartAppControl() {
    if (qEnvironmentVariableIsSet("MK3D_SMART_APP_CONTROL")) return qEnvironmentVariableIntValue("MK3D_SMART_APP_CONTROL");
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy",
                     L"VerifiedAndReputablePolicyState", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return -1;
    return static_cast<int>(value);
}

}  // namespace winutil
