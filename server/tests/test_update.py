"""The release launchers are offered to update to: the update file, welcome, the outdated reply, the push."""

from __future__ import annotations

import asyncio
import json
import os

import pytest
import pytest_asyncio

from server import protocol as P
from server.app import Server, ServerConfig
from server.client import Connection, ServerError
from server.matchmaking import MatchmakingConfig

RELEASE = {
    "version": "0.1.8",
    "url": "https://github.com/Jstackzz/MK-3D-rollback/releases/download/v0.1.8/MK3D-Online-0.1.8-windows.zip",
    "sha256": "ab" * 32, "size": 96_000_000, "notes": "What is new.",
    "page": "https://github.com/Jstackzz/MK-3D-rollback/releases/tag/v0.1.8",
}


@pytest_asyncio.fixture
async def server(tmp_path, tls_files):
    cert, key, _ = tls_files
    cfg = ServerConfig(
        host="127.0.0.1", port=0, udp_port=0, db_path=str(tmp_path / "test.sqlite"), certfile=cert, keyfile=key,
        tick_seconds=0.05, update_check_interval=0.1, min_client="0.1.7", update_file=str(tmp_path / "update.json"),
        mm=MatchmakingConfig(allow_same_ip=True),
    )
    srv = Server(cfg)
    await srv.start()
    try:
        yield srv
    finally:
        await srv.stop()


def write_update(server: Server, content: str, stamp: int) -> None:
    """The update file, with its own modification time (two writes in one clock tick would look unchanged)."""
    path = server.cfg.update_file
    assert path
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    os.utime(path, ns=(stamp * 1_000_000_000, stamp * 1_000_000_000))


async def launcher(server: Server, version: str) -> tuple[Connection, dict]:
    conn = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    welcome = await conn.connect(version=version)
    return conn, welcome


def test_parse_update_checks_every_field():
    assert P.parse_update(RELEASE) == RELEASE
    assert P.parse_update({k: v for k, v in RELEASE.items() if k not in ("notes", "page")})["page"] == P.RELEASES_PAGE
    local = P.parse_update({**RELEASE, "url": "http://127.0.0.1:8000/MK3D-Online-0.1.8-windows.zip"})
    assert local["url"].startswith("http://127.0.0.1")
    for bad in ({"version": "0.1.8-beta"}, {"url": "http://example.com/a.zip"}, {"url": "file:///C:/a.zip"},
                {"sha256": "AB" * 32}, {"sha256": "ab" * 31}, {"size": 0}, {"size": True}, {"notes": "x" * 2001},
                {"page": "javascript:alert(1)"}):
        with pytest.raises(ValueError):
            P.parse_update({**RELEASE, **bad})
    with pytest.raises(ValueError):
        P.parse_update([RELEASE])


async def test_welcome_offers_a_newer_release(server):
    conn, welcome = await launcher(server, "0.1.7")
    assert "update" not in welcome  # no update file yet
    await conn.close()

    write_update(server, json.dumps(RELEASE), 1_800_000_000)
    older, welcome = await launcher(server, "0.1.7")
    assert welcome["update"] == RELEASE
    current, welcome = await launcher(server, "0.1.8")
    assert "update" not in welcome
    for c in (older, current):
        await c.close()

    write_update(server, '{"version": "0.1.9"', 1_800_000_100)  # broken: ignored, nothing offered
    conn, welcome = await launcher(server, "0.1.7")
    assert "update" not in welcome
    await conn.close()


async def test_outdated_launcher_gets_the_link_and_the_release(server):
    old = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    with pytest.raises(ServerError) as exc:
        await old.connect(version="0.1.6")
    assert exc.value.code == "client_outdated" and P.RELEASES_PAGE in exc.value.msg["message"]
    assert "update" not in exc.value.msg
    await old.close()

    write_update(server, json.dumps(RELEASE), 1_800_000_000)
    old = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    with pytest.raises(ServerError) as exc:
        await old.connect(version="0.1.6")
    # what a launcher before 0.1.7 shows, and what one with the updater installs
    assert exc.value.msg["message"] == f"Update the launcher to 0.1.7 or newer: {RELEASE['page']}"
    assert exc.value.msg["update"] == RELEASE
    await old.close()


async def test_a_release_announced_reaches_connected_launchers(server):
    older, _ = await launcher(server, "0.1.7")
    current, _ = await launcher(server, "0.1.8")
    write_update(server, json.dumps(RELEASE), 1_800_000_000)
    pushed = await older.expect("update", timeout=3)
    assert {k: pushed[k] for k in RELEASE} == RELEASE
    await asyncio.sleep(0.3)  # more ticks: offered once, and not to a launcher that has it
    assert not older.take_all("update") and not current.take_all("update")
    for c in (older, current):
        await c.close()
