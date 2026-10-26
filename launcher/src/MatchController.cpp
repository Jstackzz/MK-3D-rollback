#include "MatchController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTimer>

#include "AppPaths.h"
#include "GameData.h"
#include "GameProcess.h"
#include "ServerClient.h"
#include "UdpRendezvous.h"

namespace {
QString str(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }

QString reasonText(const QString& reason) {
    static const QHash<QString, QString> texts = {
        {QStringLiteral("declined"), QStringLiteral("a player declined")},
        {QStringLiteral("accept_timeout"), QStringLiteral("not accepted in time")},
        {QStringLiteral("no_udp"), QStringLiteral("no UDP contact with the server, check the game port")},
        {QStringLiteral("punch_failed"), QStringLiteral("could not connect to the opponent")},
        {QStringLiteral("ping_too_high"), QStringLiteral("ping above the limit")},
        {QStringLiteral("ping_timeout"), QStringLiteral("connection test timed out")},
        {QStringLiteral("peer_left"), QStringLiteral("the opponent left")},
    };
    return texts.value(reason, reason);
}
}  // namespace

MatchController::MatchController(ServerClient* server, QObject* parent)
    : QObject(parent), server_(server), udp_(new UdpRendezvous(this)) {
    connect(server_, &ServerClient::message, this, &MatchController::onServerMessage);
    connect(server_, &ServerClient::loggedIn, this, [this] {
        if (!pendingResult_.isEmpty()) {
            server_->send(pendingResult_);
            log(tr("Sent the stored result of match %1").arg(str(pendingResult_, "match")));
            pendingResult_ = {};
        }
    });
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State s) {
        if (s != ServerClient::State::Offline) return;
        if (phase_ == Phase::Searching || phase_ == Phase::Found || phase_ == Phase::Connecting) {
            releaseUdp();
            found_ = {};
            setPhase(Phase::Idle, tr("Lost the server connection"));
        }
    });
    connect(udp_, &UdpRendezvous::punchDone, this, &MatchController::onPunchDone);
    connect(udp_, &UdpRendezvous::helloTimedOut, this, [this] {
        if (phase_ == Phase::Found || phase_ == Phase::Connecting)
            log(tr("The server did not answer on UDP; is UDP port %1 blocked by a firewall?").arg(cfg::gamePort()));
    });

    parkTimer_.setSingleShot(true);
    connect(&parkTimer_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Reporting) finishMatch();
    });
    keepaliveTimer_.setInterval(15000);
    connect(&keepaliveTimer_, &QTimer::timeout, this, &MatchController::sendKeepaliveHello);
    finishTimeout_.setSingleShot(true);
    connect(&finishTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Finishing) setPhase(Phase::Idle, tr("Match over"));
    });
    startTimeout_.setSingleShot(true);
    connect(&startTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Starting) {
            log(tr("The game did not start the match within 60 s."));
            reportResult({{QStringLiteral("outcome"), QStringLiteral("error")}});
        }
    });
}

MatchController::~MatchController() = default;

void MatchController::log(const QString& text) {
    applog::write(QStringLiteral("match: ") + text);
    emit logLine(text);
}

void MatchController::setPhase(Phase p, const QString& text) {
    phase_ = p;
    phaseText_ = text;
    if (p != Phase::Searching) keepaliveTimer_.stop();
    log(text);
    emit phaseChanged(p, text);
}

int MatchController::searchSeconds() const {
    return searchClock_.isValid() ? static_cast<int>(searchClock_.elapsed() / 1000) : 0;
}

QString MatchController::gameStatus() const {
    if (localA_ || localB_)
        return tr("local test: 1 %1 / 2 %2").arg(localA_ ? localA_->status() : QStringLiteral("-"),
                                                 localB_ ? localB_->status() : QStringLiteral("-"));
    if (solo_) return tr("single player");
    return game_ ? game_->status() : tr("not running");
}

bool MatchController::openUdp(QString* error) {
    // every interface, so a player across the internet reaches this one, even when the server runs on this PC
    const QHostAddress bind = cfg::udpLoopback() ? QHostAddress(QHostAddress::LocalHost)
                                                 : QHostAddress(QHostAddress::AnyIPv4);
    return udp_->open(static_cast<quint16>(cfg::gamePort()), bind, error);
}

void MatchController::openUdpThen(std::function<void()> next, int tries) {
    QString err;
    if (openUdp(&err)) {
        next();
        return;
    }
    if (tries <= 0) {
        log(err);
        phase_ = Phase::Found;
        decline();
        return;
    }
    const QString match = str(found_, "match");
    QTimer::singleShot(100, this, [this, next, tries, match] {
        if (str(found_, "match") != match) return;  // that match is gone meanwhile
        openUdpThen(next, tries - 1);
    });
}

QString MatchController::lanEndpoint() const {
    return QStringLiteral("%1:%2").arg(server_->localAddress().toString()).arg(cfg::gamePort());
}

// A player on the server's own network shows up with an address of that network (127.0.0.1 when it plays on the
// server's PC); from anywhere else it is reached at the server's address, on its own port.
QString MatchController::reachableFromHere(const QString& endpoint) const {
    const int colon = endpoint.lastIndexOf(QLatin1Char(':'));
    const QHostAddress addr(endpoint.left(colon));
    const QHostAddress server = server_->serverAddress();
    if (colon <= 0 || addr.isNull() || server.isLoopback()) return endpoint;  // on the server's PC: as the server sees it
    const auto local = [](const QHostAddress& a) { return a.isPrivateUse() || a.isLinkLocal(); };
    if (!addr.isLoopback() && !(local(addr) && !local(server))) return endpoint;
    return server.toString() + endpoint.mid(colon);
}

QString MatchController::opponentName() const {
    const QJsonObject opp = found_.value(QStringLiteral("opponent")).toObject();
    return opp.isEmpty() ? tr("opponent") : str(opp, "name");
}

GameProcess* MatchController::ensureGame(const QString& gameId, QString* error) {
    if (solo_) {  // one game at a time, and its save is the single-player game's while it is open
        if (error) *error = tr("Close your single-player game first.");
        return nullptr;
    }
    if (phase_ == Phase::Finishing) {
        if (error) *error = tr("Your game is still playing the last match's ending: a moment.");
        return nullptr;
    }
    if (game_ && game_->isRunning() && game_->gameId() == gameId) return game_;
    if (game_) {
        game_->quit();
        game_->deleteLater();
        game_ = nullptr;
    }
    auto* g = new GameProcess(gameId, 1, this);
    connect(g, &GameProcess::statusChanged, this, &MatchController::onGameStatus);
    connect(g, &GameProcess::exited, this, &MatchController::onGameExited);
    if (!g->start(cfg::gameExe(gameId), error)) {
        delete g;
        return nullptr;
    }
    game_ = g;
    log(tr("Game %1 starting (session %2)").arg(gameId, g->sessionDir()));
    return game_;
}

bool MatchController::prepareGame(const QString& gameId, QString* error) { return ensureGame(gameId, error) != nullptr; }

bool MatchController::findMatch(const QueueRequest& req, QString* error) {
    if (!server_->isLoggedIn()) {
        if (error) *error = tr("Sign in first.");
        return false;
    }
    if (solo_) {
        if (error) *error = tr("Close your single-player game first.");
        return false;
    }
    if (phase_ != Phase::Idle && phase_ != Phase::Searching) {
        if (error) *error = tr("Busy: %1").arg(phaseText_);
        return false;
    }
    if (!ensureGame(req.game, error)) return false;
    QString uerr;
    if (!openUdp(&uerr)) {
        if (error) *error = uerr;
        return false;
    }
    udp_->startHello(server_->serverAddress(), server_->serverUdpPort(), server_->udpToken(), QString(),
                     lanEndpoint(), 3000);
    request_ = req;
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("queue_join")},
                    {QStringLiteral("game"), req.game},
                    {QStringLiteral("mode"), req.mode},
                    {QStringLiteral("ranked"), req.ranked},
                    {QStringLiteral("region"), req.region},
                    {QStringLiteral("max_ping"), req.maxPing},
                    {QStringLiteral("strict"), req.strict},
                    {QStringLiteral("character"), req.character},
                    {QStringLiteral("delay"), req.delay}};
    if (udp_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), udp_->serverRttMs());
    server_->send(msg);
    searchClock_.start();
    setPhase(Phase::Searching, tr("Searching for a %1 %2 opponent...")
                                   .arg(req.ranked ? tr("ranked") : tr("casual"), req.mode));
    keepaliveTimer_.start();
    return true;
}

void MatchController::sendKeepaliveHello() {
    if (phase_ != Phase::Searching || !server_->isLoggedIn()) return;
    QString err;
    if (!udp_->isOpen() && !openUdp(&err)) return;
    udp_->startHello(server_->serverAddress(), server_->serverUdpPort(), server_->udpToken(), QString(),
                     lanEndpoint(), 2000);
}

void MatchController::cancelSearch() {
    if (phase_ == Phase::Found || phase_ == Phase::Connecting) {
        decline();
        return;
    }
    if (phase_ != Phase::Searching) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("queue_leave")}});
    releaseUdp();
    setPhase(Phase::Idle, tr("Search cancelled"));
}

void MatchController::accept() {
    if (phase_ != Phase::Found) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("match_accept")}, {QStringLiteral("match"), str(found_, "match")}});
    setPhase(Phase::Connecting, tr("Accepted; waiting for the opponent..."));
}

void MatchController::decline() {
    if (phase_ != Phase::Found && phase_ != Phase::Connecting) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("match_decline")}, {QStringLiteral("match"), str(found_, "match")}});
    found_ = {};
    releaseUdp();
    setPhase(Phase::Idle, tr("Match declined"));
}

void MatchController::releaseUdp() { udp_->close(); }

void MatchController::onServerMessage(const QJsonObject& msg) {
    const QString type = str(msg, "type");
    const QString match = str(msg, "match");
    if (type == QLatin1String("queued")) {
        if (phase_ == Phase::Idle) {
            searchClock_.start();
            setPhase(Phase::Searching, tr("Searching..."));
        }
        if (phase_ == Phase::Searching) keepaliveTimer_.start();
    } else if (type == QLatin1String("queue_left")) {
        if (phase_ == Phase::Searching) {
            releaseUdp();
            setPhase(Phase::Idle, str(msg, "reason") == QLatin1String("removed") ? tr("Removed from the queue")
                                                                                 : tr("Search stopped"));
        }
    } else if (type == QLatin1String("match_found")) {
        found_ = msg;
        go_ = {};
        resultSent_ = false;
        QString err;
        if (!ensureGame(str(msg, "game"), &err)) {
            log(err);
            phase_ = Phase::Found;
            decline();
            return;
        }
        openUdpThen([this, msg, match] {
            udp_->startHello(server_->serverAddress(), server_->serverUdpPort(), server_->udpToken(), match,
                             lanEndpoint(), 10000);
            if (msg.value(QStringLiteral("accept_required")).toBool()) {
                setPhase(Phase::Found, tr("Match found"));
                emit matchFound(msg);
            } else {
                setPhase(Phase::Connecting, tr("Lobby match: connecting to %1...").arg(opponentName()));
                emit matchFound(msg);
            }
        }, 30);
    } else if (type == QLatin1String("match_peer")) {
        if (match != str(found_, "match")) return;
        found_.insert(QStringLiteral("opponent"), msg.value(QStringLiteral("opponent")));
        const int punchMs = msg.value(QStringLiteral("punch_timeout")).toInt(10) * 1000;
        if (msg.value(QStringLiteral("relay")).toBool()) {
            // no direct route between the two: the server passes the packets on, its UDP port is the opponent now
            setPhase(Phase::Connecting, tr("Connecting to %1 through the server...").arg(opponentName()));
            udp_->startRelayPunch(match, str(msg, "key"), msg.value(QStringLiteral("side")).toInt(), punchMs);
            return;
        }
        setPhase(Phase::Connecting, tr("Connecting to %1...").arg(opponentName()));
        const QJsonObject peer = msg.value(QStringLiteral("peer")).toObject();
        const QString pub = reachableFromHere(str(peer, "public"));
        if (pub != str(peer, "public")) log(tr("%1 plays beside the server: reaching it at %2").arg(opponentName(), pub));
        udp_->startPunch(match, str(msg, "key"), msg.value(QStringLiteral("side")).toInt(), str(peer, "lan"), pub,
                         punchMs);
    } else if (type == QLatin1String("match_go")) {
        if (match != str(found_, "match")) return;
        go_ = msg;
        releaseUdp();  // the game binds the same port right away
        pendingStart_ = startLine(go_);
        setPhase(Phase::Starting, tr("Starting the match against %1 (%2 ms)...")
                                      .arg(opponentName()).arg(msg.value(QStringLiteral("rtt_ms")).toInt()));
        emit matchStarted(msg);
        startTimeout_.start(60000);
        tryStart();
    } else if (type == QLatin1String("match_cancelled")) {
        if (match != str(found_, "match")) return;
        const bool requeued = msg.value(QStringLiteral("requeued")).toBool();
        const QString reason = str(msg, "reason");
        found_ = {};
        releaseUdp();
        emit matchCancelled(reason, requeued);
        if (requeued) {
            setPhase(Phase::Searching, tr("Back in the queue: %1").arg(reasonText(reason)));
            keepaliveTimer_.start();
            sendKeepaliveHello();
        } else {
            setPhase(Phase::Idle, tr("Match cancelled: %1").arg(reasonText(reason)));
        }
    } else if (type == QLatin1String("match_result")) {
        if (match != str(go_, "match")) return;
        QJsonObject result = msg;
        const int side = go_.value(QStringLiteral("side")).toInt();
        const int winner = msg.value(QStringLiteral("winner")).toInt();
        result.insert(QStringLiteral("side"), side);
        result.insert(QStringLiteral("opponent"), side == 1 ? str(go_, "p2_name") : str(go_, "p1_name"));
        if (winner == 1 || winner == 2) {
            const int mine = msg.value(side == 1 ? QStringLiteral("p1_rounds") : QStringLiteral("p2_rounds")).toInt();
            const int theirs = msg.value(side == 1 ? QStringLiteral("p2_rounds") : QStringLiteral("p1_rounds")).toInt();
            result.insert(QStringLiteral("you_won"), winner == side);
            result.insert(QStringLiteral("score"), QStringLiteral("%1-%2").arg(mine).arg(theirs));
        }
        emit matchFinished(result);
        finishMatch();
    } else if (type == QLatin1String("result_ack")) {
        log(tr("Result of %1 acknowledged by the server").arg(match));
    }
}

void MatchController::onPunchDone(bool ok, int rtt, const QString& remote, const QString& via) {
    if (found_.isEmpty()) return;
    QJsonObject report{{QStringLiteral("type"), QStringLiteral("ping_report")},
                       {QStringLiteral("match"), str(found_, "match")},
                       {QStringLiteral("ok"), ok},
                       {QStringLiteral("rtt_ms"), rtt}};
    if (ok) {
        report.insert(QStringLiteral("remote"), remote);
        report.insert(QStringLiteral("via"), via);
    }
    server_->send(report);
    log(ok ? tr("Connected to %1 via %2 (%3), %4 ms").arg(opponentName(), via, remote).arg(rtt)
           : tr("Could not reach %1 this way").arg(opponentName()));
}

QString MatchController::startLine(const QJsonObject& go) const {
    const QStringList tokens = {
        QStringLiteral("start"),
        QStringLiteral("game=") + str(go, "game"),
        QStringLiteral("mode=") + str(go, "mode"),
        QStringLiteral("side=%1").arg(go.value(QStringLiteral("side")).toInt()),
        QStringLiteral("local_port=%1").arg(cfg::gamePort()),
        QStringLiteral("remote=") + str(go, "remote"),
        QStringLiteral("delay=%1").arg(go.value(QStringLiteral("delay")).toInt()),
        QStringLiteral("seed=%1").arg(go.value(QStringLiteral("seed")).toInteger()),
        QStringLiteral("p1=") + (str(go, "p1").isEmpty() ? QStringLiteral("-") : str(go, "p1")),
        QStringLiteral("p2=") + (str(go, "p2").isEmpty() ? QStringLiteral("-") : str(go, "p2")),
        QStringLiteral("rounds=%1").arg(go.value(QStringLiteral("rounds")).toInt(3)),
        QStringLiteral("ranked=%1").arg(go.value(QStringLiteral("ranked")).toBool() ? 1 : 0),
        QStringLiteral("match=") + str(go, "match"),
        QStringLiteral("p1_name=") + str(go, "p1_name"),
        QStringLiteral("p2_name=") + str(go, "p2_name"),
    };
    return tokens.join(QLatin1Char(' '));
}

void MatchController::tryStart() {
    if (pendingStart_.isEmpty() || !game_) return;
    if (!game_->isReady()) {
        log(tr("Waiting for the game to reach its hold point (status: %1)").arg(game_->status()));
        return;
    }
    game_->command(pendingStart_);
    pendingStart_.clear();
}

void MatchController::onGameStatus(const QString& line) {
    emit gameStatusChanged(line);
    const QString st = line.section(QLatin1Char(' '), 0, 0);
    const QHash<QString, QString> keys = GameProcess::parseKeys(line);
    const QString goMatch = str(go_, "match");
    const bool forThisMatch = !keys.contains(QStringLiteral("match")) || keys.value(QStringLiteral("match")) == goMatch;
    const bool inOnline = (phase_ == Phase::Starting || phase_ == Phase::Playing) && !go_.isEmpty();
    if (st == QLatin1String("ready")) {
        if (phase_ == Phase::Finishing) {
            finishTimeout_.stop();
            setPhase(Phase::Idle, tr("Match over"));
        }
        tryStart();
    } else if (st == QLatin1String("in_match")) {
        if (phase_ == Phase::Starting && forThisMatch) {
            startTimeout_.stop();
            setPhase(Phase::Playing, tr("In match against %1").arg(opponentName()));
            server_->send({{QStringLiteral("type"), QStringLiteral("match_state")},
                           {QStringLiteral("match"), goMatch},
                           {QStringLiteral("state"), QStringLiteral("in_match")}});
        }
    } else if (st == QLatin1String("result") && inOnline && forThisMatch) {
        reportResult({{QStringLiteral("winner"), keys.value(QStringLiteral("winner")).toInt()},
                      {QStringLiteral("p1_rounds"), keys.value(QStringLiteral("p1_rounds")).toInt()},
                      {QStringLiteral("p2_rounds"), keys.value(QStringLiteral("p2_rounds")).toInt()}});
    } else if (st == QLatin1String("desync") && inOnline && forThisMatch) {
        reportResult({{QStringLiteral("outcome"), QStringLiteral("desync")},
                      {QStringLiteral("frame"), keys.value(QStringLiteral("frame")).toInt()}});
    } else if (st == QLatin1String("disconnected") && inOnline && forThisMatch) {
        reportResult({{QStringLiteral("outcome"), QStringLiteral("disconnected")}});
    } else if (st == QLatin1String("error") && inOnline) {
        reportResult({{QStringLiteral("outcome"), QStringLiteral("error")}});
    }
}

void MatchController::onGameExited() {
    switch (phase_) {
    case Phase::Searching:
        log(tr("The game closed; leaving the queue."));
        cancelSearch();
        break;
    case Phase::Found:
    case Phase::Connecting:
        decline();
        break;
    case Phase::Starting:
    case Phase::Playing:
        reportResult({{QStringLiteral("outcome"), QStringLiteral("error")}});
        break;
    case Phase::Finishing:
        finishTimeout_.stop();
        setPhase(Phase::Idle, tr("Match over"));
        break;
    default:
        break;
    }
}

void MatchController::reportResult(QJsonObject fields) {
    if (resultSent_ || go_.isEmpty()) return;
    resultSent_ = true;
    startTimeout_.stop();
    fields.insert(QStringLiteral("type"), QStringLiteral("result"));
    fields.insert(QStringLiteral("match"), str(go_, "match"));
    if (server_->isLoggedIn())
        server_->send(fields);
    else
        pendingResult_ = fields;
    const QString what = fields.contains(QStringLiteral("winner"))
                             ? tr("player %1 won %2-%3").arg(fields.value(QStringLiteral("winner")).toInt())
                                   .arg(fields.value(QStringLiteral("p1_rounds")).toInt())
                                   .arg(fields.value(QStringLiteral("p2_rounds")).toInt())
                             : fields.value(QStringLiteral("outcome")).toString();
    setPhase(Phase::Reporting, tr("Reporting the result (%1)...").arg(what));
    parkTimer_.start(10000);
}

void MatchController::finishMatch() {
    parkTimer_.stop();
    startTimeout_.stop();
    go_ = {};
    found_ = {};
    pendingStart_.clear();
    if (game_ && game_->isRunning() && !game_->isReady()) {
        // the game parks once its ending has played (it holds the game port until then): ready again, then idle
        game_->command(QStringLiteral("park"));
        finishTimeout_.start(45000);
        setPhase(Phase::Finishing, tr("Match over: the game plays its ending"));
        return;
    }
    setPhase(Phase::Idle, tr("Match over"));
}

// The whole game started plainly, its menus as on the console (arcade, its practice mode, the side modes): no hold
// point and no match. The game waiting in the background for a match closes first, so one game runs at a time.
bool MatchController::startSolo(const QString& gameId, QString* error) {
    if (solo_) {
        if (error) *error = tr("Your single-player game is already open.");
        return false;
    }
    if (phase_ != Phase::Idle) {
        if (error) *error = tr("Busy: %1").arg(phaseText_);
        return false;
    }
    if (game_) {
        GameProcess* waiting = game_;
        game_ = nullptr;
        disconnect(waiting, nullptr, this, nullptr);
        waiting->quit();
        QElapsedTimer t;
        t.start();
        while (waiting->isRunning() && t.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        waiting->deleteLater();
    }
    auto* g = new GameProcess(gameId, 1, this);
    if (!g->start(cfg::gameExe(gameId), error, false)) {
        delete g;
        return false;
    }
    solo_ = g;
    connect(g, &GameProcess::exited, this, [this, g] {
        if (solo_ != g) return;
        solo_ = nullptr;
        g->deleteLater();
        setPhase(Phase::Idle, tr("Single player closed"));
        emit gameStatusChanged(gameStatus());
    });
    log(tr("Game %1 started for single player (session %2)").arg(gameId, g->sessionDir()));
    setPhase(Phase::Solo, tr("Playing single player"));
    emit gameStatusChanged(gameStatus());
    return true;
}

void MatchController::stopSolo() {
    if (solo_) solo_->quit();  // its window closes; the exit makes the controller idle
}

bool MatchController::startLocalTest(const QString& gameId, const QString& mode, const QString& p1,
                                     const QString& p2, QString* error) {
    if (phase_ != Phase::Idle) {
        if (error) *error = tr("Busy: %1").arg(phaseText_);
        return false;
    }
    stopLocalTest();
    const QString exe = cfg::gameExe(gameId);
    localA_ = new GameProcess(gameId, 1, this);
    localB_ = new GameProcess(gameId, 2, this);
    if (!localA_->start(exe, error) || !localB_->start(exe, error)) {
        delete localA_;
        delete localB_;
        localA_ = localB_ = nullptr;
        return false;
    }
    for (GameProcess* g : {localA_, localB_}) {
        connect(g, &GameProcess::statusChanged, this, &MatchController::onLocalStatus);
        connect(g, &GameProcess::exited, this, &MatchController::onLocalStatus);
    }
    const int portA = cfg::localTestPort(1), portB = cfg::localTestPort(2);
    const quint32 seed = QRandomGenerator::global()->generate();
    const QString match = QStringLiteral("local-") + QDateTime::currentDateTime().toString(QStringLiteral("hhmmss"));
    const auto line = [&](int side, int local, int remote) {
        return QStringLiteral("start game=%1 mode=%2 side=%3 local_port=%4 remote=127.0.0.1:%5 delay=%6 seed=%7 "
                              "p1=%8 p2=%9 rounds=3 ranked=0 match=%10")
            .arg(gameId, mode).arg(side).arg(local).arg(remote).arg(cfg::inputDelay()).arg(seed).arg(p1, p2, match);
    };
    localStartA_ = line(1, portA, portB);
    localStartB_ = line(2, portB, portA);
    localResultA_.clear();
    localResultB_.clear();
    setPhase(Phase::LocalTest, tr("Local test: two instances on ports %1/%2, waiting for both to be ready").arg(portA).arg(portB));
    return true;
}

void MatchController::onLocalStatus() {
    if (!localA_ || !localB_) return;
    emit gameStatusChanged(gameStatus());
    if (!localStartA_.isEmpty() && localA_->isReady() && localB_->isReady()) {
        localA_->command(localStartA_);
        localB_->command(localStartB_);
        setPhase(Phase::LocalTest, tr("Local test: start lines written, both instances starting"));
        localStartA_.clear();
        localStartB_.clear();
    }
    const auto strip = [](const QString& s) { return s.section(QStringLiteral(" match="), 0, 0); };
    if (localA_->state() == QLatin1String("result")) localResultA_ = strip(localA_->status());
    if (localB_->state() == QLatin1String("result")) localResultB_ = strip(localB_->status());
    if (!localResultA_.isEmpty() && !localResultB_.isEmpty()) {
        setPhase(Phase::LocalTest,
                 localResultA_ == localResultB_ ? tr("Local test: both instances agree: %1").arg(localResultA_)
                                                : tr("Local test: results differ: %1 / %2").arg(localResultA_, localResultB_));
        localResultA_.clear();
        localResultB_.clear();
        localA_->command(QStringLiteral("park"));
        localB_->command(QStringLiteral("park"));
    }
    if (!localA_->isRunning() && !localB_->isRunning()) {
        log(tr("Local test: both instances exited"));
        localA_->deleteLater();
        localB_->deleteLater();
        localA_ = localB_ = nullptr;
        if (phase_ == Phase::LocalTest) setPhase(Phase::Idle, tr("Local test finished"));
    }
}

void MatchController::stopLocalTest() {
    if (!localA_ && !localB_) return;
    for (GameProcess* g : {localA_, localB_})
        if (g) {
            disconnect(g, nullptr, this, nullptr);
            g->quit();
            QTimer::singleShot(6000, g, &QObject::deleteLater);
        }
    localA_ = localB_ = nullptr;
    if (phase_ == Phase::LocalTest) setPhase(Phase::Idle, tr("Local test stopped"));
}

void MatchController::stopGame() {
    stopSolo();
    if (game_) game_->quit();
}

void MatchController::shutdown() {
    stopLocalTest();
    if (phase_ == Phase::Searching) cancelSearch();
    QList<GameProcess*> procs = findChildren<GameProcess*>();
    for (GameProcess* g : procs)
        if (g->isRunning()) g->quit();
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 2500) {
        bool any = false;
        for (GameProcess* g : procs) any = any || g->isRunning();
        if (!any) break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (GameProcess* g : procs) g->kill();
}
