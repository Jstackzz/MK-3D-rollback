#pragma once
// The spectator feed between this launcher and its game (docs/PROTOCOL.md 1.10.1): a TCP server on 127.0.0.1 the game
// connects to (its start line's feed=127.0.0.1:<port>). A player's game writes what spectators need, packets after
// their u16 length; the link sends them to the server in batches (outgoing). A spectator's game reads both players'
// packets the server passed on (deliver): everything delivered so far goes to it again whenever it connects.

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

class FeedLink : public QObject {
    Q_OBJECT
public:
    enum class Role { Player, Watcher };

    FeedLink(Role role, QObject* parent = nullptr);
    ~FeedLink() override;

    bool open(QString* error);  // listens on 127.0.0.1, a free port
    quint16 port() const;
    QString endpoint() const;   // 127.0.0.1:<port>, for the start line
    Role role() const { return role_; }
    // a spectator's: whole packets (each after its u16 length) the server passed on
    void deliver(const QByteArray& packets);
    qint64 bytes() const { return bytes_; }
    bool gameConnected() const { return sock_ != nullptr; }
    void flush();  // a player's: what came, out now (every 0.1 s by itself)

signals:
    void outgoing(const QByteArray& packets);  // a player's: whole packets its game wrote, every 0.1 s

private:
    void onConnection();
    void onReadable();

    Role role_;
    QTcpServer* server_;
    QPointer<QTcpSocket> sock_;
    QByteArray in_;       // a player's: what its game wrote and the server has not had yet
    QByteArray backlog_;  // a spectator's: everything delivered, for a game that connects (again)
    QTimer flush_;
    qint64 bytes_ = 0;
};
