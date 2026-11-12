// The jukebox (Jukebox.h): the music library, the plan, the transcoder into the games' sound banks, the cache and
// the map file, and the preview player.
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
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
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
constexpr char kCacheVersion[] = "v2";   // in the cache key: bump when the encoding changes

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

void readSoundtrackJson(const QByteArray& json, Soundtrack& s, const QDir& dir) {
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.contains(QStringLiteral("title"))) s.title = o.value(QStringLiteral("title")).toString();
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
        out << s;
    }
    return out;
}

const Soundtrack* find(const QList<Soundtrack>& library, const QString& id) {
    for (const Soundtrack& s : library)
        if (s.id.compare(id, Qt::CaseInsensitive) == 0) return &s;
    return nullptr;
}

// --- the games' stages --------------------------------------------------------------------------------------

namespace {

QMap<QString, GameMusic> loadGameMusic() {
    QMap<QString, GameMusic> out;
    QFile f(QStringLiteral(":/data/jukebox.json"));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject games = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("games")).toObject();
    for (auto it = games.begin(); it != games.end(); ++it) {
        const QJsonObject g = it.value().toObject();
        GameMusic gm;
        gm.game = it.key();
        gm.selectBank = g.value(QStringLiteral("select_bank")).toString();
        for (const QJsonValue& v : g.value(QStringLiteral("select_streams")).toArray()) gm.selectStreams << v.toInt();
        for (const QJsonValue& v : g.value(QStringLiteral("stages")).toArray()) {
            const QJsonObject o = v.toObject();
            Stage s;
            s.id = o.value(QStringLiteral("id")).toInt(-1);
            s.name = o.value(QStringLiteral("name")).toString();
            s.bank = o.value(QStringLiteral("bank")).toString();
            for (const QJsonValue& st : o.value(QStringLiteral("streams")).toArray()) s.streams << st.toInt();
            for (const QJsonValue& t : o.value(QStringLiteral("tags")).toArray()) s.tags << lower(t.toString());
            gm.stages << s;
        }
        out.insert(gm.game, gm);
    }
    return out;
}

}  // namespace

const GameMusic* gameMusic(const QString& gameId) {
    static const QMap<QString, GameMusic> all = loadGameMusic();
    auto it = all.find(gameId);
    return it == all.end() ? nullptr : &it.value();
}

// --- the configuration --------------------------------------------------------------------------------------

Config loadConfig(const QString& gameId) {
    Config c;
    const QString p = QStringLiteral("jukebox/%1/").arg(gameId);
    c.soundtrack = cfg::get(p + QStringLiteral("soundtrack")).toString();
    c.select = cfg::get(p + QStringLiteral("select"), QStringLiteral("auto")).toString();
    if (const GameMusic* gm = gameMusic(gameId))
        for (const Stage& s : gm->stages) {
            const QString v = cfg::get(p + QStringLiteral("stage_%1").arg(s.id)).toString();
            if (!v.isEmpty() && v != QLatin1String("auto")) c.stages.insert(s.id, v);
        }
    return c;
}

void saveConfig(const QString& gameId, const Config& c) {
    const QString p = QStringLiteral("jukebox/%1/").arg(gameId);
    cfg::set(p + QStringLiteral("soundtrack"), c.soundtrack);
    cfg::set(p + QStringLiteral("select"), c.select);
    if (const GameMusic* gm = gameMusic(gameId))
        for (const Stage& s : gm->stages)
            cfg::set(p + QStringLiteral("stage_%1").arg(s.id), c.stages.value(s.id, QStringLiteral("auto")));
}

QString mapFile(const QString& gameId) {
    return QDir(paths::dataDir()).filePath(QStringLiteral("jukebox-%1.txt").arg(gameId));
}

// --- the plan -------------------------------------------------------------------------------------------------

namespace {

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

bool stageMaterial(const Track& t) {
    if (t.seconds > 0 && t.seconds < kStingerSeconds) return false;
    return t.kind == QLatin1String("stage") || t.kind == QLatin1String("other");
}

int score(const Stage& stage, const Track& track) {
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

// the soundtrack's tracks for the stages left on auto: the best keyword match each, every track once while they
// last, then round the stage tracks again in their order
QMap<int, const Track*> autoPicks(const GameMusic& gm, const QList<int>& autoStages, const Soundtrack& s) {
    QList<const Track*> pool;
    for (const Track& t : s.tracks)
        if (stageMaterial(t)) pool << &t;
    if (pool.isEmpty())  // a soundtrack of themes only: anything long enough
        for (const Track& t : s.tracks)
            if (t.kind != QLatin1String("stinger") && (t.seconds <= 0 || t.seconds >= kStingerSeconds)) pool << &t;
    QMap<int, const Track*> out;
    if (pool.isEmpty()) return out;
    struct Cand {
        int stage;
        const Track* track;
        int score;
    };
    QList<Cand> cands;
    for (int id : autoStages) {
        const Stage* st = nullptr;
        for (const Stage& x : gm.stages)
            if (x.id == id) st = &x;
        if (!st) continue;
        for (const Track* t : pool) {
            const int sc = score(*st, *t);
            if (sc > 0) cands.append({id, t, sc});
        }
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    QSet<const Track*> used;
    for (const Cand& c : cands) {
        if (out.contains(c.stage) || used.contains(c.track)) continue;
        out.insert(c.stage, c.track);
        used.insert(c.track);
    }
    // the rest: unused tracks first, in the soundtrack's order, then around again
    QList<const Track*> rest;
    for (const Track* t : pool)
        if (!used.contains(t)) rest << t;
    int k = 0;
    for (int id : autoStages) {
        if (out.contains(id)) continue;
        const Track* t = rest.isEmpty() ? pool[k % pool.size()] : rest[k % rest.size()];
        out.insert(id, t);
        k++;
    }
    return out;
}

}  // namespace

QString describeChoice(const QString& choice, const QList<Soundtrack>& library) {
    if (choice.isEmpty() || choice == QLatin1String("auto")) return QStringLiteral("Auto");
    if (choice == QLatin1String("own")) return QStringLiteral("The game's own");
    QString sid;
    if (const Track* t = trackByChoice(choice, library, &sid)) {
        const Soundtrack* s = find(library, sid);
        return QStringLiteral("%1: %2").arg(s ? s->title : sid, t->title);
    }
    return QStringLiteral("%1 (missing)").arg(choice);
}

Plan resolve(const QString& gameId, const Config& config, const QList<Soundtrack>& library) {
    Plan plan;
    const GameMusic* gm = gameMusic(gameId);
    if (!gm) return plan;
    const Soundtrack* chosen = config.soundtrack.isEmpty() ? nullptr : find(library, config.soundtrack);
    if (!config.soundtrack.isEmpty() && !chosen)
        plan.notes << QStringLiteral("The soundtrack \"%1\" is not in the music folder: the game's own music plays where nothing else is picked.").arg(config.soundtrack);

    QList<int> autoStages;
    for (const Stage& s : gm->stages) {
        const QString choice = config.stages.value(s.id, QStringLiteral("auto"));
        if (choice == QLatin1String("auto") && chosen) autoStages << s.id;
    }
    const QMap<int, const Track*> autos = chosen ? autoPicks(*gm, autoStages, *chosen) : QMap<int, const Track*>();

    for (const Stage& s : gm->stages) {
        Pick p;
        p.stage = s.id;
        p.bank = s.bank;
        p.streams = s.streams;
        const QString choice = config.stages.value(s.id, QStringLiteral("auto"));
        const Track* t = nullptr;
        QString sid;
        if (choice == QLatin1String("own")) {
            t = nullptr;
        } else if (choice == QLatin1String("auto")) {
            t = autos.value(s.id, nullptr);
            sid = chosen ? chosen->id : QString();
        } else {
            t = trackByChoice(choice, library, &sid);
            if (!t) plan.notes << QStringLiteral("%1: the song \"%2\" is not in the music folder any more; the game's own plays.").arg(s.name, choice);
        }
        if (t && s.bank.isEmpty()) {
            plan.notes << QStringLiteral("%1: its music cannot be changed yet (the bank it plays from is not known); the game's own plays.").arg(s.name);
            t = nullptr;
        }
        if (t) {
            p.soundtrack = sid;
            p.file = t->file;
            p.title = t->title;
        }
        plan.stages << p;
    }

    Pick sel;
    sel.stage = -1;
    sel.bank = gm->selectBank;
    sel.streams = gm->selectStreams;
    const Track* st = nullptr;
    QString sid;
    if (config.select == QLatin1String("own")) {
        st = nullptr;
    } else if (config.select == QLatin1String("auto")) {
        if (chosen) {
            if (chosen->select >= 0) {
                st = &chosen->tracks[chosen->select];
                sid = chosen->id;
            } else {
                plan.notes << QStringLiteral("%1 has no select-screen track in your folder, so the game's own select music plays (pick one by hand if you like).").arg(chosen->title);
            }
        }
    } else {
        st = trackByChoice(config.select, library, &sid);
        if (!st) plan.notes << QStringLiteral("Select screen: the song \"%1\" is not in the music folder any more; the game's own plays.").arg(config.select);
    }
    if (st && (sel.bank.isEmpty() || sel.streams.isEmpty())) {
        plan.notes << QStringLiteral("The select screen's music cannot be changed yet for this game (its stream is not known); the game's own plays.");
        st = nullptr;
    }
    if (st) {
        sel.soundtrack = sid;
        sel.file = st->file;
        sel.title = st->title;
    }
    plan.select = sel;
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

struct Bank {
    int version = 0;
    QList<BankEntry> entries;
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
            if (!readFormat(d, e)) {
                if (error) *error = QStringLiteral("stream %1 has no RIFF header").arg(e.id);
                return false;
            }
            b.entries << e;
        }
        return true;
    }
    if (b.version == 2) {
        const int n = int(u32(d, 4)) - 1;
        const uint32_t residentAt = u32(d, 0x14) ? 0x800 : 0;
        for (int i = 0; i < n && i < 1024; i++) {
            BankEntry e;
            e.index = i;
            const qsizetype at = 0x34 + 24 * i;
            e.id = int(u32(d, at) & 0xFFFF);
            e.off = u32(d, at + 4);
            e.size = u32(d, at + 8);
            e.hdr = u32(d, at + 12);
            e.resident = u32(d, at + 20) != 0;
            e.data = e.resident ? e.off + residentAt : e.off;
            e.dataSize = e.size;
            if (!readFormat(d, e)) {
                if (error) *error = QStringLiteral("stream %1 has no RIFF header").arg(e.id);
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

bool replaceStreams(const QByteArray& bank, const QList<int>& streams, const QString& track, double maxSeconds,
                    QByteArray& out, QString* error) {
    Bank b;
    if (!parseBank(bank, b, error)) return false;
    QList<BankEntry*> targets;
    for (int id : streams) {
        BankEntry* found = nullptr;
        for (BankEntry& e : b.entries)
            if (e.id == id) found = &e;
        if (!found || found->tag != 0x69) {
            if (error) *error = QStringLiteral("stream %1 is not a music stream of the bank").arg(id);
            return false;
        }
        targets << found;
    }
    int rate = 0, channels = 0;
    QVector<int16_t> pcm;
    if (!decodeAudio(track, rate, channels, pcm, error, maxSeconds)) return false;
    QMap<QPair<int, int>, QByteArray> encoded;  // (channels, rate) -> blocks
    for (BankEntry* e : targets) {
        const QPair<int, int> key(e->channels, e->rate);
        if (encoded.contains(key)) continue;
        const QVector<int16_t> conv = convertPcm(pcm, channels, rate, e->channels, e->rate);
        encoded.insert(key, encodeXboxAdpcm(conv, e->channels));
    }
    const qsizetype align = 0x800;
    qsizetype headerEnd = 0;
    if (b.version == 1) {
        headerEnd = bank.size();
        for (const BankEntry& e : b.entries) headerEnd = std::min<qsizetype>(headerEnd, e.off);
    } else {
        headerEnd = 0x800;
    }
    out = bank.left(headerEnd);
    if (b.version == 2) {
        // the resident block (the sounds kept in memory, their offsets from its start) laid out again in case one of
        // them is replaced (Armageddon's select-screen music is one); its size at [0x14] follows
        qsizetype rpos = 0;
        bool any = false;
        for (BankEntry& e : b.entries) {
            if (!e.resident) continue;
            any = true;
            rpos = (rpos + 3) / 4 * 4;
            QByteArray blob;
            if (targets.contains(&e)) {
                blob = encoded[qMakePair(e.channels, e.rate)];
                const QByteArray h = xboxHeader(e.channels, e.rate, uint32_t(blob.size()), false);
                std::memcpy(out.data() + e.hdr, h.constData(), size_t(h.size()));
                e.size = uint32_t(blob.size());
            } else {
                blob = bank.mid(e.data, e.dataSize);
            }
            out.append(QByteArray(int(0x800 + rpos - out.size()), '\0'));
            out.append(blob);
            e.off = uint32_t(rpos);
            rpos += blob.size();
        }
        if (any) put32(out, 0x14, uint32_t(out.size() - 0x800));
    }
    qsizetype pos = out.size();
    for (BankEntry& e : b.entries) {
        if (e.resident) continue;
        pos = (pos + align - 1) / align * align;
        out.append(QByteArray(int(pos - out.size()), '\0'));
        const bool target = targets.contains(&e);
        QByteArray blob;
        if (target) {
            const QByteArray& blocks = encoded[qMakePair(e.channels, e.rate)];
            if (b.version == 1) {
                blob = xboxHeader(e.channels, e.rate, uint32_t(blocks.size()), true) + blocks;
            } else {
                blob = blocks;
                const QByteArray h = xboxHeader(e.channels, e.rate, uint32_t(blocks.size()), false);
                std::memcpy(out.data() + e.hdr, h.constData(), size_t(h.size()));
            }
            e.size = uint32_t(blob.size());
        } else {
            blob = bank.mid(e.off, e.size);
        }
        e.off = uint32_t(pos);
        out.append(blob);
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

// --- applying a plan ------------------------------------------------------------------------------------------

namespace {

QString cacheKey(const Pick& p, const QList<int>& streams) {
    const QFileInfo f(p.file);
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QDir::toNativeSeparators(f.absoluteFilePath()).toLower().toUtf8());
    h.addData(QByteArray::number(f.size()));
    h.addData(QByteArray::number(f.lastModified().toSecsSinceEpoch()));
    h.addData(p.bank.toLower().toUtf8());
    QStringList ids;
    for (int s : streams) ids << QString::number(s);
    h.addData(ids.join(QLatin1Char(',')).toUtf8());
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

}  // namespace

Applier::Applier(QObject* parent) : QObject(parent) {}
Applier::~Applier() = default;

bool Applier::apply(const QString& gameId, const Plan& plan) {
    if (running_) return false;
    running_ = true;
    const QString banksDir = QDir(cfg::gameFolder(gameId)).filePath(QStringLiteral("assets/sndsxbox"));
    const QString cacheRoot = QDir(libraryFolder()).filePath(QStringLiteral("cache/%1").arg(gameId));
    const QString map = mapFile(gameId);
    QPointer<Applier> self(this);
    QThread* worker = QThread::create([self, gameId, plan, banksDir, cacheRoot, map] {
        QList<Pick> picks = plan.stages;
        picks << plan.select;
        QStringList mapLines;
        mapLines << QStringLiteral("# MK3D jukebox for %1, written by the launcher; the game reads it again when it changes").arg(gameId);
        int total = 0;
        for (const Pick& p : picks)
            if (!p.file.isEmpty()) total++;
        int done = 0;
        QString failure;
        QMap<QString, QString> made;  // "<bank>/<key>" -> folder, so two stages sharing a song and a bank share a folder
        for (const Pick& p : picks) {
            if (p.file.isEmpty() || p.bank.isEmpty()) continue;
            if (self) emit self->progress(done, total, p.stage < 0 ? QStringLiteral("select screen: %1").arg(p.title) : p.title);
            QString error;
            const QString original = QDir(banksDir).filePath(p.bank + QStringLiteral(".msx"));
            QFile in(original);
            if (!in.open(QIODevice::ReadOnly)) {
                failure = QStringLiteral("The game's sound bank %1 is not there (is the game built?).").arg(QDir::toNativeSeparators(original));
                break;
            }
            const QByteArray bank = in.readAll();
            QList<int> streams = p.streams;
            if (streams.isEmpty()) {
                const GameMusic* gm = gameMusic(gameId);
                const double minSeconds = 12;
                for (const auto& [id, seconds] : bankStreams(bank))
                    if (seconds >= minSeconds) streams << id;
                (void)gm;
            }
            if (streams.isEmpty()) {
                failure = QStringLiteral("%1 has no music stream to replace.").arg(p.bank);
                break;
            }
            const QString key = cacheKey(p, streams);
            const QString folder = QDir(cacheRoot).filePath(p.bank + QLatin1Char('/') + key);
            const QString target = QDir(folder).filePath(p.bank + QStringLiteral(".msx"));
            if (!QFileInfo::exists(target)) {
                QDir().mkpath(folder);
                QByteArray out;
                if (!replaceStreams(bank, streams, p.file, kMaxSongSeconds, out, &error)) {
                    failure = QStringLiteral("%1: %2").arg(p.title, error);
                    break;
                }
                if (!writeWhole(target, out, &error)) {
                    failure = error;
                    break;
                }
                const QString mbx = QDir(banksDir).filePath(p.bank + QStringLiteral(".mbx"));
                QFile::remove(QDir(folder).filePath(p.bank + QStringLiteral(".mbx")));
                QFile::copy(mbx, QDir(folder).filePath(p.bank + QStringLiteral(".mbx")));
                QFile note(QDir(folder).filePath(QStringLiteral("track.txt")));
                if (note.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    note.write((QDir::toNativeSeparators(p.file) + QStringLiteral("\r\n") + p.title + QStringLiteral("\r\n")).toUtf8());
            }
            made.insert(p.bank + QLatin1Char('/') + key, folder);
            const QString nativeFolder = QDir::toNativeSeparators(folder);
            if (p.stage < 0) mapLines << QStringLiteral("select %1 %2").arg(p.bank, nativeFolder);
            else mapLines << QStringLiteral("arena %1 %2 %3").arg(p.stage).arg(p.bank, nativeFolder);
            done++;
        }
        bool ok = failure.isEmpty();
        QString message;
        if (ok) {
            QString why;
            if (!winutil::writeFileAtomic(map, mapLines.join(QStringLiteral("\r\n")).toUtf8() + "\r\n", &why)) {
                ok = false;
                message = QStringLiteral("Could not write the jukebox map: %1").arg(why);
            } else {
                message = total ? QStringLiteral("%1 song%2 ready. A stage plays its song from the next fight on; the select "
                                                 "screen from the next time the game starts.")
                                      .arg(total).arg(total == 1 ? QString() : QStringLiteral("s"))
                                : QStringLiteral("The game plays its own music.");
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
