#pragma once
// In-app updates. The server names the latest release (in welcome, in an "update" push once one is announced, and
// in the reply that turns away a launcher too old for it: docs/PROTOCOL.md 1.1): its version, the zip's link, size
// and SHA-256, a few lines on what is new. The launcher offers it; on a yes it downloads the zip, checks its size and
// SHA-256 against the server's (the server's certificate is pinned, the download host is not trusted), unpacks it in
// the update folder beside the install and swaps the files in: each file the release has is renamed aside (a
// running program's file can be renamed, not overwritten) and the new one moved into its place, and a failure puts
// back every file it moved. Then the new launcher starts (--updated) and this one quits; the new one deletes the
// files set aside once they are free. Only the release's files change: settings, the games' saves, disc data and
// built code stay, and each game prepares its code again on its next start.

#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(QObject* parent = nullptr);

    // the release, when it is newer than this launcher and its fields are sound; {} otherwise
    static QJsonObject usable(const QJsonValue& release);
    // why this install cannot update itself (a source build, a folder it cannot write to), empty when it can
    static QString blocker();
    // what the update set aside, deleted once free (every start; --updated retries while the old launcher exits)
    static void cleanUp();

    bool busy() const { return busy_; }
    QString version() const { return release_.value(QStringLiteral("version")).toString(); }
    void start(const QJsonObject& release);  // download, check and unpack; then readyToInstall
    void install();                          // the files in, the new launcher started: installed
    void cancel();                           // before install: the install stays as it is

signals:
    void progress(const QString& text, int percent);  // percent -1: no measure
    void readyToInstall();                            // unpacked and checked; nothing of the install changed yet
    void failed(const QString& text);                 // the install is as it was
    // the new files are in and the new launcher is starting (`started`): this one closes now
    void installed(const QString& version, bool started);

private:
    void onData();
    void onDownloaded();
    void unpack(bool powershell);
    void onUnpacked(bool powershell, int exitCode);
    void fail(const QString& text);

    QNetworkAccessManager* net_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QPointer<QProcess> unzip_;
    QFile file_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    qint64 written_ = 0;
    QJsonObject release_;
    bool busy_ = false;
    bool ready_ = false;
};
