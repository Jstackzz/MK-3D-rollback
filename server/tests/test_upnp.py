"""UPnP port mapping for a server on a home PC, against a stand-in router on 127.0.0.1."""

from __future__ import annotations

import asyncio
import http.server
import re
import threading

import pytest

from server import upnp
from server.app import Server, ServerConfig

SERVICE = "urn:schemas-upnp-org:service:WANIPConnection:1"
DESCRIPTION = f"""<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0"><device>
 <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
 <deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
  <deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
   <serviceList><service><serviceType>{SERVICE}</serviceType><controlURL>/ctl/IPConn</controlURL></service></serviceList>
  </device></deviceList>
 </device></deviceList>
</device></root>"""


class Router:
    """Serves the description, answers GetExternalIPAddress with `address`, refuses leases other than 0 (error 725,
    as some routers do) and records every call."""

    def __init__(self, address: str = "198.18.0.7") -> None:
        self.address = address
        self.calls: list[tuple[str, str]] = []
        router = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args) -> None:
                pass

            def reply(self, code: int, text: str) -> None:
                data = text.encode()
                self.send_response(code)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self) -> None:
                self.reply(200, DESCRIPTION)

            def do_POST(self) -> None:
                body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
                action = self.headers.get("SOAPAction", "").strip('"').rsplit("#", 1)[-1]
                router.calls.append((action, body))
                if action == "GetExternalIPAddress":
                    self.reply(200, f"<NewExternalIPAddress>{router.address}</NewExternalIPAddress>")
                elif action == "AddPortMapping" and "<NewLeaseDuration>0<" not in body:
                    self.reply(500, "<UPnPError><errorCode>725</errorCode></UPnPError>")
                else:
                    self.reply(200, "<ok/>")

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.location = f"http://127.0.0.1:{self.httpd.server_address[1]}/rootDesc.xml"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def mapped(self) -> list[tuple[str, str]]:
        out = []
        for action, body in self.calls:
            if action == "AddPortMapping" and "<NewLeaseDuration>0<" in body:
                port = re.search(r"<NewExternalPort>(\d+)<", body).group(1)
                out.append((port, re.search(r"<NewProtocol>(\w+)<", body).group(1)))
        return out

    def close(self) -> None:
        self.httpd.shutdown()


@pytest.fixture
def router():
    r = Router()
    yield r
    r.close()


def test_open_ports_maps_both_with_a_permanent_lease_when_asked(router):
    gateway, address = upnp.open_ports([(7777, "TCP"), (7778, "UDP")], location=router.location)
    assert address == "198.18.0.7"
    assert gateway.control_url.endswith("/ctl/IPConn") and gateway.service_type == SERVICE
    assert gateway.local_ip == "127.0.0.1"
    assert router.mapped() == [("7777", "TCP"), ("7778", "UDP")]
    first = next(body for action, body in router.calls if action == "AddPortMapping")
    assert "<NewLeaseDuration>3600<" in first and "<NewInternalClient>127.0.0.1<" in first


def test_a_router_behind_carrier_grade_nat_is_reported(router):
    router.address = "100.72.1.9"
    with pytest.raises(upnp.UpnpError, match="carrier-grade NAT"):
        upnp.open_ports([(7777, "TCP")], location=router.location)
    assert not router.mapped()


def test_addresses_no_mapping_can_open():
    assert upnp.behind_another_nat("192.168.1.1") and upnp.behind_another_nat("100.64.0.1")
    assert upnp.behind_another_nat("10.1.2.3") and upnp.behind_another_nat("172.20.0.1")
    assert not upnp.behind_another_nat("198.18.0.7") and not upnp.behind_another_nat("81.2.69.160")


async def test_server_opens_and_closes_its_ports(router, tmp_path, tls_files):
    cert, key, _ = tls_files
    srv = Server(ServerConfig(host="127.0.0.1", port=0, udp_port=0, db_path=str(tmp_path / "s.sqlite"),
                              certfile=cert, keyfile=key, upnp=True, upnp_location=router.location))
    await srv.start()
    try:
        for _ in range(100):
            if srv.public_address:
                break
            await asyncio.sleep(0.05)
        assert srv.public_address == "198.18.0.7"
        assert router.mapped() == [(str(srv.tcp_port), "TCP"), (str(srv.udp_port), "UDP")]
    finally:
        await srv.stop()
    deleted = [re.search(r"<NewProtocol>(\w+)<", body).group(1) for action, body in router.calls
               if action == "DeletePortMapping"]
    assert deleted == ["TCP", "UDP"]
