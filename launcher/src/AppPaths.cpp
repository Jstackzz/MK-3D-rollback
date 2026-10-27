#include "AppPaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>

#include "GameData.h"

namespace {
QString g_config;
QString g_dataDir;
QSettings* g_settings = nullptr;
QSettings* g_defaults = nullptr;  // defaults.ini beside the launcher: what a package sets up (the server), read only
QMutex g_logMutex;

bool writableDir(const QString& dir) {
    QDir().mkpath(dir);
    QFile probe(QDir(dir).filePath(QStringLiteral(".write-test")));
    if (!probe.open(QIODevice::WriteOnly)) return false;
    probe.close();
    probe.remove();
    return true;
}
}  // namespace

namespace paths {

void init(const QString& configOverride) {
    if (!configOverride.isEmpty()) {
        g_config = QFileInfo(configOverride).absoluteFilePath();
    } else {
        const QString exeDir = QCoreApplication::applicationDirPath();
        const QString dir = writableDir(exeDir)
                                ? exeDir
                                : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        g_config = QDir(dir).filePath(QStringLiteral("mk3d-launcher.ini"));
    }
    g_dataDir = QFileInfo(g_config).absolutePath();
    QDir().mkpath(g_dataDir);
    QDir().mkpath(sessionsDir());
    g_settings = new QSettings(g_config, QSettings::IniFormat);
    const QString defaults = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("defaults.ini"));
    if (QFileInfo::exists(defaults)) g_defaults = new QSettings(defaults, QSettings::IniFormat);
}

QString configFile() { return g_config; }
QString dataDir() { return g_dataDir; }
QString sessionsDir() { return QDir(g_dataDir).filePath(QStringLiteral("sessions")); }
QString logFile() { return QDir(g_dataDir).filePath(QStringLiteral("mk3d-launcher.log")); }

QString detectedRepoRoot() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 8; ++i) {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("tools/py/xiso.py")))) return dir.absolutePath();
        if (!dir.cdUp()) break;
    }
    return QCoreApplication::applicationDirPath();
}

bool isPackage() { return QFileInfo(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("games"))).isDir(); }

QString packagedGameFolder(const QString& gameId) {
    const QString dir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("games/") + gameId);
    return QFileInfo(dir).isDir() ? dir : QString();
}

QString toolchainDir() { return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("toolchain")); }

}  // namespace paths

namespace cfg {

QSettings& store() { return *g_settings; }
QVariant get(const QString& key, const QVariant& fallback) {
    return g_settings->value(key, g_defaults ? g_defaults->value(key, fallback) : fallback);
}
void set(const QString& key, const QVariant& value) {
    g_settings->setValue(key, value);
    g_settings->sync();
}

QString serverAddress() { return get(QStringLiteral("server/address"), QStringLiteral("127.0.0.1:7777")).toString(); }
QString fingerprint() { return get(QStringLiteral("server/fingerprint")).toString(); }

QString repoRoot() {
    const QString v = get(QStringLiteral("tools/repo_root")).toString();
    return v.isEmpty() ? paths::detectedRepoRoot() : v;
}

QString python() { return get(QStringLiteral("tools/python"), QStringLiteral("python")).toString(); }

QString gameFolder(const QString& gameId) {
    const QString v = get(QStringLiteral("games/%1/folder").arg(gameId)).toString();
    if (!v.isEmpty()) return v;
    const QString packaged = paths::packagedGameFolder(gameId);
    return packaged.isEmpty() ? QDir(repoRoot()).filePath(QStringLiteral("recomp/%1").arg(gameId)) : packaged;
}

QString gameExe(const QString& gameId) {
    const QString v = get(QStringLiteral("games/%1/exe").arg(gameId)).toString();
    if (!v.isEmpty()) return v;
    const Game* g = GameData::instance().game(gameId);
    const QString exe = g && !g->defaultExe.isEmpty() ? g->defaultExe : gameId.toUpper() + QStringLiteral(".exe");
    // a package keeps the executable in the game folder, a source checkout in its build output
    const QDir folder(gameFolder(gameId));
    if (QFileInfo::exists(folder.filePath(exe))) return folder.filePath(exe);
    return folder.filePath(QStringLiteral("out/build/release/") + exe);
}

QString region() { return get(QStringLiteral("online/region"), QStringLiteral("eu")).toString(); }
int maxPing() { return get(QStringLiteral("online/max_ping"), 150).toInt(); }
bool strict() { return get(QStringLiteral("online/strict"), false).toBool(); }
bool pickInGame() { return get(QStringLiteral("online/pick_in_game"), true).toBool(); }
bool practiceWhileWaiting() { return get(QStringLiteral("online/practice_while_waiting"), true).toBool(); }
bool deathTraps() { return get(QStringLiteral("online/death_traps"), true).toBool(); }
int inputDelay() { return qBound(0, get(QStringLiteral("online/delay"), 2).toInt(), 4); }
int gamePort() { return get(QStringLiteral("online/game_port"), 7001).toInt(); }
// as a game the launcher does not start: borderless at the monitor's own resolution
QString displayMode() { return get(QStringLiteral("display/mode"), QStringLiteral("borderless")).toString(); }
QString resolution() { return get(QStringLiteral("display/resolution"), QStringLiteral("desktop")).toString(); }
bool vsync() { return get(QStringLiteral("display/vsync"), true).toBool(); }
bool udpLoopback() { return get(QStringLiteral("dev/udp_loopback"), false).toBool(); }
bool upnp() { return get(QStringLiteral("online/upnp"), true).toBool(); }
int localTestPort(int instance) {
    return get(QStringLiteral("dev/local_port%1").arg(instance), instance == 1 ? 7001 : 7002).toInt();
}

}  // namespace cfg

namespace applog {

void write(const QString& line) {
    QMutexLocker lock(&g_logMutex);
    QFile f(paths::logFile());
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    f.write((stamp + QLatin1Char(' ') + line + QLatin1Char('\n')).toUtf8());
}

}  // namespace applog
