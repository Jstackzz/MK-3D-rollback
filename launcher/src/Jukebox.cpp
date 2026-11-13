// The jukebox (Jukebox.h): the music library, the music sets and the plan, the transcoder into the games' sound banks,
// the cache and the map file, and the preview player.
#include "Jukebox.h"

#include "AppPaths.h"
#include "WinUtil.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

namespace jukebox {

namespace {

constexpr double kMaxSongSeconds = 600;  // a song longer than this is cut (the game loops what it gets)
constexpr double kStingerSeconds = 40;   // a track shorter than this is no stage music (a fatality sting)
constexpr double kMenuSeconds = 45;      // a menu theme shorter than this is an intro, not the menu's music
constexpr char kCacheVersion[] = "v6";   // in the cache key: bump when the banks are made differently (v4: loudness, v5: cues, v6: loops)
constexpr qint64 kCacheBudget = qint64(3) * 1024 * 1024 * 1024;  // the cache is trimmed to this, the oldest first

QString lower(const QString& s) { return s.toLower(); }

// --- the library -------------------------------------------------------------------------------------------

QStringList tagsOf(const QString& title) {
    static const QSet<QString> stop = {QStringLiteral("the"), QStringLiteral("of"), QStringLiteral("a"),
                                       QStringLiteral("and"), QStringLiteral("ost"), QStringLiteral("mortal"),
                                       QStringLiteral("kombat"), QStringLiteral("mk"), QStringLiteral("stage")};
    QStringList out;
    const QString t = lower(title).replace(QRegularExpression(QStringLiteral("[^a-z0-9' ]+")), QStringLiteral(" "));
    for (const QString& w : t.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        if (!stop.contains(w) && !out.contains(w)) out << w;
    return out;
}

QString titleFromFile(const QString& file) {
    QString t = QFileInfo(file).completeBaseName();
    t.remove(QRegularExpression(QStringLiteral("^\\d+\\s*[-.]\\s*")));
    return t;
}

// when the game came out, for the order of the soundtracks (a soundtrack.json's "year" first; others after these)
double yearFor(const QString& id) {
    static const QMap<QString, double> known = {
        {QStringLiteral("mk1"), 1992},    {QStringLiteral("mk2"), 1993},  {QStringLiteral("mk3"), 1995},
        {QStringLiteral("umk3"), 1995.5}, {QStringLiteral("mk4"), 1997},  {QStringLiteral("mkda"), 2002},
        {QStringLiteral("mkd"), 2004},    {QStringLiteral("mka"), 2006},  {QStringLiteral("mkvsdc"), 2008},
        {QStringLiteral("mk9"), 2011},    {QStringLiteral("mkx"), 2015},  {QStringLiteral("mk11"), 2019},
        {QStringLiteral("mk1-2023"), 2023}};
    return known.value(lower(id), 0);
}

// the game tabs' names
QString shortTitleFor(const QString& id, const QString& title) {
    static const QMap<QString, QString> known = {
        {QStringLiteral("mk1"), QStringLiteral("MK (1992)")},     {QStringLiteral("mk2"), QStringLiteral("MK II")},
        {QStringLiteral("mk3"), QStringLiteral("MK3")},           {QStringLiteral("umk3"), QStringLiteral("UMK3")},
        {QStringLiteral("mk4"), QStringLiteral("MK4")},           {QStringLiteral("mkvsdc"), QStringLiteral("MK vs DC")},
        {QStringLiteral("mk9"), QStringLiteral("MK (2011)")},     {QStringLiteral("mkx"), QStringLiteral("MKX")},
        {QStringLiteral("mk11"), QStringLiteral("MK11")},         {QStringLiteral("mk1-2023"), QStringLiteral("MK1 (2023)")},
        {QStringLiteral("mkda"), QStringLiteral("Deadly Alliance")}, {QStringLiteral("mkd"), QStringLiteral("Deception")},
        {QStringLiteral("mka"), QStringLiteral("Armageddon")}};
    const auto it = known.find(lower(id));
    if (it != known.end()) return it.value();
    QString t = title;
    t.replace(QRegularExpression(QStringLiteral("(?i)^mortal kombat\\b")), QStringLiteral("MK"));
    return t.size() > 18 ? t.left(17) + QChar(0x2026) : t;
}

void readSoundtrackJson(const QByteArray& json, Soundtrack& s, const QDir& dir) {
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.contains(QStringLiteral("title"))) s.title = o.value(QStringLiteral("title")).toString();
    if (o.contains(QStringLiteral("short"))) s.shortTitle = o.value(QStringLiteral("short")).toString();
    if (o.contains(QStringLiteral("year"))) s.year = o.value(QStringLiteral("year")).toDouble();
    const QString select = o.value(QStringLiteral("select")).toString();
    QList<Track> listed;
    for (const QJsonValue& v : o.value(QStringLiteral("tracks")).toArray()) {
        const QJsonObject t = v.toObject();
        Track tr;
        tr.file = dir.filePath(t.value(QStringLiteral("file")).toString());
        if (!QFileInfo::exists(tr.file)) continue;
        tr.title = t.value(QStringLiteral("title")).toString();
        if (tr.title.isEmpty()) tr.title = titleFromFile(tr.file);
        tr.kind = t.value(QStringLiteral("kind")).toString(QStringLiteral("other"));
        tr.seconds = t.value(QStringLiteral("seconds")).toDouble();
        for (const QJsonValue& tag : t.value(QStringLiteral("tags")).toArray()) tr.tags << lower(tag.toString());
        if (tr.tags.isEmpty()) tr.tags = tagsOf(tr.title);
        listed << tr;
    }
    // files the json does not know are still tracks
    for (const Track& have : s.tracks) {
        const bool known = std::any_of(listed.cbegin(), listed.cend(), [&](const Track& t) {
            return QFileInfo(t.file).fileName().compare(QFileInfo(have.file).fileName(), Qt::CaseInsensitive) == 0;
        });
        if (!known) listed << have;
    }
    s.tracks = listed;
    s.select = -1;
    for (int i = 0; i < s.tracks.size(); ++i)
        if (!select.isEmpty() && QFileInfo(s.tracks[i].file).fileName().compare(select, Qt::CaseInsensitive) == 0) s.select = i;
    if (s.select < 0)
        for (int i = 0; i < s.tracks.size(); ++i)
            if (s.tracks[i].kind == QLatin1String("select")) s.select = i;
}

}  // namespace

const QStringList& audioExtensions() {
    static const QStringList e = {QStringLiteral("m4a"), QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("flac"),
                                  QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("wma"), QStringLiteral("aac"),
                                  QStringLiteral("webm"), QStringLiteral("mp4"), QStringLiteral("wv"), QStringLiteral("aif")};
    return e;
}

QString libraryFolder() {
    const QString v = cfg::get(QStringLiteral("jukebox/folder")).toString();
    if (!v.isEmpty()) return v;
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QDir(music.isEmpty() ? QDir::homePath() : music).filePath(QStringLiteral("MK3D Jukebox"));
}

void setLibraryFolder(const QString& folder) { cfg::set(QStringLiteral("jukebox/folder"), folder); }

QList<Soundtrack> scanLibrary(const QString& folder) {
    QList<Soundtrack> out;
    const QDir root(folder);
    if (!root.exists()) return out;
    for (const QFileInfo& sub : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (sub.fileName().compare(QLatin1String("cache"), Qt::CaseInsensitive) == 0) continue;
        Soundtrack s;
        s.id = sub.fileName();
        s.title = s.id;
        s.folder = sub.absoluteFilePath();
        const QDir dir(s.folder);
        QStringList filters;
        for (const QString& e : audioExtensions()) filters << QStringLiteral("*.") + e;
        for (const QFileInfo& f : dir.entryInfoList(filters, QDir::Files, QDir::Name)) {
            Track t;
            t.file = f.absoluteFilePath();
            t.title = titleFromFile(t.file);
            t.kind = QStringLiteral("stage");
            t.tags = tagsOf(t.title);
            s.tracks << t;
        }
        if (s.tracks.isEmpty()) continue;
        QFile own(dir.filePath(QStringLiteral("soundtrack.json")));
        QFile builtIn(QStringLiteral(":/soundtracks/%1.json").arg(s.id.toLower()));
        if (own.open(QIODevice::ReadOnly)) readSoundtrackJson(own.readAll(), s, dir);
        else if (builtIn.open(QIODevice::ReadOnly)) readSoundtrackJson(builtIn.readAll(), s, dir);
        if (s.shortTitle.isEmpty()) s.shortTitle = shortTitleFor(s.id, s.title);
        for (const char* name : {"cover.jpg", "cover.png", "cover.jpeg", "folder.jpg", "folder.png"}) {
            const QString p = dir.filePath(QString::fromLatin1(name));
            if (QFileInfo::exists(p)) {
                s.cover = p;
                break;
            }
        }
        if (s.year <= 0) s.year = yearFor(s.id);
        out << s;
    }
    // in the order the games came out (MK 1992, MK II, MK3, UMK3, MK4 ...), the ones of no known year after them
    std::stable_sort(out.begin(), out.end(), [](const Soundtrack& a, const Soundtrack& b) {
        const double ya = a.year > 0 ? a.year : 1e9, yb = b.year > 0 ? b.year : 1e9;
        if (ya != yb) return ya < yb;
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    });
    return out;
}

const Soundtrack* find(const QList<Soundtrack>& library, const QString& id) {
    for (const Soundtrack& s : library)
        if (s.id.compare(id, Qt::CaseInsensitive) == 0) return &s;
    return nullptr;
}

const Track* trackByChoice(const QString& choice, const QList<Soundtrack>& library, QString* soundtrackId) {
    const int slash = choice.indexOf(QLatin1Char('/'));
    if (slash <= 0) return nullptr;
    const Soundtrack* s = find(library, choice.left(slash));
    if (!s) return nullptr;
    const QString file = choice.mid(slash + 1);
    for (const Track& t : s->tracks)
        if (QFileInfo(t.file).fileName().compare(file, Qt::CaseInsensitive) == 0) {
            if (soundtrackId) *soundtrackId = s->id;
            return &t;
        }
    return nullptr;
}

QString choiceOf(const Soundtrack& s, const Track& t) { return s.id + QLatin1Char('/') + QFileInfo(t.file).fileName(); }

// --- the games' stages and screens ------------------------------------------------------------------------------

namespace {

QList<int> streamsOf(const QJsonValue& v) {
    if (v.isString() && v.toString() == QLatin1String("all")) return {-1};
    if (v.isString() && v.toString() == QLatin1String("cue")) return {-2};  // the round music's cues (roundMusicStreams)
    QList<int> out;
    for (const QJsonValue& s : v.toArray()) out << s.toInt();
    return out;
}

Slot slotOf(const QJsonObject& o) {
    Slot s;
    s.name = o.value(QStringLiteral("name")).toString();
    s.bank = o.value(QStringLiteral("bank")).toString();
    s.streams = streamsOf(o.value(QStringLiteral("streams")));
    for (const QJsonValue& t : o.value(QStringLiteral("tags")).toArray()) s.tags << lower(t.toString());
    s.why = o.value(QStringLiteral("why")).toString();
    return s;
}

QMap<QString, GameMusic> loadGameMusic() {
    QMap<QString, GameMusic> out;
    QFile f(QStringLiteral(":/data/jukebox.json"));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject games = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("games")).toObject();
    for (auto it = games.begin(); it != games.end(); ++it) {
        const QJsonObject g = it.value().toObject();
        GameMusic gm;
        gm.game = it.key();
        for (const QJsonValue& v : g.value(QStringLiteral("stages")).toArray()) {
            const QJsonObject o = v.toObject();
            Slot s = slotOf(o);
            s.stage = o.value(QStringLiteral("id")).toInt(-1);
            s.key = QStringLiteral("stage:%1").arg(s.stage);
            gm.places << s;
        }
        for (const QJsonValue& v : g.value(QStringLiteral("screens")).toArray()) {
            const QJsonObject o = v.toObject();
            Slot s = slotOf(o);
            s.screen = o.value(QStringLiteral("id")).toString();
            s.key = QStringLiteral("screen:") + s.screen;
            gm.places << s;
        }
        out.insert(gm.game, gm);
    }
    return out;
}

}  // namespace

const Slot* GameMusic::slot(const QString& key) const {
    for (const Slot& s : places)
        if (s.key == key) return &s;
    return nullptr;
}

const GameMusic* gameMusic(const QString& gameId) {
    static const QMap<QString, GameMusic> all = loadGameMusic();
    auto it = all.find(gameId);
    return it == all.end() ? nullptr : &it.value();
}

const Pick* Plan::pick(const QString& slot) const {
    for (const Pick& p : picks)
        if (p.slot == slot) return &p;
    return nullptr;
}

// --- the music sets: presets and shuffles ---------------------------------------------------------------------

namespace {

bool stageMaterial(const Track& t) {
    if (t.seconds > 0 && t.seconds < kStingerSeconds) return false;
    return t.kind == QLatin1String("stage") || t.kind == QLatin1String("other");
}

bool menuMaterial(const Track& t) {
    return (t.kind == QLatin1String("menu") || t.kind == QLatin1String("theme")) && (t.seconds <= 0 || t.seconds >= kMenuSeconds);
}

// a soundtrack's main-menu music: its first long menu track, else its first long theme
const Track* menuTrack(const Soundtrack& s) {
    for (const char* kind : {"menu", "theme"})
        for (const Track& t : s.tracks)
            if (t.kind == QLatin1String(kind) && (t.seconds <= 0 || t.seconds >= kMenuSeconds)) return &t;
    return nullptr;
}

int score(const Slot& stage, const Track& track) {
    int n = 0;
    const QString title = lower(track.title);
    const QString name = lower(stage.name);
    if (title.contains(name)) n += 6;
    for (const QString& tag : stage.tags) {
        if (track.tags.contains(tag)) n += tag.contains(QLatin1Char(' ')) ? 4 : 2;
        else if (tag.contains(QLatin1Char(' ')) && title.contains(tag)) n += 3;
    }
    return n;
}

// the soundtrack's tracks for the stages: the best keyword match each, every track once while they last, then round
// the stage tracks again in their order
QMap<QString, const Track*> stagePicks(const GameMusic& gm, const Soundtrack& s) {
    QList<const Track*> pool;
    for (const Track& t : s.tracks)
        if (stageMaterial(t)) pool << &t;
    if (pool.isEmpty())  // a soundtrack of themes only: anything long enough
        for (const Track& t : s.tracks)
            if (t.kind != QLatin1String("stinger") && (t.seconds <= 0 || t.seconds >= kStingerSeconds)) pool << &t;
    QMap<QString, const Track*> out;
    if (pool.isEmpty()) return out;
    struct Cand {
        const Slot* stage;
        const Track* track;
        int score;
    };
    QList<Cand> cands;
    QList<const Slot*> stages;
    for (const Slot& st : gm.places) {
        if (!st.isStage() || st.bank.isEmpty()) continue;
        stages << &st;
        for (const Track* t : pool) {
            const int sc = score(st, *t);
            if (sc > 0) cands.append({&st, t, sc});
        }
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    QSet<const Track*> used;
    for (const Cand& c : cands) {
        if (out.contains(c.stage->key) || used.contains(c.track)) continue;
        out.insert(c.stage->key, c.track);
        used.insert(c.track);
    }
    // the rest: the unused tracks first, in the soundtrack's order, then the whole soundtrack round again (a
    // soundtrack of 8 stage songs for a game of 20 stages: each song twice or three times, not the last one 12 times)
    QList<const Track*> order;
    for (const Track* t : pool)
        if (!used.contains(t)) order << t;
    int k = 0;
    for (const Slot* st : stages) {
        if (out.contains(st->key)) continue;
        out.insert(st->key, k < order.size() ? order[k] : pool[(k - order.size()) % pool.size()]);
        k++;
    }
    return out;
}

using Entry = QPair<const Soundtrack*, const Track*>;

// a shuffle: the stages draw from the soundtrack's (or every soundtrack's) stage music, each song once while they
// last; the select screen gets the soundtrack's own select music (with every soundtrack, one of theirs), the main
// menu a theme
QMap<QString, QString> shuffleChoices(const GameMusic& gm, const QList<Soundtrack>& library, const QString& sid,
                                      quint32 draw) {
    QRandomGenerator rng(draw ^ quint32(qHash(gm.game)));
    QList<Entry> stage, menu, select;
    for (const Soundtrack& s : library) {
        if (sid != QLatin1String("*") && s.id.compare(sid, Qt::CaseInsensitive) != 0) continue;
        for (const Track& t : s.tracks) {
            if (stageMaterial(t)) stage << Entry(&s, &t);
            if (menuMaterial(t)) menu << Entry(&s, &t);
        }
        if (s.select >= 0) select << Entry(&s, &s.tracks[s.select]);
    }
    auto shuffled = [&rng](QList<Entry> v) {
        for (qsizetype i = v.size() - 1; i > 0; --i) v.swapItemsAt(i, qsizetype(rng.bounded(quint32(i + 1))));
        return v;
    };
    QMap<QString, QString> out;
    const QList<Entry> deck = shuffled(stage);
    int k = 0;
    for (const Slot& s : gm.places) {
        if (s.isStage()) {
            if (deck.isEmpty() || s.bank.isEmpty()) continue;
            const Entry e = deck[k++ % deck.size()];
            out.insert(s.key, choiceOf(*e.first, *e.second));
        } else if (s.screen == QLatin1String("select") && !select.isEmpty()) {
            const Entry e = select[int(rng.bounded(quint32(select.size())))];
            out.insert(s.key, choiceOf(*e.first, *e.second));
        } else if (s.screen == QLatin1String("menu") && !menu.isEmpty()) {
            const Entry e = menu[int(rng.bounded(quint32(menu.size())))];
            out.insert(s.key, choiceOf(*e.first, *e.second));
        }
    }
    return out;
}

}  // namespace

int Config::playlistIndex() const {
    if (!set.startsWith(QLatin1String("playlist:"))) return -1;
    const int n = set.mid(9).toInt();
    return n >= 1 && n <= kPlaylists ? n - 1 : -1;
}

QString Config::setSoundtrack() const {
    if (set.startsWith(QLatin1String("preset:"))) return set.mid(7);
    if (set.startsWith(QLatin1String("shuffle:"))) return set.mid(8);
    return {};
}

QMap<QString, QString> presetChoices(const QString& gameId, const Soundtrack& s) {
    QMap<QString, QString> out;
    const GameMusic* gm = gameMusic(gameId);
    if (!gm) return out;
    const QMap<QString, const Track*> stages = stagePicks(*gm, s);
    for (const Slot& slot : gm->places) {
        const Track* t = nullptr;
        if (slot.isStage()) t = stages.value(slot.key, nullptr);
        else if (slot.screen == QLatin1String("select") && s.select >= 0) t = &s.tracks[s.select];
        else if (slot.screen == QLatin1String("menu")) t = menuTrack(s);
        if (t) out.insert(slot.key, choiceOf(s, *t));
    }
    return out;
}

QString describeSet(const QString& set, const QList<Soundtrack>& library) {
    if (set.isEmpty() || set == QLatin1String("own")) return QStringLiteral("The game's own music");
    if (set == QLatin1String("shuffle:*")) return QStringLiteral("Every soundtrack, shuffled");
    if (set.startsWith(QLatin1String("playlist:"))) return QStringLiteral("My Playlist %1").arg(set.mid(9));
    const bool shuffle = set.startsWith(QLatin1String("shuffle:"));
    const QString sid = set.mid(shuffle ? 8 : 7);
    const Soundtrack* s = find(library, sid);
    return QStringLiteral("%1, %2").arg(s ? s->title : sid, shuffle ? QStringLiteral("shuffled") : QStringLiteral("preset"));
}

// --- the configuration --------------------------------------------------------------------------------------

Config loadConfig(const QString& gameId) {
    Config c;
    const QString p = QStringLiteral("jukebox/%1/").arg(gameId);
    const QString set = cfg::get(p + QStringLiteral("set")).toString();
    if (!set.isEmpty()) {
        c.set = set;
        for (int i = 0; i < kPlaylists; i++) {
            const QByteArray json = cfg::get(p + QStringLiteral("playlist%1").arg(i + 1)).toString().toUtf8();
            const QJsonObject o = QJsonDocument::fromJson(json).object();
            for (auto it = o.begin(); it != o.end(); ++it) c.playlists[i].insert(it.key(), it.value().toString());
        }
        c.shuffleDraw = cfg::get(p + QStringLiteral("shuffle_draw"), 1).toUInt();
        return c;
    }
    // the first jukebox's settings (test 15): a soundtrack, with songs picked by hand for some stages or the select
    // screen; those become My Playlist 1 (the soundtrack's preset with the hand picks over it)
    const QString old = cfg::get(p + QStringLiteral("soundtrack")).toString();
    QMap<QString, QString> hand;
    const QString select = cfg::get(p + QStringLiteral("select")).toString();
    if (!select.isEmpty() && select != QLatin1String("auto")) hand.insert(QStringLiteral("screen:select"), select);
    if (const GameMusic* gm = gameMusic(gameId))
        for (const Slot& s : gm->places) {
            if (!s.isStage()) continue;
            const QString v = cfg::get(p + QStringLiteral("stage_%1").arg(s.stage)).toString();
            if (!v.isEmpty() && v != QLatin1String("auto")) hand.insert(s.key, v);
        }
    if (hand.isEmpty()) {
        if (!old.isEmpty()) c.set = QStringLiteral("preset:") + old;
        return c;
    }
    if (!old.isEmpty()) {
        const QList<Soundtrack> library = scanLibrary(libraryFolder());
        if (const Soundtrack* s = find(library, old)) c.playlists[0] = presetChoices(gameId, *s);
    }
    for (auto it = hand.cbegin(); it != hand.cend(); ++it) c.playlists[0].insert(it.key(), it.value());
    c.set = QStringLiteral("playlist:1");
    return c;
}

void saveConfig(const QString& gameId, const Config& c) {
    const QString p = QStringLiteral("jukebox/%1/").arg(gameId);
    cfg::set(p + QStringLiteral("set"), c.set);
    for (int i = 0; i < kPlaylists; i++) {
        QJsonObject o;
        for (auto it = c.playlists[i].cbegin(); it != c.playlists[i].cend(); ++it) o.insert(it.key(), it.value());
        cfg::set(p + QStringLiteral("playlist%1").arg(i + 1), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    }
    cfg::set(p + QStringLiteral("shuffle_draw"), c.shuffleDraw);
}

QString mapFile(const QString& gameId) {
    return QDir(paths::dataDir()).filePath(QStringLiteral("jukebox-%1.txt").arg(gameId));
}

// --- the plan -------------------------------------------------------------------------------------------------

Plan resolve(const QString& gameId, const Config& config, const QList<Soundtrack>& library) {
    Plan plan;
    const GameMusic* gm = gameMusic(gameId);
    if (!gm) return plan;
    QMap<QString, QString> choices;
    const QString sid = config.setSoundtrack();
    const int list = config.playlistIndex();
    if (!sid.isEmpty() && sid != QLatin1String("*") && !find(library, sid)) {
        plan.notes << QStringLiteral("The soundtrack \"%1\" is not in the music folder: the game's own music plays.").arg(sid);
    } else if (config.set.startsWith(QLatin1String("preset:"))) {
        choices = presetChoices(gameId, *find(library, sid));
    } else if (config.set.startsWith(QLatin1String("shuffle:"))) {
        choices = shuffleChoices(*gm, library, sid, config.shuffleDraw);
    } else if (list >= 0) {
        choices = config.playlists[list];
    }
    for (const Slot& s : gm->places) {
        Pick p;
        p.slot = s.key;
        p.stage = s.stage;
        p.screen = s.screen;
        p.bank = s.bank;
        p.streams = s.streams;
        const QString c = choices.value(s.key);
        if (!c.isEmpty() && c != QLatin1String("own")) {
            QString tsid;
            const Track* t = trackByChoice(c, library, &tsid);
            if (!t) {
                plan.notes << QStringLiteral("%1: the song \"%2\" is not in the music folder any more; the game's own plays.").arg(s.name, c);
            } else if (s.bank.isEmpty()) {
                if (list >= 0) plan.notes << QStringLiteral("%1 keeps the game's own music: %2").arg(s.name, s.why);
            } else {
                p.soundtrack = tsid;
                p.file = t->file;
                p.title = t->title;
            }
        }
        plan.picks << p;
    }
    return plan;
}

// --- Media Foundation decoding ------------------------------------------------------------------------------

namespace {

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

bool mfReady() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return ok;
}

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

}  // namespace

bool decodeAudio(const QString& path, int& rate, int& channels, QVector<int16_t>& pcm, QString* error, double maxSeconds) {
    ComInit com;
    if (!mfReady()) {
        if (error) *error = QStringLiteral("Media Foundation is not available on this Windows");
        return false;
    }
    ComPtr<IMFSourceReader> reader;
    const std::wstring w = path.toStdWString();
    HRESULT hr = MFCreateSourceReaderFromURL(w.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        if (error) *error = QStringLiteral("cannot open %1 (%2)").arg(QFileInfo(path).fileName()).arg(unsigned(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want.p);
    if (FAILED(hr)) {
        if (error) *error = QStringLiteral("no decoder for %1 (%2)").arg(QFileInfo(path).fileName()).arg(unsigned(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    ComPtr<IMFMediaType> actual;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual);
    UINT32 ch = 0, sr = 0, bits = 0;
    actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    if (!ch || !sr || bits != 16) {
        if (error) *error = QStringLiteral("%1 decodes to an unexpected format").arg(QFileInfo(path).fileName());
        return false;
    }
    rate = int(sr);
    channels = int(ch);
    pcm.clear();
    const qint64 maxSamples = maxSeconds > 0 ? qint64(maxSeconds * sr) * ch : -1;
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) {
            if (error) *error = QStringLiteral("%1 could not be decoded (%2)").arg(QFileInfo(path).fileName()).arg(unsigned(hr), 8, 16, QLatin1Char('0'));
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
            const qsizetype n = qsizetype(len / 2);
            const qsizetype at = pcm.size();
            pcm.resize(at + n);
            std::memcpy(pcm.data() + at, data, size_t(n) * 2);
            buf->Unlock();
        }
        if (maxSamples > 0 && pcm.size() >= maxSamples) {
            pcm.resize(maxSamples);
            break;
        }
    }
    if (pcm.isEmpty()) {
        if (error) *error = QStringLiteral("%1 holds no audio").arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

// --- resampling ---------------------------------------------------------------------------------------------

namespace {

constexpr int kPhases = 1024, kTaps = 32;  // a windowed sinc, 16 taps each side, 1024 fractional positions
constexpr double kPi = 3.14159265358979323846;

const std::vector<float>& sincTable(double cutoff) {
    // one table per cutoff (two in practice: down and up)
    static std::mutex lock;
    static std::vector<std::pair<double, std::vector<float>>> tables;
    std::lock_guard<std::mutex> g(lock);
    for (auto& [c, t] : tables)
        if (std::abs(c - cutoff) < 1e-9) return t;
    std::vector<float> t(size_t(kPhases) * kTaps);
    for (int ph = 0; ph < kPhases; ph++) {
        const double frac = double(ph) / kPhases;
        double sum = 0;
        for (int k = 0; k < kTaps; k++) {
            const double x = (k - kTaps / 2 + 1) - frac;  // distance from the output time to input sample k
            const double w = 0.5 * (1 + std::cos(kPi * x / (kTaps / 2)));  // Hann over the taps
            const double s = x == 0 ? 1.0 : std::sin(kPi * cutoff * x) / (kPi * cutoff * x);
            const double v = cutoff * s * w;
            t[size_t(ph) * kTaps + k] = float(v);
            sum += v;
        }
        for (int k = 0; k < kTaps; k++) t[size_t(ph) * kTaps + k] = float(t[size_t(ph) * kTaps + k] / sum);
    }
    tables.emplace_back(cutoff, std::move(t));
    return tables.back().second;
}

}  // namespace

QVector<int16_t> convertPcm(const QVector<int16_t>& pcm, int channels, int rate, int outChannels, int outRate) {
    // channels first
    QVector<int16_t> mixed;
    const qsizetype frames = pcm.size() / channels;
    if (channels == outChannels) {
        mixed = pcm;
    } else {
        mixed.resize(frames * outChannels);
        for (qsizetype f = 0; f < frames; f++)
            for (int c = 0; c < outChannels; c++) {
                int v;
                if (channels == 1) v = pcm[f];
                else if (c < channels) v = pcm[f * channels + c];
                else v = pcm[f * channels + (channels - 1)];
                if (outChannels == 1 && channels >= 2) v = (pcm[f * channels] + pcm[f * channels + 1]) / 2;
                mixed[f * outChannels + c] = int16_t(v);
            }
    }
    if (rate == outRate) return mixed;
    const double ratio = double(rate) / outRate;  // input frames per output frame
    const double cutoff = std::min(1.0, 1.0 / ratio) * 0.92;
    const std::vector<float>& table = sincTable(cutoff);
    const qsizetype outFrames = qsizetype(double(frames) / ratio);
    QVector<int16_t> out(outFrames * outChannels);
    for (qsizetype i = 0; i < outFrames; i++) {
        const double t = i * ratio;
        const qsizetype k0 = qsizetype(std::floor(t));
        const int ph = int((t - double(k0)) * kPhases) % kPhases;
        const float* w = &table[size_t(ph) * kTaps];
        for (int c = 0; c < outChannels; c++) {
            double acc = 0;
            for (int k = 0; k < kTaps; k++) {
                const qsizetype src = k0 + (k - kTaps / 2 + 1);
                if (src < 0 || src >= frames) continue;
                acc += w[k] * mixed[src * outChannels + c];
            }
            out[i * outChannels + c] = int16_t(std::clamp<int>(int(std::lround(acc)), -32768, 32767));
        }
    }
    return out;
}

// --- Xbox ADPCM -----------------------------------------------------------------------------------------------

namespace {

const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStepTable[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                            31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                            130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                            544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                            2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                            9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

}  // namespace

QByteArray encodeXboxAdpcm(const QVector<int16_t>& pcm, int channels) {
    const qsizetype frames = pcm.size() / channels;
    const qsizetype blocks = frames / 64;
    QByteArray out;
    out.reserve(blocks * 36 * channels);
    std::vector<int> pred(size_t(channels), 0), idx(size_t(channels), 0);
    std::vector<uint8_t> nib(size_t(channels) * 64);
    for (qsizetype b = 0; b < blocks; b++) {
        const qsizetype base = b * 64 * channels;
        // the header: the block's first sample as the predictor, played as is, and the step index to go on with
        for (int c = 0; c < channels; c++) {
            pred[c] = pcm[base + c];
            const int16_t p = int16_t(pred[c]);
            out.append(char(p & 0xFF));
            out.append(char((p >> 8) & 0xFF));
            out.append(char(idx[c]));
            out.append(char(0));
        }
        for (int c = 0; c < channels; c++) {
            int p = pred[c], i = idx[c];
            for (int s = 0; s < 64; s++) {
                const qsizetype at = base + qsizetype(s + 1) * channels + c;  // the 64th is the next block's first
                const int sample = at < pcm.size() ? pcm[at] : p;
                const int step = kStepTable[i];
                int diff = sample - p;
                int n = 0;
                if (diff < 0) {
                    n = 8;
                    diff = -diff;
                }
                if (diff >= step) {
                    n |= 4;
                    diff -= step;
                }
                if (diff >= step >> 1) {
                    n |= 2;
                    diff -= step >> 1;
                }
                if (diff >= step >> 2) n |= 1;
                int d = step >> 3;
                if (n & 1) d += step >> 2;
                if (n & 2) d += step >> 1;
                if (n & 4) d += step;
                p = std::clamp(n & 8 ? p - d : p + d, -32768, 32767);
                i = std::clamp(i + kIndexTable[n], 0, 88);
                nib[size_t(c) * 64 + s] = uint8_t(n);
            }
            idx[c] = i;
        }
        for (int k = 0; k < 8; k++)
            for (int c = 0; c < channels; c++)
                for (int j = 0; j < 8; j += 2)
                    out.append(char(nib[size_t(c) * 64 + k * 8 + j] | nib[size_t(c) * 64 + k * 8 + j + 1] << 4));
    }
    return out;
}

// --- loudness -------------------------------------------------------------------------------------------------

namespace {

constexpr double kLevelSeconds = 240;  // how much of a game's stream is measured
constexpr double kMaxBoostDb = 20;     // a song is made at most this much louder ...
constexpr double kMaxCutDb = 15;       // ... or quieter
constexpr double kCeilingDb = -1;      // the limiter keeps a song's peaks under this (dBFS)

// Xbox ADPCM blocks to interleaved samples, 64 frames a block as the games play them (the header's sample, then 63
// of the 64 codes; the last one is the next block's first sample)
QVector<int16_t> decodeXboxAdpcm(const char* data, qsizetype size, int channels) {
    const qsizetype block = 36 * qsizetype(channels);
    const qsizetype blocks = channels > 0 ? size / block : 0;
    QVector<int16_t> out(blocks * 64 * channels);
    for (qsizetype b = 0; b < blocks; b++) {
        const auto* p = reinterpret_cast<const uint8_t*>(data) + b * block;
        int16_t* o = out.data() + b * 64 * channels;
        for (int c = 0; c < channels; c++) {
            int pred = int16_t(uint16_t(p[4 * c] | p[4 * c + 1] << 8));
            int idx = std::min(int(p[4 * c + 2]), 88);
            o[c] = int16_t(pred);
            int at = 1;
            for (int k = 0; k < 8; k++) {
                const uint8_t* w = p + 4 * channels + (k * channels + c) * 4;
                for (int j = 0; j < 8; j++, at++) {
                    const int n = (w[j / 2] >> ((j & 1) * 4)) & 15;
                    const int step = kStepTable[idx];
                    int d = step >> 3;
                    if (n & 1) d += step >> 2;
                    if (n & 2) d += step >> 1;
                    if (n & 4) d += step;
                    pred = std::clamp(n & 8 ? pred - d : pred + d, -32768, 32767);
                    idx = std::clamp(idx + kIndexTable[n], 0, 88);
                    if (at < 64) o[at * channels + c] = int16_t(pred);
                }
            }
        }
    }
    return out;
}

// One stage of the BS.1770 K-weighting filter (transposed direct form II), made for the sound's rate
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Biquad shelfStage(int rate) {  // +4 dB above about 1.7 kHz, the head's own boost
    const double gainDb = 3.99984385397, q = 0.7071752369554193, fc = 1681.9744509555319;
    const double a = std::pow(10.0, gainDb / 40), w0 = 2 * kPi * fc / rate, alpha = std::sin(w0) / (2 * q),
                 c = std::cos(w0), s = 2 * std::sqrt(a) * alpha;
    const double a0 = (a + 1) - (a - 1) * c + s;
    Biquad f;
    f.b0 = a * ((a + 1) + (a - 1) * c + s) / a0;
    f.b1 = -2 * a * ((a - 1) + (a + 1) * c) / a0;
    f.b2 = a * ((a + 1) + (a - 1) * c - s) / a0;
    f.a1 = 2 * ((a - 1) - (a + 1) * c) / a0;
    f.a2 = ((a + 1) - (a - 1) * c - s) / a0;
    return f;
}

Biquad highPassStage(int rate) {  // the lowest bass counts less
    const double q = 0.5003270373253953, fc = 38.13547087613982;
    const double w0 = 2 * kPi * fc / rate, alpha = std::sin(w0) / (2 * q), c = std::cos(w0), a0 = 1 + alpha;
    Biquad f;
    f.b0 = (1 + c) / 2 / a0;
    f.b1 = -(1 + c) / a0;
    f.b2 = (1 + c) / 2 / a0;
    f.a1 = -2 * c / a0;
    f.a2 = (1 - alpha) / a0;
    return f;
}

// How loud a sound is, in LUFS (ITU-R BS.1770: K-weighted, 400 ms blocks every 100 ms, the blocks under -70 LUFS and
// those 10 LU under the rest's loudness left out). A mono sound counts as played on both speakers, as the games do.
// NaN when it is shorter than a block or silent.
double loudnessOf(const int16_t* pcm, qsizetype frames, int channels, int rate) {
    const qsizetype step = rate / 10;
    if (channels <= 0 || step <= 0 || frames < 4 * step) return std::nan("");
    std::vector<Biquad> shelf(size_t(channels), shelfStage(rate)), high(size_t(channels), highPassStage(rate));
    const double weight = channels == 1 ? 2.0 : 1.0;
    std::vector<double> steps;  // each 100 ms: the K-weighted energy summed over the channels
    steps.reserve(size_t(frames / step));
    double acc = 0;
    for (qsizetype f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
            const double y = high[c].run(shelf[c].run(pcm[f * channels + c] / 32768.0));
            acc += y * y;
        }
        if ((f + 1) % step == 0) {
            steps.push_back(acc * weight);
            acc = 0;
        }
    }
    std::vector<double> blocks;  // each block's mean power
    for (size_t i = 3; i < steps.size(); i++) blocks.push_back((steps[i - 3] + steps[i - 2] + steps[i - 1] + steps[i]) / (4.0 * step));
    const auto lufs = [](double power) { return -0.691 + 10 * std::log10(power); };
    double sum = 0;
    int n = 0;
    for (double p : blocks)
        if (p > 0 && lufs(p) > -70) sum += p, n++;
    if (!n) return std::nan("");
    const double gate = lufs(sum / n) - 10;
    double kept = 0;
    int m = 0;
    for (double p : blocks)
        if (p > 0 && lufs(p) > -70 && lufs(p) > gate) kept += p, m++;
    return m ? lufs(kept / m) : std::nan("");
}

// A sound made gainDb louder (or quieter), its peaks held under kCeilingDb by a limiter that looks 5 ms ahead (the
// gain comes down smoothly before a peak and goes back up over about 200 ms), so a song brought up does not clip
void applyGain(QVector<int16_t>& pcm, int channels, int rate, double gainDb) {
    if (channels <= 0 || rate <= 0 || std::abs(gainDb) < 0.01) return;
    const double gain = std::pow(10.0, gainDb / 20), ceiling = 32767.0 * std::pow(10.0, kCeilingDb / 20);
    const qsizetype frames = pcm.size() / channels;
    const qsizetype ahead = std::max(1, rate / 200);
    const double release = 1 - std::exp(-1.0 / (0.2 * rate));
    const auto reduction = [&](qsizetype f) {  // what the frame needs to stay under the ceiling
        int peak = 0;
        for (int c = 0; c < channels; c++) peak = std::max(peak, std::abs(int(pcm[f * channels + c])));
        const double p = peak * gain;
        return p > ceiling ? ceiling / p : 1.0;
    };
    // the smallest need over the next `ahead` frames (a sliding minimum), averaged over the last `ahead` of those:
    // the average of minimums whose windows all hold this frame is never more than this frame's own need
    std::vector<std::pair<qsizetype, double>> window;  // (frame, need), the needs rising: a monotonic queue
    size_t head = 0;
    qsizetype next = 0;
    std::vector<double> last(size_t(ahead), 1.0);
    double sum = double(ahead), g = 1;
    for (qsizetype n = -(ahead - 1); n < frames; n++) {
        for (; next < frames && next <= n + ahead - 1; next++) {
            const double r = reduction(next);
            while (window.size() > head && window.back().second >= r) window.pop_back();
            window.emplace_back(next, r);
        }
        while (head < window.size() && window[head].first < n) head++;
        if (head > 4096 && head * 2 > window.size()) {  // keep the queue short
            window.erase(window.begin(), window.begin() + qsizetype(head));
            head = 0;
        }
        const double low = head < window.size() ? window[head].second : 1.0;
        const size_t slot = size_t((n + ahead) % ahead);
        sum += low - last[slot];
        last[slot] = low;
        if (n < 0) continue;
        g = std::min(sum / double(ahead), g + (1 - g) * release);
        const double k = gain * g;
        for (int c = 0; c < channels; c++) {
            int16_t& s = pcm[n * channels + c];
            s = int16_t(std::clamp<long>(std::lround(s * k), -32768, 32767));
        }
    }
}

}  // namespace

// --- loops ----------------------------------------------------------------------------------------------------
//
// A song is made to loop as game music does: its intro once, then its loop over and over. Soundtrack rips play a
// game's loop twice or more and fade out; the loop is the stretch that comes back one period later. Found from the
// song alone (build/jukebox/loop-scan.py is the same method, tried on the whole library): 0.1 s frames of 40
// log-spaced bands (80 Hz to 5 kHz) of a mono 11025 Hz copy, each frame's own loudness taken out (a repeat that fades
// still matches), each band centred over the song and each frame normalised; for every lag from 8 s up, the longest
// stretch (gaps under 1 s closed) of frames matching the frames one lag later (similarity over 0.7). The loop is the
// shortest lag whose stretch verifies 80% of it and 20 s or more, starts in the song's first half and matches 0.8 on
// average. The period is refined to the sample by waveform correlation, the loop counted exact when nearly every
// second of it repeats sample for sample (else it repeats musically: a rip that varies, an edited extended version;
// such a loop must come back two and a half times or cover most of the song, else a longer lag is tried),
// and an exact loop's start found to the waveform: the first moment from which the song repeats one period later (the
// frames' start can be seconds early, and a loop starting in the intro would replay a moment of the intro every time
// round; a musical loop starts 1 s after the frames' start). On Deadly Alliance's own music the periods found are
// the game's own loop lengths to within 0.03 s. No such lag: the song does not repeat, and the whole of it loops (from
// where its music starts to where it ends: before a fade-out, a final decay or silence).

namespace {

constexpr int kLoopRate = 11025;
constexpr int kLoopWin = 2048;
constexpr int kLoopBands = 40;
constexpr double kLoopHop = 0.1;

void fftInPlace(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / double(len);
        const std::complex<float> wl(float(std::cos(ang)), float(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// the frames' features (frames x kLoopBands), loudness-free, centred, normalised
std::vector<float> loopFeatures(const std::vector<float>& x, qsizetype& frames) {
    const qsizetype hop = qsizetype(kLoopRate * kLoopHop);
    frames = qsizetype(x.size()) >= kLoopWin ? (qsizetype(x.size()) - kLoopWin) / hop + 1 : 0;
    std::vector<float> f(size_t(frames) * kLoopBands, 0.0f);
    if (!frames) return f;
    std::vector<float> hann(kLoopWin);
    for (int i = 0; i < kLoopWin; i++) hann[i] = float(0.5 - 0.5 * std::cos(2 * kPi * i / (kLoopWin - 1)));
    int lo[kLoopBands], hi[kLoopBands];
    for (int b = 0; b < kLoopBands; b++) {
        const double e0 = 80 * std::pow(5000.0 / 80, double(b) / kLoopBands), e1 = 80 * std::pow(5000.0 / 80, double(b + 1) / kLoopBands);
        lo[b] = int(std::ceil(e0 * kLoopWin / kLoopRate));
        hi[b] = int(std::ceil(e1 * kLoopWin / kLoopRate));  // bins with e0 <= freq < e1
    }
    std::vector<std::complex<float>> buf(kLoopWin);
    for (qsizetype t = 0; t < frames; t++) {
        for (int i = 0; i < kLoopWin; i++) buf[i] = std::complex<float>(x[size_t(t * hop + i)] * hann[i], 0);
        fftInPlace(buf);
        float* row = &f[size_t(t) * kLoopBands];
        double mean = 0;
        for (int b = 0; b < kLoopBands; b++) {
            double e = 0;
            for (int k = lo[b]; k < hi[b] && k <= kLoopWin / 2; k++) e += std::norm(buf[k]);
            row[b] = float(std::log1p(e * 1e4));
            mean += row[b];
        }
        mean /= kLoopBands;
        for (int b = 0; b < kLoopBands; b++) row[b] -= float(mean);  // the frame's own loudness out
    }
    for (int b = 0; b < kLoopBands; b++) {
        double mean = 0;
        for (qsizetype t = 0; t < frames; t++) mean += f[size_t(t) * kLoopBands + b];
        mean /= double(frames);
        for (qsizetype t = 0; t < frames; t++) f[size_t(t) * kLoopBands + b] -= float(mean);
    }
    for (qsizetype t = 0; t < frames; t++) {
        float* row = &f[size_t(t) * kLoopBands];
        double norm = 0;
        for (int b = 0; b < kLoopBands; b++) norm += double(row[b]) * row[b];
        const float inv = float(1.0 / (std::sqrt(norm) + 1e-9));
        for (int b = 0; b < kLoopBands; b++) row[b] *= inv;
    }
    return f;
}

// the level of each `seconds` block of x, dBFS
std::vector<double> blockLevels(const std::vector<float>& x, double seconds) {
    const qsizetype blk = qsizetype(kLoopRate * seconds);
    std::vector<double> out;
    for (qsizetype at = 0; at + blk <= qsizetype(x.size()); at += blk) {
        double e = 0;
        for (qsizetype i = 0; i < blk; i++) e += double(x[size_t(at + i)]) * x[size_t(at + i)];
        out.push_back(10 * std::log10(e / double(blk) + 1e-18));
    }
    return out;
}

double waveCorr(const std::vector<float>& x, qsizetype a, qsizetype b, qsizetype w) {
    if (a < 0 || b < 0 || a + w > qsizetype(x.size()) || b + w > qsizetype(x.size())) return 0;
    double ab = 0, aa = 0, bb = 0;
    for (qsizetype i = 0; i < w; i++) {
        const double u = x[size_t(a + i)], v = x[size_t(b + i)];
        ab += u * v;
        aa += u * u;
        bb += v * v;
    }
    return aa > 1e-9 && bb > 1e-9 ? ab / std::sqrt(aa * bb) : 0;
}

}  // namespace

LoopInfo findLoop(const QVector<int16_t>& pcm, int channels, int rate) {
    LoopInfo li;
    const QVector<int16_t> mono = convertPcm(pcm, channels, rate, 1, kLoopRate);
    std::vector<float> x(size_t(mono.size()));
    for (qsizetype i = 0; i < mono.size(); i++) x[size_t(i)] = mono[i] / 32768.0f;
    const double dur = double(x.size()) / kLoopRate;
    li.end = dur;
    // where the music starts and ends: the first 0.05 s over -45 dBFS; after the last 0.4 s block within 10 dB of the
    // song's loud level (its 75th percentile), so a fade-out, a final decay or silence is left off
    {
        const std::vector<double> fine = blockLevels(x, 0.05);
        for (size_t i = 0; i < fine.size(); i++)
            if (fine[i] > -45) {
                li.begin = double(i) * 0.05;
                break;
            }
        std::vector<double> lev = blockLevels(x, 0.4);
        if (lev.size() >= 5) {
            std::vector<double> sorted = lev;
            std::sort(sorted.begin(), sorted.end());
            const double loud = sorted[size_t(0.75 * double(sorted.size() - 1))];
            for (size_t i = lev.size(); i-- > 0;)
                if (lev[i] >= loud - 10) {
                    li.end = std::min(dur, double(i + 1) * 0.4);
                    break;
                }
        }
        if (li.end <= li.begin + 1) {
            li.begin = 0;
            li.end = dur;
        }
    }
    // trailing silence (under -50 dBFS): the repeat is looked for before it
    double silence = 0;
    {
        const std::vector<double> lev = blockLevels(x, 0.05);
        size_t last = lev.size();
        while (last > 0 && lev[last - 1] <= -50) last--;
        silence = double(lev.size() - last) * 0.05;
    }
    qsizetype frames = 0;
    const std::vector<float> f = loopFeatures(x, frames);
    const qsizetype endF = std::min<qsizetype>(frames, qsizetype((dur - silence) / kLoopHop));
    const qsizetype lo = qsizetype(8 / kLoopHop), gap = qsizetype(1.0 / kLoopHop), minRun = qsizetype(20 / kLoopHop);
    if (endF < 3 * lo) return li;
    std::vector<float> d(static_cast<size_t>(endF), 0.0f);
    for (qsizetype lag = lo; lag <= qsizetype(double(endF) / 1.5); lag++) {
        const qsizetype n = endF - lag;
        if (n < lo) break;
        qsizetype bestStart = 0, bestLen = 0, runStart = -1, last = -(gap + 2);
        for (qsizetype t = 0; t < n; t++) {
            const float* u = &f[size_t(t) * kLoopBands];
            const float* v = &f[size_t(t + lag) * kLoopBands];
            float s = 0;
            for (int b = 0; b < kLoopBands; b++) s += u[b] * v[b];
            d[size_t(t)] = s;
            if (s > 0.7f) {
                if (t - last > gap + 1) runStart = t;
                last = t;
                if (last - runStart + 1 > bestLen) {
                    bestLen = last - runStart + 1;
                    bestStart = runStart;
                }
            }
        }
        if (bestLen < std::max<qsizetype>(qsizetype(0.8 * double(lag)), minRun) || double(bestStart) * kLoopHop > 0.5 * dur) continue;
        double mean = 0;
        for (qsizetype t = bestStart; t < bestStart + bestLen; t++) mean += d[size_t(t)];
        mean /= double(bestLen);
        if (mean < 0.8) continue;
        // the shortest such lag: the loop itself (a longer one would be two of it)
        const double start = double(bestStart) * kLoopHop + 0.3, lagS = double(lag) * kLoopHop;
        // the period to the sample: where a 3 s stretch inside the repeat matches best, within 0.15 s of the lag
        const qsizetype w = 3 * kLoopRate, a = qsizetype((start + std::min(lagS / 2, 6.0)) * kLoopRate);
        double best = -1;
        qsizetype bestOff = qsizetype(lagS * kLoopRate);
        if (a + w < qsizetype(x.size())) {
            double aa = 0;
            for (qsizetype i = 0; i < w; i++) aa += double(x[size_t(a + i)]) * x[size_t(a + i)];
            const qsizetype from = qsizetype((lagS - 0.15) * kLoopRate), to = qsizetype((lagS + 0.15) * kLoopRate);
            double bb = 0;
            bool primed = false;
            for (qsizetype off = from; off <= to && a + off + w <= qsizetype(x.size()); off++) {
                if (!primed) {
                    for (qsizetype i = 0; i < w; i++) bb += double(x[size_t(a + off + i)]) * x[size_t(a + off + i)];
                    primed = true;
                } else {
                    const double out = x[size_t(a + off - 1)], in = x[size_t(a + off + w - 1)];
                    bb += in * in - out * out;
                }
                double ab = 0;
                for (qsizetype i = 0; i < w; i++) ab += double(x[size_t(a + i)]) * x[size_t(a + off + i)];
                const double c = aa > 1e-9 && bb > 1e-9 ? ab / std::sqrt(aa * bb) : 0;
                if (c > best) {
                    best = c;
                    bestOff = off;
                }
            }
        }
        const double period = double(bestOff) / kLoopRate;
        // exact: 80% or more of its 1 s windows, over the verified stretch, match one period later sample for sample
        qsizetype good = 0, total = 0;
        const qsizetype until = qsizetype((double(bestStart + bestLen) * kLoopHop + period) * kLoopRate);
        for (qsizetype t = qsizetype(start * kLoopRate); t + bestOff + kLoopRate <= std::min<qsizetype>(qsizetype(x.size()), until); t += kLoopRate) {
            total++;
            if (waveCorr(x, t, t + bestOff, kLoopRate) >= 0.9) good++;
        }
        li.found = true;
        li.exact = total && double(good) / double(total) >= 0.8;
        li.period = period;
        // the loop start to the waveform: the first moment (0.05 s steps, from 3 s before the frames' start to 15 s
        // after) from which 0.25 s windows match one period later (0.9 or more) for 1.5 s on, so that the loop never
        // takes a moment of the intro with it (the frames' start can be early by seconds); a little after it. A
        // musical repeat (not sample for sample) starts 1 s after the frames' start.
        li.start = double(bestStart) * kLoopHop + 1.0;
        if (li.exact) {
            const qsizetype w = kLoopRate / 4, step = kLoopRate / 20, hold = 30;
            const qsizetype from = std::max<qsizetype>(0, qsizetype((double(bestStart) * kLoopHop - 3) * kLoopRate));
            const qsizetype to = qsizetype((double(bestStart) * kLoopHop + 15) * kLoopRate);
            std::vector<char> ok;
            for (qsizetype t = from; t <= to; t += step) ok.push_back(waveCorr(x, t, t + bestOff, w) >= 0.9);
            qsizetype found = -1;
            for (qsizetype i = 0; i + hold <= qsizetype(ok.size()) && found < 0; i++) {
                bool all = true;
                for (qsizetype k = 0; k < hold && all; k++) all = ok[size_t(i + k)];
                if (all) found = i;
            }
            if (found >= 0) li.start = double(from + found * step) / kLoopRate + 0.1;
            else li.exact = false;  // no stretch repeats sample for sample: a musical repeat after all
        }
        // a musical repeat must show more: its stretch comes back at least two and a half times, or the loop covers
        // most of the song (else it may be one section of a song that moves on: a longer lag, or the whole song)
        if (!li.exact && double(bestLen) * kLoopHop < 1.5 * period && li.start + 2 * period < 0.75 * li.end) {
            li.found = false;
            continue;
        }
        break;
    }
    return li;
}

QString describeLoop(const LoopInfo& li) {
    if (li.found)
        return QStringLiteral("loop %1 s from %2 s (%3)")
            .arg(li.period, 0, 'f', 2)
            .arg(li.start, 0, 'f', 1)
            .arg(li.exact ? QStringLiteral("exact") : QStringLiteral("musical"));
    return QStringLiteral("no repeat: the whole song loops, %1 s to %2 s").arg(li.begin, 0, 'f', 1).arg(li.end, 0, 'f', 1);
}

namespace {

// a song cut into what the game's music pieces play: its intro (once), its loop (over and over; the loop's last
// moments cross-faded into what comes before the loop's start, so that going round is seamless), the two in a row
// (a piece the game plays once and ends on) and a moment of silence (a piece before an intro). made: the song as the
// stream's format has it (channels, rate). A loop's length is whole ADPCM blocks (64 frames).
struct SongPieces {
    QVector<int16_t> intro, loop, once, silence;
};

// frames [from, from + inFrames) of pcm (both may be fractional) resampled to exactly outFrames: a loop of the song's
// own period made whole ADPCM blocks long (a stretch of at most 32 frames over the loop: far too little to hear), so
// that its end runs into its start exactly
QVector<int16_t> stretchPcm(const QVector<int16_t>& pcm, int channels, double from, double inFrames, qsizetype outFrames) {
    const qsizetype frames = pcm.size() / channels;
    const double ratio = inFrames / double(outFrames);
    const std::vector<float>& table = sincTable(0.92);
    QVector<int16_t> out(outFrames * channels);
    for (qsizetype i = 0; i < outFrames; i++) {
        const double t = from + double(i) * ratio;
        const qsizetype k0 = qsizetype(std::floor(t));
        const int ph = int((t - double(k0)) * kPhases) % kPhases;
        const float* w = &table[size_t(ph) * kTaps];
        for (int c = 0; c < channels; c++) {
            double acc = 0;
            for (int k = 0; k < kTaps; k++) {
                const qsizetype src = k0 + (k - kTaps / 2 + 1);
                if (src < 0 || src >= frames) continue;
                acc += w[k] * pcm[src * channels + c];
            }
            out[i * channels + c] = int16_t(std::clamp<long>(std::lround(acc), -32768, 32767));
        }
    }
    return out;
}

SongPieces cutSong(const QVector<int16_t>& made, int channels, int rate, const LoopInfo& li) {
    SongPieces p;
    const qsizetype frames = made.size() / channels;
    const auto slice = [&](qsizetype from, qsizetype n) {
        return made.mid(from * channels, n * channels);
    };
    qsizetype introAt = 0, introN = 0, loopAt = 0, loopN = 0, crossFrom = 0, crossN = 0;
    bool equalPower = true;
    if (li.found) {
        introAt = 0;
        introN = std::min<qsizetype>(qsizetype(std::llround(li.start * rate)), frames);
        loopAt = introN;
        loopN = std::max<qsizetype>(64, qsizetype(std::llround(li.period * rate / 64)) * 64);
        if (double(loopAt) + li.period * rate + 1 > double(frames)) loopN = 0;  // not all there: the whole song loops
        crossN = std::min<qsizetype>({qsizetype((li.exact ? 0.06 : 1.0) * rate), introN, loopN / 4});
        crossFrom = loopAt - crossN;  // what comes before the loop's start
        equalPower = !li.exact;
    }
    const bool periodic = li.found && loopN >= 64;
    if (!periodic) {
        const qsizetype begin = std::min<qsizetype>(qsizetype(li.begin * rate), frames);
        const qsizetype end = std::clamp<qsizetype>(qsizetype(li.end * rate), begin, frames);
        crossN = std::min<qsizetype>(qsizetype(2.0 * rate), (end - begin) / 4);
        introAt = begin;
        introN = std::max<qsizetype>(crossN, 64);
        loopAt = introAt + introN;
        loopN = std::max<qsizetype>(0, end - loopAt) / 64 * 64;
        crossFrom = introAt;  // the loop goes round into the song's beginning
        crossN = std::min(crossN, loopN / 2);
        equalPower = true;
    }
    p.intro = slice(introAt, introN);
    p.loop = periodic ? stretchPcm(made, channels, li.start * rate, li.period * rate, loopN)  // exactly one period
                      : slice(loopAt, loopN);
    for (qsizetype i = 0; i < crossN && loopN > 0; i++) {
        const double u = (double(i) + 0.5) / double(crossN);
        const double win = equalPower ? std::sin(u * kPi / 2) : u, wout = equalPower ? std::cos(u * kPi / 2) : 1 - u;
        for (int c = 0; c < channels; c++) {
            const qsizetype at = (loopN - crossN + i) * channels + c;
            const double v = p.loop[at] * wout + made[(crossFrom + i) * channels + c] * win;
            p.loop[at] = int16_t(std::clamp<long>(std::lround(v), -32768, 32767));
        }
    }
    if (p.intro.size() < 64 * channels) p.intro.resize(64 * channels);  // a stream holds one block at least
    if (p.loop.size() < 64 * channels) p.loop.resize(64 * channels);
    p.once = p.intro + p.loop;
    p.silence = QVector<int16_t>(qsizetype(std::max(64, rate / 10 / 64 * 64)) * channels, 0);
    return p;
}

}  // namespace

// --- the banks ------------------------------------------------------------------------------------------------

namespace {

uint32_t u32(const QByteArray& d, qsizetype at) {
    if (at < 0 || at + 4 > d.size()) return 0;
    uint32_t v;
    std::memcpy(&v, d.constData() + at, 4);
    return v;
}
uint16_t u16(const QByteArray& d, qsizetype at) {
    if (at < 0 || at + 2 > d.size()) return 0;
    uint16_t v;
    std::memcpy(&v, d.constData() + at, 2);
    return v;
}
void put32(QByteArray& d, qsizetype at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); }
void put16(QByteArray& d, qsizetype at, uint16_t v) { std::memcpy(d.data() + at, &v, 2); }

struct BankEntry {
    int index = 0;
    int id = 0;
    uint32_t off = 0, size = 0;  // as the table has them
    uint32_t hdr = 0;            // where the RIFF/fmt header is (version 1: at off; 2: in the file header)
    uint32_t data = 0, dataSize = 0;  // the ADPCM blocks
    bool resident = false;
    int tag = 0, channels = 0, rate = 0;
};

// Version 1 (Deadly Alliance): 12-byte entries from 0x18, each sound a whole RIFF WAVE. Version 2 (Deception,
// Armageddon): 24-byte entries from 0x34 (id, offset, size, RIFF header's place in the file header, 40, resident),
// every sound's 40-byte RIFF header in the file header; the resident block (the sounds the game loads whole at boot,
// offsets from its start) follows the file header on a 0x800 boundary, [0x14] its size; the streamed sounds after it
struct Bank {
    int version = 0;
    QList<BankEntry> entries;
    uint32_t headerEnd = 0;   // where the file header ends (version 2: on its 0x800 boundary)
    uint32_t residentAt = 0;  // version 2: the resident block's start
};

bool readFormat(const QByteArray& d, BankEntry& e) {
    if (d.mid(e.hdr, 4) != "RIFF" || d.mid(e.hdr + 8, 8) != "WAVEfmt ") return false;
    e.tag = u16(d, e.hdr + 20);
    e.channels = u16(d, e.hdr + 22);
    e.rate = int(u32(d, e.hdr + 24));
    return e.channels > 0 && e.rate > 0;
}

bool parseBank(const QByteArray& d, Bank& b, QString* error) {
    b.version = int(u32(d, 0));
    b.entries.clear();
    if (b.version == 1) {
        const int n = int(u32(d, 4));
        b.headerEnd = uint32_t(d.size());
        for (int i = 0; i < n && i < 1024; i++) {
            BankEntry e;
            e.index = i;
            e.id = int(u32(d, 0x18 + 12 * i) & 0xFFFF);
            e.off = u32(d, 0x18 + 12 * i + 4);
            e.size = u32(d, 0x18 + 12 * i + 8);
            if (!e.size) break;
            e.hdr = e.off;
            e.data = e.off + 0x30;
            e.dataSize = e.size > 0x30 ? e.size - 0x30 : 0;
            if (e.off + qsizetype(e.size) > d.size() || !readFormat(d, e)) {
                if (error) *error = QStringLiteral("stream %1 is not a sound of the bank").arg(e.id);
                return false;
            }
            b.headerEnd = std::min(b.headerEnd, e.off);
            b.entries << e;
        }
        return true;
    }
    if (b.version == 2) {
        const int n = int(u32(d, 4)) - 1;
        if (n < 0 || n > 4096 || 0x34 + 24 * qsizetype(n) > d.size()) {
            if (error) *error = QStringLiteral("not a sound bank");
            return false;
        }
        uint32_t end = 0x34 + 24 * uint32_t(n);
        for (int i = 0; i < n; i++) end = std::max(end, u32(d, 0x34 + 24 * i + 12) + 40);
        b.headerEnd = (end + 0x7FF) & ~0x7FFu;
        b.residentAt = u32(d, 0x14) ? b.headerEnd : 0;
        for (int i = 0; i < n; i++) {
            BankEntry e;
            e.index = i;
            const qsizetype at = 0x34 + 24 * i;
            e.id = int(u32(d, at) & 0xFFFF);
            e.off = u32(d, at + 4);
            e.size = u32(d, at + 8);
            e.hdr = u32(d, at + 12);
            e.resident = u32(d, at + 20) != 0;
            e.data = e.resident ? e.off + b.residentAt : e.off;
            e.dataSize = e.size;
            if (e.data + qsizetype(e.dataSize) > d.size() || !readFormat(d, e)) {
                if (error) *error = QStringLiteral("stream %1 is not a sound of the bank").arg(e.id);
                return false;
            }
            b.entries << e;
        }
        return true;
    }
    if (error) *error = QStringLiteral("not a sound bank (version %1)").arg(b.version);
    return false;
}

double secondsOf(const BankEntry& e) {
    if (e.tag == 0x69) return double(e.dataSize / (36u * unsigned(e.channels)) * 64) / e.rate;
    return 0;
}

QByteArray xboxHeader(int ch, int rate, uint32_t dataSize, bool withDataChunk) {
    QByteArray h;
    h.resize(withDataChunk ? 0x30 : 0x28);
    std::memcpy(h.data(), "RIFF", 4);
    put32(h, 4, (withDataChunk ? 0x30 : 0x28) + dataSize);  // the games' tool: the whole size, not size - 8
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    put32(h, 16, 20);
    put16(h, 20, 0x69);
    put16(h, 22, uint16_t(ch));
    put32(h, 24, uint32_t(rate));
    put32(h, 28, uint32_t(rate) * 36u * unsigned(ch) / 64u);
    put16(h, 32, uint16_t(36 * ch));
    put16(h, 34, 4);
    put16(h, 36, 2);
    put16(h, 38, 64);
    if (withDataChunk) {
        std::memcpy(h.data() + 40, "data", 4);
        put32(h, 44, dataSize);
    }
    return h;
}

}  // namespace

QList<QPair<int, double>> bankStreams(const QByteArray& bank) {
    QList<QPair<int, double>> out;
    Bank b;
    if (!parseBank(bank, b, nullptr)) return out;
    for (const BankEntry& e : b.entries)
        if (!e.resident) out << qMakePair(e.id, secondsOf(e));
    return out;
}

bool replaceStreamsMany(const QByteArray& bank, const QList<StreamSong>& songs, double maxSeconds, QByteArray& out,
                        QString* error, QList<int>* streamedIds, QList<double>* gainsDb, QMap<int, double>* newSeconds,
                        const QMap<int, SongPart>* parts, QList<LoopInfo>* loops) {
    Bank b;
    if (!parseBank(bank, b, error)) return false;
    QMap<int, int> songOf;  // entry index -> the song it gets
    for (int si = 0; si < songs.size(); si++) {
        for (int id : songs[si].streams) {
            bool found = false;
            for (const BankEntry& e : b.entries) {
                if (id != -1 && e.id != id) continue;
                if (e.tag != 0x69) {
                    if (id == -1) continue;  // every music stream: a sound of another format stays
                    break;
                }
                found = true;
                if (songOf.contains(e.index) && songOf.value(e.index) != si) {
                    if (error) *error = QStringLiteral("stream %1 is given two songs").arg(e.id);
                    return false;
                }
                songOf.insert(e.index, si);
            }
            if (!found) {
                if (error) *error = QStringLiteral("stream %1 is not a music stream of the bank").arg(id);
                return false;
            }
        }
    }
    // each song once per format its streams have, as loud as the music it replaces (the longest of its streams as
    // the game has it): a song mastered quieter than the game's music is brought up to it, a louder one down; then cut
    // into the parts its streams play (cutSong: the intro, the loop, the two in a row, a moment of silence)
    const auto partOf = [&](const BankEntry& e) { return parts ? parts->value(e.id, SongPart::Loop) : SongPart::Loop; };
    using BlockKey = QPair<QPair<int, int>, QPair<int, int>>;  // ((song, part), (channels, rate))
    const auto keyOf = [&](int si, const BankEntry& e) {
        return BlockKey(qMakePair(si, int(partOf(e))), qMakePair(e.channels, e.rate));
    };
    QMap<BlockKey, QByteArray> blocks;
    if (gainsDb) *gainsDb = QList<double>(songs.size(), 0.0);
    if (loops) *loops = QList<LoopInfo>(songs.size());
    for (int si = 0; si < songs.size(); si++) {
        QSet<QPair<int, int>> formats;
        BankEntry longest;
        for (const BankEntry& e : b.entries)
            if (songOf.value(e.index, -1) == si) {
                formats.insert(qMakePair(e.channels, e.rate));
                if (!longest.channels || secondsOf(e) > secondsOf(longest)) longest = e;
            }
        if (formats.isEmpty()) continue;
        int rate = 0, channels = 0;
        QVector<int16_t> pcm;
        if (!decodeAudio(songs[si].file, rate, channels, pcm, error, maxSeconds)) return false;
        const LoopInfo li = findLoop(pcm, channels, rate);
        if (loops) (*loops)[si] = li;
        const qsizetype measured = std::min<qsizetype>(longest.dataSize, qsizetype(kLevelSeconds * longest.rate / 64 + 1) * 36 * longest.channels);
        const QVector<int16_t> game = decodeXboxAdpcm(bank.constData() + longest.data, measured, longest.channels);
        const double target = loudnessOf(game.constData(), game.size() / longest.channels, longest.channels, longest.rate);
        const double own = loudnessOf(pcm.constData(), pcm.size() / channels, channels, rate);
        const bool level = std::isfinite(target) && std::isfinite(own);
        for (const auto& f : formats) {
            const QVector<int16_t> converted = convertPcm(pcm, channels, rate, f.first, f.second);
            double gainDb = level ? std::clamp(target - own, -kMaxCutDb, kMaxBoostDb) : 0.0;
            QVector<int16_t> made = converted;
            applyGain(made, f.first, f.second, gainDb);
            // the limiter takes some of a big boost back off the peaks: what is missing is made up (twice at most)
            for (int pass = 0; level && pass < 2; pass++) {
                const double got = loudnessOf(made.constData(), made.size() / f.first, f.first, f.second);
                const double more = std::min(target - got, kMaxBoostDb - gainDb);
                if (!std::isfinite(got) || more < 0.3) break;
                gainDb += more;
                made = converted;
                applyGain(made, f.first, f.second, gainDb);
            }
            if (gainsDb) (*gainsDb)[si] = gainDb;
            const SongPieces pieces = cutSong(made, f.first, f.second, li);
            for (const BankEntry& e : b.entries) {
                if (songOf.value(e.index, -1) != si || e.channels != f.first || e.rate != f.second) continue;
                const BlockKey key = keyOf(si, e);
                if (blocks.contains(key)) continue;
                const SongPart part = partOf(e);
                const QVector<int16_t>& piece = part == SongPart::Intro  ? pieces.intro
                                                : part == SongPart::Once  ? pieces.once
                                                : part == SongPart::Silence ? pieces.silence
                                                                            : pieces.loop;
                blocks.insert(key, encodeXboxAdpcm(piece, f.first));
            }
        }
    }
    if (newSeconds) {
        newSeconds->clear();
        for (const BankEntry& e : b.entries) {
            const int si = songOf.value(e.index, -1);
            if (si < 0) continue;
            const QByteArray& data = blocks[keyOf(si, e)];
            newSeconds->insert(e.id, double(data.size() / (36 * e.channels) * 64) / e.rate);
        }
    }
    const qsizetype align = 0x800;
    out = bank.left(b.headerEnd);
    if (b.version == 2) {
        // the resident sounds that stay, laid out again in the resident block; one replaced becomes a streamed sound
        // (the game has no room for a longer one: it would not load it at all)
        qsizetype rpos = 0;
        for (BankEntry& e : b.entries) {
            if (!e.resident) continue;
            if (songOf.contains(e.index)) {
                e.resident = false;
                put32(out, 0x34 + 24 * e.index + 20, 0);
                if (streamedIds) *streamedIds << e.id;
                continue;
            }
            rpos = (rpos + 3) / 4 * 4;
            out.append(QByteArray(int(b.headerEnd + rpos - out.size()), '\0'));
            out.append(bank.mid(e.data, e.dataSize));
            e.off = uint32_t(rpos);
            rpos += e.dataSize;
        }
        const qsizetype rsize = rpos ? (rpos + 0x7FF) / 0x800 * 0x800 : 0;
        out.append(QByteArray(int(b.headerEnd + rsize - out.size()), '\0'));
        put32(out, 0x14, uint32_t(rsize));
    }
    QMap<BlockKey, QPair<uint32_t, uint32_t>> placed;  // a song part's blocks already in the file
    qsizetype pos = out.size();
    for (BankEntry& e : b.entries) {
        if (e.resident) continue;
        const int si = songOf.value(e.index, -1);
        if (si >= 0) {
            const BlockKey key = keyOf(si, e);
            const QByteArray& data = blocks[key];
            if (b.version == 2) {
                const QByteArray h = xboxHeader(e.channels, e.rate, uint32_t(data.size()), false);
                std::memcpy(out.data() + e.hdr, h.constData(), size_t(h.size()));
            }
            if (placed.contains(key)) {  // the streams of one song playing one part share one copy of it
                e.off = placed[key].first;
                e.size = placed[key].second;
                continue;
            }
            pos = (pos + align - 1) / align * align;
            out.append(QByteArray(int(pos - out.size()), '\0'));
            const QByteArray blob = b.version == 1 ? xboxHeader(e.channels, e.rate, uint32_t(data.size()), true) + data : data;
            e.off = uint32_t(pos);
            e.size = uint32_t(blob.size());
            placed.insert(key, qMakePair(e.off, e.size));
            out.append(blob);
        } else {
            pos = (pos + align - 1) / align * align;
            out.append(QByteArray(int(pos - out.size()), '\0'));
            const QByteArray blob = b.version == 1 ? bank.mid(e.off, e.size) : bank.mid(e.data, e.dataSize);
            e.off = uint32_t(pos);
            out.append(blob);
        }
        pos = out.size();
    }
    for (const BankEntry& e : b.entries) {
        const qsizetype at = b.version == 1 ? 0x18 + 12 * e.index + 4 : 0x34 + 24 * e.index + 4;
        put32(out, at, e.off);
        put32(out, at + 4, e.size);
    }
    put32(out, b.version == 1 ? 0x10 : 0x20, uint32_t(out.size()));
    return true;
}

QByteArray setStreamed(const QByteArray& mbx, const QList<int>& ids) {
    // the descriptor's sounds: 36-byte records from [0x10], their count at [8]; +8 the id, +32 1 when streamed
    QByteArray out = mbx;
    const uint32_t at = u32(out, 0x10);
    const int n = u16(out, 8);
    for (int i = 0; i < n; i++) {
        const qsizetype rec = at + 36 * qsizetype(i);
        if (rec + 36 > out.size()) break;
        if (ids.contains(int(u32(out, rec + 8) & 0xFFFF))) put32(out, rec + 32, 1);
    }
    return out;
}

// --- the descriptor's music cues -------------------------------------------------------------------------------

namespace {

// The cue section is the fourth of the descriptor's sections (their offsets from [0x10], so [0x1C] to [0x20]), an
// array of elements: 44 bytes in version 6 (Deadly Alliance: +4 flags, +8 the seconds), 48 in version 11 (Deception,
// Armageddon: +8 flags, +24 the seconds). An element's first word is 0x2000NNNN for sound NNNN (flags 0x10301 play
// once, 0x301 loop; 0x10101 and 0x101 for a sound the game keeps in memory, as Deception's main menu) or 0xFFFFFFFF
// with flags 7 for a cue's end (its seconds: after the last element started, or -1 never) or with flags 8 for going on
// within the cue (its seconds after the element before started, the elements after it timed from it: Armageddon's
// Tekunin Prison and Goro's Lair start their music so); anything else is left alone.
struct CueElement {
    qsizetype at = 0;       // where it is in the descriptor
    qsizetype secondsAt = 0;
    enum Kind { Stream, End, Next, Other } kind = Other;
    int id = -1;
    bool loop = false;
};

QList<CueElement> cueElements(const QByteArray& mbx) {
    QList<CueElement> out;
    const uint32_t version = u32(mbx, 0);
    const int size = version == 6 ? 44 : version == 11 ? 48 : 0;
    if (!size) return out;
    const int flagsAt = version == 6 ? 4 : 8, secondsAt = version == 6 ? 8 : 24;
    const qsizetype from = u32(mbx, 0x1C), to = std::min<qsizetype>(u32(mbx, 0x20), mbx.size());
    for (qsizetype at = from; at + size <= to; at += size) {
        CueElement e;
        e.at = at;
        e.secondsAt = at + secondsAt;
        const uint32_t word = u32(mbx, at), flags = u32(mbx, at + flagsAt);
        if ((word >> 16) == 0x2000 && ((flags & 0xFFFF) == 0x0301 || (flags & 0xFFFF) == 0x0101)) {
            e.kind = CueElement::Stream;
            e.id = int(word & 0xFFFF);
            e.loop = !(flags & 0x10000);
        } else if (word == 0xFFFFFFFFu && flags == 7) {
            e.kind = CueElement::End;
        } else if (word == 0xFFFFFFFFu && flags == 8) {
            e.kind = CueElement::Next;
        }
        out << e;
    }
    return out;
}

float f32(const QByteArray& d, qsizetype at) {
    const uint32_t u = u32(d, at);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void putF32(QByteArray& d, qsizetype at, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    put32(d, at, u);
}

}  // namespace

QList<int> roundMusicStreams(const QByteArray& mbx, const QMap<int, double>& seconds) {
    // the cues, split at their ends
    QList<QList<CueElement>> cues(1);
    for (const CueElement& e : cueElements(mbx)) {
        if (e.kind == CueElement::Stream) cues.last() << e;
        if (e.kind == CueElement::End && !cues.last().isEmpty()) cues << QList<CueElement>();
    }
    int longest = -1;
    for (const auto& cue : cues)
        for (const CueElement& e : cue)
            if (e.loop && seconds.contains(e.id) && (longest < 0 || seconds[e.id] > seconds[longest])) longest = e.id;
    QList<int> out;
    if (longest < 0) return out;
    for (const auto& cue : cues) {
        bool round = false;
        for (const CueElement& e : cue) round = round || (e.loop && e.id == longest);
        if (!round) continue;
        for (const CueElement& e : cue)
            if (!out.contains(e.id)) out << e.id;
    }
    return out;
}

QMap<int, SongPart> streamParts(const QByteArray& mbx) {
    // A looped piece plays the loop. The pieces a cue plays once before its looped one (a chain) must hold the song's
    // intro once between them: a chain of one is that piece; in a longer chain the piece that is already some cue's
    // intro, else its last one; the chain's other pieces a moment of silence (Deception starts some rounds with a
    // short piece before the intro, Armageddon's Tekunin Prison has a chain of two whose first starts the later
    // rounds alone). A piece played once with no loop after it plays the intro and one loop in a row.
    QSet<int> looped, once;
    QList<QList<int>> chains;
    QList<CueElement> cue;
    const auto flush = [&] {
        int firstLoop = -1;
        for (int i = 0; i < cue.size() && firstLoop < 0; i++)
            if (cue[i].loop) firstLoop = i;
        QList<int> chain;
        for (int i = 0; i < cue.size(); i++) {
            if (cue[i].loop) looped.insert(cue[i].id);
            else if (firstLoop >= 0 && i < firstLoop) chain << cue[i].id;
            else once.insert(cue[i].id);
        }
        if (!chain.isEmpty()) chains << chain;
        cue.clear();
    };
    for (const CueElement& e : cueElements(mbx)) {
        if (e.kind == CueElement::Stream) cue << e;
        if (e.kind == CueElement::End) flush();  // a going-on marker (Next) keeps the cue going
    }
    flush();
    QSet<int> intro;
    for (const QList<int>& c : chains)
        if (c.size() == 1) intro.insert(c.first());
    for (const QList<int>& c : chains) {
        bool has = false;
        for (int id : c) has = has || intro.contains(id);
        if (!has) intro.insert(c.last());
    }
    QMap<int, SongPart> out;
    for (const QList<int>& c : chains)
        for (int id : c) out.insert(id, intro.contains(id) ? SongPart::Intro : SongPart::Silence);
    for (int id : once)
        if (!out.contains(id)) out.insert(id, SongPart::Once);
    for (int id : looped) out.insert(id, SongPart::Loop);
    return out;
}

QByteArray fitCueTimes(const QByteArray& mbx, const QMap<int, double>& newSeconds) {
    QByteArray out = mbx;
    double after = -1;  // the new length of the replaced stream the element before this one plays, or -1
    for (const CueElement& e : cueElements(mbx)) {
        if (e.kind == CueElement::Stream) {
            if (after >= 0) putF32(out, e.secondsAt, float(after));
            after = newSeconds.value(e.id, -1);
        } else {
            const bool timed = e.kind == CueElement::Next || (e.kind == CueElement::End && f32(mbx, e.secondsAt) >= 0);
            if (timed && after >= 0) putF32(out, e.secondsAt, float(after));
            after = -1;  // what follows a going-on marker is timed from it (0: at once)
        }
    }
    return out;
}

// --- applying a plan ------------------------------------------------------------------------------------------

namespace {

QString cacheKey(const QString& bank, const QList<StreamSong>& songs) {
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(bank.toLower().toUtf8());
    for (const StreamSong& s : songs) {
        const QFileInfo f(s.file);
        h.addData(QDir::toNativeSeparators(f.absoluteFilePath()).toLower().toUtf8());
        h.addData(QByteArray::number(f.size()));
        h.addData(QByteArray::number(f.lastModified().toSecsSinceEpoch()));
        QStringList ids;
        for (int id : s.streams) ids << QString::number(id);
        h.addData(ids.join(QLatin1Char(',')).toUtf8());
        h.addData(QByteArrayLiteral("|"));
    }
    h.addData(QByteArray::number(kMaxSongSeconds));
    h.addData(kCacheVersion);
    return QString::fromLatin1(h.result().toHex().left(16));
}

bool writeWhole(const QString& path, const QByteArray& data, QString* error) {
    QFile f(path + QStringLiteral(".tmp"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    f.close();
    QFile::remove(path);
    if (!QFile::rename(f.fileName(), path)) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

// the cache's folders the games' maps name now
QSet<QString> folderInMaps() {
    QSet<QString> out;
    const QDir dir(paths::dataDir());
    for (const QFileInfo& m : dir.entryInfoList({QStringLiteral("jukebox-*.txt")}, QDir::Files)) {
        QFile f(m.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        for (const QByteArray& raw : f.readAll().split('\n')) {
            const QStringList w = QString::fromUtf8(raw).trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (w.size() < 3 || w[0].startsWith(QLatin1Char('#'))) continue;
            const int from = w[0] == QLatin1String("arena") ? 3 : 2;
            if (w.size() > from) out.insert(QDir::cleanPath(QDir::fromNativeSeparators(w.mid(from).join(QLatin1Char(' ')))).toLower());
        }
    }
    return out;
}

// the cache kept under its budget: the folders no map names go, the longest unused first
void trimCache(const QString& cacheTop) {
    const QSet<QString> keep = folderInMaps();
    struct Folder {
        QString path;
        qint64 bytes = 0;
        QDateTime used;
    };
    QList<Folder> folders;
    qint64 total = 0;
    const QDir top(cacheTop);
    for (const QFileInfo& game : top.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        for (const QFileInfo& bank : QDir(game.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            for (const QFileInfo& copy : QDir(bank.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                Folder f;
                f.path = copy.absoluteFilePath();
                for (const QFileInfo& file : QDir(f.path).entryInfoList(QDir::Files)) {
                    f.bytes += file.size();
                    if (!f.used.isValid() || file.lastModified() > f.used) f.used = file.lastModified();
                }
                total += f.bytes;
                if (!keep.contains(QDir::cleanPath(f.path).toLower())) folders << f;
            }
    std::sort(folders.begin(), folders.end(), [](const Folder& a, const Folder& b) { return a.used < b.used; });
    for (const Folder& f : folders) {
        if (total <= kCacheBudget) break;
        if (QDir(f.path).removeRecursively()) total -= f.bytes;  // one a game has open stays (Windows keeps it)
    }
}

}  // namespace

Applier::Applier(QObject* parent) : QObject(parent) {}
Applier::~Applier() = default;

bool Applier::apply(const QString& gameId, const Plan& plan) {
    if (running_) return false;
    running_ = true;
    const QString banksDir = QDir(cfg::gameFolder(gameId)).filePath(QStringLiteral("assets/sndsxbox"));
    const QString cacheTop = QDir(libraryFolder()).filePath(QStringLiteral("cache"));
    const QString cacheRoot = QDir(cacheTop).filePath(gameId);
    const QString map = mapFile(gameId);
    QPointer<Applier> self(this);
    QThread* worker = QThread::create([self, gameId, plan, banksDir, cacheTop, cacheRoot, map] {
        QStringList mapLines;
        mapLines << QStringLiteral("# MK3D jukebox for %1, written by the launcher; the game reads it again when it changes").arg(gameId);
        QString failure;
        // a bank is made once with every song that goes into it: stages and screens that share one bank each get
        // their own streams of the one copy; picks that would give a stream two songs (stages whose music is the
        // same bank: Deadly Alliance's shared tunes, Armageddon's jukebox bank, all of its streams) get a copy each
        struct Job {
            QString bank;
            QList<StreamSong> songs;
            QStringList titles;
            QList<const Pick*> picks;
            QSet<int> streams;
            bool all = false;
        };
        QList<Job> jobs;
        int total = 0;
        for (const Pick& p : plan.picks) {
            if (p.file.isEmpty() || p.bank.isEmpty()) continue;
            QList<int> streams = p.streams;
            if (streams.isEmpty() || streams == QList<int>{-2}) {
                // the round music's streams from the descriptor's cues ({-2}), else (or when it has none this reads)
                // every stream of 12 s or more
                QMap<int, double> lengths;
                QFile in(QDir(banksDir).filePath(p.bank + QStringLiteral(".msx")));
                if (in.open(QIODevice::ReadOnly))
                    for (const auto& [id, seconds] : bankStreams(in.readAll())) lengths.insert(id, seconds);
                streams.clear();
                if (p.streams == QList<int>{-2}) {
                    QFile mbx(QDir(banksDir).filePath(p.bank + QStringLiteral(".mbx")));
                    if (mbx.open(QIODevice::ReadOnly)) streams = roundMusicStreams(mbx.readAll(), lengths);
                }
                if (streams.isEmpty())
                    for (auto it = lengths.cbegin(); it != lengths.cend(); ++it)
                        if (it.value() >= 12) streams << it.key();
            }
            if (streams.isEmpty()) {
                failure = QStringLiteral("%1 has no music stream to replace (is the game built?).").arg(p.bank);
                break;
            }
            const bool all = streams.contains(-1);
            Job* job = nullptr;
            bool present = false;
            for (Job& j : jobs) {
                if (j.bank != p.bank) continue;
                bool overlap = all || j.all;
                for (int s : streams)
                    if (j.streams.contains(s)) overlap = true;
                for (const StreamSong& s : j.songs)  // the same song on the same streams: that copy serves
                    if (s.file == p.file && s.streams == streams) present = true;
                if (present || !overlap) {
                    job = &j;
                    break;
                }
            }
            if (!job) {
                jobs << Job{p.bank, {}, {}, {}, {}, false};
                job = &jobs.last();
            }
            if (!present) {
                job->songs << StreamSong{streams, p.file};
                job->titles << p.title;
                for (int s : streams) job->streams.insert(s);
                job->all = job->all || all;
                total++;
            }
            job->picks << &p;
        }
        int done = 0;
        QSet<QString> lines;
        for (const Job& j : jobs) {
            if (!failure.isEmpty()) break;
            if (self) {
                QString what = j.titles.size() == 1 ? j.titles.first() : QStringLiteral("%1 songs for %2").arg(j.titles.size()).arg(j.bank);
                emit self->progress(done, total, what);
            }
            QString error;
            const QString original = QDir(banksDir).filePath(j.bank + QStringLiteral(".msx"));
            QFile in(original);
            if (!in.open(QIODevice::ReadOnly)) {
                failure = QStringLiteral("The game's sound bank %1 is not there (is the game built?).").arg(QDir::toNativeSeparators(original));
                break;
            }
            const QByteArray bank = in.readAll();
            const QString key = cacheKey(j.bank, j.songs);
            const QString folder = QDir(cacheRoot).filePath(j.bank + QLatin1Char('/') + key);
            const QString target = QDir(folder).filePath(j.bank + QStringLiteral(".msx"));
            const QString note = QDir(folder).filePath(QStringLiteral("track.txt"));
            if (!QFileInfo::exists(target)) {
                QDir().mkpath(folder);
                QByteArray out;
                QList<int> streamed;
                QList<double> gains;
                QMap<int, double> newSeconds;
                QList<LoopInfo> loops;
                QFile mbxIn(QDir(banksDir).filePath(j.bank + QStringLiteral(".mbx")));
                QByteArray mbx = mbxIn.open(QIODevice::ReadOnly) ? mbxIn.readAll() : QByteArray();
                const QMap<int, SongPart> parts = streamParts(mbx);
                if (!replaceStreamsMany(bank, j.songs, kMaxSongSeconds, out, &error, &streamed, &gains, &newSeconds,
                                        parts.isEmpty() ? nullptr : &parts, &loops)) {
                    failure = QStringLiteral("%1: %2").arg(j.titles.join(QStringLiteral(", ")), error);
                    break;
                }
                QFile n(note);  // what the copy holds, and how much louder each song was made
                if (n.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QString text;
                    for (int i = 0; i < j.songs.size(); i++) {
                        QStringList ids;
                        for (int id : j.songs[i].streams) ids << (id < 0 ? QStringLiteral("all") : QString::number(id));
                        text += QDir::toNativeSeparators(j.songs[i].file) + QStringLiteral("\r\n  ") + j.titles[i] +
                                QStringLiteral(" (streams %1, %2 dB, %3)\r\n")
                                    .arg(ids.join(QLatin1Char(',')))
                                    .arg(gains.value(i) >= 0 ? QStringLiteral("+%1").arg(gains.value(i), 0, 'f', 1)
                                                             : QString::number(gains.value(i), 'f', 1))
                                    .arg(describeLoop(loops.value(i)));
                    }
                    n.write(text.toUtf8());
                }
                // the descriptor goes with it: sounds made streamed say so there, and its music cues wait for each part
                // to end before they go on (the game starts a cue's next stream after the time the descriptor gives,
                // the old stream's length: a song would play over itself)
                if (!streamed.isEmpty()) mbx = setStreamed(mbx, streamed);
                if (!mbx.isEmpty()) mbx = fitCueTimes(mbx, newSeconds);
                if (!mbx.isEmpty() && !writeWhole(QDir(folder).filePath(j.bank + QStringLiteral(".mbx")), mbx, &error)) {
                    failure = error;
                    break;
                }
                if (!writeWhole(target, out, &error)) {
                    failure = error;
                    break;
                }
            }
            {
                QFile n(note);  // its date is when the copy was last used (the cache trims by it)
                if (n.open(QIODevice::ReadWrite)) n.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
            }
            const QString nativeFolder = QDir::toNativeSeparators(folder);
            for (const Pick* p : j.picks) {
                const QString line = p->stage >= 0 ? QStringLiteral("arena %1 %2 %3").arg(p->stage).arg(j.bank, nativeFolder)
                                                   : QStringLiteral("select %1 %2").arg(j.bank, nativeFolder);
                if (!lines.contains(line)) mapLines << line;
                lines.insert(line);
            }
            done += j.songs.size();
        }
        bool ok = failure.isEmpty();
        QString message;
        if (ok) {
            QString why;
            if (!winutil::writeFileAtomic(map, mapLines.join(QStringLiteral("\r\n")).toUtf8() + "\r\n", &why)) {
                ok = false;
                message = QStringLiteral("Could not write the jukebox map: %1").arg(why);
            } else {
                message = total ? QStringLiteral("%1 song%2 ready. A stage plays its song from the next fight on, a menu "
                                                 "from the next time it comes up.")
                                      .arg(total).arg(total == 1 ? QString() : QStringLiteral("s"))
                                : QStringLiteral("The game plays its own music.");
                trimCache(cacheTop);
            }
        } else {
            message = failure;
        }
        if (self) {
            QMetaObject::invokeMethod(self, [self, ok, message] {
                if (!self) return;
                self->running_ = false;
                emit self->finished(ok, message);
            }, Qt::QueuedConnection);
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return true;
}

// --- the launcher's one applier ---------------------------------------------------------------------------------

Service& Service::instance() {
    static Service* s = new Service;
    return *s;
}

Service::Service() {
    connect(&applier_, &Applier::progress, this, [this](int done, int total, const QString& what) {
        emit progress(current_, done, total, what);
    });
    connect(&applier_, &Applier::finished, this, [this](bool ok, const QString& message) {
        const QString game = current_;
        current_.clear();
        emit finished(game, ok, message);
        next();
    });
}

void Service::apply(const QString& gameId) {
    if (!queue_.contains(gameId)) queue_ << gameId;
    if (!applier_.running()) next();
}

void Service::next() {
    while (!queue_.isEmpty() && !applier_.running()) {
        const QString game = queue_.takeFirst();
        const Plan plan = resolve(game, loadConfig(game), scanLibrary(libraryFolder()));
        current_ = game;
        if (applier_.apply(game, plan)) return;
        current_.clear();
    }
}

void Service::gameStarting(const QString& gameId) {
    // a shuffle draws new songs for this time; any set is applied again, which only rewrites the map while its banks
    // are in the cache, and makes them anew when the launcher makes them differently (a new cache version: a player
    // who updates keeps no banks of the old kind)
    Config c = loadConfig(gameId);
    if (c.set.startsWith(QLatin1String("shuffle:"))) {
        c.shuffleDraw = QRandomGenerator::global()->generate() | 1u;
        saveConfig(gameId, c);
        emit configChanged(gameId);
    } else if ((c.set.isEmpty() || c.set == QLatin1String("own")) && !QFileInfo::exists(mapFile(gameId))) {
        return;  // never used: nothing to do
    }
    apply(gameId);
}

// --- the preview player -------------------------------------------------------------------------------------

struct Preview::Impl {
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    std::shared_ptr<QVector<int16_t>> pcm;  // the buffer XAudio2 reads while it plays
    QString file;
    QTimer poll;
    int generation = 0;

    bool engine() {
        if (xa) return true;
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(xa->CreateMasteringVoice(&master))) {
            xa->Release();
            xa = nullptr;
            return false;
        }
        return true;
    }
    void dropVoice() {
        if (voice) {
            voice->Stop();
            voice->FlushSourceBuffers();
            voice->DestroyVoice();
            voice = nullptr;
        }
        pcm.reset();
    }
    bool queued() const {
        if (!voice) return false;
        XAUDIO2_VOICE_STATE st{};
        voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return st.BuffersQueued > 0;
    }
};

Preview::Preview(QObject* parent) : QObject(parent), impl_(new Impl) {
    impl_->poll.setInterval(250);
    connect(&impl_->poll, &QTimer::timeout, this, [this] {
        if (!impl_->queued()) {
            impl_->poll.stop();
            const QString f = impl_->file;
            impl_->dropVoice();
            impl_->file.clear();
            emit stateChanged(false, f);
        }
    });
}

Preview::~Preview() {
    impl_->dropVoice();
    if (impl_->master) impl_->master->DestroyVoice();
    if (impl_->xa) impl_->xa->Release();
    delete impl_;
}

bool Preview::playing() const { return impl_->queued(); }
QString Preview::file() const { return impl_->file; }

void Preview::stop() {
    impl_->poll.stop();
    const QString f = impl_->file;
    impl_->dropVoice();
    impl_->file.clear();
    impl_->generation++;
    if (!f.isEmpty()) emit stateChanged(false, f);
}

void Preview::play(const QString& file) {
    stop();
    if (!impl_->engine()) return;
    impl_->file = file;
    const int gen = ++impl_->generation;
    emit stateChanged(true, file);
    QPointer<Preview> self(this);
    QThread* worker = QThread::create([self, file, gen] {
        auto pcm = std::make_shared<QVector<int16_t>>();
        int rate = 0, channels = 0;
        QString error;
        const bool ok = decodeAudio(file, rate, channels, *pcm, &error, 90);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, ok, rate, channels, pcm] {
            if (!self || self->impl_->generation != gen) return;
            if (!ok) {
                const QString f = self->impl_->file;
                self->impl_->file.clear();
                emit self->stateChanged(false, f);
                return;
            }
            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = WORD(channels);
            wf.nSamplesPerSec = DWORD(rate);
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = WORD(channels * 2);
            wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
            if (FAILED(self->impl_->xa->CreateSourceVoice(&self->impl_->voice, &wf))) {
                const QString f = self->impl_->file;
                self->impl_->file.clear();
                emit self->stateChanged(false, f);
                return;
            }
            self->impl_->pcm = pcm;
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = UINT32(pcm->size() * 2);
            xb.pAudioData = reinterpret_cast<const BYTE*>(pcm->constData());
            xb.Flags = XAUDIO2_END_OF_STREAM;
            self->impl_->voice->SubmitSourceBuffer(&xb);
            self->impl_->voice->SetVolume(0.8f);
            self->impl_->voice->Start();
            self->impl_->poll.start();
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

}  // namespace jukebox
