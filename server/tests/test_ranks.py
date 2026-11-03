"""Ranked titles by Elo (0.1.21): the ladder, and the title the server sends with every ranked rating."""

from __future__ import annotations

import asyncio

from server import elo

from .conftest import hello_and_accept, queue_msg
from .test_series import play

LADDER = [
    ("Ed Boon", None), ("Apprentice", 1300), ("Kombatant", 1450), ("Warrior", 1550), ("Gladiator", 1600),
    ("Master", 1650), ("Grand Master", 1700), ("Demigod", 1750), ("God", 1800), ("Elder God", 1850),
    ("Elder God Supreme", 1900), ("Elder God of Destruction", 2000),
]


def test_titles_by_elo():
    assert [(r["title"], r["from"]) for r in elo.ladder()] == LADDER
    assert elo.title(elo.INITIAL) == "Kombatant"  # where everyone starts
    for (name, floor), (_, upper) in zip(LADDER, LADDER[1:] + [(None, None)]):
        if floor is not None:
            assert elo.title(floor) == name
        if upper is not None:
            assert elo.title(upper - 1) == name
    assert elo.title(-40) == "Ed Boon" and elo.title(3000) == "Elder God of Destruction"


def set_rating(server, name, rating, game="mkd", mode="kombat"):
    account = server.db.account_by_name(name)["id"]
    server.db.conn.execute(
        "INSERT OR REPLACE INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
        "VALUES (?, ?, ?, 'ranked', ?, 3, 2, 0)", (account, game, mode, rating))


async def test_every_ranked_rating_comes_with_its_title(server, connect, udp_peers):
    conns = [await connect(n) for n in ("Fujin", "Raijin", "Shinnok")]
    assert conns[0].welcome["ranks"] == elo.ladder()  # the ladder, for the launcher to show
    for name, rating in (("Fujin", 1612), ("Raijin", 1549), ("Shinnok", 1287)):
        set_rating(server, name, rating)

    prof = await conns[0].request({"type": "profile"})
    assert prof["stats"]["mkd"]["kombat"]["ranked"] == {"elo": 1612, "title": "Gladiator", "wins": 3, "losses": 2}
    board = await conns[2].request({"type": "leaderboard", "game": "mkd", "mode": "kombat"})
    assert [(r["name"], r["elo"], r["title"]) for r in board["rows"]] == [
        ("Fujin", 1612, "Gladiator"), ("Raijin", 1549, "Kombatant"), ("Shinnok", 1287, "Ed Boon")]
    assert board["me"] == {"rank": 3, "elo": 1287, "title": "Ed Boon", "wins": 3, "losses": 2}

    players = conns[:2]
    udps = [await udp_peers(), await udp_peers()]
    queued = [await c.request(queue_msg(ranked=True)) for c in players]
    assert [(q["elo"], q["title"]) for q in queued] == [(1612, "Gladiator"), (1549, "Kombatant")]
    # the opponent stays hidden until both accept, then comes with their rating and its title
    founds = [await c.expect("match_found") for c in players]
    assert all(f["opponent"] is None for f in founds)
    for c, udp, found in zip(players, udps, founds):
        await hello_and_accept(server, c, udp, found)
    peers = [await c.expect("match_peer") for c in players]
    assert (peers[0]["opponent"]["elo"], peers[0]["opponent"]["title"]) == (1549, "Kombatant")
    assert (peers[1]["opponent"]["elo"], peers[1]["opponent"]["title"]) == (1612, "Gladiator")
    reports = await asyncio.gather(*[
        udp.punch(peer["peer"], match=peer["match"], key=peer["key"], side=peer["side"], timeout=5)
        for udp, peer in zip(udps, peers)])
    for c, peer, report in zip(players, peers, reports):
        c.send({"type": "ping_report", "match": peer["match"], **report})
    gos = [await c.expect("match_go") for c in players]

    # a set's state names the opponent's title; its end, each player's title before and after
    by_side = {go["side"]: c for c, go in zip(players, gos)}
    s1, s2 = await play(by_side[1], by_side[2], gos[0]["match"], 1)
    assert {s["opponent"]: s["opponent_title"] for s in (s1, s2)} == {"Raijin": "Kombatant", "Fujin": "Gladiator"}
    left = await players[0].request({"type": "series_ready", "series": s1["series"], "ready": False})
    assert left["type"] == "series_left"
    over = await players[1].expect("series", state="over")
    assert over["winner"] == "Raijin"
    assert (over["elo"]["Fujin"]["title_before"], over["elo"]["Fujin"]["title_after"]) == ("Gladiator", "Warrior")
    # up a rank, from beating a Gladiator who left the set
    assert over["elo"]["Raijin"] == {"before": 1549, "after": 1568, "delta": 19, "title_before": "Kombatant",
                                     "title_after": "Warrior"}
