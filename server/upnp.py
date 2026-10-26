"""Opens the server's ports on the home router (UPnP Internet Gateway Device: SSDP discovery, the device description,
SOAP AddPortMapping), so a server on a home PC needs no manual port forwarding. Standard library only; the calls
block, so the server runs them in a thread. The launcher does the same for the game port (launcher/src/Upnp.cpp)."""

from __future__ import annotations

import ipaddress
import logging
import re
import socket
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass

log = logging.getLogger("mk3d.upnp")

SSDP = ("239.255.255.250", 1900)
SEARCH_TARGETS = (
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
)
LEASE_SECONDS = 3600
TIMEOUT = 4.0


class UpnpError(Exception):
    pass


@dataclass
class Gateway:
    control_url: str
    service_type: str
    local_ip: str  # this machine's address on the router's network


def discover(timeout: float = 3.0, location: str | None = None) -> Gateway:
    """The first router that answers with a WAN connection service (`location`: a description to use instead)."""
    locations = [location] if location else _search(timeout)
    if not locations:
        raise UpnpError("no UPnP router answered")
    for loc in locations:
        try:
            with urllib.request.urlopen(loc, timeout=TIMEOUT) as r:
                gateway = _read_description(loc, r.read())
        except (OSError, ET.ParseError) as exc:
            log.debug("upnp: %s: %s", loc, exc)
            continue
        if gateway is not None:
            return gateway
    raise UpnpError("the router offers no port mapping (UPnP)")


def _search(timeout: float) -> list[str]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        sock.settimeout(timeout)
        for target in SEARCH_TARGETS:
            msg = (f'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 2\r\n'
                   f"ST: {target}\r\n\r\n")
            sock.sendto(msg.encode(), SSDP)
        found: list[str] = []
        while True:
            try:
                data, _ = sock.recvfrom(4096)
            except OSError:  # the timeout
                return found
            m = re.search(rb"^location:[ \t]*(\S+)", data, re.IGNORECASE | re.MULTILINE)
            if m:
                url = m.group(1).decode("latin-1")
                if url.startswith("http://") and url not in found:
                    found.append(url)
    finally:
        sock.close()


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _read_description(location: str, body: bytes) -> Gateway | None:
    root = ET.fromstring(body)
    base = location
    for el in root.iter():
        if _local_name(el.tag) == "URLBase" and (el.text or "").strip():
            base = el.text.strip()
    for service in root.iter():
        if _local_name(service.tag) != "service":
            continue
        fields = {_local_name(child.tag): (child.text or "").strip() for child in service}
        stype, control = fields.get("serviceType", ""), fields.get("controlURL", "")
        if control and (":WANIPConnection:" in stype or ":WANPPPConnection:" in stype):
            url = urllib.parse.urljoin(base, control)
            return Gateway(url, stype, _route_address(urllib.parse.urlsplit(url)))
    return None


def _route_address(parts: urllib.parse.SplitResult) -> str:
    """This machine's address towards the router: what the router forwards to."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((parts.hostname or "", parts.port or 80))
        return sock.getsockname()[0]
    finally:
        sock.close()


def _soap(gateway: Gateway, action: str, args: str = "") -> str:
    body = (f'<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            f's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:{action} '
            f'xmlns:u="{gateway.service_type}">{args}</u:{action}></s:Body></s:Envelope>\r\n').encode()
    req = urllib.request.Request(gateway.control_url, data=body, method="POST", headers={
        "Content-Type": 'text/xml; charset="utf-8"',
        "SOAPAction": f'"{gateway.service_type}#{action}"',
    })
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8", "replace")
        m = re.search(r"<errorCode>(\d+)</errorCode>", text)
        raise UpnpError(f"UPnP error {m.group(1) if m else exc.code}") from None


def external_address(gateway: Gateway) -> str:
    m = re.search(r"<NewExternalIPAddress>([^<]*)</NewExternalIPAddress>", _soap(gateway, "GetExternalIPAddress"))
    return m.group(1).strip() if m else ""


NOT_PUBLIC = tuple(ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
                                                    "169.254.0.0/16", "100.64.0.0/10"))


def behind_another_nat(address: str) -> bool:
    """A router address no mapping can open from outside: the router is behind another NAT itself (the launcher's
    test, launcher/src/Upnp.cpp)."""
    try:
        ip = ipaddress.ip_address(address)
    except ValueError:
        return False
    return any(ip in net for net in NOT_PUBLIC)


def add_mapping(gateway: Gateway, port: int, protocol: str, description: str = "MK3D Online server") -> None:
    """Forwards external `port` to this machine's same port; a router that only keeps permanent mappings gets one."""
    for lease in (LEASE_SECONDS, 0):
        args = (f"<NewRemoteHost></NewRemoteHost><NewExternalPort>{port}</NewExternalPort>"
                f"<NewProtocol>{protocol}</NewProtocol><NewInternalPort>{port}</NewInternalPort>"
                f"<NewInternalClient>{gateway.local_ip}</NewInternalClient><NewEnabled>1</NewEnabled>"
                f"<NewPortMappingDescription>{description}</NewPortMappingDescription>"
                f"<NewLeaseDuration>{lease}</NewLeaseDuration>")
        try:
            _soap(gateway, "AddPortMapping", args)
            return
        except UpnpError as exc:
            if "725" not in str(exc) or lease == 0:
                raise


def delete_mapping(gateway: Gateway, port: int, protocol: str) -> None:
    _soap(gateway, "DeletePortMapping", f"<NewRemoteHost></NewRemoteHost><NewExternalPort>{port}</NewExternalPort>"
                                        f"<NewProtocol>{protocol}</NewProtocol>")


def open_ports(ports: list[tuple[int, str]], location: str | None = None) -> tuple[Gateway, str]:
    """Maps each (port, "TCP"/"UDP"); returns the router and its public address. Raises UpnpError."""
    gateway = discover(location=location)
    address = external_address(gateway)
    if behind_another_nat(address):
        raise UpnpError(f"the router's own address {address} is not public (carrier-grade NAT): no port can be "
                        "opened from here; host the server on a machine with a public address")
    for port, protocol in ports:
        add_mapping(gateway, port, protocol)
    return gateway, address
