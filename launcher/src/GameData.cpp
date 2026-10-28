#include "GameData.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QTimeZone>
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>

const GameMode* Game::mode(const QString& modeId) const {
    for (const auto& m : modes)
        if (m.id == modeId) return &m;
    return nullptr;
}

QString Game::otherRelease(const QByteArray& xbe, bool* modified) const {
    if (modified) *modified = false;
    const QByteArray sha1 = QCryptographicHash::hash(xbe, QCryptographicHash::Sha1).toHex();
    if (xbeSha1.isEmpty() || QString::fromLatin1(sha1).compare(xbeSha1, Qt::CaseInsensitive) == 0) return {};
    const auto tr = [](const char* text) { return QCoreApplication::translate("Game", text); };
    // the XBE header (base address, time stamp, certificate address) and its certificate (title ID and name, game
    // region, version)
    const auto u32 = [&xbe](qint64 at) {
        return at >= 0 && at + 4 <= xbe.size() ? qFromLittleEndian<quint32>(xbe.constData() + at) : 0u;
    };
    const qint64 cert = qint64(u32(0x118)) - qint64(u32(0x104));
    const quint32 time = u32(0x114), xbeId = u32(cert + 0x8), region = u32(cert + 0xA0), version = u32(cert + 0xAC);
    QString name;
    if (cert > 0 && cert + 0xC + 80 <= xbe.size())
        name = QString::fromUtf16(reinterpret_cast<const char16_t*>(xbe.constData() + cert + 0xC), 40).section(QChar(0), 0, 0);
    const QString built = QDateTime::fromSecsSinceEpoch(time, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd"));
    QString what;
    if (!xbe.startsWith("XBEH") || cert <= 0) {
        what = tr("not an Xbox game's default.xbe");
    } else if (xbeId != titleId.toUInt(nullptr, 16)) {
        what = tr("another game, %1 (title ID %2)").arg(name, QString::number(xbeId, 16).toUpper());
    } else if (!(region & 1)) {
        what = (region & 4   ? tr("the PAL release of %1 (Europe, Australia)")
                : region & 2 ? tr("the Japanese release of %1")
                             : tr("another region's release of %1"))
                   .arg(title);
    } else if (xbeTime && time == xbeTime) {
        if (modified) *modified = true;
        return tr("This disc is a modified copy of %1 (%2): the same build of the game, with changes (a patch for a "
                  "modded console, say). It may work, or crash where it was changed.")
            .arg(title, edition);
    } else {
        what = (version < xbeVersion   ? tr("an earlier print of %1 (version %2, built %3)")
                : version > xbeVersion ? tr("a later print of %1 (version %2, built %3)")
                                       : tr("another build of %1 (version %2, built %3)"))
                   .arg(title).arg(version).arg(built);
    }
    return tr("This disc is %1, not %2 (%3): MK3D Online's %4 is made from that release only. Another release's code is "
              "laid out differently, so the game would crash with it (and could not play the release everyone else "
              "has online). Use a disc image of %2 (%3): Build game extracts it over this one.")
        .arg(what, title, edition, shortName.isEmpty() ? title : shortName);
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
        g.xbeTime = quint32(o.value(QStringLiteral("xbe_time")).toInteger());
        g.xbeVersion = quint32(o.value(QStringLiteral("xbe_version")).toInt());
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
            m.deathTraps = mo.value(QStringLiteral("death_traps")).toBool();
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
