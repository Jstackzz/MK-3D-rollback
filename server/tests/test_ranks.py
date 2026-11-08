"""Ranked titles by Elo (0.1.21; the 16 badges' ladder since 2026-11-07): the ladder, and the title the server sends
with every rating, ranked or not (lobby members, a match's players)."""

from __future__ import annotations

import asyncio

from server import elo

from .conftest import hello_and_accept, queue_msg
from .test_lobby import create_msg
from .test_series import play

LADDER = [
    ("Beginner", None), ("Learner", 1425), ("Fighter", 1475), ("Expert", 1525), ("Warrior", 1575),
    ("Gladiator", 1625), ("Master", 1675), ("Commander", 1725), ("Grandmaster", 1775), ("Warlord", 1825),
    ("Chosen One", 1875), ("Demi God", 1925), ("God", 1975), ("Elder God", 2025), ("Titan", 2100), ("Eldest God", 2200),
]


def test_titles_by_elo():
    assert [(r["title"], r["from"]) for r in elo.ladder()] == LADDER
    assert elo.title(elo.INITIAL) == "Fighter"  # where everyone starts
    for (name, floor), (_, upper) in zip(LADDER, LADDER[1:] + [(None, None)]):
        if floor is not None:
            assert elo.title(floor) == name
        if upper is not None:
            assert elo.title(upper - 1) == name
    assert elo.title(-40) == "Beginner" and elo.title(3000) == "Eldest God"


def set_rating(server, name, rating, game="mkd", mode="kombat"):
    account = server.db.account_by_name(name)["id"]
    server.db.conn.execute(
        "INSERT OR REPLACE INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
        "VALUES (?, ?, ?, 'ranked', ?, 3, 2, 0)", (account, game, mode, rating))


async def test_every_ranked_rating_comes_with_its_title(server, connect, udp_peers):
    conns = [await connect(n) for n in ("Fujin", "Raijin", "Shinnok")]
    assert conns[0].welcome["ranks"] == elo.ladder()  # the ladder, for the launcher to show
    for name, rating in (("Fujin", 1630), ("Raijin", 1570), ("Shinnok", 1287)):
        set_rating(server, name, rating)

    prof = await conns[0].request({"type": "profile"})
    assert prof["stats"]["mkd"]["kombat"]["ranked"] == {"elo": 1630, "title": "Gladiator", "wins": 3, "losses": 2}
    board = await conns[2].request({"type": "leaderboard", "game": "mkd", "mode": "kombat"})
    assert [(r["name"], r["elo"], r["title"]) for r in board["rows"]] == [
        ("Fujin", 1630, "Gladiator"), ("Raijin", 1570, "Expert"), ("Shinnok", 1287, "Beginner")]
    assert board["me"] == {"rank": 3, "elo": 1287, "title": "Beginner", "wins": 3, "losses": 2}

    players = conns[:2]
    udps = [await udp_peers(), await udp_peers()]
    queued = [await c.request(queue_msg(ranked=True)) for c in players]
    assert [(q["elo"], q["title"]) for q in queued] == [(1630, "Gladiator"), (1570, "Expert")]
    # the opponent stays hidden until both accept, then comes with their rating and its title
    founds = [await c.expect("match_found") for c in players]
    assert all(f["opponent"] is None for f in founds)
    for c, udp, found in zip(players, udps, founds):
        await hello_and_accept(server, c, udp, found)
    peers = [await c.expect("match_peer") for c in players]
    assert (peers[0]["opponent"]["elo"], peers[0]["opponent"]["title"]) == (1570, "Expert")
    assert (peers[1]["opponent"]["elo"], peers[1]["opponent"]["title"]) == (1630, "Gladiator")
    reports = await asyncio.gather(*[
        udp.punch(peer["peer"], match=peer["match"], key=peer["key"], side=peer["side"], timeout=5)
        for udp, peer in zip(udps, peers)])
    for c, peer, report in zip(players, peers, reports):
        c.send({"type": "ping_report", "match": peer["match"], **report})
    gos = [await c.expect("match_go") for c in players]

    # a set's state names the opponent's title; its end, each player's title before and after
    by_side = {go["side"]: c for c, go in zip(players, gos)}
    s1, s2 = await play(by_side[1], by_side[2], gos[0]["match"], 1)
    assert {s["opponent"]: s["opponent_title"] for s in (s1, s2)} == {"Raijin": "Expert", "Fujin": "Gladiator"}
    left = await players[0].request({"type": "series_ready", "series": s1["series"], "ready": False})
    assert left["type"] == "series_left"
    over = await players[1].expect("series", state="over")
    assert over["winner"] == "Raijin"
    assert (over["elo"]["Fujin"]["title_before"], over["elo"]["Fujin"]["title_after"]) == ("Gladiator", "Warrior")
    # up a rank, from beating a Gladiator who left the set
    assert over["elo"]["Raijin"] == {"before": 1570, "after": 1589, "delta": 19, "title_before": "Expert",
                                     "title_after": "Warrior"}


async def test_lobby_members_come_with_their_titles_in_its_game_and_mode(server, connect):
    host, guest = await connect("Kitana"), await connect("Jade")
    set_rating(server, "Kitana", 1990)  # a God at kombat
    set_rating(server, "Kitana", 1400, mode="chess")  # a Beginner at chess kombat
    created = await host.request(create_msg())
    code = created["lobby"]["id"]
    assert [(m["name"], m["title"]) for m in created["lobby"]["members"]] == [("Kitana", "God")]
    # Jade never played ranked: the title everyone starts with
    joined = await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    assert [(m["name"], m["title"]) for m in joined["lobby"]["members"]] == [("Kitana", "God"), ("Jade", "Fighter")]
    await guest.request({"type": "lobby_leave"})
    await host.request({"type": "lobby_leave"})
    chess = await host.request(create_msg(mode="chess"))
    assert [(m["name"], m["title"]) for m in chess["lobby"]["members"]] == [("Kitana", "Beginner")]
