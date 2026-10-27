#include "Updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <windows.h>

#include <algorithm>

#include "AppPaths.h"

namespace {

QString installDir() { return QCoreApplication::applicationDirPath(); }
QString workDir() { return QDir(installDir()).filePath(QStringLiteral("update")); }
QString stageDir() { return QDir(workDir()).filePath(QStringLiteral("stage")); }
QString oldList() { return QDir(workDir()).filePath(QStringLiteral("old-files.txt")); }
const QString kOldSuffix = QStringLiteral(".mk3d-old");

// a newer than b, part by part (0.1.10 after 0.1.9)
bool newer(const QString& a, const QString& b) {
    const QStringList x = a.split(QLatin1Char('.')), y = b.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < std::max(x.size(), y.size()); i++) {
        const int p = i < x.size() ? x[i].toInt() : 0, q = i < y.size() ? y[i].toInt() : 0;
        if (p != q) return p > q;
    }
    return false;
}

// what the server may link to: https, or plain http to this PC (development)
bool downloadLink(const QString& url) {
    static const QRegularExpression local(QStringLiteral("^http://(127\\.0\\.0\\.1|localhost)(:\\d+)?/"));
    return url.size() <= 500 && (url.startsWith(QLatin1String("https://")) || local.match(url).hasMatch());
}

std::wstring native(const QString& path) { return QDir::toNativeSeparators(path).toStdWString(); }

bool move(const QString& from, const QString& to, DWORD flags) {
    return MoveFileExW(native(from).c_str(), native(to).c_str(), flags) != 0;
}

QString winError(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    const QString s = text ? QString::fromWCharArray(text).trimmed() : QStringLiteral("error %1").arg(code);
    if (text) LocalFree(text);
    return s;
}

// every file under `from` into its place under `to`, the installed one (if any) renamed to <name>.mk3d-old first (a
// running program's file can be renamed, not replaced); the old names go into `olds`. A failure puts back every file
// moved so far and says which file it was.
bool swapIn(const QString& from, const QString& to, QStringList* olds, QString* error) {
    QStringList files;
    QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) files << it.next();
    files.sort();
    struct Moved {
        QString target, staged, old;
    };
    QList<Moved> done;
    auto undo = [&done] {
        for (auto m = done.crbegin(); m != done.crend(); ++m) {
            move(m->target, m->staged, MOVEFILE_REPLACE_EXISTING);
            if (!m->old.isEmpty()) move(m->old, m->target, MOVEFILE_REPLACE_EXISTING);
        }
    };
    const QDir src(from), dst(to);
    for (const QString& f : files) {
        const QString rel = src.relativeFilePath(f);
        const QString target = dst.filePath(rel);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QString old;
        if (QFileInfo::exists(target)) {
            old = target + kOldSuffix;
            // one left from an update before whose program still runs keeps its name
            for (int n = 2; QFileInfo::exists(old) && !QFile::remove(old); n++) old = target + kOldSuffix + QString::number(n);
            if (!move(target, old, 0)) {
                *error = QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(rel), winError(GetLastError()));
                undo();
                return false;
            }
        }
        if (!move(f, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            *error = QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(rel), winError(GetLastError()));
            if (!old.isEmpty()) move(old, target, MOVEFILE_REPLACE_EXISTING);
            undo();
            return false;
        }
        done.append({target, f, old});
        if (!old.isEmpty()) olds->append(old);
    }
    return true;
}

}  // namespace

Updater::Updater(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {}

QJsonObject Updater::usable(const QJsonValue& value) {
    const QJsonObject r = value.toObject();
    static const QRegularExpression version(QStringLiteral("^\\d+(\\.\\d+)*$")), sha(QStringLiteral("^[0-9a-f]{64}$"));
    const QString v = r.value(QStringLiteral("version")).toString();
    const double size = r.value(QStringLiteral("size")).toDouble();
    if (!version.match(v).hasMatch() || !newer(v, QStringLiteral(MK3D_LAUNCHER_VERSION))) return {};
    if (!downloadLink(r.value(QStringLiteral("url")).toString())) return {};
    if (!sha.match(r.value(QStringLiteral("sha256")).toString()).hasMatch() || size <= 0 || size > 1 << 30) return {};
    return r;
}

QString Updater::blocker() {
    if (!paths::isPackage()) return tr("A source build updates with git, not here.");
    QFile probe(QDir(installDir()).filePath(QStringLiteral("update-write-test.tmp")));
    if (!probe.open(QIODevice::WriteOnly)) {
        return tr("The launcher cannot write to its folder (%1). Move the MK3D-Online folder somewhere you can write "
                  "to, such as Documents, or update by hand.")
            .arg(QDir::toNativeSeparators(installDir()));
    }
    probe.close();
    probe.remove();
    return {};
}

void Updater::cleanUp() {
    QFile list(oldList());
    if (!list.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // no update went in: what one closed halfway left (a download, an unpacked release) goes
        if (QFileInfo::exists(workDir()) && QDir(workDir()).removeRecursively())
            applog::write(QStringLiteral("update: an unfinished update's files deleted"));
        return;
    }
    QStringList left;
    const QString root = QDir(installDir()).absolutePath() + QLatin1Char('/');
    int removed = 0;
    while (!list.atEnd()) {
        const QString path = QString::fromUtf8(list.readLine()).trimmed();
        // only what an update set aside in this install
        if (path.isEmpty() || !path.startsWith(root, Qt::CaseInsensitive) || !path.contains(kOldSuffix)) continue;
        if (!QFileInfo::exists(path) || QFile::remove(path)) removed++;
        else left << path;
    }
    list.close();
    if (left.isEmpty()) {
        QDir(workDir()).removeRecursively();
        applog::write(QStringLiteral("update: %1 old file(s) deleted").arg(removed));
        return;
    }
    if (list.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) list.write(left.join(QLatin1Char('\n')).toUtf8());
    applog::write(QStringLiteral("update: %1 old file(s) deleted, %2 still in use").arg(removed).arg(left.size()));
}

void Updater::start(const QJsonObject& release) {
    if (busy_) return;
    release_ = release;
    busy_ = true;
    ready_ = false;
    written_ = 0;
    hash_.reset();
    QDir(stageDir()).removeRecursively();
    QDir().mkpath(workDir());
    file_.setFileName(QDir(workDir()).filePath(QStringLiteral("MK3D-Online-%1.zip.part").arg(version())));
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Could not write %1.").arg(QDir::toNativeSeparators(file_.fileName())));
        return;
    }
    QNetworkRequest req{QUrl(release_.value(QStringLiteral("url")).toString())};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MK3D-Online/" MK3D_LAUNCHER_VERSION));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);  // a stall, not the whole download
    applog::write(QStringLiteral("update: downloading %1 from %2").arg(version(), req.url().toString()));
    reply_ = net_->get(req);
    connect(reply_, &QNetworkReply::readyRead, this, &Updater::onData);
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64) {
        const qint64 size = qint64(release_.value(QStringLiteral("size")).toDouble());
        emit progress(tr("Downloading %1: %2 of %3 MB").arg(version()).arg(got / 1048576).arg(size / 1048576),
                      size > 0 ? int(std::min<qint64>(100, got * 100 / size)) : -1);
    });
    connect(reply_, &QNetworkReply::finished, this, &Updater::onDownloaded);
    emit progress(tr("Downloading %1...").arg(version()), 0);
}

void Updater::onData() {
    if (!reply_) return;
    const QByteArray data = reply_->readAll();
    written_ += data.size();
    if (written_ > qint64(release_.value(QStringLiteral("size")).toDouble())) {
        reply_->abort();  // more than the release is: not it
        return;
    }
    hash_.addData(data);
    if (file_.write(data) != data.size()) reply_->abort();
}

void Updater::onDownloaded() {
    if (!reply_) return;
    onData();  // anything not read yet
    QNetworkReply* reply = reply_;
    reply_ = nullptr;
    reply->deleteLater();
    file_.close();
    if (!busy_) return;  // cancelled
    const qint64 size = qint64(release_.value(QStringLiteral("size")).toDouble());
    if (reply->error() != QNetworkReply::NoError && written_ <= size) {
        fail(tr("The download failed: %1").arg(reply->errorString()));
        return;
    }
    const QString sha = QString::fromLatin1(hash_.result().toHex());
    if (written_ != size || sha != release_.value(QStringLiteral("sha256")).toString()) {
        applog::write(QStringLiteral("update: got %1 bytes, SHA-256 %2; expected %3, %4")
                          .arg(written_).arg(sha).arg(size).arg(release_.value(QStringLiteral("sha256")).toString()));
        fail(tr("The download is not the release the server named (its size or SHA-256 differs). Nothing was "
                "changed."));
        return;
    }
    const QString zip = file_.fileName().chopped(5);  // without .part
    QFile::remove(zip);
    if (!QFile::rename(file_.fileName(), zip)) {
        fail(tr("Could not write %1.").arg(QDir::toNativeSeparators(zip)));
        return;
    }
    file_.setFileName(zip);
    applog::write(QStringLiteral("update: %1 downloaded and checked (SHA-256 %2)").arg(version(), sha));
    unpack(false);
}

// Windows' own tar unpacks zips (Windows 10 1803 on); PowerShell's Expand-Archive if it cannot
void Updater::unpack(bool powershell) {
    emit progress(tr("Unpacking %1...").arg(version()), -1);
    QDir(stageDir()).removeRecursively();
    QDir().mkpath(stageDir());
    unzip_ = new QProcess(this);
    const QString system = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("System32"));
    if (powershell) {
        auto quoted = [](const QString& s) { return QLatin1Char('\'') + QString(s).replace(QLatin1Char('\''), QStringLiteral("''")) + QLatin1Char('\''); };
        unzip_->setProgram(QDir(system).filePath(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe")));
        unzip_->setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
                              QStringLiteral("Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force")
                                  .arg(quoted(QDir::toNativeSeparators(file_.fileName())), quoted(QDir::toNativeSeparators(stageDir())))});
    } else {
        unzip_->setProgram(QDir(system).filePath(QStringLiteral("tar.exe")));
        unzip_->setArguments({QStringLiteral("-x"), QStringLiteral("-f"), QDir::toNativeSeparators(file_.fileName()),
                              QStringLiteral("-C"), QDir::toNativeSeparators(stageDir())});
    }
    connect(unzip_, &QProcess::finished, this, [this, powershell](int code, QProcess::ExitStatus status) {
        onUnpacked(powershell, status == QProcess::NormalExit ? code : -1);
    });
    connect(unzip_, &QProcess::errorOccurred, this, [this, powershell](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) onUnpacked(powershell, -1);
    });
    unzip_->start();
}

void Updater::onUnpacked(bool powershell, int exitCode) {
    if (unzip_) unzip_->deleteLater();
    unzip_ = nullptr;
    if (!busy_) return;  // cancelled
    // the release named: its launcher there, and VERSION.txt's first line "MK3D-Online <version>, packaged ..."
    const QDir root(QDir(stageDir()).filePath(QStringLiteral("MK3D-Online")));
    QFile stamp(root.filePath(QStringLiteral("VERSION.txt")));
    const bool whole = exitCode == 0 && QFileInfo::exists(root.filePath(QStringLiteral("MK3DLauncher.exe"))) &&
                       stamp.open(QIODevice::ReadOnly | QIODevice::Text) &&
                       QString::fromUtf8(stamp.readLine()).remove(QChar(0xFEFF)).startsWith(
                           QStringLiteral("MK3D-Online %1,").arg(version()));
    stamp.close();
    if (!whole) {
        applog::write(QStringLiteral("update: unpacking with %1 failed (exit %2)")
                          .arg(powershell ? QStringLiteral("PowerShell") : QStringLiteral("tar")).arg(exitCode));
        if (!powershell) {
            unpack(true);
            return;
        }
        fail(tr("Could not unpack the release. Nothing was changed."));
        return;
    }
    ready_ = true;
    emit progress(tr("%1 is ready to install.").arg(version()), 100);
    emit readyToInstall();
}

void Updater::install() {
    if (!busy_ || !ready_) return;
    emit progress(tr("Installing %1...").arg(version()), -1);
    QStringList olds;
    QString error;
    const QString from = QDir(stageDir()).filePath(QStringLiteral("MK3D-Online"));
    if (!swapIn(from, installDir(), &olds, &error)) {
        applog::write(QStringLiteral("update: installing failed at %1; every file put back").arg(error));
        fail(tr("Could not replace %1. Nothing was changed: try again once nothing uses that file (a game, an "
                "antivirus scan).").arg(error));
        return;
    }
    QFile list(oldList());
    if (list.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) list.write(olds.join(QLatin1Char('\n')).toUtf8());
    list.close();
    QDir(stageDir()).removeRecursively();
    QFile::remove(file_.fileName());
    applog::write(QStringLiteral("update: %1 installed (%2 files replaced), starting it").arg(version()).arg(olds.size()));
    const QString exe = QDir(installDir()).filePath(QStringLiteral("MK3DLauncher.exe"));
    const bool started = QProcess::startDetached(
        exe, {QStringLiteral("--updated"), QStringLiteral(MK3D_LAUNCHER_VERSION), QStringLiteral("--config"), paths::configFile()},
        installDir());
    if (!started) applog::write(QStringLiteral("update: the new launcher did not start"));
    busy_ = false;
    emit installed(version(), started);
}

void Updater::cancel() {
    if (!busy_) return;
    busy_ = false;
    ready_ = false;
    if (reply_) reply_->abort();
    if (unzip_) unzip_->kill();
    file_.close();
    QDir(workDir()).removeRecursively();
    applog::write(QStringLiteral("update: %1 cancelled").arg(version()));
}

void Updater::fail(const QString& text) {
    busy_ = false;
    ready_ = false;
    if (reply_) reply_->abort();
    file_.close();
    QFile::remove(file_.fileName());
    QDir(stageDir()).removeRecursively();
    applog::write(QStringLiteral("update: %1").arg(text));
    emit failed(text);
}
