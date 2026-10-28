#pragma once
// The launcher's UDP side during match setup (docs/PROTOCOL.md section 2), on the game port:
// hello to the server's rendezvous, then punch both peer candidates and measure the round trip.

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTimer>

class QUdpSocket;

class UdpRendezvous : public QObject {
    Q_OBJECT
public:
    explicit UdpRendezvous(QObject* parent = nullptr);
    ~UdpRendezvous() override;

    // `bind` is AnyIPv4 normally; loopback when the server itself is on this PC (no firewall prompt)
    bool open(quint16 port, const QHostAddress& bind, QString* error);
    void close();
    bool isOpen() const;
    quint16 port() const { return port_; }

    void startHello(const QHostAddress& server, quint16 serverPort, const QString& token, const QString& match,
                    const QString& lan, int timeoutMs = 10000);
    int serverRttMs() const { return serverRtt_; }

    void startPunch(const QString& match, const QString& key, int side, const QString& lan, const QString& pub,
                    int timeoutMs);
    // the same through the server's relay (the two could not reach each other): the opponent's packets come and go
    // through the server's UDP port, the one the hellos went to
    void startRelayPunch(const QString& match, const QString& key, int side, int timeoutMs);
    // through the relay too, by way of the server's connection (no UDP from here reaches the server): `addr`:`port`
    // is the local port that stands in for the relay (TunnelBridge)
    void startTunnelPunch(const QString& match, const QString& key, int side, int timeoutMs, const QHostAddress& addr,
                          quint16 port);

signals:
    void helloAcked(const QString& publicEndpoint, int rttMs);
    void helloTimedOut();
    void punchDone(bool ok, int rttMs, const QString& remote, const QString& via);

private:
    struct Candidate {
        QString via;
        QHostAddress addr;
        quint16 port = 0;
    };
    enum class Phase { Idle, Punching, Pinging, Done };

    void onReadyRead();
    void onHelloTick();
    void beginPunch(int timeoutMs);
    void onPunchTick();
    void finishPunch(bool ok);
    void sendJson(const QJsonObject& obj, const QHostAddress& addr, quint16 port);
    void sendHello();

    QUdpSocket* sock_ = nullptr;
    quint16 port_ = 0;
    QHostAddress bind_;
    bool blocked_ = false;  // development (cfg::udpBlocked): nothing goes to the server's UDP port
    QElapsedTimer clock_;
    QTimer helloTimer_, punchTimer_;

    QHostAddress server_;
    quint16 serverPort_ = 0;
    QString token_, helloMatch_, lan_;
    qint64 helloStarted_ = 0;
    int helloTimeoutMs_ = 10000;
    int serverRtt_ = -1;

    QString match_, key_;
    int side_ = 0;
    QList<Candidate> candidates_;
    Phase phase_ = Phase::Idle;
    qint64 punchStarted_ = 0;
    int punchTimeoutMs_ = 10000;
    Candidate working_;
    int pingsSent_ = 0;
    qint64 lastPingAt_ = 0;
    QHash<int, qint64> pingSentAt_;
    QList<qint64> rtts_;
};
