"""Spectators in lobbies (watch, play, the feed of a lobby match) and the death traps setting."""

from __future__ import annotations

import asyncio
import base64
import struct

import pytest

from server.client import ServerError

from .conftest import queue_msg, run_to_go


def create_msg(**kw):
    return {"type": "lobby_create", "name": "Watch party", "game": "mkd", "mode": "kombat", "password": "",
            "rounds": 3, "region": "eu", "character": "Scorpion", **kw}


def packet(kind: int, side: int, payload: bytes = b"", seq: int = 1) -> bytes:
    """One spectator feed packet of a game, after its u16 length (recomp/common/src/mk3d/netplay.cpp)."""
    body = struct.pack("<IBBH", 0x52444B4D, kind, side, seq) + payload
    return struct.pack("<H", len(body)) + body


def own_inputs(side: int, first: int, count: int) -> bytes:
    return packet(7, side, struct.pack("<IB", first, count) + b"\0" * (8 * count))


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


async def start_lobby_match(server, connect, udp_peers, *, death_traps=True):
    host = await connect("Raiden")
    guest = await connect("Shinnok")
    watcher = await connect("Fujin")
    code = (await host.request(create_msg(death_traps=death_traps)))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    joined = await watcher.request({"type": "lobby_join", "lobby": code, "spectate": True})
    members = {m["name"]: m["spectator"] for m in joined["lobby"]["members"]}
    assert members == {"Raiden": False, "Shinnok": False, "Fujin": True}
    assert joined["lobby"]["state"] == "full" and joined["lobby"]["death_traps"] is death_traps
    with pytest.raises(ServerError) as exc:
        await watcher.request({"type": "lobby_ready", "ready": True})
    assert exc.value.code == "not_player"
    for conn in (host, guest):
        await conn.request({"type": "lobby_ready", "ready": True})
    await host.request({"type": "lobby_start"})
    go_host, go_guest = await run_to_go(server, [(host, await udp_peers()), (guest, await udp_peers())])
    return code, host, guest, watcher, go_host, go_guest


async def test_spectator_gets_the_match_and_both_feeds(server, connect, udp_peers):
    code, host, guest, watcher, go, _ = await start_lobby_match(server, connect, udp_peers, death_traps=False)
    assert go["feed"] is True and go["death_traps"] is False
    spec = await watcher.expect("spectate")
    assert (spec["match"], spec["lobby"], spec["seed"], spec["rounds"]) == (go["match"], code, go["seed"], 3)
    assert (spec["p1_name"], spec["p2_name"], spec["p1"], spec["p2"]) == ("Raiden", "Shinnok", "Scorpion", "Kenshi")
    assert spec["death_traps"] is False

    # each player's launcher passes on its own game's packets; the spectator gets them as they come
    p1 = packet(5, 0, b"\x02\x0c\0\0\0\0\0\0\0", seq=0) + own_inputs(0, 0, 4)
    host.send({"type": "feed", "match": go["match"], "data": b64(p1)})
    assert base64.b64decode((await watcher.expect("spectate_feed"))["data"]) == p1
    p2 = own_inputs(1, 0, 4)
    guest.send({"type": "feed", "match": go["match"], "data": b64(p2)})
    assert base64.b64decode((await watcher.expect("spectate_feed"))["data"]) == p2

    # a player's launcher passes on its own side's packets only, whole ones
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "feed", "match": go["match"], "data": b64(own_inputs(0, 4, 4))})
    assert exc.value.code == "bad_request"
    with pytest.raises(ServerError) as exc:
        await host.request({"type": "feed", "match": go["match"], "data": b64(own_inputs(0, 4, 4)[:-3])})
    assert exc.value.code == "bad_request"
    watcher.send({"type": "feed", "match": go["match"], "data": b64(own_inputs(0, 9, 1))})  # not a player: dropped

    # a spectator who comes in now gets the match and everything so far
    late = await connect("Kitana")
    await late.request({"type": "lobby_join", "lobby": code, "spectate": True})
    assert (await late.expect("spectate"))["match"] == go["match"]
    assert base64.b64decode((await late.expect("spectate_feed"))["data"]) == p1 + p2

    # the result ends it for the spectators too
    for conn in (host, guest):
        await conn.request({"type": "result", "match": go["match"], "winner": 1, "p1_rounds": 2, "p2_rounds": 0})
    for conn in (watcher, late):
        end = await conn.expect("spectate_end")
        assert (end["match"], end["status"], end["winner"], end["p1_rounds"]) == (go["match"], "agreed", 1, 2)
    # the end of the match still plays after the result: its feed is still passed on
    host.send({"type": "feed", "match": go["match"], "data": b64(own_inputs(0, 4, 2))})
    assert (await watcher.expect("spectate_feed"))["match"] == go["match"]
    assert server.feeds[go["match"]].expires is not None


async def test_play_and_watch_roles(server, connect, udp_peers):
    host = await connect("Sindel")
    guest = await connect("Jade")
    code = (await host.request(create_msg()))["lobby"]["id"]
    watcher = await connect("Tanya")
    state = await watcher.request({"type": "lobby_join", "lobby": code, "spectate": True})
    assert state["lobby"]["state"] == "open"
    listed = await guest.request({"type": "lobby_list", "game": "mkd"})
    assert [(lb["players"], lb["spectators"]) for lb in listed["lobbies"]] == [(1, 1)]
    # the spectator takes the free place, the host steps aside to watch
    state = await watcher.request({"type": "lobby_role", "role": "player"})
    assert [m["spectator"] for m in state["lobby"]["members"]] == [False, False]
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_join", "lobby": code})
    assert exc.value.code == "lobby_full"
    await guest.request({"type": "lobby_join", "lobby": code, "spectate": True})
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_role", "role": "player"})
    assert exc.value.code == "lobby_full"
    state = await host.request({"type": "lobby_role", "role": "spectator"})
    assert {m["name"]: m["spectator"] for m in state["lobby"]["members"]} == {
        "Sindel": True, "Tanya": False, "Jade": True}
    await guest.request({"type": "lobby_role", "role": "player"})
    for conn in (watcher, guest):
        await conn.request({"type": "lobby_ready", "ready": True})
    # a host who watches still starts the lobby's matches, between its two players
    await host.request({"type": "lobby_start"})
    founds = [await conn.expect("match_found") for conn in (watcher, guest)]
    assert {f["side"] for f in founds} == {1, 2}
    # a spectator leaving while the match is set up does not cancel it
    await host.request({"type": "lobby_leave"})
    await asyncio.sleep(0.2)
    lobby = server.lobbies[code]
    assert lobby.match is not None and lobby.match.state in ("accepted", "punching")
    assert lobby.host.name == "Tanya"
    # a player cannot step aside while their match is being set up
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_role", "role": "spectator"})
    assert exc.value.code == "busy"


async def test_death_traps_never_in_ranked(server, connect, udp_peers):
    a = await connect("Reptile")
    b = await connect("Ermac")
    await a.request({**queue_msg(ranked=True, character="Reptile"), "death_traps": True})
    await b.request({**queue_msg(ranked=True, character="Ermac"), "death_traps": True})
    found = [await conn.expect("match_found") for conn in (a, b)]
    assert [f["death_traps"] for f in found] == [False, False]


async def test_death_traps_off_when_either_player_turns_them_off(server, connect, udp_peers):
    a = await connect("Sektor")
    b = await connect("Cyrax")
    await a.request({**queue_msg(character="Sektor"), "death_traps": False})
    await b.request(queue_msg(character="Cyrax"))
    go = await run_to_go(server, [(a, await udp_peers()), (b, await udp_peers())])
    assert [g["death_traps"] for g in go] == [False, False]
    assert all(g["feed"] is False for g in go)  # queue matches have no spectators
    for conn in (a, b):
        await conn.request({"type": "result", "match": go[0]["match"], "winner": 1, "p1_rounds": 2, "p2_rounds": 1})

    c = await connect("Smoke")
    d = await connect("Noob")
    await c.request(queue_msg(character="Smoke"))
    await d.request(queue_msg(character="NoobSaibot"))
    go = await run_to_go(server, [(c, await udp_peers()), (d, await udp_peers())])
    assert [g["death_traps"] for g in go] == [True, True]
