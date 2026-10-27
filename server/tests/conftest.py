"""Fixtures: a real server (TLS on 127.0.0.1, ephemeral ports, temp database) and connected clients."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator, Awaitable, Callable

import pytest
import pytest_asyncio

from server.app import Server, ServerConfig
from server.client import Connection, UdpPeer
from server.makecert import generate
from server.matchmaking import MatchmakingConfig

PASSWORD = "test-password-1"  # test-only credential for throw-away accounts in temp databases


@pytest.fixture(scope="session")
def tls_files(tmp_path_factory: pytest.TempPathFactory) -> tuple[str, str, str]:
    folder = tmp_path_factory.mktemp("tls")
    fingerprint = generate(folder / "cert.pem", folder / "key.pem")
    return str(folder / "cert.pem"), str(folder / "key.pem"), fingerprint


@pytest_asyncio.fixture
async def server(tmp_path, tls_files) -> AsyncIterator[Server]:
    cert, key, _ = tls_files
    cfg = ServerConfig(
        host="127.0.0.1", port=0, udp_port=0, db_path=str(tmp_path / "test.sqlite"), certfile=cert, keyfile=key,
        tick_seconds=0.05, online_push_interval=0.1, mm=MatchmakingConfig(allow_same_ip=True),
        relay_only=False,  # the tests take the direct way unless they ask for the relay (a server's default)
    )
    srv = Server(cfg)
    await srv.start()
    try:
        yield srv
    finally:
        await srv.stop()


Connector = Callable[..., Awaitable[Connection]]


@pytest_asyncio.fixture
async def connect(server: Server) -> AsyncIterator[Connector]:
    conns: list[Connection] = []

    async def factory(name: str | None = None, *, password: str = PASSWORD, register: bool = True,
                      fingerprint: str | None = None) -> Connection:
        conn = Connection("127.0.0.1", server.tcp_port, fingerprint=fingerprint or server.fingerprint)
        await conn.connect()
        conns.append(conn)
        if name is not None:
            if register:
                await conn.register(name, password)
            else:
                await conn.login(name, password)
        return conn

    yield factory
    for conn in conns:
        await conn.close()


@pytest_asyncio.fixture
async def udp_peers() -> AsyncIterator[Callable[[], Awaitable[UdpPeer]]]:
    peers: list[UdpPeer] = []

    async def factory() -> UdpPeer:
        peer = await UdpPeer.open("127.0.0.1", 0)
        peers.append(peer)
        return peer

    yield factory
    for peer in peers:
        peer.close()


def queue_msg(game: str = "mkd", mode: str = "kombat", *, ranked: bool = False, region: str = "eu",
              max_ping: int = 0, strict: bool = False, character: str = "Scorpion", delay: int = 2) -> dict:
    return {"type": "queue_join", "game": game, "mode": mode, "ranked": ranked, "region": region,
            "max_ping": max_ping, "strict": strict, "character": character, "delay": delay}


async def hello_and_accept(server: Server, conn: Connection, udp: UdpPeer, found: dict, *, accept: bool = True) -> dict:
    ack = await udp.hello(("127.0.0.1", server.udp_port), conn.account["udp_token"], match=found["match"],
                          lan=f"127.0.0.1:{udp.port}", timeout=3)
    assert ack is not None and ack["type"] == "hello_ack"
    if found["accept_required"]:
        conn.send({"type": "match_accept" if accept else "match_decline", "match": found["match"]})
    return ack


async def run_to_go(server: Server, pairs: list[tuple[Connection, UdpPeer]]) -> list[dict]:
    """Both players already got match_found: hello, accept, punch, report; returns both match_go."""
    founds = [await conn.expect("match_found") for conn, _ in pairs]
    for (conn, udp), found in zip(pairs, founds):
        await hello_and_accept(server, conn, udp, found)
    peers = [await conn.expect("match_peer") for conn, _ in pairs]
    reports = await asyncio.gather(*[
        udp.punch(peer["peer"], match=peer["match"], key=peer["key"], side=peer["side"], timeout=5)
        for (_, udp), peer in zip(pairs, peers)
    ])
    for (conn, _), peer, report in zip(pairs, peers, reports):
        assert report["ok"], report
        conn.send({"type": "ping_report", "match": peer["match"], **report})
    return [await conn.expect("match_go") for conn, _ in pairs]
