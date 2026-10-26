#include "UdpRendezvous.h"

#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QUdpSocket>

#include <algorithm>

#include "AppPaths.h"

namespace {
constexpr int kPings = 5;
constexpr int kMinPongs = 3;

QHostAddress plainV4(const QHostAddress& a) {
    bool ok = false;
    const quint32 v4 = a.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : a;
}

bool parseEndpoint(const QString& text, QHostAddress* addr, quint16* port) {
    const int colon = text.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    QHostAddress a(text.left(colon));
    const uint p = text.mid(colon + 1).toUInt();
    if (a.isNull() || a.protocol() != QAbstractSocket::IPv4Protocol || p == 0 || p > 65535) return false;
    *addr = a;
    *port = static_cast<quint16>(p);
    return true;
}
}  // namespace

UdpRendezvous::UdpRendezvous(QObject* parent) : QObject(parent) {
    clock_.start();
    helloTimer_.setInterval(250);
    connect(&helloTimer_, &QTimer::timeout, this, &UdpRendezvous::onHelloTick);
    punchTimer_.setInterval(100);
    connect(&punchTimer_, &QTimer::timeout, this, &UdpRendezvous::onPunchTick);
}

UdpRendezvous::~UdpRendezvous() { close(); }

bool UdpRendezvous::open(quint16 port, const QHostAddress& bind, QString* error) {
    if (sock_ && port_ == port && bind_ == bind) return true;
    close();
    sock_ = new QUdpSocket(this);
    if (!sock_->bind(bind, port)) {
        if (error) *error = tr("Cannot use UDP port %1: %2").arg(port).arg(sock_->errorString());
        delete sock_;
        sock_ = nullptr;
        return false;
    }
    port_ = port;
    bind_ = bind;
    connect(sock_, &QUdpSocket::readyRead, this, &UdpRendezvous::onReadyRead);
    applog::write(QStringLiteral("udp: bound %1:%2").arg(bind.toString()).arg(port));
    return true;
}

void UdpRendezvous::close() {
    helloTimer_.stop();
    punchTimer_.stop();
    phase_ = Phase::Idle;
    match_.clear();
    key_.clear();
    if (sock_) {
        sock_->close();
        sock_->deleteLater();
        sock_ = nullptr;
        applog::write(QStringLiteral("udp: released port %1").arg(port_));
    }
}

bool UdpRendezvous::isOpen() const { return sock_ != nullptr; }

void UdpRendezvous::sendJson(const QJsonObject& obj, const QHostAddress& addr, quint16 port) {
    if (!sock_ || addr.isNull() || port == 0) return;
    sock_->writeDatagram(QJsonDocument(obj).toJson(QJsonDocument::Compact), addr, port);
}

void UdpRendezvous::startHello(const QHostAddress& server, quint16 serverPort, const QString& token,
                               const QString& match, const QString& lan, int timeoutMs) {
    server_ = plainV4(server);
    serverPort_ = serverPort;
    token_ = token;
    helloMatch_ = match;
    lan_ = lan;
    helloTimeoutMs_ = timeoutMs;
    helloStarted_ = clock_.elapsed();
    sendHello();
    helloTimer_.start();
}

void UdpRendezvous::sendHello() {
    sendJson({{QStringLiteral("type"), QStringLiteral("hello")},
              {QStringLiteral("token"), token_},
              {QStringLiteral("match"), helloMatch_},
              {QStringLiteral("lan"), lan_},
              {QStringLiteral("t"), clock_.elapsed()}},
             server_, serverPort_);
}

void UdpRendezvous::onHelloTick() {
    if (clock_.elapsed() - helloStarted_ > helloTimeoutMs_) {
        helloTimer_.stop();
        applog::write(QStringLiteral("udp: no hello_ack from %1:%2").arg(server_.toString()).arg(serverPort_));
        emit helloTimedOut();
        return;
    }
    sendHello();
}

void UdpRendezvous::startPunch(const QString& match, const QString& key, int side, const QString& lan,
                               const QString& pub, int timeoutMs) {
    match_ = match;
    key_ = key;
    side_ = side;
    candidates_.clear();
    for (const auto& [via, text] : {std::pair{QStringLiteral("lan"), lan}, std::pair{QStringLiteral("public"), pub}}) {
        Candidate c;
        c.via = via;
        if (!parseEndpoint(text, &c.addr, &c.port)) continue;
        const bool dup = std::any_of(candidates_.cbegin(), candidates_.cend(),
                                     [&](const Candidate& o) { return o.addr == c.addr && o.port == c.port; });
        if (!dup) candidates_.append(c);
    }
    beginPunch(timeoutMs);
}

void UdpRendezvous::startRelayPunch(const QString& match, const QString& key, int side, int timeoutMs) {
    match_ = match;
    key_ = key;
    side_ = side;
    candidates_ = {Candidate{QStringLiteral("relay"), server_, serverPort_}};
    beginPunch(timeoutMs);
}

void UdpRendezvous::beginPunch(int timeoutMs) {
    punchTimeoutMs_ = timeoutMs;
    punchStarted_ = clock_.elapsed();
    pingsSent_ = 0;
    pingSentAt_.clear();
    rtts_.clear();
    phase_ = candidates_.isEmpty() ? Phase::Done : Phase::Punching;
    if (phase_ == Phase::Done) {
        emit punchDone(false, 0, QString(), QString());
        return;
    }
    onPunchTick();
    punchTimer_.start();
}

void UdpRendezvous::onPunchTick() {
    const qint64 now = clock_.elapsed();
    if (phase_ == Phase::Punching) {
        if (now - punchStarted_ > punchTimeoutMs_) {
            finishPunch(false);
            return;
        }
        for (const auto& c : candidates_)
            sendJson({{QStringLiteral("type"), QStringLiteral("punch")}, {QStringLiteral("match"), match_},
                      {QStringLiteral("key"), key_}, {QStringLiteral("side"), side_}},
                     c.addr, c.port);
    } else if (phase_ == Phase::Pinging) {
        if (pingsSent_ < kPings) {
            pingSentAt_.insert(pingsSent_, now);
            sendJson({{QStringLiteral("type"), QStringLiteral("ping")}, {QStringLiteral("match"), match_},
                      {QStringLiteral("key"), key_}, {QStringLiteral("seq"), pingsSent_}, {QStringLiteral("t"), now}},
                     working_.addr, working_.port);
            ++pingsSent_;
            lastPingAt_ = now;
        } else if (rtts_.size() >= kPings || now - lastPingAt_ > 500) {
            finishPunch(rtts_.size() >= kMinPongs);
        }
    }
}

void UdpRendezvous::finishPunch(bool ok) {
    phase_ = Phase::Done;
    punchTimer_.stop();
    int rtt = 0;
    if (ok && !rtts_.isEmpty()) {
        QList<qint64> sorted = rtts_;
        std::sort(sorted.begin(), sorted.end());
        rtt = static_cast<int>(sorted.at(sorted.size() / 2));
    }
    const QString remote = ok ? QStringLiteral("%1:%2").arg(working_.addr.toString()).arg(working_.port) : QString();
    applog::write(QStringLiteral("udp: punch %1, rtt %2 ms via %3 %4").arg(ok ? "ok" : "failed").arg(rtt).arg(working_.via, remote));
    emit punchDone(ok, rtt, remote, ok ? working_.via : QString());
}

void UdpRendezvous::onReadyRead() {
    while (sock_ && sock_->hasPendingDatagrams()) {
        const QNetworkDatagram d = sock_->receiveDatagram(2048);
        const QHostAddress from = plainV4(d.senderAddress());
        const quint16 fromPort = static_cast<quint16>(d.senderPort());
        QJsonParseError err{};
        const QJsonObject o = QJsonDocument::fromJson(d.data(), &err).object();
        if (err.error != QJsonParseError::NoError) continue;
        const QString type = o.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("hello_ack")) {
            if (!helloTimer_.isActive()) continue;
            helloTimer_.stop();
            serverRtt_ = static_cast<int>(qMax<qint64>(0, clock_.elapsed() - o.value(QStringLiteral("t")).toInteger()));
            const QString pub = o.value(QStringLiteral("public")).toString();
            applog::write(QStringLiteral("udp: hello_ack public %1, server rtt %2 ms").arg(pub).arg(serverRtt_));
            emit helloAcked(pub, serverRtt_);
            continue;
        }
        if (key_.isEmpty() || o.value(QStringLiteral("match")).toString() != match_ ||
            o.value(QStringLiteral("key")).toString() != key_)
            continue;  // not our match: ignore
        if (type == QLatin1String("punch") && phase_ == Phase::Punching &&
            std::none_of(candidates_.cbegin(), candidates_.cend(),
                         [&](const Candidate& c) { return c.addr == from && c.port == fromPort; })) {
            // the peer's router sends it from another public port than the server saw: punch that one too, or
            // only one side would ever hear back
            candidates_.append(Candidate{QStringLiteral("public"), from, fromPort});
            applog::write(QStringLiteral("udp: punch from %1:%2, added as a candidate").arg(from.toString()).arg(fromPort));
        }
        if (type == QLatin1String("punch") || type == QLatin1String("ping")) {
            QJsonObject reply = o;
            reply.insert(QStringLiteral("type"), type == QLatin1String("punch") ? QStringLiteral("punch_ack")
                                                                                : QStringLiteral("pong"));
            reply.insert(QStringLiteral("side"), side_);
            sendJson(reply, from, fromPort);
        } else if (type == QLatin1String("punch_ack") && phase_ == Phase::Punching) {
            working_ = Candidate{QStringLiteral("public"), from, fromPort};
            for (const auto& c : candidates_)
                if (c.addr == from && c.port == fromPort) working_.via = c.via;
            phase_ = Phase::Pinging;
            pingsSent_ = 0;
            lastPingAt_ = clock_.elapsed();
        } else if (type == QLatin1String("pong") && phase_ == Phase::Pinging) {
            const int seq = o.value(QStringLiteral("seq")).toInt(-1);
            if (pingSentAt_.contains(seq)) {
                rtts_.append(clock_.elapsed() - pingSentAt_.take(seq));
                if (rtts_.size() >= kPings) finishPunch(true);
            }
        }
    }
}
