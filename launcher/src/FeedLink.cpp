#include "FeedLink.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>

namespace {
// what may wait for a server connection that does not take it, or that a spectator's game is behind on
constexpr qsizetype kMaxBuffered = qsizetype(16) << 20;

// the whole packets at the front of a stream of [u16 length][packet], the rest left for later
qsizetype WholePackets(const QByteArray& data) {
    qsizetype at = 0;
    while (data.size() - at >= 2) {
        const qsizetype n = qFromLittleEndian<quint16>(data.constData() + at);
        if (data.size() - at - 2 < n) break;
        at += 2 + n;
    }
    return at;
}
}  // namespace

FeedLink::FeedLink(Role role, QObject* parent) : QObject(parent), role_(role), server_(new QTcpServer(this)) {
    connect(server_, &QTcpServer::newConnection, this, &FeedLink::onConnection);
    flush_.setInterval(100);
    connect(&flush_, &QTimer::timeout, this, &FeedLink::flush);
}

FeedLink::~FeedLink() {
    if (sock_) sock_->abort();
}

bool FeedLink::open(QString* error) {
    if (server_->isListening()) return true;
    if (!server_->listen(QHostAddress::LocalHost, 0)) {
        if (error) *error = tr("Spectator feed: %1").arg(server_->errorString());
        return false;
    }
    if (role_ == Role::Player) flush_.start();
    return true;
}

quint16 FeedLink::port() const { return server_->serverPort(); }

QString FeedLink::endpoint() const { return QStringLiteral("127.0.0.1:%1").arg(port()); }

// the game connects (again: a game that lost the connection retries every half second)
void FeedLink::onConnection() {
    while (QTcpSocket* s = server_->nextPendingConnection()) {
        if (sock_) sock_->abort();
        sock_ = s;
        s->setParent(this);
        s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(s, &QTcpSocket::readyRead, this, &FeedLink::onReadable);
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        if (role_ == Role::Player) in_.clear();  // a new connection starts with a whole packet
        if (role_ == Role::Watcher && !backlog_.isEmpty()) s->write(backlog_);
    }
}

void FeedLink::onReadable() {
    if (!sock_) return;
    const QByteArray data = sock_->readAll();
    if (role_ != Role::Player) return;  // a spectator's game writes nothing
    if (in_.size() + data.size() > kMaxBuffered) return;
    in_ += data;
}

void FeedLink::flush() {
    const qsizetype whole = WholePackets(in_);
    if (!whole) return;
    // a message carries at most ~9 KB of packets (the server takes lines up to 16 KB, base64 adds a third)
    qsizetype at = 0;
    while (at < whole) {
        qsizetype end = at;
        while (end < whole) {
            const qsizetype n = 2 + qFromLittleEndian<quint16>(in_.constData() + end);
            if (end > at && end + n - at > 9 * 1024) break;
            end += n;
        }
        bytes_ += end - at;
        emit outgoing(in_.mid(at, end - at));
        at = end;
    }
    in_.remove(0, whole);
}

void FeedLink::deliver(const QByteArray& packets) {
    if (packets.isEmpty() || WholePackets(packets) != packets.size()) return;  // never a part of a packet
    if (backlog_.size() + packets.size() > kMaxBuffered) return;
    backlog_ += packets;
    bytes_ += packets.size();
    if (sock_ && sock_->state() == QAbstractSocket::ConnectedState) sock_->write(packets);
}
