#include "GameData.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

const GameMode* Game::mode(const QString& modeId) const {
    for (const auto& m : modes)
        if (m.id == modeId) return &m;
    return nullptr;
}

QList<GameMode> Game::onlineModes() const {
    QList<GameMode> out;
    for (const auto& m : modes)
        if (m.online) out.append(m);
    return out;
}

QList<Character> Game::roster(const QString& modeId) const {
    const GameMode* m = mode(modeId);
    const QString key = m && !m->roster.isEmpty() ? m->roster : QStringLiteral("fighters");
    return rosters.value(key, rosters.value(QStringLiteral("fighters")));
}

QString Game::characterName(const QString& characterId) const {
    for (auto it = rosters.cbegin(); it != rosters.cend(); ++it)
        for (const auto& c : it.value())
            if (c.id == characterId) return c.name;
    return characterId;
}

GameData& GameData::instance() {
    static GameData data;
    return data;
}

bool GameData::load(QString* error) {
    games_.clear();
    for (const char* id : {"mkda", "mkd", "mka"}) {
        QFile f(QStringLiteral(":/data/%1.json").arg(QLatin1String(id)));
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("missing game data %1").arg(f.fileName());
            return false;
        }
        QJsonParseError perr{};
        const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &perr).object();
        if (perr.error != QJsonParseError::NoError) {
            if (error) *error = QStringLiteral("%1: %2").arg(f.fileName(), perr.errorString());
            return false;
        }
        Game g;
        g.id = o.value(QStringLiteral("id")).toString();
        g.title = o.value(QStringLiteral("title")).toString();
        g.shortName = o.value(QStringLiteral("short")).toString();
        g.edition = o.value(QStringLiteral("edition")).toString();
        g.titleId = o.value(QStringLiteral("title_id")).toString();
        g.defaultExe = o.value(QStringLiteral("default_exe")).toString();
        g.xbeSha1 = o.value(QStringLiteral("xbe_sha1")).toString();
        g.notes = o.value(QStringLiteral("notes")).toString();
        for (const auto& mv : o.value(QStringLiteral("modes")).toArray()) {
            const QJsonObject mo = mv.toObject();
            GameMode m;
            m.id = mo.value(QStringLiteral("id")).toString();
            m.name = mo.value(QStringLiteral("name")).toString();
            m.description = mo.value(QStringLiteral("description")).toString();
            m.roster = mo.value(QStringLiteral("roster")).toString();
            m.characterSelect = mo.value(QStringLiteral("character_select")).toString(QStringLiteral("launcher"));
            m.online = mo.value(QStringLiteral("online")).toBool();
            m.ranked = mo.value(QStringLiteral("ranked")).toBool();
            for (const auto& r : mo.value(QStringLiteral("rounds")).toArray()) m.rounds.append(r.toInt());
            m.defaultRounds = mo.value(QStringLiteral("default_rounds")).toInt(3);
            g.modes.append(m);
        }
        const QJsonObject rosters = o.value(QStringLiteral("rosters")).toObject();
        for (auto it = rosters.begin(); it != rosters.end(); ++it) {
            QList<Character> list;
            for (const auto& cv : it.value().toArray()) {
                const QJsonObject co = cv.toObject();
                list.append({co.value(QStringLiteral("id")).toString(), co.value(QStringLiteral("name")).toString(),
                             co.value(QStringLiteral("secret")).toBool()});
            }
            g.rosters.insert(it.key(), list);
        }
        for (const auto& nv : o.value(QStringLiteral("not_on_xbox")).toArray())
            g.notOnXbox.append(nv.toObject().value(QStringLiteral("name")).toString());
        games_.append(g);
    }
    return true;
}

const Game* GameData::game(const QString& id) const {
    for (const auto& g : games_)
        if (g.id == id) return &g;
    return nullptr;
}

const QList<QPair<QString, QString>>& GameData::regions() {
    static const QList<QPair<QString, QString>> list = {
        {QStringLiteral("na"), QStringLiteral("North America")}, {QStringLiteral("sa"), QStringLiteral("South America")},
        {QStringLiteral("eu"), QStringLiteral("Europe")},        {QStringLiteral("me"), QStringLiteral("Middle East")},
        {QStringLiteral("af"), QStringLiteral("Africa")},        {QStringLiteral("as"), QStringLiteral("Asia")},
        {QStringLiteral("oc"), QStringLiteral("Oceania")},
    };
    return list;
}

QString GameData::regionName(const QString& id) {
    for (const auto& r : regions())
        if (r.first == id) return r.second;
    return id;
}
