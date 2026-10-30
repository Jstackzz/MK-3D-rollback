#pragma once
// One game process and its session folder: environment, cmd.txt (launcher -> game) and
// status.txt (game -> launcher), docs/PROTOCOL.md section 3.

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

class GameProcess : public QObject {
    Q_OBJECT
public:
    GameProcess(const QString& gameId, int instance, QObject* parent = nullptr);
    ~GameProcess() override;

    // Starts `exe` (a .py file is run with the configured Python, for the development stand-in).
    // `netMode` false starts it plainly (first-launch compile, single player), without MK3D_NET.
    bool start(const QString& exe, QString* error, bool netMode = true);
    // one more environment variable for the next start (MK3D_TRAINING for practice while a search waits)
    void setEnv(const QString& name, const QString& value) { extraEnv_.insert(name, value); }
    // more command-line arguments for the next start (--prepare)
    void setArgs(const QStringList& args) { extraArgs_ = args; }
    // "quit" command (a plain game: its window is closed, as the player would), then kill after 5 s
    void quit();
    void kill();
    bool isRunning() const;
    qint64 pid() const;  // 0 when not running

    QString gameId() const { return gameId_; }
    int instance() const { return instance_; }
    QString sessionDir() const { return sessionDir_; }
    QString status() const { return status_; }  // first line of status.txt
    int exitCode() const { return exitCode_; }  // the process's, once it has ended
    QString state() const;                     // first word of it
    bool isReady() const { return state() == QLatin1String("ready"); }

    // Write one command line; if the previous command has not been taken yet it waits up to 2 s
    // (a newer command replaces a waiting one).
    void command(const QString& line);

    static QHash<QString, QString> parseKeys(const QString& line);  // key=value tokens after the verb

    // what to tell a player whose Windows blocks the games (`blocked`: it just did): Smart App Control and how to
    // switch it off
    static QString smartAppControlHelp(bool blocked = true);
    // what to tell a player whose antivirus took a game's file (`file`, its name): a false alarm, as a new release's
    // files and the code each PC builds for itself are new to it, and where Windows Security gives it back
    static QString antivirusHelp(const QString& file);
    // a game built before (its code DLL beside it) whose code no longer matches its executable, as after an update:
    // its next start builds the code again (runtime app/game_code.cpp, compiler\work\<exe>\built.txt)
    static bool codeStale(const QString& exe);
    // the game's extracted default.xbe, in the assets folder the game itself reads; empty before it is extracted
    static QString xbeFile(const QString& exe);

signals:
    void statusChanged(const QString& line);
    void outputLine(const QString& line);
    void exited(int exitCode, bool crashed);

private:
    void poll();
    void writeCommand(const QString& line);

    QString gameId_;
    int instance_;
    QProcess* proc_ = nullptr;
    QTimer poll_;
    QString sessionDir_, cmdPath_, statusPath_;
    QString status_;
    QString pendingCmd_;
    QElapsedTimer lastWrite_;
    bool netMode_ = true;
    bool quitting_ = false;
    int exitCode_ = 0;
    QByteArray outBuf_;
    QHash<QString, QString> extraEnv_;
    QStringList extraArgs_;
};
