"""Lobbies: create (with password), list, join, chat, character, ready, host start, leave."""

from __future__ import annotations

import asyncio

import pytest

from server.client import ServerError

from .conftest import hello_and_accept


def create_msg(**kw):
    return {"type": "lobby_create", "name": "Friday sets", "game": "mkd", "mode": "kombat", "password": "",
            "rounds": 3, "region": "eu", "character": "Scorpion", **kw}


async def test_lobby_create_join_ready_start(server, connect, udp_peers):
    host = await connect("Goro")
    guest = await connect("ShaoKahn")
    created = await host.request(create_msg(password="pw-123"))
    lobby = created["lobby"]
    code = lobby["id"]
    assert len(code) == 5 and lobby["locked"] and lobby["state"] == "open"
    assert lobby["members"] == [{"name": "Goro", "character": "Scorpion", "ready": False, "host": True,
                                 "spectator": False, "link": None, "ping": None}]

    listed = await guest.request({"type": "lobby_list", "game": "mkd"})
    assert [(lb["id"], lb["players"], lb["locked"]) for lb in listed["lobbies"]] == [(code, 1, True)]
    assert (await guest.request({"type": "lobby_list", "game": "mka"}))["lobbies"] == []

    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_join", "lobby": code, "password": "wrong"})
    assert exc.value.code == "bad_lobby_password"
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_join", "lobby": "ZZZZZ"})
    assert exc.value.code == "no_such_lobby"
    joined = await guest.request({"type": "lobby_join", "lobby": code.lower(), "password": "pw-123",
                                  "character": "Kenshi"})
    assert [m["name"] for m in joined["lobby"]["members"]] == ["Goro", "ShaoKahn"]
    pushed = await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 2)
    assert pushed["lobby"]["state"] == "full"

    third = await connect("Moloch")
    with pytest.raises(ServerError) as exc:
        await third.request({"type": "lobby_join", "lobby": code, "password": "pw-123"})
    assert exc.value.code == "lobby_full"
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "queue_join", "game": "mkd", "mode": "kombat", "region": "eu"})
    assert exc.value.code == "busy"

    # chat reaches both members and is kept in the lobby state
    await guest.request({"type": "lobby_chat", "text": "  good   luck  "})
    chat = await host.expect("lobby_chat")
    assert (chat["from"], chat["text"], chat["lobby"]) == ("ShaoKahn", "good luck", code)
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_chat", "text": "spam"})
    assert exc.value.code == "slow_down"

    await guest.request({"type": "lobby_character", "character": "Mileena"})
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_start"})
    assert exc.value.code == "not_host"
    await host.request({"type": "lobby_ready", "ready": True})
    with pytest.raises(ServerError) as exc:
        await host.request({"type": "lobby_start"})
    assert exc.value.code == "not_ready"
    state = await guest.request({"type": "lobby_ready", "ready": True})
    assert [m["ready"] for m in state["lobby"]["members"]] == [True, True]
    assert state["lobby"]["members"][1]["character"] == "Mileena"
    assert state["lobby"]["chat"][-1]["text"] == "good luck"

    await host.request({"type": "lobby_start"})
    fh, fg = await host.expect("match_found"), await guest.expect("match_found")
    assert fh["match"] == fg["match"] and fh["lobby"] == code
    assert fh["accept_required"] is False and fh["side"] == 1 and fg["side"] == 2
    assert fg["opponent"]["name"] == "Goro"
    in_match = await guest.expect("lobby", pred=lambda m: m["lobby"]["state"] == "in_match")
    assert in_match["lobby"]["match"] == fh["match"]

    # the rest of the flow is the queue one, without the accept step
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
    gh = await host.expect("match_go")
    assert (gh["p1"], gh["p2"], gh["p1_name"], gh["lobby"]) == ("Scorpion", "Mileena", "Goro", code)
    assert gh["ranked"] is False

    # results finish the match; the lobby opens again with ready flags cleared
    for conn in (host, guest):
        await conn.request({"type": "result", "match": gh["match"], "winner": 2, "p1_rounds": 1, "p2_rounds": 2})
    assert (await host.expect("match_result"))["status"] == "agreed"
    reopened = await host.expect("lobby", pred=lambda m: m["lobby"]["state"] == "full" and m["lobby"]["match"] is None)
    assert [m["ready"] for m in reopened["lobby"]["members"]] == [False, False]


async def test_host_leaving_hands_over_and_empty_lobby_closes(server, connect):
    host = await connect("Fujin")
    guest = await connect("Rain")
    code = (await host.request(create_msg(name="")))["lobby"]["id"]
    assert server.lobbies[code].name == "Fujin's lobby"
    await guest.request({"type": "lobby_join", "lobby": code})
    left = await host.request({"type": "lobby_leave"})
    assert left == {"type": "lobby_left", "lobby": code, "id": left["id"]}
    update = await guest.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    assert update["lobby"]["host"] == "Rain" and update["lobby"]["members"][0]["host"] is True
    await guest.close()
    await asyncio.sleep(0.2)
    assert code not in server.lobbies


async def test_a_dropped_connection_gets_its_lobby_place_back(server, connect):
    host = await connect("Goro")
    guest = await connect("ShaoKahn")
    code = (await host.request(create_msg()))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 2)
    token = guest.account["token"]
    await guest.close()  # the connection drops: the place is kept a while
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)

    back = await connect()
    await back.request({"type": "auth", "token": token})
    pushed = await back.expect("lobby")
    assert [(m["name"], m["character"], m["spectator"]) for m in pushed["lobby"]["members"]] == [
        ("Goro", "Scorpion", False), ("ShaoKahn", "Kenshi", False)]
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 2)

    # signing out gives the place up, and so does a grace that ran out
    await back.request({"type": "logout"})
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    again = await connect("ShaoKahn", register=False)
    await asyncio.sleep(0.2)
    assert [m.session.name for m in server.lobbies[code].members] == ["Goro"]
    await again.request({"type": "lobby_join", "lobby": code})
    server.cfg.lobby_rejoin_grace = 0.0
    await again.close()
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    await asyncio.sleep(0.05)
    await connect("ShaoKahn", register=False)
    await asyncio.sleep(0.2)
    assert [m.session.name for m in server.lobbies[code].members] == ["Goro"]
