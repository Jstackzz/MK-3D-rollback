#pragma once
// The jukebox: a soundtrack of another Mortal Kombat for a game's stages and select screen, or a song per stage,
// as Tekken 8 has it. The player's music lives in a folder (soundtracks as subfolders of audio files, each with an
// optional soundtrack.json naming its tracks' kinds and keywords); the launcher re-encodes the chosen tracks into
// copies of the game's own sound banks (sndsxbox\<bank>.msx, Xbox ADPCM) in a cache, and writes a map file the game
// reads (recomp/common/src/mk3d/jukebox.h): which bank to open instead of its own for which arena. Nothing in the
// simulation changes, so online matches and watched lobbies are untouched: each player hears their own choice.

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace jukebox {

struct Track {
    QString file;      // full path
    QString title;     // "Dead Pool"
    QString kind;      // stage, select, menu, stinger, theme, variant, other
    QStringList tags;  // lower case keywords
    double seconds = 0;
};

struct Soundtrack {
    QString id;      // the folder's name
    QString title;   // "Mortal Kombat II (arcade)"
    QString folder;  // full path
    QList<Track> tracks;
    int select = -1;  // the select screen's track, an index into tracks, -1 none
};

// a stage of one of the three games, and the music bank a fight there loads
struct Stage {
    int id = -1;          // the game's own arena id (what the game's jukebox adapter reports)
    QString name;         // "Acid Bath"
    QString bank;         // "mk5tune1"; empty while unknown (the game's own music stays)
    QList<int> streams;   // the bank's streams the music is in (every long one); empty: all of 12 s or more
    QStringList tags;     // keywords for the automatic picks
};

struct GameMusic {
    QString game;
    QList<Stage> stages;
    QString selectBank;         // the front end's bank holding the select screen's music
    QList<int> selectStreams;   // ... and the stream(s) that are it
};

// a slot's choice: "auto" (from the soundtrack), "own" (the game's own music) or "<soundtrack id>/<file name>"
struct Config {
    QString soundtrack;  // the soundtrack id, empty for the game's own music everywhere
    QString select = QStringLiteral("auto");
    QMap<int, QString> stages;  // by arena id; missing: auto
};

// what the plan ends up playing where
struct Pick {
    int stage = -1;  // -1: the select screen
    QString bank;
    QList<int> streams;
    QString soundtrack;  // empty: the game's own
    QString file;        // the track (full path)
    QString title;
};
struct Plan {
    QList<Pick> stages;
    Pick select;
    QStringList notes;  // what could not be done as asked, for the page
};

const QStringList& audioExtensions();
QString libraryFolder();                       // cfg jukebox/folder, default Music\MK3D Jukebox
void setLibraryFolder(const QString& folder);
QList<Soundtrack> scanLibrary(const QString& folder);
const Soundtrack* find(const QList<Soundtrack>& library, const QString& id);
const GameMusic* gameMusic(const QString& gameId);  // from data/jukebox.json; null for an unknown game

Config loadConfig(const QString& gameId);
void saveConfig(const QString& gameId, const Config& config);
QString mapFile(const QString& gameId);  // jukebox-<game>.txt beside the launcher's settings: the game's MK3D_JUKEBOX

Plan resolve(const QString& gameId, const Config& config, const QList<Soundtrack>& library);
QString describeChoice(const QString& choice, const QList<Soundtrack>& library);

// --- the transcoder, usable on any thread ---------------------------------------------------------------------

// the file decoded by Media Foundation to 16-bit PCM at its own rate and channels; at most maxSeconds of it
bool decodeAudio(const QString& path, int& rate, int& channels, QVector<int16_t>& pcm, QString* error,
                 double maxSeconds = 0);
// interleaved 16-bit PCM to another rate and channel count (windowed sinc)
QVector<int16_t> convertPcm(const QVector<int16_t>& pcm, int channels, int rate, int outChannels, int outRate);
// interleaved PCM to Xbox ADPCM blocks (36 bytes a channel for 64 frames; the tail that fills no block is dropped)
QByteArray encodeXboxAdpcm(const QVector<int16_t>& pcm, int channels);
// a sound bank (.msx) with the given streams replaced by the same track, re-encoded in each stream's own format;
// false with why when the bank cannot be read or a stream is not there
bool replaceStreams(const QByteArray& bank, const QList<int>& streams, const QString& track, double maxSeconds,
                    QByteArray& out, QString* error);
// the streams of a bank: (id, seconds) for every streamed sound
QList<QPair<int, double>> bankStreams(const QByteArray& bank);

// --- applying a plan: the cache and the map file --------------------------------------------------------------

// Runs a plan on a worker thread: each pick's bank is made once (the cache keyed by the track, the bank and the
// streams), then the map file is written. Progress and the end are signalled on the thread that made the Applier.
class Applier : public QObject {
    Q_OBJECT
public:
    explicit Applier(QObject* parent = nullptr);
    ~Applier() override;
    // false when one is already running
    bool apply(const QString& gameId, const Plan& plan);
    bool running() const { return running_; }

signals:
    void progress(int done, int total, const QString& what);
    void finished(bool ok, const QString& message);

private:
    bool running_ = false;
};

// a track played on this PC (the page's preview): decoded on a worker thread, then through XAudio2; one at a time
class Preview : public QObject {
    Q_OBJECT
public:
    explicit Preview(QObject* parent = nullptr);
    ~Preview() override;
    void play(const QString& file);
    void stop();
    bool playing() const;

signals:
    void stateChanged(bool playing, const QString& file);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace jukebox
