#pragma once

#include <functional>
// Drives one player's online match from queue_join to match_result (docs/PROTOCOL.md 1.7-1.9,
// section 2) and owns the game processes: the prelaunched one, single player, and the local test pair.

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class ServerClient;
class UdpRendezvous;
class GameProcess;

struct QueueRequest {
    QString game;
    QString mode;
    QString region;
    QString character;
    bool ranked = false;
    bool strict = false;
    int maxPing = 0;
    int delay = 2;
};

class MatchController : public QObject {
    Q_OBJECT
public:
    // Finishing: the result is in, the game still plays the match's ending (finisher, winner's pose) until it is back
    // at its hold point; no new match meanwhile, the game still holds the game port
    enum class Phase { Idle, Searching, Found, Connecting, Starting, Playing, Reporting, Finishing, Solo, LocalTest };
    Q_ENUM(Phase)

    explicit MatchController(ServerClient* server, QObject* parent = nullptr);
    ~MatchController() override;

    Phase phase() const { return phase_; }
    QString phaseText() const { return phaseText_; }
    QueueRequest lastRequest() const { return request_; }
    QJsonObject currentFound() const { return found_; }
    int searchSeconds() const;
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
    // the whole game with its own menus, no match: online play waits until it is closed
    bool startSolo(const QString& gameId, QString* error);
    void stopSolo();
    bool soloRunning() const { return solo_ != nullptr; }
    bool startLocalTest(const QString& gameId, const QString& mode, const QString& p1, const QString& p2,
                        QString* error);
    void stopLocalTest();
    void stopGame();
    void shutdown();

signals:
    void phaseChanged(MatchController::Phase phase, const QString& text);
    void matchFound(const QJsonObject& found);
    void matchCancelled(const QString& reason, bool requeued);
    void matchStarted(const QJsonObject& go);
    void matchFinished(const QJsonObject& result);
    void seriesChanged(const QJsonObject& series);
    void gameStatusChanged(const QString& text);
    void logLine(const QString& text);

private:
    void setPhase(Phase p, const QString& text);
    void onServerMessage(const QJsonObject& msg);
    void onGameStatus(const QString& line);
    void onGameExited();
    void onPunchDone(bool ok, int rtt, const QString& remote, const QString& via);
    void onLocalStatus();
    GameProcess* ensureGame(const QString& gameId, QString* error);
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

    ServerClient* server_;
    UdpRendezvous* udp_;
    GameProcess* game_ = nullptr;
    GameProcess* solo_ = nullptr;
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
    QTimer parkTimer_, keepaliveTimer_, startTimeout_, finishTimeout_;
    QString localStartA_, localStartB_, localResultA_, localResultB_;
};
