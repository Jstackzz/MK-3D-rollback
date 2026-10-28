#pragma once
// A match through the server's connection, for a PC whose UDP never reaches the server (a firewall's, a VPN's or a
// network's doing; docs/PROTOCOL.md 2.4.1): a UDP port on 127.0.0.1 stands in for the server's relay. What the game
// port sends to it (the launcher's punch, then the game) goes to the server in `tunnel` messages, and what comes back
// that way goes to the game port from here, so both take this port for the relay's.

#include <QHostAddress>
#include <QObject>

class QUdpSocket;
class ServerClient;

class TunnelBridge : public QObject {
    Q_OBJECT
public:
    TunnelBridge(ServerClient* server, quint16 gamePort, QObject* parent = nullptr);

    bool open(QString* error);  // a free port on 127.0.0.1
    QHostAddress address() const { return QHostAddress(QHostAddress::LocalHost); }
    quint16 port() const;
    qint64 packetsOut() const { return out_; }
    qint64 packetsIn() const { return in_; }

private:
    void onReadyRead();
    void onTunnelData(const QByteArray& packet);

    ServerClient* server_;
    QUdpSocket* sock_;
    quint16 gamePort_;
    qint64 out_ = 0, in_ = 0;
};
