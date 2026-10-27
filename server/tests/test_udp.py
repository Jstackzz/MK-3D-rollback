"""UDP rendezvous: endpoint learning, endpoint exchange, punch/ping, ping-based cancellation."""

from __future__ import annotations

import asyncio

from server.app import ServerConfig

from .conftest import hello_and_accept, queue_msg, run_to_go


async def test_hello_ack_reports_public_endpoint(server, connect, udp_peers):
    a = await connect("Ermac")
    udp = await udp_peers()
    ack = await udp.hello(("127.0.0.1", server.udp_port), a.account["udp_token"], lan=f"192.168.1.20:{udp.port}")
    assert ack is not None
    assert ack["public"] == f"127.0.0.1:{udp.port}"
    assert ack["rtt_ms"] >= 0
    session = server.by_account[server.db.account_by_name("Ermac")["id"]]
    assert session.udp_public == ("127.0.0.1", udp.port)
    assert session.udp_lan == f"192.168.1.20:{udp.port}"


async def test_unknown_token_is_never_answered(server, udp_peers):
    udp = await udp_peers()
    ack = await udp.hello(("127.0.0.1", server.udp_port), "f" * 32, timeout=0.6)
    assert ack is None
    udp.sendto({"type": "nonsense"}, ("127.0.0.1", server.udp_port))
    assert await udp.recv(0.3, lambda m, a: True) is None


async def test_rendezvous_exchanges_endpoints_and_punches(server, connect, udp_peers):
    a, b = await connect("Scorpion"), await connect("SubZero")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request(queue_msg(character="Scorpion"))
    await b.request(queue_msg(character="SubZero"))
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    # no addresses before both accepted
    await hello_and_accept(server, a, ua, fa)
    await asyncio.sleep(0.2)
    assert not a.take_all("match_peer")
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    assert pa["key"] == pb["key"] and len(pa["key"]) == 32
    assert pa["peer"] == {"public": f"127.0.0.1:{ub.port}", "lan": f"127.0.0.1:{ub.port}"}
    assert pb["peer"] == {"public": f"127.0.0.1:{ua.port}", "lan": f"127.0.0.1:{ua.port}"}
    assert pa["same_nat"] is True
    assert pa["opponent"]["name"] == "SubZero"
    ra, rb = await asyncio.gather(
        ua.punch(pa["peer"], match=pa["match"], key=pa["key"], side=pa["side"], timeout=5),
        ub.punch(pb["peer"], match=pb["match"], key=pb["key"], side=pb["side"], timeout=5),
    )
    assert ra["ok"] and rb["ok"]
    assert ra["remote"] == f"127.0.0.1:{ub.port}" and rb["remote"] == f"127.0.0.1:{ua.port}"
    a.send({"type": "ping_report", "match": pa["match"], **ra})
    b.send({"type": "ping_report", "match": pb["match"], **rb})
    ga, gb = await a.expect("match_go"), await b.expect("match_go")
    assert ga["seed"] == gb["seed"] and ga["match"] == gb["match"]
    assert {ga["side"], gb["side"]} == {1, 2}
    assert ga["remote"] == f"127.0.0.1:{ub.port}"
    assert ga["p1"] == gb["p1"] and ga["p2"] == gb["p2"]
    assert {ga["p1"], ga["p2"]} == {"Scorpion", "SubZero"}
    assert ga["delay"] == 2 and ga["rounds"] == 3


async def test_punch_failure_cancels_and_is_remembered(server, connect, udp_peers):
    server.cfg.relay = False
    a, b = await connect("Raiden"), await connect("Shinnok")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    a.send({"type": "ping_report", "match": pa["match"], "ok": False, "rtt_ms": 0})
    b.send({"type": "ping_report", "match": pb["match"], "ok": True, "rtt_ms": 20, "remote": pb["peer"]["public"]})
    ca, cb = await a.expect("match_cancelled"), await b.expect("match_cancelled")
    assert ca["reason"] == cb["reason"] == "punch_failed"
    assert ca["requeued"] and cb["requeued"]
    # both are back in the queue, but the matchmaker does not pair them again
    await asyncio.sleep(0.4)
    assert not a.take_all("match_found") and len(server.queue) == 2


async def relay_to_go(server, a, b, ua, ub, pa, pb):
    """Both got the relay's match_peer: punch and ping through the server's UDP port, report; both match_go."""
    relay = {"public": f"127.0.0.1:{server.udp_port}", "lan": ""}
    ra, rb = await asyncio.gather(
        ua.punch(relay, match=pa["match"], key=pa["key"], side=pa["side"], timeout=5),
        ub.punch(relay, match=pb["match"], key=pb["key"], side=pb["side"], timeout=5),
    )
    assert ra["ok"] and rb["ok"], (ra, rb)
    assert ra["remote"] == rb["remote"] == f"127.0.0.1:{server.udp_port}"
    a.send({"type": "ping_report", "match": pa["match"], **ra, "via": "relay"})
    b.send({"type": "ping_report", "match": pb["match"], **rb, "via": "relay"})
    return await a.expect("match_go"), await b.expect("match_go")


async def test_failed_punch_falls_back_to_the_relay(server, connect, udp_peers):
    a, b = await connect("Mileena"), await connect("Baraka")
    ua, ub, stranger = await udp_peers(), await udp_peers(), await udp_peers()
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    assert not pa.get("relay")
    a.send({"type": "ping_report", "match": pa["match"], "ok": False, "rtt_ms": 0})
    b.send({"type": "ping_report", "match": pb["match"], "ok": False, "rtt_ms": 0})
    ra, rb = await a.expect("match_peer"), await b.expect("match_peer")
    assert ra["relay"] is True and rb["relay"] is True and ra["key"] == pa["key"]
    ga, gb = await relay_to_go(server, a, b, ua, ub, ra, rb)
    assert ga["remote"] == gb["remote"] == f"127.0.0.1:{server.udp_port}"
    assert ga["seed"] == gb["seed"]
    # a game packet goes to the other player byte for byte, both ways
    to_server = ("127.0.0.1", server.udp_port)
    packet = b"MKDR\x03\x01\x00\x00" + bytes(range(40))
    assert ua.transport is not None and ub.transport is not None and stranger.transport is not None
    ua.transport.sendto(packet, to_server)
    data, addr = await asyncio.wait_for(ub.raw.get(), 2)
    assert data == packet and addr == to_server
    ub.transport.sendto(packet[::-1], to_server)
    data, _ = await asyncio.wait_for(ua.raw.get(), 2)
    assert data == packet[::-1]
    # someone else's packets go nowhere
    stranger.transport.sendto(packet, to_server)
    await asyncio.sleep(0.3)
    assert ua.raw.empty() and ub.raw.empty()


async def test_relay_only_skips_the_direct_attempt(server, connect, udp_peers):
    assert not ServerConfig().relay_only  # a server's default: straight, unless a player hides their address
    server.cfg.relay_only = True
    a, b = await connect("Kobra"), await connect("Kira")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    assert pa["relay"] is True and pb["relay"] is True
    assert pa["peer"] == pb["peer"] == {"public": "", "lan": ""}  # neither gets the other's address
    ga, gb = await relay_to_go(server, a, b, ua, ub, pa, pb)
    assert ga["remote"] == gb["remote"] == f"127.0.0.1:{server.udp_port}"  # their games send to the server only


async def test_relay_failure_cancels(server, connect, udp_peers):
    a, b = await connect("Drahmin"), await connect("Moloch")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    for _ in range(2):  # the direct attempt, then the relay
        pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
        a.send({"type": "ping_report", "match": pa["match"], "ok": False, "rtt_ms": 0})
        b.send({"type": "ping_report", "match": pb["match"], "ok": False, "rtt_ms": 0})
    ca, cb = await a.expect("match_cancelled"), await b.expect("match_cancelled")
    assert ca["reason"] == cb["reason"] == "punch_failed"


async def test_relay_route_ends_with_the_match(server, connect, udp_peers):
    server.cfg.relay_only = True
    server.cfg.relay_linger = 0.1
    a, b = await connect("HsuHao"), await connect("Darrius")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    ga, _ = await relay_to_go(server, a, b, ua, ub, pa, pb)
    for conn in (a, b):
        await conn.request({"type": "result", "match": ga["match"], "winner": 1, "p1_rounds": 2, "p2_rounds": 0})
    await a.expect("match_result")
    await asyncio.sleep(0.4)
    assert not server.relay_routes
    assert ua.transport is not None
    ua.transport.sendto(b"MKDR\x03\x01\x00\x00", ("127.0.0.1", server.udp_port))
    await asyncio.sleep(0.3)
    assert ub.raw.empty()


async def test_relay_is_rate_limited(server, connect, udp_peers):
    server.cfg.relay_only = True
    server.cfg.relay_burst = 20
    server.cfg.relay_rate = 1
    a, b = await connect("Ashrah"), await connect("Dairou")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    await relay_to_go(server, a, b, ua, ub, pa, pb)
    while not ub.raw.empty():
        ub.raw.get_nowait()
    assert ua.transport is not None
    for _ in range(100):
        ua.transport.sendto(b"MKDR\x03\x01\x00\x00", ("127.0.0.1", server.udp_port))
    await asyncio.sleep(0.5)
    assert 0 < ub.raw.qsize() <= 22


async def test_ping_above_max_ping_cancels(server, connect, udp_peers):
    a, b = await connect("Kenshi"), await connect("Hotaru")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request(queue_msg(max_ping=50))
    await b.request(queue_msg(max_ping=0))
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    for conn, peer in ((a, pa), (b, pb)):
        conn.send({"type": "ping_report", "match": peer["match"], "ok": True, "rtt_ms": 120,
                   "remote": peer["peer"]["public"]})
    ca = await a.expect("match_cancelled")
    assert ca["reason"] == "ping_too_high"
    ids = sorted(server.db.account_by_name(n)["id"] for n in ("Kenshi", "Hotaru"))
    assert server.ping_cache.get(ids[0], ids[1], now=__import__("time").time()) == 120


async def test_missing_udp_hello_cancels(server, connect, udp_peers):
    server.cfg.udp_timeout = 0.3
    a, b = await connect("Kabal"), await connect("Kira")
    ua = await udp_peers()
    await a.request(queue_msg())
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    b.send({"type": "match_accept", "match": fb["match"]})  # accepts but its UDP never arrives
    ca, cb = await a.expect("match_cancelled", timeout=3), await b.expect("match_cancelled", timeout=3)
    assert ca["reason"] == "no_udp"
    assert ca["requeued"] is True and cb["requeued"] is False


async def test_disconnect_during_setup_requeues_the_other(server, connect, udp_peers):
    a, b = await connect("Nitara"), await connect("Mavado")
    await a.request(queue_msg())
    await b.request(queue_msg())
    await a.expect("match_found")
    await b.expect("match_found")
    await b.close()
    ca = await a.expect("match_cancelled", timeout=3)
    assert ca["reason"] == "peer_left" and ca["requeued"] is True


async def test_full_flow_helper(server, connect, udp_peers):
    a, b = await connect("Reptile"), await connect("Frost")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request(queue_msg(game="mka", mode="motor"))
    await b.request(queue_msg(game="mka", mode="motor"))
    gos = await run_to_go(server, [(a, ua), (b, ub)])
    assert gos[0]["game"] == "mka" and gos[0]["mode"] == "motor"


async def test_a_player_who_hides_their_address_plays_through_the_relay(server, connect, udp_peers):
    a, b = await connect("Havik"), await connect("Hotaru")
    ua, ub = await udp_peers(), await udp_peers()
    await a.request({**queue_msg(), "relay": True})  # only one of the two asks
    await b.request(queue_msg())
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    assert pa["relay"] is True and pb["relay"] is True
    assert pa["peer"] == pb["peer"] == {"public": "", "lan": ""}
    ga, gb = await relay_to_go(server, a, b, ua, ub, pa, pb)
    assert ga["remote"] == gb["remote"] == f"127.0.0.1:{server.udp_port}"
