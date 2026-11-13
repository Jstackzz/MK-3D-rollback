#pragma once
// The jukebox, as Tekken 8 has it: a game plays a music set, either its own music, a soundtrack of another Mortal
// Kombat (its preset songs per stage, or shuffled), every soundtrack shuffled, or one of three playlists where the
// player picks a song from any soundtrack for each stage and each menu screen. The player's music lives in a folder
// (soundtracks as subfolders of audio files, each with an optional soundtrack.json naming its tracks' kinds and
// keywords); the launcher re-encodes the chosen tracks into copies of the game's own sound banks
// (sndsxbox\<bank>.msx, Xbox ADPCM) in a cache, and writes a map file the game reads
// (recomp/common/src/mk3d/jukebox.h): which bank to open instead of its own, for which arena or for the front end.
// Nothing in the simulation changes, so online matches and watched lobbies are untouched: each player hears their
// own choice.

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
    QString shortTitle;  // "MK II", for the game tabs
    QString folder;  // full path
    QString cover;   // a picture in the folder (cover.jpg / cover.png / folder.jpg), empty for none
    double year = 0;  // when the game came out (the order of the soundtracks), 0 unknown
    QList<Track> tracks;
    int select = -1;  // the select screen's track, an index into tracks, -1 none
};

// a place of a game the jukebox sets the music of: a stage, or a screen of the front end
struct Slot {
    QString key;          // "stage:12", "screen:menu"
    int stage = -1;       // the game's own arena id (what its jukebox adapter reports); -1 for a screen
    QString screen;       // "menu", "select"; empty for a stage
    QString name;         // "Acid Bath", "Main menu"
    QString bank;         // the sound bank its music is in; empty: cannot be changed (the game's own stays)
    QList<int> streams;   // the bank's streams that are its music; empty: every stream of 12 s or more; {-1}: all;
                          // {-2}: the round music's cues in the bank's descriptor (roundMusicStreams)
    QStringList tags;     // keywords for the preset picks
    QString why;          // why it cannot be changed, when it cannot
    bool isStage() const { return stage >= 0; }
};

struct GameMusic {
    QString game;
    QList<Slot> places;  // the stages, then the screens
    const Slot* slot(const QString& key) const;
};

constexpr int kPlaylists = 3;

// what a game plays: set is "own", "preset:<soundtrack>", "shuffle:<soundtrack>", "shuffle:*" or "playlist:<1..3>";
// a playlist maps slot keys to "<soundtrack>/<file>" (missing: the game's own)
struct Config {
    QString set = QStringLiteral("own");
    QMap<QString, QString> playlists[kPlaylists];
    quint32 shuffleDraw = 1;  // the current shuffle (a new one each time the game starts)
    int playlistIndex() const;  // 0..2 for a playlist set, else -1
    QString setSoundtrack() const;  // the soundtrack of a preset or shuffle set ("*" for all), else empty
};

// what the plan ends up playing where
struct Pick {
    QString slot;   // the slot's key
    int stage = -1;
    QString screen;
    QString bank;
    QList<int> streams;
    QString soundtrack;  // empty: the game's own
    QString file;        // the track (full path)
    QString title;
};
struct Plan {
    QList<Pick> picks;  // one a slot, in the game's slot order
    QStringList notes;  // what could not be done as asked, for the page
    const Pick* pick(const QString& slot) const;
};

const QStringList& audioExtensions();
QString libraryFolder();                       // cfg jukebox/folder, default Music\MK3D Jukebox
void setLibraryFolder(const QString& folder);
QList<Soundtrack> scanLibrary(const QString& folder);
const Soundtrack* find(const QList<Soundtrack>& library, const QString& id);
const Track* trackByChoice(const QString& choice, const QList<Soundtrack>& library, QString* soundtrackId = nullptr);
QString choiceOf(const Soundtrack& s, const Track& t);  // "<soundtrack>/<file>"
const GameMusic* gameMusic(const QString& gameId);  // from data/jukebox.json; null for an unknown game

Config loadConfig(const QString& gameId);
void saveConfig(const QString& gameId, const Config& config);
QString mapFile(const QString& gameId);  // jukebox-<game>.txt beside the launcher's settings: the game's MK3D_JUKEBOX

// slot key -> choice: what a soundtrack's preset plays where (the stages' best fits by name, its select-screen track,
// its menu theme)
QMap<QString, QString> presetChoices(const QString& gameId, const Soundtrack& soundtrack);
Plan resolve(const QString& gameId, const Config& config, const QList<Soundtrack>& library);
QString describeSet(const QString& set, const QList<Soundtrack>& library);

// --- the transcoder, usable on any thread ---------------------------------------------------------------------

// the file decoded by Media Foundation to 16-bit PCM at its own rate and channels; at most maxSeconds of it
bool decodeAudio(const QString& path, int& rate, int& channels, QVector<int16_t>& pcm, QString* error,
                 double maxSeconds = 0);
// interleaved 16-bit PCM to another rate and channel count (windowed sinc)
QVector<int16_t> convertPcm(const QVector<int16_t>& pcm, int channels, int rate, int outChannels, int outRate);
// interleaved PCM to Xbox ADPCM blocks (36 bytes a channel for 64 frames; the tail that fills no block is dropped)
QByteArray encodeXboxAdpcm(const QVector<int16_t>& pcm, int channels);

// a song for some streams of a bank ({-1}: every stream)
struct StreamSong {
    QList<int> streams;
    QString file;
};
// the bank with every song's streams replaced by that song, re-encoded in each stream's format and cut at maxSeconds;
// the streams a song fills share one copy of it. Each song is made as loud as the longest stream it replaces (BS.1770
// loudness, a limiter on its peaks); gainsDb gets what each song was changed by. A resident sound (one the game keeps
// in memory, loaded whole at boot, with no room to grow) that is replaced becomes a streamed one: its id goes to
// streamedIds, for the .mbx (setStreamed); newSeconds gets every replaced stream's new length (for fitCueTimes). False
// with why when the bank cannot be read or a stream is not there.
bool replaceStreamsMany(const QByteArray& bank, const QList<StreamSong>& songs, double maxSeconds, QByteArray& out,
                        QString* error, QList<int>* streamedIds = nullptr, QList<double>* gainsDb = nullptr,
                        QMap<int, double>* newSeconds = nullptr);
// a bank's descriptor (.mbx) with the given sounds marked streamed
QByteArray setStreamed(const QByteArray& mbx, const QList<int>& ids);
// A descriptor's music cues string streams together: each element plays a stream once or looped, a given number of
// seconds after the element before it started (the stream's own length, as the descriptor has it), and a cue ends at
// its end element (after some seconds, or never: its last stream loops). The streams of a stage's round music: every
// stream of the cues that loop the bank's longest looping stream (the round's intro and its loop, a pre-intro too;
// not the "finish him" loop or the round-end sting). seconds: the bank's streams' lengths (bankStreams). Empty when
// the descriptor has no cues this reads (Deadly Alliance's version 6 and Deception's and Armageddon's version 11 do).
QList<int> roundMusicStreams(const QByteArray& mbx, const QMap<int, double>& seconds);
// the descriptor with its cues fitted to replaced streams: an element after a replaced stream starts once that stream's
// new length has passed, and a cue that ends after one ends then, so a song never plays over itself
QByteArray fitCueTimes(const QByteArray& mbx, const QMap<int, double>& newSeconds);
// the streams of a bank: (id, seconds) for every sound that is not resident
QList<QPair<int, double>> bankStreams(const QByteArray& bank);

// --- applying a plan: the cache and the map file --------------------------------------------------------------

// Runs a plan on a worker thread: each bank is made once with every song that goes into it (the cache keyed by the
// bank and its songs), then the map file is written and the cache trimmed. Progress and the end are signalled on the
// thread that made the Applier.
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

// The one applier of the launcher: the page's changes and a shuffle drawn again when a game starts. Applies the
// newest plan of a game once the one running is done.
class Service : public QObject {
    Q_OBJECT
public:
    static Service& instance();
    void apply(const QString& gameId);  // the saved settings' plan, now or once the running one is done
    // a game starts: a shuffle set draws new songs, and any set is applied again (cached banks: only the map is
    // written; banks the launcher now makes differently are made anew); the map follows a moment later
    void gameStarting(const QString& gameId);
    bool running() const { return applier_.running(); }

signals:
    void progress(const QString& gameId, int done, int total, const QString& what);
    void finished(const QString& gameId, bool ok, const QString& message);
    void configChanged(const QString& gameId);  // a new shuffle was drawn

private:
    Service();
    void next();
    Applier applier_;
    QString current_;
    QStringList queue_;
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
    QString file() const;

signals:
    void stateChanged(bool playing, const QString& file);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace jukebox
