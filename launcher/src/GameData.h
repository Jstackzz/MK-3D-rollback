#pragma once
// Games, modes and rosters from the bundled launcher/data/<game>.json files.

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
    QString notes;
    QList<GameMode> modes;
    QHash<QString, QList<Character>> rosters;
    QStringList notOnXbox;

    const GameMode* mode(const QString& modeId) const;
    QList<GameMode> onlineModes() const;
    QList<Character> roster(const QString& modeId) const;
    QString characterName(const QString& characterId) const;
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
