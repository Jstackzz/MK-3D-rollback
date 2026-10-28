#pragma once
// Games, modes and rosters from the bundled launcher/data/<game>.json files.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

struct Character {
    QString id;
    QString name;
    bool secret = false;
};

struct GameMode {
    QString id;
    QString name;
    QString description;
    QString roster;
    QString characterSelect;  // "launcher" or "in_game"
    bool online = false;
    bool ranked = false;
    QList<int> rounds;
    int defaultRounds = 3;
    bool deathTraps = false;  // its fights' arenas have death traps (Deception, Armageddon), a match may turn off
    bool picksInGame() const { return characterSelect == QLatin1String("in_game"); }
};

struct Game {
    QString id;
    QString title;
    QString shortName;
    QString edition;
    QString titleId;
    QString defaultExe;
    QString xbeSha1;  // the default.xbe this build is made from
    quint32 xbeTime = 0;     // its header's time stamp: every build of the game has its own
    quint32 xbeVersion = 0;  // its certificate's version: a later print of the game has a higher one
    QString notes;
    QList<GameMode> modes;
    QHash<QString, QList<Character>> rosters;
    QStringList notOnXbox;

    const GameMode* mode(const QString& modeId) const;
    QList<GameMode> onlineModes() const;
    QList<Character> roster(const QString& modeId) const;
    QString characterName(const QString& characterId) const;
    // what a disc's default.xbe is, when not the one this build is made from (empty when it is): another game, region
    // or print, which the game would crash with (its code is laid out differently), or a modified copy of the right
    // release (`modified` set: the same build of the game, patched), which may work. For the player.
    QString otherRelease(const QByteArray& xbe, bool* modified = nullptr) const;
};

class GameData {
public:
    static GameData& instance();
    bool load(QString* error);
    const QList<Game>& games() const { return games_; }
    const Game* game(const QString& id) const;

    static const QList<QPair<QString, QString>>& regions();  // id, display name
    static QString regionName(const QString& id);

private:
    QList<Game> games_;
};
