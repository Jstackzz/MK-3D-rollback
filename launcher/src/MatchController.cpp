#include "MatchController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaEnum>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

#include <memory>

#include "AppPaths.h"
#include "FeedLink.h"
#include "GameData.h"
#include "GameProcess.h"
#include "ServerClient.h"
#include "TunnelBridge.h"
#include "UdpRendezvous.h"
#include "WinUtil.h"

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
        {QStringLiteral("series_over"), QStringLiteral("the series is over")},
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
    // UDP from here that never reaches the server (a firewall, a VPN, a network that blocks it): the match goes
    // through the server's connection instead, and so do the next ones until a hello gets its answer again
    connect(udp_, &UdpRendezvous::helloAcked, this, [this] {
        if (!udpBlocked_) return;
        udpBlocked_ = false;
        log(tr("The server answers on UDP again: matches go the usual way"));
    });
    connect(udp_, &UdpRendezvous::helloTimedOut, this, [this] {
        if (!udpBlocked_)
            log(tr("The server did not answer on UDP (is UDP port %1 blocked by a firewall or a VPN?): matches go "
                   "through the server's connection instead").arg(cfg::gamePort()));
        udpBlocked_ = true;
        if (phase_ == Phase::Found || phase_ == Phase::Connecting) requestTunnel(str(found_, "match"));
    });

    parkTimer_.setSingleShot(true);
    connect(&parkTimer_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Reporting) finishMatch();
    });
    keepaliveTimer_.setInterval(15000);
    connect(&keepaliveTimer_, &QTimer::timeout, this, &MatchController::sendKeepaliveHello);
    finishTimeout_.setSingleShot(true);
    connect(&finishTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ != Phase::Finishing) return;
        closeFeed();
        closeWatchFeed();
        setPhase(Phase::Idle, finishText_.isEmpty() ? tr("Match over") : finishText_);
    });
    // a watched match whose game never got into it (the players' feed never came): parked after this
    watchTimeout_.setSingleShot(true);
    connect(&watchTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Watching) endWatch(tr("The watched match never came through"));
    });
    // the players' match ended early, maybe before both their games met: the watching game would wait for that
    // meeting at its select screen (two minutes, its picture still), so it parks
    watchEndTimeout_.setSingleShot(true);
    connect(&watchEndTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Watching && !watchInMatch_) endWatch(tr("The watched match ended before it got going"));
    });
    startTimeout_.setSingleShot(true);
    connect(&startTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Starting) {
            log(tr("The game did not start the match within %1 s.").arg(startTimeout_.interval() / 1000));
            reportResult({{QStringLiteral("outcome"), QStringLiteral("error")}});
        }
    });
}

MatchController::~MatchController() = default;

namespace {
QString gameName(const QString& gameId) {
    const Game* g = GameData::instance().game(gameId);
    return !g ? gameId : g->shortName.isEmpty() ? g->title : g->shortName;
}
}  // namespace

// A game that crashed: an exception's exit code (0x8... or 0xC...; never the launcher's own kill, 0xF291, nor a
// process ended from outside, 0xFFFFFFFF), or any other failure whose log ends with the crash line the game writes
// (runtime app/main.cpp LogCrashLine: the exception, where as a module and offset, the thread). The server logs it
// with the player, so a crash many players have shows up as one.
void MatchController::watchForCrash(GameProcess* g, const QString& during) {
    // the phase as the game went: its "exited" status comes before the exit, and onGameStatus moves the match on
    auto phaseAtExit = std::make_shared<Phase>(phase_);
    connect(g, &GameProcess::statusChanged, this, [this, phaseAtExit](const QString& line) {
        if (line.endsWith(QLatin1String("exited"))) *phaseAtExit = phase_;
    });
    connect(g, &GameProcess::exited, this, [this, g, during, phaseAtExit](int exitCode, bool) {
        const quint32 code = quint32(exitCode);
        QString line;
        if (code != 0 && code != 0xF291u && code != 0xFFFFFFFFu) {
            QFile file(QDir(g->sessionDir()).filePath(QStringLiteral("game.log")));
            if (file.open(QIODevice::ReadOnly)) {
                if (file.size() > 65536) file.seek(file.size() - 65536);
                const QList<QByteArray> lines = file.readAll().split('\n');
                for (auto it = lines.crbegin(); it != lines.crend() && line.isEmpty(); ++it) {
                    const qsizetype at = it->indexOf("crash: ");
                    if (at >= 0) line = QString::fromLatin1(it->mid(at + 7)).trimmed();
                }
            }
        }
        const bool exception = code >= 0x80000000u && code != 0xFFFFFFFFu;
        if (!exception && line.isEmpty()) return;
        // "C0000005 at MKA.exe+1A2B3C on a game thread": where it happened, after its code
        QString where = line.section(QStringLiteral(" at "), 1);
        if (where.isEmpty()) where = line;
        where.replace(QRegularExpression(QStringLiteral("[^\\x20-\\x7E]")), QStringLiteral("?"));
        const QString hex = QString::number(code, 16).toUpper();
        const QString what = during.isEmpty()
                                 ? QString::fromLatin1(QMetaEnum::fromType<Phase>().valueToKey(int(*phaseAtExit)))
                                 : during;
        log(tr("The game crashed (%1%2)").arg(hex, where.isEmpty() ? QString() : QStringLiteral(" at ") + where));
        if (!server_->isLoggedIn()) return;
        server_->send({{QStringLiteral("type"), QStringLiteral("crash_report")},
                       {QStringLiteral("game"), g->gameId()},
                       {QStringLiteral("code"), hex},
                       {QStringLiteral("where"), where.left(150)},
                       {QStringLiteral("during"), what.left(40)}});
    });
}

bool MatchController::prepareStaleCode() {
    if (!prepQueue_.isEmpty()) return true;
    // not a game running already (a search started first): it builds its own code, behind its own window
    const auto running = [this](const QString& id) {
        return (game_ && game_->isRunning() && game_->gameId() == id) || (solo_ && solo_->gameId() == id);
    };
    for (const Game& g : GameData::instance().games())
        if (!running(g.id) && GameProcess::codeStale(cfg::gameExe(g.id))) prepQueue_ << g.id;
    if (prepQueue_.isEmpty()) return false;
    prepTotal_ = int(prepQueue_.size());
    prepFailed_.clear();
    log(tr("New executables: building the code again for %1").arg(prepQueue_.join(QStringLiteral(", "))));
    prepareNext();
    return true;
}

void MatchController::prepareNext() {
    while (!prepQueue_.isEmpty()) {
        const QString id = prepQueue_.first();
        auto* g = new GameProcess(id, 1, this);
        g->setArgs({QStringLiteral("--prepare")});
        watchForCrash(g, QStringLiteral("preparing"));
        QString err;
        if (!g->start(cfg::gameExe(id), &err, false)) {
            log(tr("Preparing %1: %2").arg(gameName(id), err));
            delete g;
            prepFailed_ << gameName(id);
            prepQueue_.removeFirst();
            continue;
        }
        connect(g, &GameProcess::exited, this, [this, g, id](int code, bool crashed) {
            g->deleteLater();
            if (prepQueue_.isEmpty() || prepQueue_.first() != id) return;  // shut down meanwhile
            prepQueue_.removeFirst();
            if (code == 0 && !crashed) {
                log(tr("%1 is ready for this version").arg(gameName(id)));
            } else {
                log(tr("Preparing %1 did not finish (exit code %2): its next start does it").arg(gameName(id)).arg(code));
                prepFailed_ << gameName(id);
            }
            prepareNext();
        });
        const int n = prepTotal_ - int(prepQueue_.size()) + 1;
        emit codePrepChanged(tr("Preparing %1 for this version (%2 of %3), its window shows how far it is: it can be "
                                "played once that is done.")
                                 .arg(gameName(id)).arg(n).arg(prepTotal_),
                             false);
        return;
    }
    emit codePrepChanged(prepFailed_.isEmpty()
                             ? tr("The games are ready for this version.")
                             : tr("Preparing %1 did not finish: it is done on its next start.")
                                   .arg(prepFailed_.join(QStringLiteral(", "))),
                         true);
    emit gameStatusChanged(gameStatus());  // an update offer or install waiting for this goes on
}

bool MatchController::codeReady(const QString& gameId, QString* error) const {
    if (!prepQueue_.contains(gameId)) return true;
    if (error)
        *error = prepQueue_.first() == gameId
                     ? tr("%1 is being prepared for this version (its window shows how far it is): it can be played "
                          "once that is done.").arg(gameName(gameId))
                     : tr("%1 is prepared for this version next, after %2: it can be played once that is done.")
                           .arg(gameName(gameId), gameName(prepQueue_.first()));
    return false;
}

void MatchController::log(const QString& text) {
    applog::write(QStringLiteral("match: ") + text);
    emit logLine(text);
}

void MatchController::setPhase(Phase p, const QString& text) {
    if (p == Phase::Idle && solo_ && !soloFrozen_) p = Phase::Solo;  // nothing online: the single-player game's
    phase_ = p;
    phaseText_ = text;
    if (p != Phase::Searching) keepaliveTimer_.stop();
    if (tunnel_ && p != Phase::Found && p != Phase::Connecting && p != Phase::Starting && p != Phase::Playing &&
        p != Phase::Reporting && p != Phase::Finishing)
        closeTunnel();  // no match of this player's any more
    log(text);
    emit phaseChanged(p, text);
    if (p == Phase::Idle) sendSeriesReady();
    maybeBackToSolo();
    if (p == Phase::Searching) maybeStartPractice();
    if (p == Phase::Idle) startWatch();  // a lobby match waiting to be watched
    maybeCloseIdleGame();
}

void MatchController::freezeSolo() {
    if (!solo_ || soloFrozen_) return;
    soloHidden_.clear();
    soloFrozen_ = true;
    const QString what = soloPractice_ ? tr("practice") : tr("single player");
    if (!winutil::freezeProcess(solo_->pid(), &soloHidden_)) log(tr("Standby: %1 could not be frozen").arg(what));
    log(tr("Standby: %1 set aside for the match").arg(what));
}

void MatchController::thawSolo() {
    if (!solo_ || !soloFrozen_) return;
    soloFrozen_ = false;
    winutil::thawProcess(solo_->pid(), soloHidden_);
    soloHidden_.clear();
}

void MatchController::freezeGame() {
    if (!game_ || gameFrozen_ || !game_->isRunning()) return;
    gameHidden_.clear();
    gameFrozen_ = true;
    winutil::freezeProcess(game_->pid(), &gameHidden_);
}

void MatchController::thawGame() {
    if (!game_ || !gameFrozen_) return;
    gameFrozen_ = false;
    winutil::thawProcess(game_->pid(), gameHidden_);
    gameHidden_.clear();
}

// Standby: once nothing more is to be played online (the match is over and no rematch or set's next match follows,
// or it was cancelled before it started), the match's game is frozen at its hold point for the next one and single
// player runs on where it stood.
void MatchController::maybeBackToSolo() {
    if (!soloFrozen_ || !solo_) return;
    if (phase_ != Phase::Idle && phase_ != Phase::Searching) return;
    const QString state = str(series_, "state");
    if (state == QLatin1String("between") || state == QLatin1String("playing")) return;
    thawSolo();
    log(soloPractice_ ? tr("Standby: back to practice") : tr("Standby: back to single player"));
    if (phase_ == Phase::Idle) setPhase(Phase::Solo, soloPractice_ ? tr("Practicing") : tr("Playing single player"));
    // in front while the match's game still runs: taking the foreground from a frozen window would wait on it for ever
    emit soloResumed();
    freezeGame();
}

void MatchController::seriesReady(bool ready, const QString& choice) {
    if (series_.isEmpty()) return;
    wantNext_ = ready;
    nextChoice_ = choice;
    if (!ready) {
        server_->send({{QStringLiteral("type"), QStringLiteral("series_ready")},
                       {QStringLiteral("series"), str(series_, "series")},
                       {QStringLiteral("ready"), false}});
        return;
    }
    sendSeriesReady();
}

// the next match of the series is asked for once this launcher is idle: the game is back at its hold point and has
// let go of the game port
void MatchController::sendSeriesReady() {
    if (!wantNext_ || phase_ != Phase::Idle || series_.isEmpty() || str(series_, "state") != QLatin1String("between") ||
        !server_->isLoggedIn())
        return;
    wantNext_ = false;
    server_->send({{QStringLiteral("type"), QStringLiteral("series_ready")},
                   {QStringLiteral("series"), str(series_, "series")},
                   {QStringLiteral("ready"), true},
                   {QStringLiteral("choice"), nextChoice_}});
}

int MatchController::searchSeconds() const {
    return searchClock_.isValid() ? static_cast<int>(searchClock_.elapsed() / 1000) : 0;
}

// as the server computes it (server/matchmaking.py max_ping_now)
int MatchController::raisedMaxPing() const {
    const int set = request_.maxPing;
    if (phase_ != Phase::Searching || pingWiden_.isEmpty() || set <= 0 || !searchClock_.isValid()) return 0;
    const double over = searchClock_.elapsed() / 1000.0 - pingWiden_.value(QStringLiteral("after")).toDouble(30);
    if (over < 0) return 0;
    const double every = qMax(1.0, pingWiden_.value(QStringLiteral("every")).toDouble(15));
    const int raised = qMin(pingWiden_.value(QStringLiteral("cap")).toInt(250),
                            set + pingWiden_.value(QStringLiteral("step")).toInt(25) * (1 + int(over / every)));
    return raised > set ? raised : 0;
}

int MatchController::serverRttMs() const { return udp_->serverRttMs() >= 0 ? udp_->serverRttMs() : server_->rttMs(); }

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
    closeWhenIdle_ = false;  // wanted again before a lobby left could close it
    // one game runs at a time: while single player runs, only an accepted match (which freezes it) brings this one
    if (solo_ && !soloFrozen_) {
        if (error) *error = tr("Close your single-player game first.");
        return nullptr;
    }
    if (phase_ == Phase::Finishing) {
        if (error) *error = tr("Your game is still playing the last match's ending: a moment.");
        return nullptr;
    }
    if (game_ && game_->isRunning() && game_->gameId() == gameId) {  // it built its own code, if it had to
        if (gameFrozen_) {
            thawGame();
            showWhenReady_ = soloFrozen_;
            if (game_->isReady()) emit gameShown();  // at its hold point: up at once
        }
        return game_;
    }
    if (!codeReady(gameId, error)) return nullptr;
    if (game_) {
        thawGame();  // a frozen game would not see its quit command
        game_->quit();
        game_->deleteLater();
        game_ = nullptr;
    }
    showWhenReady_ = soloFrozen_;
    auto* g = new GameProcess(gameId, 1, this);
    watchForCrash(g);  // first: it says what was going on before onGameExited moves the match on
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
    if (phase_ != Phase::Idle && phase_ != Phase::Searching && phase_ != Phase::Solo) {
        if (error) *error = tr("Busy: %1").arg(phaseText_);
        return false;
    }
    // standby: single player runs on while this searches; the match's game starts once a match is accepted
    if (!solo_ && !ensureGame(req.game, error)) return false;
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
                    {QStringLiteral("delay"), req.delay},
                    {QStringLiteral("death_traps"), req.deathTraps},
                    {QStringLiteral("relay"), cfg::hideIp()},
                    {QStringLiteral("link"), server_->linkType()}};
    if (serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), serverRttMs());
    pingWiden_ = {};
    server_->send(msg);
    searchClock_.start();
    setPhase(Phase::Searching, tr("Searching for a %1 %2 opponent...")
                                   .arg(req.ranked ? tr("ranked") : tr("Kasual"), req.mode));
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
    if (solo_ && !soloFrozen_) {  // standby: single player steps aside, the match's game comes up
        freezeSolo();
        QString err;
        if (!ensureGame(str(found_, "game"), &err)) {
            log(err);
            thawSolo();
            decline();
            return;
        }
    }
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
        pingWiden_ = msg.value(QStringLiteral("ping_widen")).toObject();
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
        closeTunnel();
        if (msg.contains(QStringLiteral("connection"))) log(tr("Opponent's connection: %1").arg(connectionText(msg)));
        resultSent_ = false;
        closeFeed();
        QString err;
        // standby: the match's game waits for the accept, which sets single player aside
        const bool standby = solo_ && !soloFrozen_ && msg.value(QStringLiteral("accept_required")).toBool();
        if (!standby && !ensureGame(str(msg, "game"), &err)) {
            log(err);
            phase_ = Phase::Found;
            decline();
            return;
        }
        openUdpThen([this, msg, match] {
            // UDP that got nowhere lately: through the server's connection at once; else a few seconds' hellos (well
            // within the server's 10 s), then that way
            if (udpBlocked_)
                requestTunnel(match);
            else
                udp_->startHello(server_->serverAddress(), server_->serverUdpPort(), server_->udpToken(), match,
                                 lanEndpoint(), 4000);
            if (msg.value(QStringLiteral("accept_required")).toBool()) {
                setPhase(Phase::Found, tr("Match found"));
                emit matchFound(msg);
            } else {
                setPhase(Phase::Connecting, msg.contains(QStringLiteral("series"))
                                                ? tr("Next match: connecting to %1...").arg(opponentName())
                                                : tr("Lobby match: connecting to %1...").arg(opponentName()));
                emit matchFound(msg);
            }
        }, 30);
    } else if (type == QLatin1String("match_peer")) {
        if (match != str(found_, "match")) return;
        found_.insert(QStringLiteral("opponent"), msg.value(QStringLiteral("opponent")));
        const int punchMs = msg.value(QStringLiteral("punch_timeout")).toInt(10) * 1000;
        if (msg.value(QStringLiteral("tunnel")).toBool()) {
            // through the server's connection: the punch, then the game, send to a local port that passes everything
            // on to it, and the game's remote is that port (what the ping report says comes back with match_go)
            closeTunnel();
            tunnel_ = new TunnelBridge(server_, static_cast<quint16>(cfg::gamePort()), this);
            QString err;
            if (!tunnel_->open(&err)) {
                log(err);
                closeTunnel();
                decline();
                return;
            }
            setPhase(Phase::Connecting, tr("Connecting to %1 through the server's connection...").arg(opponentName()));
            udp_->startTunnelPunch(match, str(msg, "key"), msg.value(QStringLiteral("side")).toInt(), punchMs,
                                   tunnel_->address(), tunnel_->port());
            return;
        }
        if (msg.value(QStringLiteral("relay")).toBool()) {
            // no direct route between the two: the server passes the packets on, its UDP port is the opponent now
            setPhase(Phase::Connecting, tr("Connecting to %1 through the server...").arg(opponentName()));
            udp_->startRelayPunch(match, str(msg, "key"), msg.value(QStringLiteral("side")).toInt(), punchMs);
            return;
        }
        setPhase(Phase::Connecting, tr("Connecting to %1...").arg(opponentName()));
        const QJsonObject peer = msg.value(QStringLiteral("peer")).toObject();
        const QString pub = reachableFromHere(str(peer, "public"));
        if (pub != str(peer, "public")) log(tr("%1 plays beside the server: reaching them on its network").arg(opponentName()));
        udp_->startPunch(match, str(msg, "key"), msg.value(QStringLiteral("side")).toInt(), str(peer, "lan"), pub,
                         punchMs);
    } else if (type == QLatin1String("match_go")) {
        if (match != str(found_, "match")) return;
        go_ = msg;
        releaseUdp();  // the game binds the same port right away
        openFeed(msg);
        pendingStart_ = startLine(go_);
        setPhase(Phase::Starting, tr("Starting the match against %1 (%2 ms)...")
                                      .arg(opponentName()).arg(msg.value(QStringLiteral("rtt_ms")).toInt()));
        emit matchStarted(msg);
        // a player who picks on the game's select screen has 75 s there (40 in Armageddon) before the game picks for
        // them, and the select screen gives up after 4 minutes: the start waits for that
        const bool picksInGame = str(msg, "p1").isEmpty() || str(msg, "p1") == QLatin1String("-") ||
                                 str(msg, "p2").isEmpty() || str(msg, "p2") == QLatin1String("-");
        startTimeout_.start(picksInGame ? 300000 : 60000);
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
    } else if (type == QLatin1String("series")) {
        const QString state = str(msg, "state");
        series_ = msg;
        // a ranked set goes on by itself: its next match once this game is back at its hold point
        if (state == QLatin1String("between") && msg.value(QStringLiteral("ranked")).toBool() &&
            !msg.value(QStringLiteral("you_ready")).toBool())
            wantNext_ = true;
        if (state == QLatin1String("over")) wantNext_ = false;
        emit seriesChanged(msg);
        if (state == QLatin1String("over")) series_ = {};
        sendSeriesReady();
        maybeBackToSolo();
    } else if (type == QLatin1String("result_ack")) {
        log(tr("Result of %1 acknowledged by the server").arg(match));
    } else if (type == QLatin1String("spectate")) {
        // a lobby match this player watches: its game plays it from both players' feeds, once it is free
        if (match == str(watch_, "match")) return;
        for (const QJsonObject& q : std::as_const(watchQueue_))
            if (str(q, "match") == match) return;
        while (watchQueue_.size() >= 2) watchData_.remove(str(watchQueue_.takeFirst(), "match"));  // long missed
        watchQueue_.append(msg);
        watchData_.insert(match, QByteArray());
        log(tr("Lobby match %1 vs %2 to watch").arg(str(msg, "p1_name"), str(msg, "p2_name")));
        startWatch();
    } else if (type == QLatin1String("spectate_feed")) {
        const QByteArray data = QByteArray::fromBase64(str(msg, "data").toLatin1());
        if (watchFeed_ && match == watchFeedMatch_) watchFeed_->deliver(data);
        else if (watchData_.contains(match)) watchData_[match] += data;
    } else if (type == QLatin1String("spectate_end")) {
        // the players are done; this game, behind them, plays on to the same end with what it has. A match that ended
        // early (a player left, a desync: not "agreed") may have ended before both games even met.
        const bool whole = str(msg, "status") == QLatin1String("agreed");
        if (match == str(watch_, "match")) {
            log(tr("The players finished the match being watched (%1)").arg(str(msg, "status")));
            if (!whole && !watchInMatch_) {
                if (!pendingStart_.isEmpty()) endWatch(tr("The watched match ended before it got going"));  // not sent
                else watchEndTimeout_.start(15000);
            }
        } else if (!whole) {
            for (qsizetype i = 0; i < watchQueue_.size(); ++i) {
                if (str(watchQueue_[i], "match") != match) continue;
                log(tr("Lobby match %1 vs %2 ended early: not watching it")
                        .arg(str(watchQueue_[i], "p1_name"), str(watchQueue_[i], "p2_name")));
                watchQueue_.removeAt(i);
                watchData_.remove(match);
                break;
            }
        }
    }
}

// --- spectators (docs/PROTOCOL.md 1.10.1)

void MatchController::openFeed(const QJsonObject& go) {
    closeFeed();
    if (!go.value(QStringLiteral("feed")).toBool()) return;
    auto* feed = new FeedLink(FeedLink::Role::Player, this);
    QString err;
    if (!feed->open(&err)) {
        log(err);
        delete feed;
        return;
    }
    const QString match = str(go, "match");
    connect(feed, &FeedLink::outgoing, this, [this, match](const QByteArray& packets) {
        if (server_->isLoggedIn())
            server_->send({{QStringLiteral("type"), QStringLiteral("feed")},
                           {QStringLiteral("match"), match},
                           {QStringLiteral("data"), QString::fromLatin1(packets.toBase64())}});
    });
    feed_ = feed;
}

void MatchController::requestTunnel(const QString& match) {
    if (match.isEmpty() || match == tunnelAsked_) return;
    tunnelAsked_ = match;
    server_->send({{QStringLiteral("type"), QStringLiteral("udp_tunnel")}, {QStringLiteral("match"), match}});
}

void MatchController::closeTunnel() {
    if (!tunnel_) return;
    log(tr("Through the server's connection: %1 packets out, %2 in").arg(tunnel_->packetsOut()).arg(tunnel_->packetsIn()));
    tunnel_->deleteLater();
    tunnel_ = nullptr;
}

// once the game is back at its hold point: what it wrote last (the match's end) goes out first
void MatchController::closeFeed() {
    if (!feed_) return;
    QPointer<FeedLink> feed = feed_;
    feed_ = nullptr;
    QTimer::singleShot(1500, this, [this, feed] {
        if (!feed) return;
        feed->flush();
        log(tr("Spectator feed: %1 KB passed on").arg(feed->bytes() / 1024));
        feed->deleteLater();
    });
}

QString MatchController::watchStartLine(const QJsonObject& info) const {
    const QStringList tokens = {
        QStringLiteral("start"),
        QStringLiteral("game=") + str(info, "game"),
        QStringLiteral("mode=") + str(info, "mode"),
        QStringLiteral("watch=1"),
        QStringLiteral("feed=") + (watchFeed_ ? watchFeed_->endpoint() : QString()),
        QStringLiteral("seed=%1").arg(info.value(QStringLiteral("seed")).toInteger()),
        QStringLiteral("p1=") + (str(info, "p1").isEmpty() ? QStringLiteral("-") : str(info, "p1")),
        QStringLiteral("p2=") + (str(info, "p2").isEmpty() ? QStringLiteral("-") : str(info, "p2")),
        QStringLiteral("rounds=%1").arg(info.value(QStringLiteral("rounds")).toInt(3)),
        QStringLiteral("traps=%1").arg(info.value(QStringLiteral("death_traps")).toBool(true) ? 1 : 0),
        QStringLiteral("ranked=0"),
        QStringLiteral("match=") + str(info, "match"),
        QStringLiteral("p1_name=") + str(info, "p1_name"),
        QStringLiteral("p2_name=") + str(info, "p2_name"),
    };
    return tokens.join(QLatin1Char(' '));
}

// the next lobby match to watch, once nothing else runs here (a search, a match, single player, the last match's end)
void MatchController::startWatch() {
    if (!watch_.isEmpty() || watchQueue_.isEmpty() || phase_ != Phase::Idle) return;
    const QJsonObject info = watchQueue_.takeFirst();
    const QString match = str(info, "match");
    const QByteArray data = watchData_.take(match);
    QString err;
    if (!ensureGame(str(info, "game"), &err)) {
        log(tr("Cannot watch the lobby's match: %1").arg(err));
        return;
    }
    auto* feed = new FeedLink(FeedLink::Role::Watcher, this);
    if (!feed->open(&err)) {
        log(err);
        delete feed;
        return;
    }
    feed->deliver(data);
    closeWatchFeed();
    watchFeed_ = feed;
    watchFeedMatch_ = match;
    watch_ = info;
    watchInMatch_ = false;
    watchEndTimeout_.stop();
    finishText_ = tr("Done watching");
    pendingStart_ = watchStartLine(info);
    setPhase(Phase::Watching, tr("Watching %1 vs %2: the match is being set up...")
                                  .arg(str(info, "p1_name"), str(info, "p2_name")));
    // the players pick on the select screen (up to 75 s), and the game may start behind them
    watchTimeout_.start(6 * 60 * 1000);
    tryStart();
    emit gameShown();  // the watched match comes to the game
}

void MatchController::onWatchStatus(const QString& state) {
    if (state == QLatin1String("ready")) {
        tryStart();
    } else if (state == QLatin1String("in_match")) {
        watchTimeout_.stop();
        watchEndTimeout_.stop();
        watchInMatch_ = true;
        setPhase(Phase::Watching, tr("Watching %1 vs %2").arg(str(watch_, "p1_name"), str(watch_, "p2_name")));
        emit gameShown();
    } else if (state == QLatin1String("result")) {
        endWatch(tr("Done watching %1 vs %2").arg(str(watch_, "p1_name"), str(watch_, "p2_name")));
    } else if (state == QLatin1String("desync")) {
        endWatch(tr("The watched match went out of step with the players' games; stopped watching"));
    } else if (state == QLatin1String("disconnected") || state == QLatin1String("error")) {
        endWatch(tr("The watched match ended before its end (a player left)"));
    }
}

// the game parks once the watched match's end has played (link RequestPark), from the rest of the feed, which goes on
// to it until then; then the next one may come
void MatchController::endWatch(const QString& text) {
    watchTimeout_.stop();
    watchEndTimeout_.stop();
    watchInMatch_ = false;
    const bool started = pendingStart_.isEmpty();  // the game took the watched match's start: it parks from it
    watch_ = {};
    pendingStart_.clear();
    finishText_ = text;
    if (started && game_ && game_->isRunning() && !game_->isReady()) {
        game_->command(QStringLiteral("park"));
        finishTimeout_.start(45000);
        setPhase(Phase::Finishing, text);
        return;
    }
    closeWatchFeed();
    setPhase(Phase::Idle, text);
}

void MatchController::closeWatchFeed() {
    if (!watchFeed_) return;
    watchFeed_->deleteLater();
    watchFeed_ = nullptr;
    watchFeedMatch_.clear();
}

void MatchController::stopWatching() {
    watchQueue_.clear();
    watchData_.clear();
    if (phase_ == Phase::Watching) endWatch(tr("Stopped watching"));
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
    // never the address: it is the opponent's (or the server's), and players show these pages on stream
    log(ok ? tr("Connected to %1 %2, %3 ms")
                 .arg(opponentName(), via == QLatin1String("tunnel")  ? tr("through the server's connection")
                                      : via == QLatin1String("relay") ? tr("through the server")
                                                                      : tr("directly"))
                 .arg(rtt)
           : tr("Could not reach %1 this way").arg(opponentName()));
}

// match_found's `connection`: the opponent's link and ping, and the expected ping between the two when known
QString MatchController::connectionText(const QJsonObject& found) {
    const QJsonObject c = found.value(QStringLiteral("connection")).toObject();
    const QString link = c.value(QStringLiteral("link")).toString();
    QStringList parts{link == QLatin1String("ethernet") ? tr("Ethernet (wired)")
                      : link == QLatin1String("wifi")   ? tr("Wi-Fi")
                      : link == QLatin1String("other")  ? tr("other connection")
                                                        : tr("connection type unknown")};
    if (const QString ping = pingText(found); !ping.isEmpty()) parts << ping;
    return parts.join(QStringLiteral("  ·  "));
}

QString MatchController::pingText(const QJsonObject& found) {
    const QJsonObject c = found.value(QStringLiteral("connection")).toObject();
    const int estimate = c.value(QStringLiteral("estimate")).toInt(-1);
    const int theirs = c.value(QStringLiteral("ping")).toInt(-1);
    if (estimate >= 0)
        return (c.value(QStringLiteral("measured")).toBool() ? tr("ping %1 ms between you (measured last time)")
                                                             : tr("ping about %1 ms between you")).arg(estimate);
    if (theirs >= 0) return tr("their ping to the server %1 ms").arg(theirs);
    return {};
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
        QStringLiteral("traps=%1").arg(go.value(QStringLiteral("death_traps")).toBool(true) ? 1 : 0),
        QStringLiteral("match=") + str(go, "match"),
        QStringLiteral("p1_name=") + str(go, "p1_name"),
        QStringLiteral("p2_name=") + str(go, "p2_name"),
    };
    QString line = tokens.join(QLatin1Char(' '));
    if (feed_) line += QStringLiteral(" feed=") + feed_->endpoint();
    return line;
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
    if (phase_ == Phase::Watching) {
        onWatchStatus(st);
        return;
    }
    const QHash<QString, QString> keys = GameProcess::parseKeys(line);
    const QString goMatch = str(go_, "match");
    const bool forThisMatch = !keys.contains(QStringLiteral("match")) || keys.value(QStringLiteral("match")) == goMatch;
    const bool inOnline = (phase_ == Phase::Starting || phase_ == Phase::Playing) && !go_.isEmpty();
    if (st == QLatin1String("ready")) {
        if (showWhenReady_) {  // standby: started for an accepted match, its window is up now
            showWhenReady_ = false;
            emit gameShown();
        }
        if (phase_ == Phase::Finishing) {
            finishTimeout_.stop();
            closeFeed();
            closeWatchFeed();
            setPhase(Phase::Idle, finishText_.isEmpty() ? tr("Match over") : finishText_);
        }
        tryStart();
        maybeStartPractice();
    } else if (st == QLatin1String("in_match")) {
        if (phase_ == Phase::Starting && forThisMatch) {
            startTimeout_.stop();
            setPhase(Phase::Playing, tr("In match against %1").arg(opponentName()));
            server_->send({{QStringLiteral("type"), QStringLiteral("match_state")},
                           {QStringLiteral("match"), goMatch},
                           {QStringLiteral("state"), QStringLiteral("in_match")}});
        }
    } else if (st == QLatin1String("result") && inOnline && forThisMatch) {
        QJsonObject result{{QStringLiteral("winner"), keys.value(QStringLiteral("winner")).toInt()},
                           {QStringLiteral("p1_rounds"), keys.value(QStringLiteral("p1_rounds")).toInt()},
                           {QStringLiteral("p2_rounds"), keys.value(QStringLiteral("p2_rounds")).toInt()}};
        // the fighters the game saw, what a rematch with the same fighters starts with
        for (const QString k : {QStringLiteral("p1_char"), QStringLiteral("p2_char")})
            if (keys.contains(k)) result.insert(k, keys.value(k));
        reportResult(result);
    } else if (st == QLatin1String("desync") && inOnline && forThisMatch) {
        reportResult({{QStringLiteral("outcome"), QStringLiteral("desync")},
                      {QStringLiteral("frame"), keys.value(QStringLiteral("frame")).toInt()}});
    } else if (st == QLatin1String("disconnected") && inOnline && forThisMatch) {
        reportResult({{QStringLiteral("outcome"), QStringLiteral("disconnected")}});
    } else if (st == QLatin1String("error") && inOnline) {
        // "error exited": GameProcess's word for a game that closed by itself (Alt+F4, a crash), which ends the match
        // for both: the player left it (onGameExited)
        reportResult({{QStringLiteral("outcome"),
                       line.startsWith(QLatin1String("error exited")) ? QStringLiteral("left") : QStringLiteral("error")}});
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
        // the player closed their game (or it crashed) during the match: they left it, which ends the series (and
        // loses a ranked set) instead of offering a rematch
        reportResult({{QStringLiteral("outcome"), QStringLiteral("left")}});
        break;
    case Phase::Finishing:
        finishTimeout_.stop();
        setPhase(Phase::Idle, tr("Match over"));
        break;
    case Phase::Watching:
        endWatch(tr("The game closed while watching"));
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
    finishText_.clear();
    if (game_ && game_->isRunning() && !game_->isReady()) {
        // the game parks once its ending has played (it holds the game port until then): ready again, then idle
        game_->command(QStringLiteral("park"));
        finishTimeout_.start(45000);
        setPhase(Phase::Finishing, tr("Match over: the game plays its ending"));
        return;
    }
    closeFeed();
    setPhase(Phase::Idle, tr("Match over"));
}

// The whole game started plainly, its menus as on the console (arcade, its practice mode, the side modes): no hold
// point and no match. One game runs at a time: the game waiting in the background for a match closes first, or,
// while a search runs (standby), is frozen until a match is accepted.
bool MatchController::startSolo(const QString& gameId, QString* error, bool practice) {
    if (!codeReady(gameId, error)) return false;
    if (solo_) {
        if (error) *error = tr("Your single-player game is already open.");
        return false;
    }
    if (phase_ != Phase::Idle && phase_ != Phase::Searching) {
        if (error) *error = tr("Busy: %1").arg(phaseText_);
        return false;
    }
    if (game_ && phase_ == Phase::Searching) {
        freezeGame();
    } else if (game_) {
        GameProcess* waiting = game_;
        game_ = nullptr;
        gameFrozen_ = false;
        disconnect(waiting, nullptr, this, nullptr);
        waiting->quit();
        QElapsedTimer t;
        t.start();
        while (waiting->isRunning() && t.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        waiting->deleteLater();
    }
    auto* g = new GameProcess(gameId, 1, this);
    if (practice) g->setEnv(QStringLiteral("MK3D_TRAINING"), QStringLiteral("1"));
    watchForCrash(g, practice ? QStringLiteral("practice") : QStringLiteral("single player"));
    if (!g->start(cfg::gameExe(gameId), error, false)) {
        delete g;
        thawGame();
        return false;
    }
    solo_ = g;
    soloPractice_ = practice;
    soloFrozen_ = false;
    connect(g, &GameProcess::exited, this, [this, g] {
        if (solo_ != g) return;
        const QString closed = soloPractice_ ? tr("Practice closed") : tr("Single player closed");
        solo_ = nullptr;
        soloPractice_ = false;
        soloFrozen_ = false;
        soloHidden_.clear();
        g->deleteLater();
        if (phase_ == Phase::Solo) {
            setPhase(Phase::Idle, closed);
        } else {
            log(closed);
            // still searching: the match's game waits for it in the open again
            QString err;
            if (phase_ == Phase::Searching && !ensureGame(request_.game, &err)) log(err);
        }
        emit gameStatusChanged(gameStatus());
    });
    log(tr("Game %1 started for %2 (session %3)")
            .arg(gameId, practice ? tr("practice") : tr("single player"), g->sessionDir()));
    if (phase_ == Phase::Idle) setPhase(Phase::Solo, practice ? tr("Practicing") : tr("Playing single player"));
    else if (practice) log(tr("Practice while you wait: pick your fighter, then the dummy; a match found comes up over it"));
    else log(tr("Playing single player while the search runs: a match found comes up over it"));
    emit gameStatusChanged(gameStatus());
    emit soloStarted();
    return true;
}

// Practice while a Ranked or Kasual search waits: once the match's game is at its hold point, ready for a match, it is
// frozen there (a match found thaws it at once) and the game starts again for practice (standby takes it from there)
void MatchController::maybeStartPractice() {
    if (phase_ != Phase::Searching || solo_ || !game_ || !game_->isReady() || !cfg::practiceWhileWaiting()) return;
    QString err;
    if (!startSolo(request_.game, &err, true)) log(tr("Practice while you wait: %1").arg(err));
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
    if (!codeReady(gameId, error)) return false;
    stopLocalTest();
    const QString exe = cfg::gameExe(gameId);
    localA_ = new GameProcess(gameId, 1, this);
    localB_ = new GameProcess(gameId, 2, this);
    watchForCrash(localA_, QStringLiteral("local test"));
    watchForCrash(localB_, QStringLiteral("local test"));
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
    thawSolo();  // frozen games would not see their quit
    stopSolo();
    thawGame();
    if (game_) game_->quit();
}

void MatchController::closeGameWhenIdle() {
    closeWhenIdle_ = true;
    maybeCloseIdleGame();
}

void MatchController::maybeCloseIdleGame() {
    if (!closeWhenIdle_ || phase_ != Phase::Idle || !series_.isEmpty() || !watchQueue_.isEmpty()) return;
    closeWhenIdle_ = false;
    if (!game_ || !game_->isRunning()) return;
    log(tr("Left the lobby: its game closes"));
    thawGame();  // a frozen game would not see its quit
    GameProcess* g = game_;
    game_ = nullptr;
    disconnect(g, nullptr, this, nullptr);  // its exit is no match's business
    g->quit();
    QTimer::singleShot(6000, g, &QObject::deleteLater);
    emit gameStatusChanged(gameStatus());
}

void MatchController::shutdown() {
    prepQueue_.clear();  // a game being prepared stops with the rest, and the next start carries on
    stopLocalTest();
    if (phase_ == Phase::Searching) cancelSearch();
    thawSolo();  // frozen games would not see their quit
    thawGame();
    // guarded: an exit handler run by the wait below may delete its game (single player's does)
    QList<QPointer<GameProcess>> procs;
    for (GameProcess* g : findChildren<GameProcess*>()) procs.append(g);
    for (const QPointer<GameProcess>& g : procs)
        if (g->isRunning()) g->quit();
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 2500) {
        bool any = false;
        for (const QPointer<GameProcess>& g : procs) any = any || (g && g->isRunning());
        if (!any) break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (const QPointer<GameProcess>& g : procs)
        if (g) g->kill();
}
