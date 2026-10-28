#include "TunnelBridge.h"

#include <QNetworkDatagram>
#include <QUdpSocket>

#include "AppPaths.h"
#include "ServerClient.h"

TunnelBridge::TunnelBridge(ServerClient* server, quint16 gamePort, QObject* parent)
    : QObject(parent), server_(server), sock_(new QUdpSocket(this)), gamePort_(gamePort) {
    connect(sock_, &QUdpSocket::readyRead, this, &TunnelBridge::onReadyRead);
    connect(server_, &ServerClient::tunnelData, this, &TunnelBridge::onTunnelData);
}

bool TunnelBridge::open(QString* error) {
    if (!sock_->bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        if (error) *error = tr("Cannot open a local UDP port for the match: %1").arg(sock_->errorString());
        return false;
    }
    applog::write(QStringLiteral("tunnel: port %1 stands in for the server's relay").arg(sock_->localPort()));
    return true;
}

quint16 TunnelBridge::port() const { return sock_->localPort(); }

void TunnelBridge::onReadyRead() {
    while (sock_->hasPendingDatagrams()) {
        const QNetworkDatagram d = sock_->receiveDatagram(2048);
        // the game port's only (the launcher's punch, then the game): nothing else on this PC goes out this way
        if (d.senderPort() != gamePort_ || !d.senderAddress().isLoopback()) continue;
        server_->sendTunnel(d.data());
        ++out_;
    }
}

void TunnelBridge::onTunnelData(const QByteArray& packet) {
    sock_->writeDatagram(packet, QHostAddress(QHostAddress::LocalHost), gamePort_);
    ++in_;
}
