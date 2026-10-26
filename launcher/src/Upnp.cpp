#include "Upnp.h"

#include <QNetworkAccessManager>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUdpSocket>
#include <QXmlStreamReader>

#include "AppPaths.h"

namespace {

constexpr int kLeaseSeconds = 3600;
constexpr int kRenewMs = 20 * 60 * 1000;
constexpr int kDiscoverMs = 3000;
constexpr int kHttpTimeoutMs = 4000;
constexpr quint16 kSsdpPort = 1900;

// the gateway device, then its connection services directly: some routers answer only one of them
const char* const kSearchTargets[] = {
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
};

QByteArray searchMessage(const char* target) {
    return QByteArray("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
           target + "\r\n\r\n";
}

// a router address no mapping can open from outside: the router itself is behind another NAT
bool behindAnotherNat(const QHostAddress& a) {
    return a.isPrivateUse() || a.isLinkLocal() || a.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10);
}

QString element(const QString& body, const QString& name) {
    const QRegularExpression re(QStringLiteral("<%1>([^<]*)</%1>").arg(name));
    return re.match(body).captured(1).trimmed();
}

}  // namespace

UpnpMapper::UpnpMapper(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {
    net_->setProxy(QNetworkProxy::NoProxy);  // the router is on this network, never behind a proxy
    discoverTimer_.setSingleShot(true);
    connect(&discoverTimer_, &QTimer::timeout, this, &UpnpMapper::onDiscoverTimeout);
    renewTimer_.setInterval(kRenewMs);
    connect(&renewTimer_, &QTimer::timeout, this, [this] {
        if (port_) map(port_, probeOnly_);
    });
}

void UpnpMapper::map(quint16 port, bool probeOnly) {
    port_ = port;
    probeOnly_ = probeOnly;
    if (busy_) return;  // the attempt under way maps port_ when it gets there
    busy_ = true;
    if (controlUrl_.isValid()) {
        askExternalAddress();  // the router is known: map again straight away
    } else if (fixedLocation_.isValid()) {
        locations_ = {fixedLocation_};
        tryNextLocation();
    } else {
        discover();
    }
}

void UpnpMapper::discover() {
    setStatus(tr("looking for the router (UPnP)"));
    locations_.clear();
    delete ssdp_;
    ssdp_ = new QUdpSocket(this);
    if (!ssdp_->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        finish(tr("no UPnP (%1)").arg(ssdp_->errorString()));
        return;
    }
    connect(ssdp_, &QUdpSocket::readyRead, this, &UpnpMapper::onSsdpReadyRead);
    const QHostAddress group(QStringLiteral("239.255.255.250"));
    // on every network adapter, so a VPN or virtual adapter being the default one does not hide the router
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) ||
            !flags.testFlag(QNetworkInterface::CanMulticast) || flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        const auto entries = iface.addressEntries();
        if (std::none_of(entries.cbegin(), entries.cend(), [](const QNetworkAddressEntry& e) {
                return e.ip().protocol() == QAbstractSocket::IPv4Protocol;
            }))
            continue;
        ssdp_->setMulticastInterface(iface);
        for (const char* target : kSearchTargets) ssdp_->writeDatagram(searchMessage(target), group, kSsdpPort);
    }
    discovering_ = true;
    discoverTimer_.start(kDiscoverMs);
}

void UpnpMapper::onSsdpReadyRead() {
    static const QRegularExpression locationRe(QStringLiteral("^LOCATION:[ \\t]*(\\S+)"),
                                               QRegularExpression::CaseInsensitiveOption |
                                                   QRegularExpression::MultilineOption);
    while (ssdp_ && ssdp_->hasPendingDatagrams()) {
        const QNetworkDatagram d = ssdp_->receiveDatagram();
        const QUrl url(locationRe.match(QString::fromLatin1(d.data())).captured(1));
        if (url.isValid() && url.scheme() == QLatin1String("http") && !locations_.contains(url)) locations_.append(url);
    }
}

void UpnpMapper::onDiscoverTimeout() {
    discovering_ = false;
    if (locations_.isEmpty()) {
        finish(tr("no UPnP router answered"));
        return;
    }
    tryNextLocation();
}

void UpnpMapper::tryNextLocation() {
    if (locations_.isEmpty()) {
        finish(tr("the router offers no port mapping (UPnP)"));
        return;
    }
    const QUrl location = locations_.takeFirst();
    QNetworkRequest req(location);
    req.setTransferTimeout(kHttpTimeoutMs);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, location] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            applog::write(QStringLiteral("upnp: %1: %2").arg(location.toString(), reply->errorString()));
            tryNextLocation();
            return;
        }
        readDescription(location, reply->readAll());
    });
}

void UpnpMapper::readDescription(const QUrl& location, const QByteArray& body) {
    QXmlStreamReader xml(body);
    QUrl base = location;
    QString type, control, curType, curControl;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == u"URLBase") {
                const QUrl b(xml.readElementText().trimmed());
                if (b.isValid() && !b.isEmpty()) base = b;
            } else if (xml.name() == u"service") {
                curType.clear();
                curControl.clear();
            } else if (xml.name() == u"serviceType") {
                curType = xml.readElementText().trimmed();
            } else if (xml.name() == u"controlURL") {
                curControl = xml.readElementText().trimmed();
            }
        } else if (xml.isEndElement() && xml.name() == u"service" && type.isEmpty() && !curControl.isEmpty() &&
                   (curType.contains(QLatin1String(":WANIPConnection:")) ||
                    curType.contains(QLatin1String(":WANPPPConnection:")))) {
            type = curType;
            control = curControl;
        }
    }
    if (type.isEmpty()) {
        tryNextLocation();
        return;
    }
    serviceType_ = type;
    controlUrl_ = base.resolved(QUrl(control));
    // this PC's address on the router's network: where the router forwards the port
    QUdpSocket route;
    route.connectToHost(QHostAddress(controlUrl_.host()), quint16(controlUrl_.port(80)));
    route.waitForConnected(1000);
    localIp_ = route.localAddress();
    if (localIp_.isNull() || localIp_.protocol() != QAbstractSocket::IPv4Protocol) {
        controlUrl_.clear();
        finish(tr("no route to the router at %1").arg(location.host()));
        return;
    }
    applog::write(QStringLiteral("upnp: router %1, %2 at %3, this PC %4")
                      .arg(location.toString(), serviceType_, controlUrl_.toString(), localIp_.toString()));
    askExternalAddress();
}

void UpnpMapper::askExternalAddress() {
    soap(QStringLiteral("GetExternalIPAddress"), QString(), [this](int code, const QString& body) {
        if (code == 200) externalIp_ = element(body, QStringLiteral("NewExternalIPAddress"));
        const QHostAddress external(externalIp_);
        if (!external.isNull() && behindAnotherNat(external)) {
            finish(tr("the router is behind another network (its address %1 is not public), so ports cannot be "
                      "opened; matches connect by hole punching or through the server").arg(externalIp_));
            return;
        }
        if (probeOnly_) {
            finish(tr("router found (UPnP), public address %1; nothing changed")
                       .arg(externalIp_.isEmpty() ? tr("unknown") : externalIp_));
            return;
        }
        addMapping(kLeaseSeconds);
    });
}

void UpnpMapper::addMapping(int leaseSeconds) {
    const quint16 port = port_;
    const QString args =
        QStringLiteral("<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort><NewProtocol>UDP"
                       "</NewProtocol><NewInternalPort>%1</NewInternalPort><NewInternalClient>%2</NewInternalClient>"
                       "<NewEnabled>1</NewEnabled><NewPortMappingDescription>MK3D Online</NewPortMappingDescription>"
                       "<NewLeaseDuration>%3</NewLeaseDuration>")
            .arg(port)
            .arg(localIp_.toString())
            .arg(leaseSeconds);
    soap(QStringLiteral("AddPortMapping"), args, [this, port, leaseSeconds](int code, const QString& body) {
        if (code == 200) {
            renewTimer_.start();
            finish(tr("UDP port %1 open on the router (UPnP)%2")
                       .arg(port)
                       .arg(externalIp_.isEmpty() ? QString() : tr(", public address %1").arg(externalIp_)));
            return;
        }
        const int error = element(body, QStringLiteral("errorCode")).toInt();
        if (error == 725 && leaseSeconds != 0) {
            addMapping(0);  // OnlyPermanentLeasesSupported
            return;
        }
        if (error == 718) {
            checkOwnMapping(port);  // ConflictInMappingEntry: maybe this PC's own, from before
            return;
        }
        finish(tr("the router did not open UDP port %1 (UPnP error %2)").arg(port).arg(error ? error : code));
    });
}

void UpnpMapper::checkOwnMapping(quint16 port) {
    const QString args = QStringLiteral("<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort>"
                                        "<NewProtocol>UDP</NewProtocol>").arg(port);
    soap(QStringLiteral("GetSpecificPortMappingEntry"), args, [this, port](int code, const QString& body) {
        const QString client = element(body, QStringLiteral("NewInternalClient"));
        if (code == 200 && QHostAddress(client) == localIp_) {
            renewTimer_.start();
            finish(tr("UDP port %1 open on the router (UPnP)").arg(port));
            return;
        }
        finish(tr("UDP port %1 is forwarded to another device on your network (%2): choose another game port in "
                  "Settings").arg(port).arg(client.isEmpty() ? tr("unknown") : client));
    });
}

void UpnpMapper::soap(const QString& action, const QString& args,
                      std::function<void(int code, const QString& body)> done) {
    const QByteArray envelope =
        QStringLiteral("<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%1 xmlns:u=\"%2\">%3"
                       "</u:%1></s:Body></s:Envelope>\r\n")
            .arg(action, serviceType_, args)
            .toUtf8();
    QNetworkRequest req(controlUrl_);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
    req.setRawHeader("SOAPAction", QStringLiteral("\"%1#%2\"").arg(serviceType_, action).toUtf8());
    req.setTransferTimeout(kHttpTimeoutMs);
    QNetworkReply* reply = net_->post(req, envelope);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        done(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), QString::fromUtf8(reply->readAll()));
    });
}

void UpnpMapper::finish(const QString& text) {
    busy_ = false;
    setStatus(text);
}

void UpnpMapper::setStatus(const QString& text) {
    if (text == status_) return;
    status_ = text;
    applog::write(QStringLiteral("upnp: ") + text);
    emit statusChanged(text);
}
