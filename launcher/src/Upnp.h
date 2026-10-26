#pragma once
// Opens the game's UDP port on the home router, as Tekken 6 Native Online's launcher does: UPnP (SSDP discovery of
// the Internet Gateway Device, its description, then a SOAP AddPortMapping), so the other player's packets get in
// even where hole punching alone would not. Best effort: without UPnP, or behind a second NAT (carrier-grade NAT),
// hole punching and the server's relay still connect the match.

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QUdpSocket;

class UpnpMapper : public QObject {
    Q_OBJECT
public:
    explicit UpnpMapper(QObject* parent = nullptr);

    // keeps external UDP `port` forwarded to this PC's `port`, renewed before its lease runs out; another port
    // replaces it. `probeOnly` finds the router and asks its public address without changing anything.
    void map(quint16 port, bool probeOnly = false);
    // a router description to use instead of discovering one (development: a stand-in router)
    void setLocation(const QUrl& location) { fixedLocation_ = location; }
    QString status() const { return status_; }
    QString externalAddress() const { return externalIp_; }

signals:
    void statusChanged(const QString& status);

private:
    void discover();
    void onSsdpReadyRead();
    void onDiscoverTimeout();
    void tryNextLocation();
    void readDescription(const QUrl& location, const QByteArray& xml);
    void askExternalAddress();
    void addMapping(int leaseSeconds);
    void checkOwnMapping(quint16 port);
    void soap(const QString& action, const QString& args, std::function<void(int code, const QString& body)> done);
    void finish(const QString& text);
    void setStatus(const QString& text);

    QNetworkAccessManager* net_ = nullptr;
    QUdpSocket* ssdp_ = nullptr;
    QTimer discoverTimer_, renewTimer_;
    QUrl fixedLocation_;
    QList<QUrl> locations_;  // routers that answered, tried in turn
    bool discovering_ = false;
    QUrl controlUrl_;
    QString serviceType_;
    QHostAddress localIp_;
    quint16 port_ = 0;
    bool probeOnly_ = false;
    bool busy_ = false;
    QString externalIp_;
    QString status_;
};
