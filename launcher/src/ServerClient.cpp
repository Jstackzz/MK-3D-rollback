#include "ServerClient.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QSslCertificate>
#include <QSslConfiguration>

#include "AppPaths.h"

namespace {
constexpr int kMaxLine = 64 * 1024;
const char* kClientName = "mk3d-launcher";

QHostAddress plainV4(const QHostAddress& a) {
    bool ok = false;
    const quint32 v4 = a.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : a;
}
}  // namespace

ServerClient::ServerClient(QObject* parent) : QObject(parent), sock_(new QSslSocket(this)) {
    QSslConfiguration conf = sock_->sslConfiguration();
    conf.setProtocol(QSsl::TlsV1_2OrLater);
    // The CA chain is not used: trust comes from the pinned fingerprint, checked in onEncrypted()
    // before a single byte of application data is sent.
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);
    sock_->setSslConfiguration(conf);

    connect(sock_, &QSslSocket::connected, this, &ServerClient::onConnected);
    connect(sock_, &QSslSocket::encrypted, this, &ServerClient::onEncrypted);
    connect(sock_, &QSslSocket::readyRead, this, &ServerClient::onReadyRead);
    connect(sock_, &QSslSocket::disconnected, this, &ServerClient::onDisconnected);
    connect(sock_, &QSslSocket::errorOccurred, this, &ServerClient::onSocketError);
    connect(sock_, &QSslSocket::sslErrors, this, &ServerClient::onSslErrors);

    pingTimer_.setInterval(30000);
    connect(&pingTimer_, &QTimer::timeout, this, &ServerClient::sendPing);
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, [this] {
        if (!userDisconnect_ && state_ == State::Offline) connectTo(address_, fingerprint_);
    });
}

QString ServerClient::normalizeFingerprint(const QString& text) {
    QString out;
    for (QChar c : text.toLower())
        if ((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))
            out.append(c);
    return out;
}

QString ServerClient::prettyFingerprint(const QString& hex) {
    const QString h = normalizeFingerprint(hex).toUpper();
    QStringList parts;
    for (int i = 0; i + 1 < h.size(); i += 2) parts << h.mid(i, 2);
    return parts.join(QLatin1Char(':'));
}

void ServerClient::connectTo(const QString& address, const QString& fingerprint) {
    reconnectTimer_.stop();
    userDisconnect_ = false;
    pinRejected_ = false;
    address_ = address.trimmed();
    fingerprint_ = normalizeFingerprint(fingerprint);
    QString rest = address_;
    plain_ = false;
    if (rest.startsWith(QLatin1String("tcp://"))) {
        plain_ = true;
        rest = rest.mid(6);
    } else if (rest.startsWith(QLatin1String("tls://"))) {
        rest = rest.mid(6);
    }
    const int colon = rest.lastIndexOf(QLatin1Char(':'));
    host_ = colon > 0 ? rest.left(colon) : rest;
    port_ = colon > 0 ? static_cast<quint16>(rest.mid(colon + 1).toUInt()) : 7777;
    if (port_ == 0) port_ = 7777;

    sock_->abort();
    reconnectTimer_.stop();  // abort() may have scheduled a reconnect for the old connection
    buffer_.clear();
    welcome_ = {};
    account_ = {};
    setState(State::Connecting);
    applog::write(QStringLiteral("server: connecting to %1:%2 (%3)").arg(host_).arg(port_).arg(plain_ ? "plain" : "tls"));
    if (plain_)
        sock_->connectToHost(host_, port_, QIODevice::ReadWrite, QAbstractSocket::IPv4Protocol);
    else
        sock_->connectToHostEncrypted(host_, port_, QIODevice::ReadWrite, QAbstractSocket::IPv4Protocol);
}

void ServerClient::disconnectFromServer() {
    userDisconnect_ = true;
    reconnectTimer_.stop();
    sock_->disconnectFromHost();
    if (sock_->state() != QAbstractSocket::UnconnectedState) sock_->abort();
    setState(State::Offline);
}

void ServerClient::setState(State s) {
    if (state_ == s) return;
    state_ = s;
    emit stateChanged(s);
}

QString ServerClient::accountName() const { return account_.value(QStringLiteral("name")).toString(); }
QString ServerClient::udpToken() const { return account_.value(QStringLiteral("udp_token")).toString(); }
QHostAddress ServerClient::serverAddress() const { return plainV4(sock_->peerAddress()); }
quint16 ServerClient::serverUdpPort() const {
    return static_cast<quint16>(welcome_.value(QStringLiteral("udp_port")).toInt(7778));
}
QHostAddress ServerClient::localAddress() const { return plainV4(sock_->localAddress()); }

void ServerClient::sendPing() {
    pingId_ = send({{QStringLiteral("type"), QStringLiteral("ping")}});
    pingClock_.start();
}

// the interface that holds the address this PC talks to the server from
QString ServerClient::linkType() const {
    const QHostAddress local = localAddress();
    if (local.isNull() || local.isLoopback()) return QString();
    for (const QNetworkInterface& nif : QNetworkInterface::allInterfaces())
        for (const QNetworkAddressEntry& e : nif.addressEntries())
            if (plainV4(e.ip()) == local) {
                switch (nif.type()) {
                    case QNetworkInterface::Ethernet: return QStringLiteral("ethernet");
                    case QNetworkInterface::Wifi: return QStringLiteral("wifi");
                    default: return QStringLiteral("other");
                }
            }
    return QString();
}

void ServerClient::onConnected() {
    if (plain_) sendHello();  // TLS waits for encrypted()
}

void ServerClient::onEncrypted() {
    const QSslCertificate cert = sock_->peerCertificate();
    peerFingerprint_ = QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex());
    if (fingerprint_.isEmpty()) {
        applog::write(QStringLiteral("server: certificate %1 is not trusted yet").arg(peerFingerprint_));
        pinRejected_ = true;
        userDisconnect_ = true;  // wait for the user's decision instead of reconnecting
        sock_->abort();
        emit untrustedCertificate(peerFingerprint_);
        return;
    }
    if (peerFingerprint_ != fingerprint_) {
        applog::write(QStringLiteral("server: certificate mismatch, got %1 expected %2").arg(peerFingerprint_, fingerprint_));
        pinRejected_ = true;
        userDisconnect_ = true;  // do not hammer a server that is not the pinned one
        sock_->abort();
        emit connectionError(tr("The server's certificate (%1) does not match the pinned fingerprint. "
                                "Check the fingerprint in Settings.").arg(prettyFingerprint(peerFingerprint_)));
        return;
    }
    sendHello();
}

void ServerClient::onSslErrors(const QList<QSslError>& errors) {
    const QString fp = QString::fromLatin1(sock_->peerCertificate().digest(QCryptographicHash::Sha256).toHex());
    if (!fingerprint_.isEmpty() && fp == fingerprint_) {
        sock_->ignoreSslErrors(errors);  // self-signed is expected; the pin is the trust anchor
    } else if (fingerprint_.isEmpty()) {
        sock_->ignoreSslErrors(errors);  // let onEncrypted() report the fingerprint for trust-on-first-use
    }
}

void ServerClient::sendHello() {
    reconnectDelayMs_ = 3000;
    QJsonObject hello{{QStringLiteral("type"), QStringLiteral("hello")},
                      {QStringLiteral("proto"), 1},
                      {QStringLiteral("client"), QLatin1String(kClientName)},
                      {QStringLiteral("version"), QStringLiteral(MK3D_LAUNCHER_VERSION)},
                      {QStringLiteral("os"), QStringLiteral("windows")}};
    send(hello);
}

int ServerClient::send(QJsonObject msg) {
    if (sock_->state() != QAbstractSocket::ConnectedState) return -1;
    const int id = nextId_++;
    msg.insert(QStringLiteral("id"), id);
    sock_->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
    return id;
}

int ServerClient::login(const QString& name, const QString& password) {
    return send({{QStringLiteral("type"), QStringLiteral("login")}, {QStringLiteral("name"), name},
                 {QStringLiteral("password"), password}});
}

int ServerClient::registerAccount(const QString& name, const QString& password) {
    return send({{QStringLiteral("type"), QStringLiteral("register")}, {QStringLiteral("name"), name},
                 {QStringLiteral("password"), password}});
}

void ServerClient::logout() {
    resumeToken_.clear();
    send({{QStringLiteral("type"), QStringLiteral("logout")}});
}

void ServerClient::onReadyRead() {
    buffer_.append(sock_->readAll());
    for (;;) {
        const qsizetype nl = buffer_.indexOf('\n');
        if (nl < 0) break;
        const QByteArray line = buffer_.left(nl).trimmed();
        buffer_.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) handle(doc.object());
    }
    if (buffer_.size() > kMaxLine) {
        applog::write(QStringLiteral("server: line too long, dropping connection"));
        sock_->abort();
    }
}

void ServerClient::handle(const QJsonObject& msg) {
    const QString type = msg.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("welcome")) {
        welcome_ = msg;
        setState(State::Online);
        pingTimer_.start();
        // a first round trip now, not in 30 s, and another once the login's own work on the server (a password's
        // hash) cannot delay the answer any more
        sendPing();
        QTimer::singleShot(2000, this, [this] {
            if (isOnline()) sendPing();
        });
        applog::write(QStringLiteral("server: welcome from %1 %2, %3 online")
                          .arg(msg.value(QStringLiteral("server")).toString(), msg.value(QStringLiteral("version")).toString())
                          .arg(msg.value(QStringLiteral("online")).toInt()));
        if (!resumeToken_.isEmpty())
            send({{QStringLiteral("type"), QStringLiteral("auth")}, {QStringLiteral("token"), resumeToken_}});
    } else if (type == QLatin1String("account")) {
        account_ = msg;
        resumeToken_ = msg.value(QStringLiteral("token")).toString();
        setState(State::LoggedIn);
        applog::write(QStringLiteral("server: logged in as %1").arg(accountName()));
        emit loggedIn(msg);
    } else if (type == QLatin1String("pong") && pingId_ >= 0 && msg.value(QStringLiteral("id")).toInt(-2) == pingId_) {
        rttMs_ = static_cast<int>(pingClock_.elapsed());
        pingId_ = -1;
    } else if (type == QLatin1String("logged_out")) {
        account_ = {};
        setState(State::Online);
        emit loggedOut();
    } else if (type == QLatin1String("error")) {
        const QString code = msg.value(QStringLiteral("code")).toString();
        const QString re = msg.value(QStringLiteral("re")).toString();
        applog::write(QStringLiteral("server: error %1 (%2): %3").arg(code, re, msg.value(QStringLiteral("message")).toString()));
        if (re == QLatin1String("auth")) {
            resumeToken_.clear();
            emit resumeFailed();
        } else if (code == QLatin1String("logged_in_elsewhere")) {
            account_ = {};
            resumeToken_.clear();
            userDisconnect_ = true;  // do not log back in and kick the other place in turn
            emit kicked(msg.value(QStringLiteral("message")).toString());
        } else if (code == QLatin1String("proto_mismatch") || code == QLatin1String("client_outdated")) {
            userDisconnect_ = true;
            emit connectionError(msg.value(QStringLiteral("message")).toString());
        }
    }
    emit message(msg);
}

void ServerClient::onDisconnected() {
    pingTimer_.stop();
    const bool wasLoggedIn = state_ == State::LoggedIn;
    account_ = {};
    setState(State::Offline);
    if (wasLoggedIn) emit loggedOut();
    applog::write(QStringLiteral("server: disconnected"));
    scheduleReconnect();
}

void ServerClient::onSocketError(QAbstractSocket::SocketError error) {
    if (error == QAbstractSocket::RemoteHostClosedError && state_ != State::Connecting) return;  // onDisconnected
    if (pinRejected_) return;
    applog::write(QStringLiteral("server: socket error %1: %2").arg(int(error)).arg(sock_->errorString()));
    emit connectionError(sock_->errorString());
    if (sock_->state() == QAbstractSocket::UnconnectedState) {
        setState(State::Offline);
        scheduleReconnect();
    }
}

void ServerClient::scheduleReconnect() {
    if (userDisconnect_ || reconnectTimer_.isActive()) return;
    reconnectTimer_.start(reconnectDelayMs_);
    reconnectDelayMs_ = qMin(reconnectDelayMs_ * 2, 30000);
}
