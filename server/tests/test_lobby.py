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


async def test_lobby_starts_by_itself_once_both_are_ready(server, connect):
    """No START: both players ready begins a countdown (the state's starts_in), un-readying calls it off, and once it
    has run the match is found as a host's lobby_start would have it."""
    server.cfg.lobby_countdown = 0.4
    host, guest = await connect("Goro"), await connect("ShaoKahn")
    code = (await host.request(create_msg()))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    first = await host.request({"type": "lobby_ready", "ready": True})
    assert first["lobby"]["starts_in"] is None
    state = await guest.request({"type": "lobby_ready", "ready": True})
    assert 0 < state["lobby"]["starts_in"] <= 0.4

    off = await host.request({"type": "lobby_ready", "ready": False})
    assert off["lobby"]["starts_in"] is None
    await asyncio.sleep(0.7)
    assert server.lobbies[code].match is None and not host.take_all("match_found")

    again = await host.request({"type": "lobby_ready", "ready": True})
    assert again["lobby"]["starts_in"] is not None
    fh, fg = await host.expect("match_found"), await guest.expect("match_found")
    assert fh["match"] == fg["match"] and fh["lobby"] == code and fh["accept_required"] is False
    assert (fh["side"], fg["side"]) == (1, 2)
    in_match = await guest.expect("lobby", pred=lambda m: m["lobby"]["state"] == "in_match")
    assert in_match["lobby"]["match"] == fh["match"] and in_match["lobby"]["starts_in"] is None


async def test_a_lobby_named_after_its_host_takes_the_next_hosts_name(server, connect):
    """The host leaves: the lobby named after them (the launcher's default) is named after the new host, so the list
    never shows two lobbies named after one player; a lobby its host named stays as it is."""
    host, guest = await connect("Goro"), await connect("ShaoKahn")
    code = (await host.request(create_msg(name="Goro's lobby")))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    await host.request({"type": "lobby_leave"})
    state = await guest.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    assert (state["lobby"]["host"], state["lobby"]["name"]) == ("ShaoKahn", "ShaoKahn's lobby")

    third = await connect("Moloch")
    code2 = (await third.request(create_msg(name="Friday sets")))["lobby"]["id"]
    fourth = await connect("Kitana")
    await fourth.request({"type": "lobby_join", "lobby": code2, "character": "Kenshi"})
    await third.request({"type": "lobby_leave"})
    kept = await fourth.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    assert (kept["lobby"]["host"], kept["lobby"]["name"]) == ("Kitana", "Friday sets")


async def test_online_counts_the_players_of_each_game(server, connect):
    """Each game's players for the matchmaking pages: searching, in its matches, in its lobbies (spectators too), and
    playing, any of those, each player once."""
    from .conftest import queue_msg

    searching, motor, host, spectator, p1, p2 = [
        await connect(n) for n in ("Kitana", "Tanya", "Sindel", "Jade", "Frost", "Sareena")]
    await searching.request(queue_msg(game="mkd"))
    await motor.request(queue_msg(game="mka", mode="motor"))
    code = (await host.request(create_msg()))["lobby"]["id"]
    await spectator.request({"type": "lobby_join", "lobby": code, "spectate": True})
    await p1.request(queue_msg(game="mkda"))
    await p2.request(queue_msg(game="mkda"))
    await p1.expect("match_found")
    online = await searching.request({"type": "online"})
    assert online["games"] == {
        "mkd": {"playing": 3, "searching": 1, "matches": 0, "lobbies": 2},
        "mka": {"playing": 1, "searching": 1, "matches": 0, "lobbies": 0},
        "mkda": {"playing": 2, "searching": 0, "matches": 2, "lobbies": 0},
    }
    assert online["queues"] == {"mka/motor/casual": 1, "mkd/kombat/casual": 1}
    # a lobby left empty is gone from the count, and the push says so
    searching.take_all("online")  # the pushes so far
    await spectator.request({"type": "lobby_leave"})
    await host.request({"type": "lobby_leave"})
    pushed = await searching.expect("online", timeout=5, pred=lambda m: "id" not in m and
                                    m["games"]["mkd"]["lobbies"] == 0)
    assert pushed["games"]["mkd"] == {"playing": 1, "searching": 1, "matches": 0, "lobbies": 0}


async def test_online_lists_who_is_searching(server, connect):
    """The matchmaking pages show who searches, for which game and queue, the longest waiting first; a search that
    ends leaves the list, and every connection is told."""
    from .conftest import queue_msg

    kitana, tanya, watcher = [await connect(n) for n in ("Kitana", "Tanya", "Jade")]
    await kitana.request(queue_msg(game="mkd"))
    await tanya.request(queue_msg(game="mka", mode="motor", ranked=True))
    online = await watcher.request({"type": "online"})
    assert online["searchers"] == [
        {"name": "Kitana", "game": "mkd", "queue": "mkd/kombat/casual"},
        {"name": "Tanya", "game": "mka", "queue": "mka/motor/ranked"},
    ]
    watcher.take_all("online")  # the pushes so far
    await kitana.request({"type": "queue_leave"})
    pushed = await watcher.expect("online", timeout=5, pred=lambda m: "id" not in m and len(m["searchers"]) == 1)
    assert pushed["searchers"] == [{"name": "Tanya", "game": "mka", "queue": "mka/motor/ranked"}]
