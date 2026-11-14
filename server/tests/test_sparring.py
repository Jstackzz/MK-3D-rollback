"""Online Practice ("sparring"): a lobby-only mode found by its code, both players picking on the game's select screen,
no records, and the next match counting down by itself once one ends."""

from __future__ import annotations

import asyncio

import pytest

from server import protocol as P
from server.client import ServerError

from .conftest import hello_and_accept


def practice_msg(**kw):
    return {"type": "lobby_create", "name": "Tech lab", "game": "mkda", "mode": "sparring", "password": "",
            "rounds": 3, "region": "eu", "character": "Scorpion", **kw}


async def go(server, host, guest, udp_peers):
    """Both players' launchers through a lobby match's start, as test_lobby's flow does: the match_go of each."""
    fh, fg = await host.expect("match_found"), await guest.expect("match_found")
    uh, ug = await udp_peers(), await udp_peers()
    await hello_and_accept(server, host, uh, fh)
    await hello_and_accept(server, guest, ug, fg)
    ph, pg = await host.expect("match_peer"), await guest.expect("match_peer")
    rh, rg = await asyncio.gather(
        uh.punch(ph["peer"], match=ph["match"], key=ph["key"], side=1, timeout=5),
        ug.punch(pg["peer"], match=pg["match"], key=pg["key"], side=2, timeout=5),
    )
    host.send({"type": "ping_report", "match": ph["match"], **rh})
    guest.send({"type": "ping_report", "match": pg["match"], **rg})
    return await host.expect("match_go"), await guest.expect("match_go")


async def test_online_practice_is_a_lobby_by_code_that_goes_on_and_records_nothing(server, connect, udp_peers):
    assert all("sparring" in modes for modes in P.GAMES.values())
    host = await connect("Liu_Kang")
    guest = await connect("Kung_Lao")
    created = await host.request(practice_msg(death_traps=True))
    lobby = created["lobby"]
    code = lobby["id"]
    # both pick on the game's own select screen; no death traps in practice
    assert lobby["members"][0]["character"] == "" and lobby["death_traps"] is False and lobby["rounds"] == 1

    # never listed, never queued: a friend joins with the code
    assert (await guest.request({"type": "lobby_list", "game": "mkda"}))["lobbies"] == []
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "queue_join", "game": "mkda", "mode": "sparring", "region": "eu"})
    assert exc.value.code == "bad_mode"
    third = await connect("Raiden")
    with pytest.raises(ServerError) as exc:  # two players, nobody watching
        await third.request({"type": "lobby_join", "lobby": code, "spectate": True})
    assert exc.value.code == "lobby_full"
    joined = await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    assert [m["character"] for m in joined["lobby"]["members"]] == ["", ""]
    assert joined["lobby"]["max_spectators"] == 0
    for spectate in (False, True):
        with pytest.raises(ServerError) as exc:
            await third.request({"type": "lobby_join", "lobby": code, "spectate": spectate})
        assert exc.value.code == "lobby_full"
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_role", "role": "spectator"})
    assert exc.value.code == "bad_request"
    await guest.request({"type": "lobby_character", "character": "Mileena"})  # no effect: picked in the game
    assert server.lobbies[code].members[1].character == ""

    await host.request({"type": "lobby_ready", "ready": True})
    await guest.request({"type": "lobby_ready", "ready": True})
    gh, gg = await go(server, host, guest, udp_peers)
    assert (gh["mode"], gh["p1"], gh["p2"], gh["ranked"]) == ("sparring", "-", "-", False)

    # a player goes back to the select screen: both games report it, nothing is recorded
    with pytest.raises(ServerError) as exc:
        await host.request({"type": "result", "match": gh["match"], "winner": 1, "p1_rounds": 1, "p2_rounds": 0,
                            "outcome": "cheese"})
    assert exc.value.code == "bad_request"
    host.take_all("lobby")  # the states before the match
    for conn in (host, guest):
        await conn.request({"type": "result", "match": gh["match"], "outcome": "sparring"})
    assert (await host.expect("match_result"))["status"] == "practice"
    prof = await host.request({"type": "profile"})
    assert "sparring" not in prof["stats"]["mkda"]
    assert server.db.match_row(gh["match"])["status"] == "practice"

    # still ready: the next one counts down by itself
    again = await host.expect("lobby", pred=lambda m: m["lobby"]["match"] is None)
    assert [m["ready"] for m in again["lobby"]["members"]] == [True, True]
    nxt = await host.expect("match_found")
    assert nxt["mode"] == "sparring" and nxt["match"] != gh["match"]


async def test_sparring_is_not_an_outcome_of_a_kombat_match(server, connect, udp_peers):
    host = await connect("Sonya")
    guest = await connect("Jax")
    created = await host.request(practice_msg(mode="kombat"))
    await guest.request({"type": "lobby_join", "lobby": created["lobby"]["id"], "character": "Jax"})
    await host.request({"type": "lobby_ready", "ready": True})
    await guest.request({"type": "lobby_ready", "ready": True})
    gh, _ = await go(server, host, guest, udp_peers)
    with pytest.raises(ServerError) as exc:
        await host.request({"type": "result", "match": gh["match"], "outcome": "sparring"})
    assert exc.value.code == "bad_request"
