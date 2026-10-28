#pragma once

#include <functional>
// Drives one player's online match from queue_join to match_result (docs/PROTOCOL.md 1.7-1.9,
// section 2) and owns the game processes: the prelaunched one, single player, and the local test pair.

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class ServerClient;
class UdpRendezvous;
class GameProcess;
class FeedLink;
class TunnelBridge;

struct QueueRequest {
    QString game;
    QString mode;
    QString region;
    QString character;
    bool ranked = false;
    bool strict = false;
    int maxPing = 0;
    int delay = 2;
    bool deathTraps = true;  // a match has them only when both players want them
};

class MatchController : public QObject {
    Q_OBJECT
public:
    // Finishing: the result is in, the game still plays the match's ending (finisher, winner's pose) until it is back
    // at its hold point; no new match meanwhile, the game still holds the game port. Watching: the game plays a lobby
    // match this player watches, from both players' feeds (docs/PROTOCOL.md 1.10.1), then finishes as above.
    enum class Phase { Idle, Searching, Found, Connecting, Starting, Playing, Reporting, Finishing, Solo, LocalTest,
                       Watching };
    Q_ENUM(Phase)

    explicit MatchController(ServerClient* server, QObject* parent = nullptr);
    ~MatchController() override;

    Phase phase() const { return phase_; }
    // a match on its way, played or watched: the game has the player's controller
    bool inMatch() const {
        return phase_ == Phase::Connecting || phase_ == Phase::Starting || phase_ == Phase::Playing ||
               phase_ == Phase::Reporting || phase_ == Phase::Finishing || phase_ == Phase::Watching;
    }
    QString phaseText() const { return phaseText_; }
    QueueRequest lastRequest() const { return request_; }
    QJsonObject currentFound() const { return found_; }
    int searchSeconds() const;
    // the max ping the server raised this search's to, waiting for a match (0: not raised): the rule it gave with
    // `queued`, from the time searched
    int raisedMaxPing() const;
    // this PC's round trip to the server, ms (the rendezvous' UDP one, else the connection's; -1 unknown)
    int serverRttMs() const;
    // match_found's `connection` in words (the log), and its ping part alone (beside the link's icon)
    static QString connectionText(const QJsonObject& found);
    static QString pingText(const QJsonObject& found);
    QString gameStatus() const;
    GameProcess* game() const { return game_; }

    bool findMatch(const QueueRequest& req, QString* error);
    void cancelSearch();
    void accept();
    void decline();
    // After a queue match the two players stay together in a series (docs/PROTOCOL.md, series): a casual rematch
    // whenever both choose one, a ranked set until one has won its matches. Its last state ({} when none).
    QJsonObject series() const { return series_; }
    // ready for the series' next match (sent once this launcher is idle again, its game back at its hold point), or
    // leaving it (which loses a ranked set once a match of it counted). Casual, the choice: "rematch" (the same
    // fighters, when both choose it) or "select" (both pick again on the select screen); a ranked set ignores it.
    void seriesReady(bool ready, const QString& choice = QStringLiteral("rematch"));
    bool prepareGame(const QString& gameId, QString* error);
    // the whole game with its own menus, no match. A search can run alongside it (standby): accepting a match freezes
    // it (winutil::freezeProcess) and starts or thaws the match's game; once the match, and the rematches or the set
    // after it, are over (or the match never started), that game is frozen in turn and single player runs on where
    // it stood. `practice`: the game goes to its practice's select screen by itself (MK3D_TRAINING), where the player
    // picks both fighters; a Ranked or Kasual search starts it once the match's game waits at its hold point
    // (cfg::practiceWhileWaiting), to play while the search runs.
    bool startSolo(const QString& gameId, QString* error, bool practice = false);
    void stopSolo();
    bool soloRunning() const { return solo_ != nullptr; }
    bool soloIsPractice() const { return solo_ && soloPractice_; }
    GameProcess* solo() const { return solo_; }
    bool startLocalTest(const QString& gameId, const QString& mode, const QString& p1, const QString& p2,
                        QString* error);
    void stopLocalTest();
    void stopGame();
    // a lobby left: its game is closed once nothing needs it any more (a lobby match it plays or watches ends first),
    // so the player need not close it; a search, a lobby's Ready or a match to watch wanting a game before then keeps it
    void closeGameWhenIdle();
    void shutdown();
    // An update leaves each game built before with code for its old executable: it builds that again now, one game
    // at a time (the game's --prepare, behind its own progress window), so the next match or single player finds it
    // ready. A game waiting for it does not start meanwhile. False when no game needs it.
    bool prepareStaleCode();
    bool preparingCode() const { return !prepQueue_.isEmpty(); }
    // a spectator in a lobby: each of its matches comes to this game as the server passes it on (one at a time, the
    // next once the game is back at its hold point); stopWatching drops what waits and parks a match being watched
    bool watching() const { return !watch_.isEmpty(); }
    QJsonObject watchedMatch() const { return watch_; }
    void stopWatching();

signals:
    void phaseChanged(MatchController::Phase phase, const QString& text);
    void matchFound(const QJsonObject& found);
    void matchCancelled(const QString& reason, bool requeued);
    void matchStarted(const QJsonObject& go);
    void matchFinished(const QJsonObject& result);
    void seriesChanged(const QJsonObject& series);
    void gameStatusChanged(const QString& text);
    void logLine(const QString& text);
    void soloResumed();  // standby: single player runs again, its window wants the front
    void soloStarted();  // single player's game was started, its window (once it has one) wants the front
    void gameShown();    // standby: the match's game started for an accepted match has its window up
    void codePrepChanged(const QString& text, bool done);  // prepareStaleCode: the game it is on, or all done

private:
    void setPhase(Phase p, const QString& text);
    void onServerMessage(const QJsonObject& msg);
    void onGameStatus(const QString& line);
    void onGameExited();
    void onPunchDone(bool ok, int rtt, const QString& remote, const QString& via);
    void onLocalStatus();
    GameProcess* ensureGame(const QString& gameId, QString* error);
    bool codeReady(const QString& gameId, QString* error) const;  // not waiting for prepareStaleCode
    // a game that crashes is reported to the server: what it was doing (`during`, else the phase), where it crashed
    void watchForCrash(GameProcess* g, const QString& during = QString());
    void prepareNext();
    QString startLine(const QJsonObject& go) const;
    void tryStart();
    void reportResult(QJsonObject fields);
    void finishMatch();
    void sendSeriesReady();
    void releaseUdp();
    void sendKeepaliveHello();
    QString lanEndpoint() const;
    QString reachableFromHere(const QString& endpoint) const;
    bool openUdp(QString* error);
    // openUdp, tried again every 0.1 s for a while when the game still holds the port (it lets go at its next frame
    // after a park); then `next` runs, or the match is declined
    void openUdpThen(std::function<void()> next, int tries);
    QString opponentName() const;
    void log(const QString& text);
    void freezeSolo();
    void thawSolo();
    void closeSolo(const QString& why);  // practice, once its search's match is accepted
    void freezeGame();
    void thawGame();
    void maybeBackToSolo();    // standby: nothing more to play online, single player again
    void maybeStartPractice();  // a search waits with the match's game at its hold point: practice meanwhile
    void maybeCloseIdleGame();  // closeGameWhenIdle, once idle
    void openFeed(const QJsonObject& go);  // a lobby match: what its spectators need, from the game to the server
    // no UDP from here reaches the server: the match goes through its connection (docs/PROTOCOL.md 2.4.1)
    void requestTunnel(const QString& match);
    void closeTunnel();
    void closeFeed();
    void closeWatchFeed();
    void startWatch();
    void endWatch(const QString& text);
    void onWatchStatus(const QString& state);
    QString watchStartLine(const QJsonObject& info) const;

    ServerClient* server_;
    UdpRendezvous* udp_;
    GameProcess* game_ = nullptr;
    GameProcess* solo_ = nullptr;
    bool soloPractice_ = false;  // solo_ was started for practice while a search waits
    bool soloFrozen_ = false, gameFrozen_ = false;
    QList<quintptr> soloHidden_, gameHidden_;  // the windows freezing hid, shown again by thawing
    bool showWhenReady_ = false;  // standby: the match's game started on accepting; to the front once it is up
    bool closeWhenIdle_ = false;  // a lobby was left: game_ goes once idle (closeGameWhenIdle)
    GameProcess* localA_ = nullptr;
    GameProcess* localB_ = nullptr;
    Phase phase_ = Phase::Idle;
    QString phaseText_;
    QueueRequest request_;
    QJsonObject found_, go_, pendingResult_, series_;
    QString pendingStart_;
    bool resultSent_ = false;
    bool wantNext_ = false;  // ready for the series' next match, not sent yet (the game still plays the last one's end)
    QString nextChoice_ = QStringLiteral("rematch");
    QElapsedTimer searchClock_;
    QJsonObject pingWiden_;  // the server's rule for raising the max ping ({} when it is not raised)
    QTimer parkTimer_, keepaliveTimer_, startTimeout_, finishTimeout_, watchTimeout_;
    QTimer watchEndTimeout_;  // the watched match ended early: the watching game's while to be in it, else it parks
    bool watchInMatch_ = false;  // the watching game plays the watched match (it was at its meeting point)
    QString localStartA_, localStartB_, localResultA_, localResultB_;
    FeedLink* feed_ = nullptr;       // this player's lobby match's feed
    TunnelBridge* tunnel_ = nullptr;  // the match's stand-in for the relay, until the game is back at its hold point
    QString tunnelAsked_;            // the match the tunnel was asked for
    bool udpBlocked_ = false;        // the last hello to the server's UDP port got no answer
    FeedLink* watchFeed_ = nullptr;  // the watched match's, to the game, until it is back at its hold point (it plays
    QString watchFeedMatch_;         // the match's end after its result, from the rest of the feed)
    QJsonObject watch_;              // the lobby match being watched (its spectate message), {} when none
    QList<QJsonObject> watchQueue_;  // lobby matches to watch next
    QHash<QString, QByteArray> watchData_;  // their feeds so far, by match
    QString finishText_;             // what the phase says once the game is back at its hold point
    QStringList prepQueue_;          // games whose code prepareStaleCode builds again, the first one's running
    QStringList prepFailed_;         // the ones it could not (their next start builds it)
    int prepTotal_ = 0;
};
