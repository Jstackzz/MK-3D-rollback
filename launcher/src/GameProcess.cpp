#include "GameProcess.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>

#include <windows.h>

#include "AppPaths.h"
#include "Controls.h"
#include "WinUtil.h"

namespace {

// A package keeps clang and lld once, in toolchain\ beside the launcher. A game's first start compiles its code with
// the tools in its own compiler folder, so they are linked there before it starts (hard links: no extra space).
void linkToolchain(const QString& gameDir) {
    const QDir compiler(QDir(gameDir).filePath(QStringLiteral("compiler")));
    const QDir toolchain(paths::toolchainDir());
    if (!compiler.exists() || !toolchain.exists()) return;  // a source build, or the player deleted the compiler
    for (const QString& name : toolchain.entryList(QDir::Files)) {
        const QString to = compiler.filePath(name);
        if (QFileInfo::exists(to)) continue;
        const QString from = toolchain.filePath(name);
        const bool linked = CreateHardLinkW(QDir::toNativeSeparators(to).toStdWString().c_str(),
                                            QDir::toNativeSeparators(from).toStdWString().c_str(), nullptr);
        if (!linked && !QFile::copy(from, to))
            applog::write(QStringLiteral("toolchain: cannot put %1 into %2").arg(name, QDir::toNativeSeparators(compiler.path())));
    }
}

// the game's files where the game looks for them (runtime app/game_files.cpp): the first assets folder at or above
// the executable's, holding default.xbe or a disc image
bool hasGameFiles(const QString& exeDir) {
    QDir dir(exeDir);
    for (int i = 0; i < 6; ++i) {
        if (dir.exists(QStringLiteral("assets"))) {
            const QDir assets(dir.filePath(QStringLiteral("assets")));
            return assets.exists(QStringLiteral("default.xbe")) ||
                   !assets.entryList({QStringLiteral("*.iso")}, QDir::Files).isEmpty();
        }
        if (!dir.cdUp()) break;
    }
    return false;
}

}  // namespace

GameProcess::GameProcess(const QString& gameId, int instance, QObject* parent)
    : QObject(parent), gameId_(gameId), instance_(instance) {
    poll_.setInterval(100);
    connect(&poll_, &QTimer::timeout, this, &GameProcess::poll);
}

GameProcess::~GameProcess() {
    if (isRunning()) {
        proc_->kill();
        proc_->waitForFinished(2000);
    }
}

bool GameProcess::isRunning() const { return proc_ && proc_->state() != QProcess::NotRunning; }

qint64 GameProcess::pid() const { return isRunning() ? proc_->processId() : 0; }

QString GameProcess::state() const { return status_.section(QLatin1Char(' '), 0, 0); }

QHash<QString, QString> GameProcess::parseKeys(const QString& line) {
    QHash<QString, QString> out;
    const QStringList tokens = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 1; i < tokens.size(); ++i) {
        const int eq = tokens[i].indexOf(QLatin1Char('='));
        if (eq > 0) out.insert(tokens[i].left(eq), tokens[i].mid(eq + 1));
    }
    return out;
}

QString GameProcess::smartAppControlHelp(bool blocked) {
    return (blocked ? tr("Windows' Smart App Control blocked the game.")
                    : tr("Windows' Smart App Control is on, and it will block the games.")) +
           QLatin1Char(' ') +
           tr("It only runs programs signed by a publisher it knows, and these games can never be: every PC builds "
              "its own copy of the game's code from its own disc. To play, switch it off in Windows Security > App & "
              "browser control > Smart App Control settings. (On some versions of Windows 11 it can only be "
              "switched back on by resetting Windows.)");
}

bool GameProcess::start(const QString& exe, QString* error, bool netMode) {
    if (isRunning()) return true;
    const QFileInfo fi(exe);
    if (exe.isEmpty() || !fi.exists()) {
        if (error) *error = tr("Game executable not found: %1. Build the game first or set its path in Settings.")
                                .arg(QDir::toNativeSeparators(exe));
        return false;
    }
    if (fi.suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) == 0) {
        if (!hasGameFiles(fi.absolutePath())) {
            if (error) *error = tr("The game's files are not there yet: open Build game and extract your disc first.");
            return false;
        }
        linkToolchain(fi.absolutePath());
    }
    // the newest sessions are kept (an online match's netplay log is about a megabyte)
    constexpr int kKeepSessions = 30;
    const QFileInfoList old = QDir(paths::sessionsDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (int i = kKeepSessions; i < old.size(); i++) QDir(old[i].absoluteFilePath()).removeRecursively();
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
    sessionDir_ = QDir(paths::sessionsDir()).filePath(QStringLiteral("%1-%2-%3").arg(gameId_, stamp).arg(instance_));
    QDir().mkpath(sessionDir_);
    cmdPath_ = QDir(sessionDir_).filePath(QStringLiteral("cmd.txt"));
    statusPath_ = QDir(sessionDir_).filePath(QStringLiteral("status.txt"));
    QFile::remove(cmdPath_);
    QFile::remove(statusPath_);

    netMode_ = netMode;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (netMode) {
        env.insert(QStringLiteral("MK3D_NET"), QStringLiteral("1"));
        env.insert(QStringLiteral("MK3D_CMD"), QDir::toNativeSeparators(cmdPath_));
        env.insert(QStringLiteral("MK3D_STATUS"), QDir::toNativeSeparators(statusPath_));
        // every online match's frames (both players' inputs, the state checks) beside the session's other files: what
        // to look at when a match went wrong; a test that names its own log keeps it
        const QString netLog = gameId_.toUpper() + QStringLiteral("_NET_LOG");
        if (!env.contains(netLog))
            env.insert(netLog, QDir::toNativeSeparators(QDir(sessionDir_).filePath(QStringLiteral("net-{match}.log"))));
    }
    env.insert(QStringLiteral("MK3D_GAME"), gameId_);
    env.insert(QStringLiteral("MK3D_SESSION_DIR"), QDir::toNativeSeparators(sessionDir_));
    env.insert(QStringLiteral("MK3D_INSTANCE"), QString::number(instance_));
    env.insert(QStringLiteral("MK3D_LOG"), QDir::toNativeSeparators(QDir(sessionDir_).filePath(QStringLiteral("game.log"))));
    env.insert(QStringLiteral("MK3D_DISPLAY"), cfg::displayMode());
    env.insert(QStringLiteral("MK3D_RESOLUTION"), cfg::resolution());
    env.insert(QStringLiteral("MK3D_VSYNC"), cfg::vsync() ? QStringLiteral("1") : QStringLiteral("0"));
    env.insert(QStringLiteral("XBE_CONTROLS"), QDir::toNativeSeparators(controls::file()));  // read again when it changes
    for (auto it = extraEnv_.cbegin(); it != extraEnv_.cend(); ++it) env.insert(it.key(), it.value());

    proc_ = new QProcess(this);
    proc_->setProcessEnvironment(env);
    proc_->setWorkingDirectory(fi.absolutePath());
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    QString program = fi.absoluteFilePath();
    QStringList args;
    if (fi.suffix().compare(QLatin1String("py"), Qt::CaseInsensitive) == 0) {
        program = cfg::python();
        args = {QStringLiteral("-u"), fi.absoluteFilePath()};
        proc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;  // the development stand-in has no window of its own
        });
    }
    connect(proc_, &QProcess::readyRead, this, [this] {
        outBuf_.append(proc_->readAll());
        qsizetype nl;
        while ((nl = outBuf_.indexOf('\n')) >= 0) {
            const QString line = QString::fromLocal8Bit(outBuf_.left(nl)).trimmed();
            outBuf_.remove(0, nl + 1);
            if (!line.isEmpty()) emit outputLine(line);
        }
    });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        poll();
        poll_.stop();
        applog::write(QStringLiteral("game %1/%2: exited with code %3%4")
                          .arg(gameId_).arg(instance_).arg(code).arg(st == QProcess::CrashExit ? " (crash)" : ""));
        const QString last = quitting_ ? QStringLiteral("exited") : QStringLiteral("error exited");
        status_ = last;
        emit statusChanged(last);
        emit exited(code, st == QProcess::CrashExit);
    });
    proc_->start(program, args);
    if (!proc_->waitForStarted(10000)) {
        if (error) {
            *error = tr("Could not start %1: %2").arg(QDir::toNativeSeparators(program), proc_->errorString());
            // "An Application Control policy has blocked this file" (English Windows) or the policy reported on
            if (winutil::smartAppControl() == 1 || proc_->errorString().contains(QLatin1String("Application Control")))
                *error += QLatin1Char('\n') + smartAppControlHelp();
        }
        // one arg() call: Windows' messages can hold a %1 of their own, which a second call would fill
        applog::write(QStringLiteral("game %1/%2: could not start %3: %4 (Smart App Control %5)")
                          .arg(gameId_, QString::number(instance_), QDir::toNativeSeparators(program),
                               proc_->errorString(), QString::number(winutil::smartAppControl())));
        delete proc_;
        proc_ = nullptr;
        return false;
    }
    winutil::gameJob().assign(proc_->processId());
    applog::write(QStringLiteral("game %1/%2: started %3 (pid %4), session %5")
                      .arg(gameId_).arg(instance_).arg(QDir::toNativeSeparators(fi.absoluteFilePath()))
                      .arg(proc_->processId()).arg(QDir::toNativeSeparators(sessionDir_)));
    status_ = QStringLiteral("starting");
    emit statusChanged(status_);
    if (netMode) poll_.start();
    return true;
}

void GameProcess::poll() {
    bool exists = false;
    const QByteArray raw = winutil::readShared(statusPath_, &exists);
    if (exists && !raw.isEmpty()) {
        QString first = QString::fromLatin1(raw).section(QLatin1Char('\n'), 0, 0).trimmed();
        if (!first.isEmpty() && first != status_) {
            status_ = first;
            applog::write(QStringLiteral("game %1/%2: status %3").arg(gameId_).arg(instance_).arg(first));
            emit statusChanged(first);
        }
    }
    if (!pendingCmd_.isEmpty() && (!QFileInfo::exists(cmdPath_) || lastWrite_.elapsed() > 2000)) {
        const QString line = pendingCmd_;
        pendingCmd_.clear();
        writeCommand(line);
    }
}

void GameProcess::command(const QString& line) {
    if (cmdPath_.isEmpty()) return;
    if (QFileInfo::exists(cmdPath_) && lastWrite_.isValid() && lastWrite_.elapsed() < 2000) {
        pendingCmd_ = line;  // previous command not taken yet
        return;
    }
    writeCommand(line);
}

void GameProcess::writeCommand(const QString& line) {
    QString err;
    if (!winutil::writeFileAtomic(cmdPath_, line.toLatin1() + '\n', &err)) {
        applog::write(QStringLiteral("game %1/%2: %3").arg(gameId_).arg(instance_).arg(err));
        return;
    }
    lastWrite_.start();
    applog::write(QStringLiteral("game %1/%2: cmd %3").arg(gameId_).arg(instance_).arg(line));
}

void GameProcess::quit() {
    if (!isRunning() || quitting_) return;
    quitting_ = true;
    pendingCmd_.clear();
    if (netMode_) writeCommand(QStringLiteral("quit"));
    else proc_->terminate();  // WM_CLOSE to its windows: the game closes as when the player closes it
    QTimer::singleShot(5000, this, [this] {
        if (isRunning()) kill();
    });
}

void GameProcess::kill() {
    if (isRunning()) {
        quitting_ = true;
        proc_->kill();
    }
}
