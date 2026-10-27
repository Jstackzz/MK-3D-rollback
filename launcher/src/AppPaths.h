#pragma once
// Where the launcher keeps its settings, logs and per-session folders, and typed settings access.

#include <QSettings>
#include <QString>
#include <QVariant>

namespace paths {
void init(const QString& configOverride);  // call once, before anything else
QString configFile();                        // mk3d-launcher.ini
QString dataDir();                           // folder of the ini: sessions\, mk3d-launcher.log
QString sessionsDir();
QString logFile();
QString detectedRepoRoot();                  // walks up from the exe looking for tools\py\xiso.py
bool isPackage();                            // a player package: games\ beside the launcher
QString packagedGameFolder(const QString& gameId);  // games\<id> beside the launcher (a player package), or empty
QString toolchainDir();                      // toolchain\ beside the launcher: clang and lld, once for every game
}  // namespace paths

namespace cfg {
QSettings& store();
QVariant get(const QString& key, const QVariant& fallback = {});
void set(const QString& key, const QVariant& value);

QString serverAddress();
QString fingerprint();
QString repoRoot();
QString python();
QString gameFolder(const QString& gameId);
QString gameExe(const QString& gameId);
QString region();
int maxPing();
bool strict();
// the player picks their fighter on the game's own select screen in online matches, not in the launcher
bool pickInGame();
// a Ranked or Kasual search starts the game's practice to play while it waits (MatchController::maybeStartPractice)
bool practiceWhileWaiting();
bool deathTraps();  // the arenas' death traps in Ranked and Kasual matches (off when either player turns them off)
int inputDelay();
int gamePort();
QString displayMode();
QString resolution();
bool vsync();
int localTestPort(int instance);
bool udpLoopback();  // development: matches on loopback only (one-PC tests, no firewall prompt)
bool upnp();         // open the game UDP port on the router (UPnP)
}  // namespace cfg

namespace applog {
void write(const QString& line);  // timestamped line in mk3d-launcher.log
}
