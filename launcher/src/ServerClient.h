#pragma once
// One TLS connection to the MK3D server (docs/PROTOCOL.md section 1): NDJSON framing, pinned
// SHA-256 certificate fingerprint, hello/welcome, token resume, keepalive and reconnect.

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSslSocket>
#include <QTimer>

class ServerClient : public QObject {
    Q_OBJECT
public:
    enum class State { Offline, Connecting, Online, LoggedIn };
    Q_ENUM(State)

    explicit ServerClient(QObject* parent = nullptr);

    // "host:port", "tls://host:port" or (development) "tcp://host:port"
    void connectTo(const QString& address, const QString& fingerprint);
    void disconnectFromServer();
    void setResumeToken(const QString& token) { resumeToken_ = token; }

    State state() const { return state_; }
    bool isOnline() const { return state_ == State::Online || state_ == State::LoggedIn; }
    bool isLoggedIn() const { return state_ == State::LoggedIn; }
    QString address() const { return address_; }
    QString accountName() const;
    QString udpToken() const;
    QJsonObject account() const { return account_; }
    QJsonObject welcome() const { return welcome_; }
    QString peerFingerprint() const { return peerFingerprint_; }
    QHostAddress serverAddress() const;
    quint16 serverUdpPort() const;
    QHostAddress localAddress() const;
    // the last round trip of a ping to the server (ms, -1 not measured yet), and the kind of network this PC reaches
    // it through: "ethernet", "wifi", "other" ("" unknown); both go to the server with searches and lobbies, which
    // show them to the opponent
    int rttMs() const { return rttMs_; }
    QString linkType() const;

    int send(QJsonObject msg);  // adds and returns a request id; -1 when offline
    int login(const QString& name, const QString& password);
    int registerAccount(const QString& name, const QString& password);
    void logout();

    static QString normalizeFingerprint(const QString& text);
    static QString prettyFingerprint(const QString& hex);

signals:
    void stateChanged(ServerClient::State state);
    void message(const QJsonObject& msg);
    void connectionError(const QString& text);
    void untrustedCertificate(const QString& fingerprint);
    void loggedIn(const QJsonObject& account);
    void loggedOut();
    void resumeFailed();
    void kicked(const QString& text);

private:
    void setState(State s);
    void onConnected();
    void onEncrypted();
    void onReadyRead();
    void onDisconnected();
    void onSocketError(QAbstractSocket::SocketError error);
    void onSslErrors(const QList<QSslError>& errors);
    void handle(const QJsonObject& msg);
    void sendHello();
    void scheduleReconnect();

    QSslSocket* sock_;
    QTimer pingTimer_;
    void sendPing();
    int pingId_ = -1, rttMs_ = -1;
    QElapsedTimer pingClock_;
    QTimer reconnectTimer_;
    QByteArray buffer_;
    State state_ = State::Offline;
    QString address_, host_, fingerprint_, peerFingerprint_, resumeToken_;
    quint16 port_ = 7777;
    bool plain_ = false;
    bool userDisconnect_ = true;
    bool pinRejected_ = false;
    int reconnectDelayMs_ = 3000;
    int nextId_ = 1;
    QJsonObject welcome_, account_;
};
